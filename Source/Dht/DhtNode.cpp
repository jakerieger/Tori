// Copyright 2026 Jake Rieger
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Dht/DhtNode.hpp"

#include "Core/Random.hpp"
#include "Dht/RoutingTable.hpp"
#include "Net/Socket.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace Tori {
    namespace {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        constexpr int kAlpha                       = 4;  // Queries in flight per lookup
        constexpr std::size_t kK                   = 8;  // Closest nodes a lookup must hear from
        constexpr std::size_t kMaxLookupCandidates = 128;
        constexpr auto kQueryTimeout               = 5s;
        constexpr auto kLookupTimeout              = 60s;
        constexpr auto kTokenRotation              = 5min;
        constexpr auto kRefreshInterval            = 15min;
        constexpr auto kBootstrapRetry             = 30s;
        constexpr auto kSnapshotInterval           = 2s;
        constexpr auto kStoredPeerLifetime         = 30min;
        constexpr std::size_t kMaxStoredPeers      = 100;  // Per info hash
        constexpr std::size_t kMaxStoredHashes     = 1000;
        constexpr std::size_t kMinHealthyNodes     = 8;

        constexpr std::array<std::pair<const char*, const char*>, 4> kPublicRouters = {{
          {"router.bittorrent.com", "6881"},
          {"dht.transmissionbt.com", "6881"},
          {"router.utorrent.com", "6881"},
          {"dht.libtorrent.org", "25401"},
        }};

        std::string IdString(const NodeId& Id) {
            return {reinterpret_cast<const char*>(Id.data()), Id.size()};
        }

        std::optional<NodeId> IdFrom(const std::string* Text) {
            if (!Text || Text->size() != 20) { return std::nullopt; }
            NodeId Id {};
            std::memcpy(Id.data(), Text->data(), 20);
            return Id;
        }

        std::string CompactPeer(const Endpoint& Peer) {
            std::string Out(reinterpret_cast<const char*>(Peer.Address.data()), 4);
            Out += static_cast<char>(Peer.Port >> 8);
            Out += static_cast<char>(Peer.Port & 0xFF);
            return Out;
        }

        std::vector<Endpoint> ResolvePublicRouters() {
            std::vector<Endpoint> Routers;
            for (const auto& [Host, Port] : kPublicRouters) {
                addrinfo Hints {};
                Hints.ai_family   = AF_INET;
                Hints.ai_socktype = SOCK_DGRAM;
                addrinfo* Results = nullptr;
                if (getaddrinfo(Host, Port, &Hints, &Results) != 0) { continue; }
                for (const addrinfo* Info = Results; Info; Info = Info->ai_next) {
                    if (const auto Address = FromSockaddr(Info->ai_addr)) { Routers.push_back(*Address); }
                }
                freeaddrinfo(Results);
            }
            return Routers;
        }
    }  // namespace

    struct DhtNode::Shared {
        DhtSettings Settings;
        DhtPeersCallback OnPeers;
        UniqueSocket Socket;
        NodeId Self {};
        std::uint16_t Port = 0;

        struct Command {
            bool IsLookup = true;
            InfoHash Hash;
            std::uint16_t AnnouncePort = 0;
            Endpoint Node;
        };

        mutable std::mutex Mutex;
        std::vector<Command> Commands;
        std::vector<Endpoint> Nodes;
        std::atomic<std::size_t> NodeCount {0};
    };

    class DhtNode::Worker {
    public:
        explicit Worker(Shared& State) : _State(State), _Table(State.Self) {
            RandomBytes(_Secret);
            _PreviousSecret = _Secret;
        }

        void Run(const std::stop_token& Stop) {
            Bootstrap(Clock::now());
            std::vector<std::uint8_t> Buffer(4096);

            while (!Stop.stop_requested()) {
                WSAPOLLFD Poll {_State.Socket.Get(), POLLRDNORM, 0};
                WSAPoll(&Poll, 1, 50);
                const Clock::time_point Now = Clock::now();

                ProcessCommands(Now);
                while (true) {
                    sockaddr_storage From {};
                    int FromLength     = sizeof(From);
                    const int Received = recvfrom(_State.Socket.Get(),
                                                  reinterpret_cast<char*>(Buffer.data()),
                                                  static_cast<int>(Buffer.size()),
                                                  0,
                                                  reinterpret_cast<sockaddr*>(&From),
                                                  &FromLength);
                    if (Received == SOCKET_ERROR) {
                        // WSAECONNRESET is an ICMP "port unreachable" for an earlier send; keep reading
                        if (WSAGetLastError() == WSAECONNRESET) { continue; }
                        break;
                    }
                    if (const auto Sender = FromSockaddr(reinterpret_cast<const sockaddr*>(&From))) {
                        HandlePacket(std::span(Buffer.data(), static_cast<std::size_t>(Received)),
                                     Sender->Unmapped(),
                                     Now);
                    }
                }

                ExpirePending(Now);
                std::erase_if(_Lookups, [&](Lookup& Target) { return Step(Target, Now); });

                if (Now >= _NextRotation) {
                    _PreviousSecret = _Secret;
                    RandomBytes(_Secret);
                    _NextRotation = Now + kTokenRotation;
                }
                if (_Table.Size() < kMinHealthyNodes && Now >= _NextBootstrap) { Bootstrap(Now); }
                if (Now >= _NextRefresh) {
                    NodeId Nearby = _State.Self;  // Same leading bytes as us, so it refreshes our neighbourhood
                    RandomBytes(std::span(Nearby).subspan(5));
                    StartLookup(false, Nearby, 0, {}, Now);
                    _NextRefresh = Now + kRefreshInterval;
                }
                _State.NodeCount = _Table.Size();
                if (Now >= _NextSnapshot) {
                    PublishSnapshot();
                    _NextSnapshot = Now + kSnapshotInterval;
                }
            }
        }

    private:
        struct Pending {
            std::uint64_t LookupId = 0;
            Endpoint To;
            Clock::time_point Sent;
        };

        struct Candidate {
            enum class Status {
                Fresh,
                Querying,
                Responded,
                Failed,
            };

            NodeContact Contact;
            Status State = Status::Fresh;
            std::string Token;
        };

        struct Lookup {
            std::uint64_t Id = 0;
            bool ForPeers    = false;
            NodeId Target {};
            std::uint16_t AnnouncePort = 0;
            std::vector<Candidate> Candidates;  // Closest first
            std::set<Endpoint> PeersSeen;
            Clock::time_point Started;
        };

        struct StoredPeer {
            Endpoint Address;
            Clock::time_point Expires;
        };

        // ---- Lookups -------------------------------------------------------------------------------------------

        void Bootstrap(const Clock::time_point Now) {
            std::vector<Endpoint> Seeds = _State.Settings.BootstrapNodes;
            if (_State.Settings.UsePublicRouters) {
                const std::vector<Endpoint> Routers = ResolvePublicRouters();
                Seeds.insert(Seeds.end(), Routers.begin(), Routers.end());
            }
            StartLookup(false, _State.Self, 0, Seeds, Now);
            _NextBootstrap = Now + kBootstrapRetry;
        }

        void StartLookup(const bool ForPeers,
                         const NodeId& Target,
                         const std::uint16_t AnnouncePort,
                         const std::vector<Endpoint>& Seeds,
                         const Clock::time_point Now) {
            Lookup Created;
            Created.Id           = ++_NextLookupId;
            Created.ForPeers     = ForPeers;
            Created.Target       = Target;
            Created.AnnouncePort = AnnouncePort;
            Created.Started      = Now;
            for (const NodeContact& Node : _Table.Closest(Target, kK * 2)) {
                AddCandidate(Created, Node);
            }

            // Seeds have unknown IDs; treat them as maximally far so known nodes are tried first
            NodeId Far = Target;
            for (std::uint8_t& Byte : Far) {
                Byte = static_cast<std::uint8_t>(~Byte);
            }
            for (const Endpoint& Seed : Seeds) {
                AddCandidate(Created, {Far, Seed});
            }

            _Lookups.push_back(std::move(Created));
        }

        void AddCandidate(Lookup& Target, const NodeContact& Node) {
            if (Node.Id == _State.Self || Node.Address.Port == 0 || Node.Address.IsV6) { return; }
            const bool Known = std::ranges::any_of(Target.Candidates, [&](const Candidate& Existing) {
                return Existing.Contact.Address == Node.Address ||
                       (Existing.Contact.Id == Node.Id && Existing.State != Candidate::Status::Fresh);
            });
            if (Known) { return; }

            const auto Position = std::ranges::find_if(Target.Candidates, [&](const Candidate& Existing) {
                return CloserTo(Target.Target, Node.Id, Existing.Contact.Id);
            });
            Target.Candidates.insert(Position, {Node});
            if (Target.Candidates.size() > kMaxLookupCandidates) { Target.Candidates.pop_back(); }
        }

        void SortCandidates(Lookup& Target) {
            std::ranges::stable_sort(Target.Candidates, [&](const Candidate& A, const Candidate& B) {
                return CloserTo(Target.Target, A.Contact.Id, B.Contact.Id);
            });
        }

        /// Advances a lookup. Returns true once it's finished: the closest kK live nodes have all answered.
        bool Step(Lookup& Target, const Clock::time_point Now) {
            bool Finished = Now - Target.Started > kLookupTimeout;

            if (!Finished) {
                int InFlight = static_cast<int>(std::ranges::count_if(Target.Candidates, [](const Candidate& C) {
                    return C.State == Candidate::Status::Querying;
                }));
                std::size_t Considered = 0;
                bool ClosestAnswered   = true;
                for (Candidate& Entry : Target.Candidates) {
                    if (Entry.State == Candidate::Status::Failed) { continue; }
                    if (Considered++ >= kK) { break; }
                    if (Entry.State == Candidate::Status::Responded) { continue; }
                    ClosestAnswered = false;
                    if (Entry.State == Candidate::Status::Fresh && InFlight < kAlpha) {
                        SendLookupQuery(Target, Entry, Now);
                        ++InFlight;
                    }
                }
                // An empty lookup waits (until its timeout) for bootstrapping to fill the routing table
                if (Considered == 0 && Target.Candidates.empty()) {
                    for (const NodeContact& Node : _Table.Closest(Target.Target, kK * 2)) {
                        AddCandidate(Target, Node);
                    }
                    return false;
                }
                Finished = (ClosestAnswered || Considered == 0) && InFlight == 0;
            }

            if (Finished && Target.ForPeers && Target.AnnouncePort != 0) {
                int Announced = 0;
                for (const Candidate& Entry : Target.Candidates) {
                    if (Entry.State != Candidate::Status::Responded || Entry.Token.empty()) { continue; }
                    SendQuery(Entry.Contact.Address,
                              "announce_peer",
                              {
                                {"info_hash", IdString(Target.Target)},
                                {"port", Target.AnnouncePort},
                                {"token", Entry.Token},
                                {"implied_port", 0},
                              },
                              0,
                              Now);
                    if (++Announced == static_cast<int>(kK)) { break; }
                }
            }
            return Finished;
        }

        void SendLookupQuery(Lookup& Target, Candidate& Entry, const Clock::time_point Now) {
            Entry.State = Candidate::Status::Querying;
            if (Target.ForPeers) {
                SendQuery(Entry.Contact.Address, "get_peers", {{"info_hash", IdString(Target.Target)}}, Target.Id, Now);
            } else {
                SendQuery(Entry.Contact.Address, "find_node", {{"target", IdString(Target.Target)}}, Target.Id, Now);
            }
        }

        Lookup* FindLookup(const std::uint64_t Id) {
            const auto It = std::ranges::find_if(_Lookups, [&](const Lookup& Target) { return Target.Id == Id; });
            return It == _Lookups.end() ? nullptr : &*It;
        }

        static Candidate* FindCandidate(Lookup& Target, const Endpoint& Address) {
            const auto It =
              std::ranges::find_if(Target.Candidates, [&](const Candidate& C) { return C.Contact.Address == Address; });
            return It == Target.Candidates.end() ? nullptr : &*It;
        }

        // ---- Sending -------------------------------------------------------------------------------------------

        void SendQuery(const Endpoint& To,
                       const std::string_view Method,
                       Bencode::Dict Arguments,
                       const std::uint64_t LookupId,
                       const Clock::time_point Now) {
            const std::uint16_t Number = _NextTransaction++;
            const std::string Transaction {static_cast<char>(Number >> 8), static_cast<char>(Number & 0xFF)};
            Arguments.emplace_back("id", IdString(_State.Self));
            _Pending[Transaction] = {LookupId, To, Now};
            SendTo(To, EncodeKrpcQuery(Transaction, Method, std::move(Arguments)));
        }

        void SendTo(const Endpoint& To, const std::string& Packet) {
            if (To.IsV6) { return; }
            sockaddr_storage Address {};
            const int Length = ToSockaddr(To, Address);
            sendto(_State.Socket.Get(),
                   Packet.data(),
                   static_cast<int>(Packet.size()),
                   0,
                   reinterpret_cast<const sockaddr*>(&Address),
                   Length);
        }

        // ---- Receiving -----------------------------------------------------------------------------------------

        void
        HandlePacket(const std::span<const std::uint8_t> Packet, const Endpoint& From, const Clock::time_point Now) {
            const std::optional<KrpcMessage> Message = DecodeKrpc(Packet);
            if (!Message) { return; }

            if (Message->Type == KrpcMessage::Kind::Query) {
                HandleQuery(*Message, From, Now);
                return;
            }

            // Only accept replies from the address the query went to
            const auto It = _Pending.find(Message->Transaction);
            if (It == _Pending.end() || It->second.To != From) { return; }
            const Pending Request = It->second;
            _Pending.erase(It);

            if (Message->Type == KrpcMessage::Kind::Response && Message->SenderId) {
                OnResponse(Request, *Message, From, Now);
            } else {
                OnFailure(Request);
            }
        }

        void OnResponse(const Pending& Request,
                        const KrpcMessage& Message,
                        const Endpoint& From,
                        const Clock::time_point Now) {
            _Table.Seen({*Message.SenderId, From}, Now);

            Lookup* Target = Request.LookupId ? FindLookup(Request.LookupId) : nullptr;
            if (!Target) { return; }

            if (Candidate* Entry = FindCandidate(*Target, From)) {
                Entry->Contact.Id = *Message.SenderId;  // Bootstrap seeds start with a placeholder ID
                Entry->State      = Candidate::Status::Responded;
                if (const std::string* Token = Message.Body.FindString("token")) { Entry->Token = *Token; }
                SortCandidates(*Target);
            }

            if (const std::string* Nodes = Message.Body.FindString("nodes")) {
                for (const NodeContact& Node : DecodeCompactNodes(*Nodes)) {
                    AddCandidate(*Target, Node);
                }
            }

            if (const Bencode::List* Values = Message.Body.FindList("values"); Values && Target->ForPeers) {
                std::vector<Endpoint> Found;
                for (const Bencode::Value& Value : *Values) {
                    if (!Value.IsString()) { continue; }
                    for (const Endpoint& Peer : ParseCompactPeers(Value.AsString(), false)) {
                        if (Target->PeersSeen.insert(Peer).second) { Found.push_back(Peer); }
                    }
                }
                if (!Found.empty() && _State.OnPeers) {
                    InfoHash Hash;
                    Hash.Bytes = Target->Target;
                    _State.OnPeers(Hash, std::move(Found));
                }
            }
        }

        void OnFailure(const Pending& Request) {
            _Table.Failed(Request.To);
            if (Lookup* Target = Request.LookupId ? FindLookup(Request.LookupId) : nullptr) {
                if (Candidate* Entry = FindCandidate(*Target, Request.To)) { Entry->State = Candidate::Status::Failed; }
            }
        }

        void ExpirePending(const Clock::time_point Now) {
            for (auto It = _Pending.begin(); It != _Pending.end();) {
                if (Now - It->second.Sent < kQueryTimeout) {
                    ++It;
                    continue;
                }
                const Pending Request = It->second;
                It                    = _Pending.erase(It);
                OnFailure(Request);
            }
        }

        // ---- Answering other nodes -----------------------------------------------------------------------------

        std::string MakeToken(const Endpoint& Requester, const std::array<std::uint8_t, 8>& Secret) const {
            std::string Input(reinterpret_cast<const char*>(Secret.data()), Secret.size());
            Input.append(reinterpret_cast<const char*>(Requester.Address.data()), 4);
            const Sha1Digest Digest = Sha1::Hash(Input);
            return {reinterpret_cast<const char*>(Digest.data()), 8};
        }

        void HandleQuery(const KrpcMessage& Query, const Endpoint& From, const Clock::time_point Now) {
            if (!Query.SenderId) {
                SendTo(From, EncodeKrpcError(Query.Transaction, 203, "missing id"));
                return;
            }

            Bencode::Dict Reply = {{"id", IdString(_State.Self)}};

            if (Query.Method == "ping") {
                // Nothing beyond our ID
            } else if (Query.Method == "find_node") {
                const std::optional<NodeId> Target = IdFrom(Query.Body.FindString("target"));
                if (!Target) { return SendTo(From, EncodeKrpcError(Query.Transaction, 203, "bad target")); }
                const std::vector<NodeContact> Closest = _Table.Closest(*Target, kK);
                Reply.emplace_back("nodes", EncodeCompactNodes(Closest));
            } else if (Query.Method == "get_peers") {
                const std::optional<NodeId> Hash = IdFrom(Query.Body.FindString("info_hash"));
                if (!Hash) { return SendTo(From, EncodeKrpcError(Query.Transaction, 203, "bad info_hash")); }
                Reply.emplace_back("token", MakeToken(From, _Secret));
                const std::vector<NodeContact> Closest = _Table.Closest(*Hash, kK);
                Reply.emplace_back("nodes", EncodeCompactNodes(Closest));

                InfoHash Key;
                Key.Bytes = *Hash;
                if (const auto Stored = _Store.find(Key); Stored != _Store.end()) {
                    std::erase_if(Stored->second, [&](const StoredPeer& Peer) { return Now >= Peer.Expires; });
                    Bencode::List Values;
                    for (const StoredPeer& Peer : Stored->second) {
                        Values.emplace_back(CompactPeer(Peer.Address));
                    }
                    if (!Values.empty()) { Reply.emplace_back("values", std::move(Values)); }
                }
            } else if (Query.Method == "announce_peer") {
                const std::optional<NodeId> Hash = IdFrom(Query.Body.FindString("info_hash"));
                const std::string* Token         = Query.Body.FindString("token");
                const Bencode::Integer* Port     = Query.Body.FindInteger("port");
                const Bencode::Integer* Implied  = Query.Body.FindInteger("implied_port");
                if (!Hash || !Token ||
                    (*Token != MakeToken(From, _Secret) && *Token != MakeToken(From, _PreviousSecret))) {
                    return SendTo(From, EncodeKrpcError(Query.Transaction, 203, "bad token"));
                }
                Endpoint Peer = From;
                if (!(Implied && *Implied == 1)) {
                    if (!Port || *Port <= 0 || *Port > 65535) {
                        return SendTo(From, EncodeKrpcError(Query.Transaction, 203, "bad port"));
                    }
                    Peer.Port = static_cast<std::uint16_t>(*Port);
                }
                StorePeer(*Hash, Peer, Now);
            } else {
                return SendTo(From, EncodeKrpcError(Query.Transaction, 204, "method unknown"));
            }

            SendTo(From, EncodeKrpcResponse(Query.Transaction, std::move(Reply)));
            _Table.Seen({*Query.SenderId, From}, Now);
        }

        void StorePeer(const NodeId& Hash, const Endpoint& Peer, const Clock::time_point Now) {
            InfoHash Key;
            Key.Bytes = Hash;
            if (!_Store.contains(Key) && _Store.size() >= kMaxStoredHashes) { return; }

            std::vector<StoredPeer>& Peers = _Store[Key];
            std::erase_if(Peers, [&](const StoredPeer& Existing) {
                return Existing.Address == Peer || Now >= Existing.Expires;
            });
            if (Peers.size() >= kMaxStoredPeers) { Peers.erase(Peers.begin()); }
            Peers.push_back({Peer, Now + kStoredPeerLifetime});
        }

        // ---- Plumbing ------------------------------------------------------------------------------------------

        void ProcessCommands(const Clock::time_point Now) {
            std::vector<Shared::Command> Commands;
            {
                std::scoped_lock Lock(_State.Mutex);
                Commands.swap(_State.Commands);
            }
            for (const Shared::Command& Item : Commands) {
                if (Item.IsLookup) {
                    StartLookup(true, Item.Hash.Bytes, Item.AnnouncePort, {}, Now);
                } else {
                    SendQuery(Item.Node, "ping", {}, 0, Now);
                }
            }
        }

        void PublishSnapshot() {
            std::vector<Endpoint> Nodes;
            for (const NodeContact& Node : _Table.All()) {
                Nodes.push_back(Node.Address);
            }
            std::scoped_lock Lock(_State.Mutex);
            _State.Nodes = std::move(Nodes);
        }

        Shared& _State;
        RoutingTable _Table;
        std::map<std::string, Pending> _Pending;
        std::uint16_t _NextTransaction = 0;
        std::vector<Lookup> _Lookups;
        std::uint64_t _NextLookupId = 0;
        std::map<InfoHash, std::vector<StoredPeer>> _Store;

        std::array<std::uint8_t, 8> _Secret {};
        std::array<std::uint8_t, 8> _PreviousSecret {};

        Clock::time_point _NextRotation = Clock::now() + kTokenRotation;
        Clock::time_point _NextBootstrap {};
        Clock::time_point _NextRefresh = Clock::now() + kRefreshInterval;
        Clock::time_point _NextSnapshot {};
    };

    DhtNode::DhtNode(DhtSettings Settings, DhtPeersCallback OnPeers) : _Shared(std::make_shared<Shared>()) {
        EnsureWinsock();
        _Shared->Settings = std::move(Settings);
        _Shared->OnPeers  = std::move(OnPeers);
        if (_Shared->Settings.Id) {
            _Shared->Self = *_Shared->Settings.Id;
        } else {
            RandomBytes(_Shared->Self);
        }

        for (const std::uint16_t Port : {_Shared->Settings.Port, std::uint16_t {0}}) {
            UniqueSocket Socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
            if (!Socket) { break; }
            sockaddr_in Address {};
            Address.sin_family      = AF_INET;
            Address.sin_addr.s_addr = htonl(_Shared->Settings.LoopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
            Address.sin_port        = htons(Port);
            if (bind(Socket.Get(), reinterpret_cast<const sockaddr*>(&Address), sizeof(Address)) == 0) {
                _Shared->Socket = std::move(Socket);
                break;
            }
        }
        if (!_Shared->Socket) { throw std::runtime_error("could not open the DHT socket"); }

        u_long NonBlocking = 1;
        ioctlsocket(_Shared->Socket.Get(), FIONBIO, &NonBlocking);
        sockaddr_storage Bound {};
        int Length = sizeof(Bound);
        getsockname(_Shared->Socket.Get(), reinterpret_cast<sockaddr*>(&Bound), &Length);
        _Shared->Port = FromSockaddr(reinterpret_cast<const sockaddr*>(&Bound))->Port;

        _Thread = std::jthread([State = _Shared](const std::stop_token Stop) { Worker(*State).Run(Stop); });
    }

    DhtNode::~DhtNode() = default;

    void DhtNode::GetPeers(const InfoHash& Hash, const std::uint16_t AnnouncePort) {
        std::scoped_lock Lock(_Shared->Mutex);
        _Shared->Commands.push_back({.IsLookup = true, .Hash = Hash, .AnnouncePort = AnnouncePort});
    }

    void DhtNode::AddNode(const Endpoint& Address) {
        std::scoped_lock Lock(_Shared->Mutex);
        _Shared->Commands.push_back({.IsLookup = false, .Node = Address.Unmapped()});
    }

    std::uint16_t DhtNode::Port() const {
        return _Shared->Port;
    }

    const NodeId& DhtNode::Id() const {
        return _Shared->Self;
    }

    std::size_t DhtNode::NodeCount() const {
        return _Shared->NodeCount.load();
    }

    std::vector<Endpoint> DhtNode::KnownNodes() const {
        std::scoped_lock Lock(_Shared->Mutex);
        return _Shared->Nodes;
    }
}  // namespace Tori
