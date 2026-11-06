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

#include "Tracker/UdpTracker.hpp"

#include "Core/ByteIO.hpp"
#include "Core/Encoding.hpp"
#include "Core/Random.hpp"
#include "Net/Socket.hpp"

#include <charconv>
#include <format>
#include <map>
#include <memory>
#include <mutex>

namespace Tori {
    namespace {
        using Clock = std::chrono::steady_clock;

        enum : std::uint32_t {
            kActionConnect  = 0,
            kActionAnnounce = 1,
            kActionError    = 3,
        };

        constexpr auto kConnectionIdLifetime = std::chrono::seconds(60);
        constexpr auto kPollSlice            = std::chrono::milliseconds(250);  // how often a wait checks Stop
        constexpr std::size_t kMaxPacketSize = 64 * 1024;

        std::uint32_t EventCode(const AnnounceEvent Event) {
            switch (Event) {
                case AnnounceEvent::Completed:
                    return 1;
                case AnnounceEvent::Started:
                    return 2;
                case AnnounceEvent::Stopped:
                    return 3;
                case AnnounceEvent::None:
                    break;
            }
            return 0;
        }

        std::string_view AsChars(const std::span<const std::uint8_t> Bytes) {
            return {reinterpret_cast<const char*>(Bytes.data()), Bytes.size()};
        }

        // Connection IDs are valid for a minute and can be shared by every torrent announcing to the same tracker
        class ConnectionCache {
        public:
            std::optional<std::uint64_t> Find(const std::string& Key) {
                std::scoped_lock Lock(_Mutex);
                const auto It = _Entries.find(Key);
                if (It == _Entries.end() || Clock::now() >= It->second.Expires) { return std::nullopt; }
                return It->second.Id;
            }

            void Store(const std::string& Key, const std::uint64_t Id) {
                std::scoped_lock Lock(_Mutex);
                _Entries[Key] = {Id, Clock::now() + kConnectionIdLifetime};
            }

            void Forget(const std::string& Key) {
                std::scoped_lock Lock(_Mutex);
                _Entries.erase(Key);
            }

        private:
            struct Entry {
                std::uint64_t Id = 0;
                Clock::time_point Expires;
            };

            std::mutex _Mutex;
            std::map<std::string, Entry> _Entries;
        };

        ConnectionCache& Connections() {
            static ConnectionCache Cache;
            return Cache;
        }

