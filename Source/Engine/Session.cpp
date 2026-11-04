#include "Engine/Session.hpp"

#include "Core/Bencode.hpp"
#include "Core/Error.hpp"
#include "Core/FileIO.hpp"
#include "Core/Utf8.hpp"
#include "Core/Win32.hpp"
#include "Dht/DhtNode.hpp"
#include "Engine/Torrent.hpp"
#include "Net/Socket.hpp"
#include "Peer/Messages.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <set>
#include <stdexcept>

namespace Tori {
    namespace {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        constexpr auto kHandshakeTimeout   = 10s;
        constexpr std::size_t kMaxPending  = 64;  // Accepted connections still sending their handshake
        constexpr auto kPollTimeout        = 100ms;
        constexpr auto kSaveInterval       = 2s;     // Torrents decide what's worth saving; this just writes it
        constexpr auto kDhtSaveInterval    = 10min;

        constexpr std::int64_t kMaxResumeFileSize = 16LL * 1024 * 1024;
        constexpr std::size_t kMaxSavedDhtNodes   = 200;

        std::int64_t UnixNow() {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::filesystem::path DhtStatePath(const std::filesystem::path& Folder) {
            return Folder / L"dht.dat";
        }

        /// Reads the DHT's saved ID and nodes. Anything unreadable is ignored: the DHT just starts afresh.
        void LoadDhtState(const std::filesystem::path& Folder, DhtSettings& Settings) {
            const std::optional<std::string> Data = ReadWholeFile(DhtStatePath(Folder), kMaxResumeFileSize);
            if (!Data) { return; }
            try {
                const Bencode::Value Root = Bencode::Decode(*Data);
                if (const std::string* Id = Root.FindString("id"); Id && Id->size() == 20) {
                    NodeId Saved {};
                    std::memcpy(Saved.data(), Id->data(), Saved.size());
                    Settings.Id = Saved;
                }
                if (const std::string* Nodes = Root.FindString("nodes")) {
                    for (const Endpoint& Node : ParseCompactPeers(*Nodes, false)) { Settings.BootstrapNodes.push_back(Node); }
                }
            } catch (const ParseError&) {}
        }

        void SetNonBlocking(const SOCKET Socket) {
            u_long NonBlocking = 1;
            ioctlsocket(Socket, FIONBIO, &NonBlocking);
        }

        bool BindAndListen(const SOCKET Socket, const sockaddr* Address, const int Length) {
            return bind(Socket, Address, Length) == 0 && listen(Socket, SOMAXCONN) == 0;
        }

        /// Dual-stack on all interfaces, or 127.0.0.1 only. Tries the requested port first, then any free port.
        UniqueSocket OpenListener(const std::uint16_t Port, const bool LoopbackOnly) {
            for (const std::uint16_t Candidate : {Port, std::uint16_t {0}}) {
                if (!LoopbackOnly) {
                    UniqueSocket Socket(socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
                    if (Socket) {
                        DWORD V6Only = 0;
                        setsockopt(Socket.Get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&V6Only), sizeof(V6Only));
                        sockaddr_in6 Address {};
                        Address.sin6_family = AF_INET6;
                        Address.sin6_addr   = in6addr_any;
                        Address.sin6_port   = htons(Candidate);
                        if (BindAndListen(Socket.Get(), reinterpret_cast<const sockaddr*>(&Address), sizeof(Address))) {
                            return Socket;
                        }
                    }
                }

                UniqueSocket Socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
                if (!Socket) { continue; }
                sockaddr_in Address {};
                Address.sin_family      = AF_INET;
                Address.sin_addr.s_addr = htonl(LoopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
                Address.sin_port        = htons(Candidate);
                if (BindAndListen(Socket.Get(), reinterpret_cast<const sockaddr*>(&Address), sizeof(Address))) {
                    return Socket;
                }
            }
            throw std::runtime_error(std::format("could not listen for peers: {}", Win32ErrorMessage(WSAGetLastError())));
        }
    }  // namespace

    struct Session::Listener {
        struct PendingConnection {
            UniqueSocket Socket;
            Endpoint Remote;
            std::array<std::uint8_t, kHandshakeSize> Buffer {};
            std::size_t Received = 0;
            Clock::time_point Accepted;
        };

        UniqueSocket Socket;
        std::vector<PendingConnection> Pending;
    };

    struct Session::Retiring {
        std::unique_ptr<Torrent> Instance;
        bool DeleteFiles = false;
    };

    Session::Session(SessionSettings Settings) : _Settings(std::move(Settings)), _LocalId(GeneratePeerId()) {
        EnsureWinsock();
        if (!_Settings.StateFolder.empty()) {
            std::error_code Ec;
            for (const wchar_t* Sub : {L"resume", L"torrents"}) {
                std::filesystem::create_directories(std::filesystem::path(LongPath(_Settings.StateFolder / Sub)), Ec);
                if (Ec) {
                    throw std::runtime_error(
                      std::format("could not create {}: {}", PathToUtf8(_Settings.StateFolder / Sub), Ec.message()));
                }
            }
        }

        _Listener         = std::make_unique<Listener>();
        _Listener->Socket = OpenListener(_Settings.ListenPort, _Settings.LoopbackOnly);
        SetNonBlocking(_Listener->Socket.Get());

        sockaddr_storage Bound {};
        int Length = sizeof(Bound);
        getsockname(_Listener->Socket.Get(), reinterpret_cast<sockaddr*>(&Bound), &Length);
        _ListenPort = FromSockaddr(reinterpret_cast<const sockaddr*>(&Bound))->Port;

        if (_Settings.EnableDht) {
            DhtSettings Dht;
            Dht.Port             = _ListenPort;
            Dht.LoopbackOnly     = _Settings.DhtLoopbackOnly;
            Dht.UsePublicRouters = _Settings.DhtUsePublicRouters;
            Dht.BootstrapNodes   = _Settings.DhtBootstrapNodes;
            if (!_Settings.StateFolder.empty()) { LoadDhtState(_Settings.StateFolder, Dht); }
            try {
                _Dht = std::make_unique<DhtNode>(std::move(Dht), [this](const InfoHash& Hash, std::vector<Endpoint> Peers) {
                    OnDhtPeers(Hash, std::move(Peers));
                });
            } catch (const std::exception&) {
                // Trackers still work without it
            }
        }

        if (!_Settings.StateFolder.empty()) { RestoreTorrents(); }
        _Thread = std::jthread([this](const std::stop_token Stop) { ListenLoop(Stop); });
    }

    Session::~Session() {
        _Thread.request_stop();
        _Thread.join();

        std::scoped_lock SaveLock(_SaveMutex);
        std::map<TorrentId, std::unique_ptr<Torrent>> Torrents;
        std::vector<Retiring> Retired;
        {
            // Ask everything to stop first, so the torrents wind down in parallel rather than one after another
            std::scoped_lock Lock(_Mutex);
            for (auto& [Id, Instance] : _Torrents) { Instance->RequestStop(); }
            for (Retiring& Entry : _Retiring) { Entry.Instance->RequestStop(); }
            Torrents = std::move(_Torrents);
            Retired  = std::move(_Retiring);
        }

        // Joined outside _Mutex, since DHT callbacks take it. Each worker publishes its final state as it stops.
        std::vector<std::pair<TorrentId, const Torrent*>> Saving;
        for (auto& [Id, Instance] : Torrents) {
            Instance->Join();
            Saving.emplace_back(Id, Instance.get());
        }
        SaveTorrents(Saving);
        Torrents.clear();

        for (Retiring& Entry : Retired) {
            Entry.Instance->Join();
            const std::shared_ptr<const Metainfo> Metadata = Entry.Instance->Metadata();
            if (Entry.DeleteFiles && Metadata) { DeleteTorrentFiles(Entry.Instance->Settings().Destination, Metadata->Files); }
        }
        Retired.clear();

        SaveDht();

        // Last, since torrents use it until they stop. Joined outside the lock: its callbacks take _Mutex.
        std::unique_ptr<DhtNode> Dht;
        {
            std::scoped_lock Lock(_DhtMutex);
            Dht = std::move(_Dht);
        }
    }

    template<class TParams>
    TorrentId Session::Launch(TParams Params) {
        std::scoped_lock Lock(_Mutex);
        for (const auto& [Id, Existing] : _Torrents) {
            if (Existing->Hash() == Params.Hash) {
                throw std::runtime_error(std::format("\"{}\" is already added", Params.DisplayName));
            }
        }

        const TorrentId Id     = _NextId++;
        Params.Id              = Id;
        Params.LocalId         = _LocalId;
        Params.ListenPort      = _ListenPort;
        Params.Limits.MaxPeers = _Settings.MaxPeersPerTorrent;
        Params.Announce        = _Settings.Announce;
        if (_Settings.EnableDht) {
            Params.FindPeers = [this](const InfoHash& Hash, const std::uint16_t AnnouncePort) {
                std::scoped_lock DhtLock(_DhtMutex);
                if (_Dht) { _Dht->GetPeers(Hash, AnnouncePort); }
            };
            Params.AddDhtNode = [this](const Endpoint& Node) {
                std::scoped_lock DhtLock(_DhtMutex);
                if (_Dht) { _Dht->AddNode(Node); }
            };
        }
        _Torrents.emplace(Id, std::make_unique<Torrent>(std::move(Params)));
        return Id;
    }

    TorrentId Session::AddTorrent(Metainfo Meta,
                                  std::filesystem::path Destination,
                                  const bool Paused,
                                  std::vector<int> SkippedFiles) {
        Torrent::Params Params;
        Params.Hash        = Meta.Hash;
        Params.DisplayName = Meta.Name;
        Params.Meta         = std::move(Meta);
        Params.Destination  = std::move(Destination);
        Params.StartPaused  = Paused;
        Params.AddedOn      = UnixNow();
        Params.SkippedFiles = std::move(SkippedFiles);
        return Launch(std::move(Params));
    }

    TorrentId Session::AddMagnet(const MagnetUri& Magnet, std::filesystem::path Destination, const bool Paused) {
        Torrent::Params Params;
        Params.Hash        = Magnet.Hash;
        Params.DisplayName = Magnet.DisplayName.empty() ? Magnet.Hash.ToHex() : Magnet.DisplayName;
        Params.Destination = std::move(Destination);
        Params.StartPaused = Paused;
        Params.AddedOn     = UnixNow();
        // Magnet links don't say how trackers are grouped, so each gets its own tier and all are used
        for (const std::string& Tracker : Magnet.Trackers) { Params.ExtraTrackers.push_back({Tracker}); }
        for (const std::string& Peer : Magnet.Peers) {
            if (const std::optional<Endpoint> Address = Endpoint::Parse(Peer)) { Params.InitialPeers.push_back(*Address); }
        }
        return Launch(std::move(Params));
    }

    void Session::OnDhtPeers(const InfoHash& Hash, std::vector<Endpoint> Peers) {
        std::scoped_lock Lock(_Mutex);
        for (const auto& [Id, Instance] : _Torrents) {
            if (Instance->Hash() == Hash) {
                Instance->AddPeers(std::move(Peers));
                return;
            }
        }
    }

    std::size_t Session::DhtNodeCount() const {
        std::scoped_lock Lock(_DhtMutex);
        return _Dht ? _Dht->NodeCount() : 0;
    }

    std::uint16_t Session::DhtPort() const {
        std::scoped_lock Lock(_DhtMutex);
        return _Dht ? _Dht->Port() : 0;
    }

    void Session::RemoveTorrent(const TorrentId Id, const bool DeleteFiles) {
        std::scoped_lock SaveLock(_SaveMutex);
        InfoHash Hash;
        {
            std::scoped_lock Lock(_Mutex);
            const auto It = _Torrents.find(Id);
            if (It == _Torrents.end()) { return; }
            Hash = It->second->Hash();
            It->second->RequestStop();
            _Retiring.push_back({std::move(It->second), DeleteFiles});
            _Torrents.erase(It);
        }
        ForgetSaved(Hash);
    }

    // ---- Persistence ---------------------------------------------------------------------------------------------

    std::filesystem::path Session::ResumePath(const InfoHash& Hash) const {
        return _Settings.StateFolder / L"resume" / PathFromUtf8(Hash.ToHex() + ".dat");
    }

    std::filesystem::path Session::TorrentPath(const InfoHash& Hash) const {
        return _Settings.StateFolder / L"torrents" / PathFromUtf8(Hash.ToHex() + ".torrent");
    }

    void Session::RestoreTorrents() {
        struct Saved {
            ResumeData Resume;
            std::optional<Metainfo> Meta;
        };
        std::vector<Saved> Found;

        std::error_code Ec;
        for (const auto& Entry : std::filesystem::directory_iterator(_Settings.StateFolder / L"resume", Ec)) {
            if (Entry.path().extension() != L".dat") { continue; }
            try {
                const std::optional<std::string> Data = ReadWholeFile(Entry.path(), kMaxResumeFileSize);
                if (!Data) { continue; }
                Saved Item {DecodeResumeData(*Data), std::nullopt};
                if (Entry.path() != ResumePath(Item.Resume.Hash)) { continue; }  // A stray copy

                try {
                    if (const std::optional<std::string> File = ReadWholeFile(TorrentPath(Item.Resume.Hash), kMaxTorrentFileSize)) {
                        Metainfo Meta = Metainfo::Parse(*File);
                        if (Meta.Hash == Item.Resume.Hash) { Item.Meta = std::move(Meta); }
                    }
                } catch (const ParseError&) {
                    // Without usable metadata it comes back like a magnet link and fetches it from peers again
                }
                Found.push_back(std::move(Item));
            } catch (const ParseError&) {
                // Unreadable; skipped, but left in place rather than deleted
            }
        }

        std::ranges::sort(Found, {}, [](const Saved& Item) { return Item.Resume.Position; });
        for (Saved& Item : Found) {
            Torrent::Params Params;
            Params.Hash          = Item.Resume.Hash;
            Params.DisplayName   = Item.Resume.Name.empty() ? Item.Resume.Hash.ToHex() : Item.Resume.Name;
            Params.Destination   = Item.Resume.Destination;
            Params.ExtraTrackers = Item.Resume.ExtraTrackers;
            Params.StartPaused   = Item.Resume.Paused;
            Params.AddedOn       = Item.Resume.AddedOn;
            Params.SkippedFiles  = Item.Resume.SkippedFiles;
            const bool HasMeta   = Item.Meta.has_value();
            Params.Meta          = std::move(Item.Meta);
            Params.Resume        = std::move(Item.Resume);

            const InfoHash Hash = Params.Hash;
            Launch(std::move(Params));
            _Persisted[Hash].HasTorrentFile = HasMeta;
        }
    }

    void Session::SaveState() {
        if (_Settings.StateFolder.empty()) { return; }
        std::scoped_lock SaveLock(_SaveMutex);
        std::vector<std::pair<TorrentId, const Torrent*>> Current;
        {
            // Holding _SaveMutex keeps RemoveTorrent from taking any of these away while they're saved
            std::scoped_lock Lock(_Mutex);
            for (const auto& [Id, Instance] : _Torrents) { Current.emplace_back(Id, Instance.get()); }
        }
        SaveTorrents(Current);
    }

    void Session::SaveTorrents(const std::span<const std::pair<TorrentId, const Torrent*>> Torrents) {
        if (_Settings.StateFolder.empty()) { return; }
        for (const auto& [Id, Instance] : Torrents) {
            Persisted& Saved = _Persisted[Instance->Hash()];
            try {
                // The metadata first, so resume data never claims pieces of a torrent that can't be restored
                if (!Saved.HasTorrentFile) {
                    if (const std::shared_ptr<const Metainfo> Meta = Instance->Metadata()) {
                        WriteFileAtomically(TorrentPath(Instance->Hash()), Meta->ToTorrentFile());
                        Saved.HasTorrentFile = true;
                    }
                }
                const std::shared_ptr<const ResumeData> Resume = Instance->ResumeState();
                if (Resume && Resume != Saved.Resume) {
                    ResumeData Data = *Resume;
                    Data.Position   = static_cast<std::int64_t>(Id);
                    WriteFileAtomically(ResumePath(Instance->Hash()), EncodeResumeData(Data));
                    Saved.Resume = Resume;
                }
            } catch (const std::runtime_error&) {
                // Disk full or the like; tried again on the next pass
            }
        }
    }

    void Session::SaveDht() {
        if (_Settings.StateFolder.empty()) { return; }
        NodeId Id {};
        std::vector<Endpoint> Nodes;
        {
            std::scoped_lock Lock(_DhtMutex);
            if (!_Dht) { return; }
            Id    = _Dht->Id();
            Nodes = _Dht->KnownNodes();
        }
        if (Nodes.empty()) { return; }  // Probably offline; the last good list is more use next time

        std::string Compact;
        for (const Endpoint& Node : Nodes) {
            if (Node.IsV6 || Compact.size() / 6 >= kMaxSavedDhtNodes) { continue; }
            Compact.append(reinterpret_cast<const char*>(Node.Address.data()), 4);
            Compact.push_back(static_cast<char>(Node.Port >> 8));
            Compact.push_back(static_cast<char>(Node.Port & 0xFF));
        }
        try {
            WriteFileAtomically(DhtStatePath(_Settings.StateFolder),
                                Bencode::Encode(Bencode::Dict {
                                  {"id", std::string_view(reinterpret_cast<const char*>(Id.data()), Id.size())},
                                  {"nodes", Compact},
                                }));
        } catch (const std::runtime_error&) {}
    }

    void Session::ForgetSaved(const InfoHash& Hash) {
        _Persisted.erase(Hash);
        if (_Settings.StateFolder.empty()) { return; }
        std::error_code Ec;
        std::filesystem::remove(std::filesystem::path(LongPath(ResumePath(Hash))), Ec);
        std::filesystem::remove(std::filesystem::path(LongPath(TorrentPath(Hash))), Ec);
    }

    template<class TAction>
    void Session::WithTorrent(const TorrentId Id, TAction&& Action) {
        std::scoped_lock Lock(_Mutex);
        if (const auto It = _Torrents.find(Id); It != _Torrents.end()) { Action(*It->second); }
    }

    void Session::Pause(const TorrentId Id) {
        WithTorrent(Id, [](Torrent& Target) { Target.Pause(); });
    }

    void Session::Resume(const TorrentId Id) {
        WithTorrent(Id, [](Torrent& Target) { Target.Resume(); });
    }

    void Session::Recheck(const TorrentId Id) {
        WithTorrent(Id, [](Torrent& Target) { Target.Recheck(); });
    }

    void Session::SetSkippedFiles(const TorrentId Id, std::vector<int> Files) {
        WithTorrent(Id, [&](Torrent& Target) { Target.SetSkippedFiles(std::move(Files)); });
    }

    std::shared_ptr<const TorrentDetails> Session::Details(const TorrentId Id) {
        std::shared_ptr<const TorrentDetails> Result;
        WithTorrent(Id, [&](Torrent& Target) { Result = Target.Details(); });
        return Result;
    }

    void Session::AddPeers(const TorrentId Id, std::vector<Endpoint> Peers) {
        WithTorrent(Id, [&](Torrent& Target) { Target.AddPeers(std::move(Peers)); });
    }

    std::optional<TorrentId> Session::Find(const InfoHash& Hash) const {
        std::scoped_lock Lock(_Mutex);
        for (const auto& [Id, Instance] : _Torrents) {
            if (Instance->Hash() == Hash) { return Id; }
        }
        return std::nullopt;
    }

    std::vector<TorrentStats> Session::Snapshot() const {
        std::scoped_lock Lock(_Mutex);
        std::vector<TorrentStats> Result;
        Result.reserve(_Torrents.size());
        for (const auto& [Id, Instance] : _Torrents) { Result.push_back(Instance->Stats()); }
        return Result;
    }

    void Session::ListenLoop(const std::stop_token& Stop) {
        Listener& State = *_Listener;
        Clock::time_point NextSave    = Clock::now() + kSaveInterval;
        Clock::time_point NextDhtSave = Clock::now() + kDhtSaveInterval;  // Once the routing table has filled up

        while (!Stop.stop_requested()) {
            std::vector<WSAPOLLFD> Fds;
            Fds.push_back({State.Socket.Get(), POLLRDNORM, 0});
            for (const Listener::PendingConnection& Entry : State.Pending) { Fds.push_back({Entry.Socket.Get(), POLLRDNORM, 0}); }
            WSAPoll(Fds.data(), static_cast<ULONG>(Fds.size()), static_cast<INT>(kPollTimeout.count()));

            const Clock::time_point Now = Clock::now();

            if (Fds[0].revents & POLLRDNORM) {
                while (State.Pending.size() < kMaxPending) {
                    sockaddr_storage From {};
                    int FromLength = sizeof(From);
                    UniqueSocket Client(accept(State.Socket.Get(), reinterpret_cast<sockaddr*>(&From), &FromLength));
                    if (!Client) { break; }
                    const std::optional<Endpoint> Remote = FromSockaddr(reinterpret_cast<const sockaddr*>(&From));
                    if (!Remote) { continue; }
                    SetNonBlocking(Client.Get());
                    State.Pending.push_back({.Socket = std::move(Client), .Remote = Remote->Unmapped(), .Accepted = Now});
                }
            }

            // Read exactly the 68-byte handshake, so nothing that follows it is consumed here
            for (std::size_t I = 1; I < Fds.size(); ++I) {
                if (!(Fds[I].revents & (POLLRDNORM | POLLHUP | POLLERR))) { continue; }
                Listener::PendingConnection& Entry = State.Pending[I - 1];
                const int Received       = recv(Entry.Socket.Get(),
                                          reinterpret_cast<char*>(Entry.Buffer.data() + Entry.Received),
                                          static_cast<int>(kHandshakeSize - Entry.Received),
                                          0);
                if (Received <= 0) {
                    if (Received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) { Entry.Socket.Reset(); }
                    continue;
                }
                Entry.Received += static_cast<std::size_t>(Received);
                if (Entry.Received < kHandshakeSize) { continue; }

                if (const std::optional<Handshake> Parsed = DecodeHandshake(Entry.Buffer)) {
                    std::scoped_lock Lock(_Mutex);
                    for (const auto& [Id, Instance] : _Torrents) {
                        if (Instance->Hash() == Parsed->Hash) {
                            Instance->AddIncoming(std::move(Entry.Socket), Entry.Remote, *Parsed);
                            break;
                        }
                    }
                }
                Entry.Socket.Reset();  // Handed over, or unknown torrent
            }

            std::erase_if(State.Pending, [&](const Listener::PendingConnection& Entry) {
                return !Entry.Socket || Now - Entry.Accepted > kHandshakeTimeout;
            });
            ReapRetired();

            if (Now >= NextSave) {
                SaveState();
                NextSave = Clock::now() + kSaveInterval;
            }
            if (Now >= NextDhtSave) {
                SaveDht();
                NextDhtSave = Now + kDhtSaveInterval;
            }
        }
    }

    void Session::ReapRetired() {
        std::vector<Retiring> Finished;
        {
            std::scoped_lock Lock(_Mutex);
            for (auto It = _Retiring.begin(); It != _Retiring.end();) {
                if (It->Instance->IsStopped()) {
                    Finished.push_back(std::move(*It));
                    It = _Retiring.erase(It);
                } else {
                    ++It;
                }
            }
        }
        // Destroying a stopped torrent just joins its finished thread; delete files only after that
        for (Retiring& Entry : Finished) {
            const std::filesystem::path Destination           = Entry.Instance->Settings().Destination;
            const std::shared_ptr<const Metainfo> Metadata    = Entry.Instance->Metadata();
            Entry.Instance.reset();
            // A magnet removed before its metadata arrived has no files to delete
            if (Entry.DeleteFiles && Metadata) { DeleteTorrentFiles(Destination, Metadata->Files); }
        }
    }

    void DeleteTorrentFiles(const std::filesystem::path& Destination, const std::vector<TorrentFile>& Files) {
        std::set<std::filesystem::path> Folders;
        for (const TorrentFile& File : Files) {
            std::error_code Ec;
            if (!File.IsPadding) { std::filesystem::remove(std::filesystem::path(LongPath(Destination / File.Path)), Ec); }
            for (std::filesystem::path Folder = File.Path.parent_path(); !Folder.empty(); Folder = Folder.parent_path()) {
                Folders.insert(Folder);
            }
        }

        // Deepest first; remove() only succeeds on folders that are now empty, so the user's own files survive
        std::vector<std::filesystem::path> Ordered(Folders.begin(), Folders.end());
        std::ranges::sort(Ordered, std::greater {}, [](const std::filesystem::path& Folder) {
            return std::distance(Folder.begin(), Folder.end());
        });
        for (const std::filesystem::path& Folder : Ordered) {
            std::error_code Ec;
            std::filesystem::remove(std::filesystem::path(LongPath(Destination / Folder)), Ec);
        }
    }
}  // namespace Tori