        /// Sends Packet and waits for a reply Parse accepts, resending with doubling timeouts. Returns nullopt when
        /// every attempt times out or the address is unreachable.
        template<class TParse>
        auto Exchange(const SOCKET Socket,
                      const std::vector<std::uint8_t>& Packet,
                      TParse&& Parse,
                      const std::stop_token& Stop,
                      const UdpTrackerTiming& Timing) -> decltype(Parse(std::span<const std::uint8_t> {})) {
            std::vector<std::uint8_t> Buffer(kMaxPacketSize);

            for (int Attempt = 0; Attempt < Timing.MaxAttempts; ++Attempt) {
                if (send(Socket, reinterpret_cast<const char*>(Packet.data()), static_cast<int>(Packet.size()), 0) ==
                    SOCKET_ERROR) {
                    return {};
                }

                const Clock::time_point Deadline = Clock::now() + Timing.BaseTimeout * (1 << Attempt);
                while (true) {
                    if (Stop.stop_requested()) { throw TrackerError("cancelled"); }
                    const Clock::time_point Now = Clock::now();
                    if (Now >= Deadline) { break; }

                    const auto Wait = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Now),
                                               std::chrono::milliseconds(kPollSlice));
                    WSAPOLLFD Poll {Socket, POLLRDNORM, 0};
                    if (WSAPoll(&Poll, 1, std::max(1, static_cast<int>(Wait.count()))) <= 0) { continue; }

                    const int Received =
                      recv(Socket, reinterpret_cast<char*>(Buffer.data()), static_cast<int>(Buffer.size()), 0);
                    if (Received == SOCKET_ERROR) {
                        // WSAECONNRESET here means an ICMP port unreachable came back: nothing is listening
                        return {};
                    }
                    if (auto Result =
                          Parse(std::span<const std::uint8_t>(Buffer.data(), static_cast<std::size_t>(Received)))) {
                        return Result;
                    }
                }
            }
            return {};
        }

        std::optional<AnnounceResponse> AnnounceToAddress(const addrinfo& Address,
                                                          const std::string& CacheKey,
                                                          const AnnounceRequest& Request,
                                                          const std::stop_token& Stop,
                                                          const UdpTrackerTiming& Timing) {
            const UniqueSocket Socket(socket(Address.ai_family, SOCK_DGRAM, IPPROTO_UDP));
            if (!Socket) { return std::nullopt; }  // e.g. no IPv6 on this machine
            // Connecting a UDP socket filters out datagrams from anyone but the tracker
            if (connect(Socket.Get(), Address.ai_addr, static_cast<int>(Address.ai_addrlen)) != 0) {
                return std::nullopt;
            }
            const bool IsV6 = Address.ai_family == AF_INET6;

            // A cached ID may have expired on the tracker's side; if so, reconnect once
            for (int Round = 0; Round < 2; ++Round) {
                std::optional<std::uint64_t> ConnectionId = Connections().Find(CacheKey);
                const bool FromCache                      = ConnectionId.has_value();

                if (!ConnectionId) {
                    const std::uint32_t Transaction = RandomU32();
                    ConnectionId                    = Exchange(
                      Socket.Get(),
                      UdpProtocol::BuildConnect(Transaction),
                      [&](const auto Packet) { return UdpProtocol::ParseConnectResponse(Packet, Transaction); },
                      Stop,
                      Timing);
                    if (!ConnectionId) { return std::nullopt; }
                    Connections().Store(CacheKey, *ConnectionId);
                }

                const std::uint32_t Transaction = RandomU32();
                try {
                    std::optional<AnnounceResponse> Response = Exchange(
                      Socket.Get(),
                      UdpProtocol::BuildAnnounce(*ConnectionId, Transaction, Request),
                      [&](const auto Packet) { return UdpProtocol::ParseAnnounceResponse(Packet, Transaction, IsV6); },
                      Stop,
                      Timing);
                    if (Response || !FromCache) { return Response; }
                } catch (const TrackerError&) {
                    if (!FromCache || Stop.stop_requested()) { throw; }
                }
                Connections().Forget(CacheKey);
            }
            return std::nullopt;
        }
    }  // namespace

    std::optional<UdpTrackerAddress> ParseUdpTrackerUrl(const std::string_view Url) {
        constexpr std::string_view kScheme = "udp://";
        if (!StartsWithIgnoreCase(Url, kScheme)) { return std::nullopt; }

        std::string_view Authority = Url.substr(kScheme.size());
        Authority                  = Authority.substr(0, Authority.find_first_of("/?"));

        std::string_view Host;
        std::string_view PortText;
        if (Authority.starts_with('[')) {
            const std::size_t Close = Authority.find(']');
            if (Close == std::string_view::npos || Close + 1 >= Authority.size() || Authority[Close + 1] != ':') {
                return std::nullopt;
            }
            Host     = Authority.substr(1, Close - 1);
            PortText = Authority.substr(Close + 2);
        } else {
            const std::size_t Colon = Authority.rfind(':');
            if (Colon == std::string_view::npos) { return std::nullopt; }
            Host     = Authority.substr(0, Colon);
            PortText = Authority.substr(Colon + 1);
        }

        std::uint32_t Port   = 0;
        const auto [Ptr, Ec] = std::from_chars(PortText.data(), PortText.data() + PortText.size(), Port);
        if (Host.empty() || Ec != std::errc {} || Ptr != PortText.data() + PortText.size() || Port == 0 ||
            Port > 65535) {
            return std::nullopt;
        }
        return UdpTrackerAddress {std::string(Host), static_cast<std::uint16_t>(Port)};
    }

    AnnounceResponse UdpAnnounce(const std::string& Url,
                                 const AnnounceRequest& Request,
                                 const std::stop_token Stop,
                                 const UdpTrackerTiming& Timing) {
        const std::optional<UdpTrackerAddress> Address = ParseUdpTrackerUrl(Url);
        if (!Address) { throw TrackerError("invalid UDP tracker URL"); }

        EnsureWinsock();
        addrinfo Hints {};
        Hints.ai_family   = AF_UNSPEC;
        Hints.ai_socktype = SOCK_DGRAM;
        Hints.ai_protocol = IPPROTO_UDP;
        addrinfo* Results = nullptr;
        if (getaddrinfo(Address->Host.c_str(), std::to_string(Address->Port).c_str(), &Hints, &Results) != 0) {
            throw TrackerError("could not resolve host");
        }
        const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> Guard(Results, freeaddrinfo);

        for (const addrinfo* Info = Results; Info; Info = Info->ai_next) {
            if (Stop.stop_requested()) { throw TrackerError("cancelled"); }
            const auto Resolved = FromSockaddr(Info->ai_addr);
            if (!Resolved) { continue; }
            if (auto Response = AnnounceToAddress(*Info, Resolved->ToString(), Request, Stop, Timing)) {
                return *Response;
            }
        }
        throw TrackerError("tracker did not respond");
    }

    namespace UdpProtocol {
        std::vector<std::uint8_t> BuildConnect(const std::uint32_t TransactionId) {
            ByteWriter Writer;
            Writer.WriteU64(kProtocolId);
            Writer.WriteU32(kActionConnect);
            Writer.WriteU32(TransactionId);
            return Writer.Take();
        }

        std::optional<std::uint64_t> ParseConnectResponse(const std::span<const std::uint8_t> Packet,
                                                          const std::uint32_t TransactionId) {
            if (Packet.size() < 8) { return std::nullopt; }
            ByteReader Reader(Packet);
            const std::uint32_t Action = Reader.ReadU32();
            if (Reader.ReadU32() != TransactionId) { return std::nullopt; }
            if (Action == kActionError) { throw TrackerError(std::string(AsChars(Reader.ReadRest()))); }
            if (Action != kActionConnect || Reader.Remaining() < 8) { return std::nullopt; }
            return Reader.ReadU64();
        }

        std::vector<std::uint8_t> BuildAnnounce(const std::uint64_t ConnectionId,
                                                const std::uint32_t TransactionId,
                                                const AnnounceRequest& Request) {
            ByteWriter Writer;
            Writer.WriteU64(ConnectionId);
            Writer.WriteU32(kActionAnnounce);
            Writer.WriteU32(TransactionId);
            Writer.WriteBytes(Request.Hash.Bytes);
            Writer.WriteBytes(Request.Id);
            Writer.WriteU64(static_cast<std::uint64_t>(Request.Downloaded));
            Writer.WriteU64(static_cast<std::uint64_t>(Request.Left));
            Writer.WriteU64(static_cast<std::uint64_t>(Request.Uploaded));
            Writer.WriteU32(EventCode(Request.Event));
            Writer.WriteU32(0);  // IP: let the tracker use the packet's source address
            Writer.WriteU32(Request.Key);
            Writer.WriteU32(static_cast<std::uint32_t>(Request.NumWant));
            Writer.WriteU16(Request.Port);
            return Writer.Take();
        }

        std::optional<AnnounceResponse> ParseAnnounceResponse(const std::span<const std::uint8_t> Packet,
                                                              const std::uint32_t TransactionId,
                                                              const bool IsV6) {
            if (Packet.size() < 8) { return std::nullopt; }
            ByteReader Reader(Packet);
            const std::uint32_t Action = Reader.ReadU32();
            if (Reader.ReadU32() != TransactionId) { return std::nullopt; }
            if (Action == kActionError) { throw TrackerError(std::string(AsChars(Reader.ReadRest()))); }
            if (Action != kActionAnnounce || Reader.Remaining() < 12) { return std::nullopt; }

            AnnounceResponse Response;
            Response.IntervalSeconds = ClampAnnounceInterval(Reader.ReadU32());
            Response.Leechers        = static_cast<int>(std::min<std::uint32_t>(Reader.ReadU32(), INT32_MAX));
            Response.Seeders         = static_cast<int>(std::min<std::uint32_t>(Reader.ReadU32(), INT32_MAX));
            Response.Peers           = ParseCompactPeers(AsChars(Reader.ReadRest()), IsV6);
            return Response;
        }
    }  // namespace UdpProtocol
}  // namespace Tori
