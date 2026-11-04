#include "Core/Bencode.hpp"
#include "Core/ByteIO.hpp"
#include "Core/PeerId.hpp"
#include "Net/Endpoint.hpp"
#include "Net/Socket.hpp"
#include "Tracker/Announcer.hpp"
#include "Tracker/HttpTracker.hpp"
#include "Tracker/UdpTracker.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <mutex>
#include <set>
#include <thread>

using namespace Tori;
using namespace std::chrono_literals;
namespace B = Tori::Bencode;

namespace {
    std::string Bytes(const char* Data, const std::size_t Size) {
        return {Data, Size};
    }

    AnnounceRequest SampleRequest() {
        AnnounceRequest Request;
        for (std::size_t I = 0; I < Request.Hash.Bytes.size(); ++I) { Request.Hash.Bytes[I] = static_cast<std::uint8_t>(I); }
        Request.Id         = GeneratePeerId();
        Request.Port       = 6881;
        Request.Uploaded   = 10;
        Request.Downloaded = 20;
        Request.Left       = 30;
        Request.Event      = AnnounceEvent::Started;
        Request.NumWant    = 33;
        Request.Key        = 0xDEADBEEF;
        return Request;
    }

    std::uint16_t LocalPort(const SOCKET Socket) {
        sockaddr_storage Address {};
        int Length = sizeof(Address);
        getsockname(Socket, reinterpret_cast<sockaddr*>(&Address), &Length);
        return FromSockaddr(reinterpret_cast<const sockaddr*>(&Address))->Port;
    }

    UniqueSocket BindLoopback(const int Type, const int Protocol) {
        EnsureWinsock();
        UniqueSocket Socket(socket(AF_INET, Type, Protocol));
        sockaddr_in Address {};
        Address.sin_family      = AF_INET;
        Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(Socket.Get(), reinterpret_cast<const sockaddr*>(&Address), sizeof(Address));
        return Socket;
    }

    /// Answers BEP 15 connect and announce requests on 127.0.0.1.
    class FakeUdpTracker {
    public:
        static constexpr std::uint64_t kConnectionId = 0x1122334455667788;

        explicit FakeUdpTracker(const int DropConnects = 0, const bool SendError = false)
            : _Socket(BindLoopback(SOCK_DGRAM, IPPROTO_UDP)), _DropConnects(DropConnects), _SendError(SendError) {
            Port    = LocalPort(_Socket.Get());
            _Thread = std::jthread([this](const std::stop_token Stop) { Serve(Stop); });
        }

        std::string Url() const { return std::format("udp://127.0.0.1:{}/announce", Port); }

        std::uint16_t Port = 0;
        std::atomic<int> ConnectsSeen {0};
        std::atomic<std::uint32_t> LastEvent {99};
        std::atomic<std::uint32_t> LastNumWant {0};

    private:
        void Serve(const std::stop_token& Stop) {
            std::vector<std::uint8_t> Buffer(2048);
            while (!Stop.stop_requested()) {
                WSAPOLLFD Poll {_Socket.Get(), POLLRDNORM, 0};
                if (WSAPoll(&Poll, 1, 20) <= 0) { continue; }

                sockaddr_storage From {};
                int FromLength     = sizeof(From);
                const int Received = recvfrom(_Socket.Get(),
                                              reinterpret_cast<char*>(Buffer.data()),
                                              static_cast<int>(Buffer.size()),
                                              0,
                                              reinterpret_cast<sockaddr*>(&From),
                                              &FromLength);
                if (Received <= 0) { continue; }

                ByteReader Reader(std::span(Buffer.data(), static_cast<std::size_t>(Received)));
                ByteWriter Reply;
                if (Received == 16) {
                    Reader.ReadU64();
                    Reader.ReadU32();
                    const std::uint32_t Transaction = Reader.ReadU32();
                    ++ConnectsSeen;
                    if (_DropConnects > 0) {
                        --_DropConnects;
                        continue;
                    }
                    Reply.WriteU32(0);
                    Reply.WriteU32(Transaction);
                    Reply.WriteU64(kConnectionId);
                } else if (Received == 98) {
                    CHECK(Reader.ReadU64() == kConnectionId);
                    Reader.ReadU32();
                    const std::uint32_t Transaction = Reader.ReadU32();
                    Reader.ReadBytes(20 + 20 + 8 + 8 + 8);
                    LastEvent = Reader.ReadU32();
                    Reader.ReadU32();
                    Reader.ReadU32();
                    LastNumWant = Reader.ReadU32();

                    if (_SendError) {
                        Reply.WriteU32(3);
                        Reply.WriteU32(Transaction);
                        Reply.WriteBytes(std::string_view("go away"));
                    } else {
                        Reply.WriteU32(1);
                        Reply.WriteU32(Transaction);
                        Reply.WriteU32(900);
                        Reply.WriteU32(3);
                        Reply.WriteU32(7);
                        Reply.WriteBytes(Bytes("\x0a\x00\x00\x01\x1a\xe1\x0a\x00\x00\x02\xc8\xd5", 12));
                    }
                } else {
                    continue;
                }
                sendto(_Socket.Get(),
                       reinterpret_cast<const char*>(Reply.Data().data()),
                       static_cast<int>(Reply.Data().size()),
                       0,
                       reinterpret_cast<const sockaddr*>(&From),
                       FromLength);
            }
        }

        UniqueSocket _Socket;
        int _DropConnects = 0;
        bool _SendError   = false;
        std::jthread _Thread;
    };

    /// Serves one canned HTTP response to every connection on 127.0.0.1.
    class FakeHttpServer {
    public:
        FakeHttpServer(const int Status, const std::string& Body) : _Socket(BindLoopback(SOCK_STREAM, IPPROTO_TCP)) {
            _Response = std::format(
              "HTTP/1.1 {} X\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", Status, Body.size(), Body);
            listen(_Socket.Get(), 4);
            Port    = LocalPort(_Socket.Get());
            _Thread = std::jthread([this](const std::stop_token Stop) { Serve(Stop); });
        }

        std::string Url() const { return std::format("http://127.0.0.1:{}/announce", Port); }

        std::string RequestLine() {
            std::scoped_lock Lock(_Mutex);
            return _RequestLine;
        }

        std::uint16_t Port = 0;

    private:
        void Serve(const std::stop_token& Stop) {
            while (!Stop.stop_requested()) {
                WSAPOLLFD Poll {_Socket.Get(), POLLRDNORM, 0};
                if (WSAPoll(&Poll, 1, 20) <= 0) { continue; }
                const UniqueSocket Client(accept(_Socket.Get(), nullptr, nullptr));
                if (!Client) { continue; }

                std::string Request;
                char Chunk[4096];
                while (Request.find("\r\n\r\n") == std::string::npos) {
                    const int Received = recv(Client.Get(), Chunk, sizeof(Chunk), 0);
                    if (Received <= 0) { break; }
                    Request.append(Chunk, static_cast<std::size_t>(Received));
                }
                {
                    std::scoped_lock Lock(_Mutex);
                    _RequestLine = Request.substr(0, Request.find("\r\n"));
                }
                send(Client.Get(), _Response.data(), static_cast<int>(_Response.size()), 0);
                shutdown(Client.Get(), SD_SEND);
            }
        }

        UniqueSocket _Socket;
        std::string _Response;
        std::mutex _Mutex;
        std::string _RequestLine;
        std::jthread _Thread;
    };

    /// Stands in for real trackers in Announcer tests.
    class FakeTrackers {
    public:
        struct Call {
            std::string Url;
            AnnounceEvent Event;
        };

        AnnounceFunction Function() {
            return [this](const std::string& Url, const AnnounceRequest& Request, const std::stop_token Stop) {
                {
                    std::scoped_lock Lock(_Mutex);
                    _Calls.push_back({Url, Request.Event});
                    if (_Failing.contains(Url)) { throw TrackerError("down"); }
                }
                if (Url.find("slow") != std::string::npos) {
                    while (!Stop.stop_requested()) { std::this_thread::sleep_for(1ms); }
                    throw TrackerError("cancelled");
                }
                AnnounceResponse Response;
                Response.IntervalSeconds = 1800;
                Response.Seeders         = 5;
                Response.Peers           = {*Endpoint::Parse("10.0.0.1:1")};
                return Response;
            };
        }

        void Fail(const std::string& Url) {
            std::scoped_lock Lock(_Mutex);
            _Failing.insert(Url);
        }

        std::vector<Call> Calls() {
            std::scoped_lock Lock(_Mutex);
            return _Calls;
        }

    private:
        std::mutex _Mutex;
        std::vector<Call> _Calls;
        std::set<std::string> _Failing;
    };

    /// Ticks until no announce is in flight, so everything due at Now has been attempted.
    void Settle(Announcer& Target, const Announcer::Clock::time_point Now) {
        for (int I = 0; I < 5000; ++I) {
            Target.Tick(Now, {});
            const auto Rows = Target.Status();
            if (std::ranges::none_of(Rows, [](const TrackerStatus& Row) { return Row.State == TrackerState::Announcing; })) {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        FAIL("announcer did not settle");
    }

    void SettleStop(Announcer& Target, const Announcer::Clock::time_point Now) {
        for (int I = 0; I < 5000 && !Target.IsFinished(); ++I) {
            Target.Tick(Now, {});
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(Target.IsFinished());
    }

    Announcer::Config TestConfig() {
        Announcer::Config Config;
        Config.ShuffleTiers = false;
        return Config;
    }
}  // namespace

TEST_CASE("PeerId has the Tori prefix and printable random part") {
    const PeerId Id = GeneratePeerId();
    CHECK(std::string_view(reinterpret_cast<const char*>(Id.data()), 8) == "-TO0100-");
    CHECK(std::ranges::all_of(Id, [](const std::uint8_t C) { return std::isalnum(C) || C == '-'; }));
    CHECK(GeneratePeerId() != Id);
}

TEST_CASE("DescribePeerId names Azureus-style clients") {
    const auto Id = [](const std::string_view Prefix) {
        PeerId Result {};
        Result.fill('x');
        std::ranges::copy(Prefix, Result.begin());
        return Result;
    };
    CHECK(DescribePeerId(Id("-qB4650-")) == "qBittorrent 4.6.5");
    CHECK(DescribePeerId(Id("-TR4060-")) == "Transmission 4.0.6");
    CHECK(DescribePeerId(Id("-LT2A00-")) == "libtorrent 2.10");  // Letters count on from 10
    CHECK(DescribePeerId(Id("-ZZ1000-")) == "ZZ 1");
    CHECK(DescribePeerId(Id("M7-4-3--")).empty());  // Mainline style isn't decoded
    CHECK(DescribePeerId(GeneratePeerId()) == "Tori 0.1");
}

TEST_CASE("Endpoint parses and formats addresses") {
    const auto V4 = Endpoint::Parse("1.2.3.4:6881");
    REQUIRE(V4.has_value());
    CHECK(V4->ToString() == "1.2.3.4:6881");
    CHECK_FALSE(V4->IsV6);

    const auto V6 = Endpoint::Parse("[2001:db8::1]:51413");
    REQUIRE(V6.has_value());
    CHECK(V6->IsV6);
    CHECK(V6->ToString() == "[2001:db8::1]:51413");

    CHECK_FALSE(Endpoint::Parse("1.2.3.4").has_value());
    CHECK_FALSE(Endpoint::Parse("1.2.3.4:0").has_value());
    CHECK_FALSE(Endpoint::Parse("1.2.3.4:70000").has_value());
    CHECK_FALSE(Endpoint::Parse("example.com:80").has_value());
    CHECK_FALSE(Endpoint::Parse("::1:80").has_value());
}

TEST_CASE("ParseCompactPeers reads IPv4 and IPv6 entries") {
    const auto V4 = ParseCompactPeers(Bytes("\x0a\x00\x00\x01\x1a\xe1\x01\x02\x03\x04\x00\x00\xff", 13), false);
    REQUIRE(V4.size() == 1);  // port 0 and the trailing byte are dropped
    CHECK(V4[0].ToString() == "10.0.0.1:6881");

    std::string V6Data(16, '\0');
    V6Data[15] = 1;
    V6Data += Bytes("\x1a\xe1", 2);
    const auto V6 = ParseCompactPeers(V6Data, true);
    REQUIRE(V6.size() == 1);
    CHECK(V6[0].ToString() == "[::1]:6881");
}

TEST_CASE("BuildAnnounceUrl adds escaped BEP 3 parameters") {
    AnnounceRequest Request = SampleRequest();
    Request.TrackerId       = "abc def";

    const std::string Url = BuildAnnounceUrl("http://t.example/announce", Request);
    CHECK(Url.starts_with("http://t.example/announce?info_hash=%00%01%02%03"));
    CHECK(Url.find("&peer_id=-TO0100-") != std::string::npos);
    CHECK(Url.find("&port=6881&uploaded=10&downloaded=20&left=30&compact=1&numwant=33&key=deadbeef") !=
          std::string::npos);
    CHECK(Url.find("&event=started") != std::string::npos);
    CHECK(Url.ends_with("&trackerid=abc%20def"));

    Request.Event = AnnounceEvent::None;
    const std::string WithQuery = BuildAnnounceUrl("http://t.example/a?passkey=x", Request);
    CHECK(WithQuery.starts_with("http://t.example/a?passkey=x&info_hash="));
    CHECK(WithQuery.find("event=") == std::string::npos);
}

TEST_CASE("ParseHttpAnnounceResponse handles all peer formats") {
    const std::string Compact = B::Encode(B::Dict {
      {"interval", 900},
      {"min interval", 5},
      {"complete", 7},
      {"incomplete", 3},
      {"tracker id", "tid"},
      {"warning message", "careful"},
      {"peers", Bytes("\x0a\x00\x00\x01\x1a\xe1", 6)},
      {"peers6", std::string(15, '\0') + Bytes("\x01\x1a\xe1", 3)},
    });
    const AnnounceResponse Response = ParseHttpAnnounceResponse(Compact + "\n");
    CHECK(Response.IntervalSeconds == 900);
    CHECK(Response.MinIntervalSeconds == 30);  // clamped
    CHECK(Response.Seeders == 7);
    CHECK(Response.Leechers == 3);
    CHECK(Response.TrackerId == "tid");
    CHECK(Response.WarningMessage == "careful");
    REQUIRE(Response.Peers.size() == 2);
    CHECK(Response.Peers[0].ToString() == "10.0.0.1:6881");
    CHECK(Response.Peers[1].ToString() == "[::1]:6881");

    const std::string Dictionary = B::Encode(B::Dict {
      {"interval", 1800},
      {"peers",
       B::List {
         B::Dict {{"ip", "1.2.3.4"}, {"port", 80}, {"peer id", "x"}},
         B::Dict {{"ip", "host.example"}, {"port", 80}},
         B::Dict {{"ip", "5.6.7.8"}, {"port", 0}},
       }},
    });
    const AnnounceResponse FromList = ParseHttpAnnounceResponse(Dictionary);
    REQUIRE(FromList.Peers.size() == 1);
    CHECK(FromList.Peers[0].ToString() == "1.2.3.4:80");

    CHECK_THROWS_WITH_AS(
      ParseHttpAnnounceResponse(B::Encode(B::Dict {{"failure reason", "unregistered torrent"}})),
      "unregistered torrent",
      TrackerError);
    CHECK_THROWS_AS(ParseHttpAnnounceResponse("<html>502</html>"), TrackerError);
    CHECK_THROWS_AS(ParseHttpAnnounceResponse("le"), TrackerError);
}

TEST_CASE("ParseUdpTrackerUrl") {
    const auto Plain = ParseUdpTrackerUrl("udp://tracker.example:1337/announce");
    REQUIRE(Plain.has_value());
    CHECK(Plain->Host == "tracker.example");
    CHECK(Plain->Port == 1337);

    const auto V6 = ParseUdpTrackerUrl("UDP://[2001:db8::1]:80");
    REQUIRE(V6.has_value());
    CHECK(V6->Host == "2001:db8::1");

    CHECK_FALSE(ParseUdpTrackerUrl("udp://tracker.example/announce").has_value());
    CHECK_FALSE(ParseUdpTrackerUrl("udp://:80").has_value());
    CHECK_FALSE(ParseUdpTrackerUrl("http://tracker.example:80").has_value());
}

TEST_CASE("UDP tracker packets match BEP 15") {
    const auto Connect = UdpProtocol::BuildConnect(0x01020304);
    CHECK(Connect == std::vector<std::uint8_t> {0, 0, 4, 0x17, 0x27, 0x10, 0x19, 0x80, 0, 0, 0, 0, 1, 2, 3, 4});

    ByteWriter Reply;
    Reply.WriteU32(0);
    Reply.WriteU32(0x01020304);
    Reply.WriteU64(42);
    CHECK(UdpProtocol::ParseConnectResponse(Reply.Data(), 0x01020304) == 42u);
    CHECK_FALSE(UdpProtocol::ParseConnectResponse(Reply.Data(), 0x99).has_value());

    const auto Announce = UdpProtocol::BuildAnnounce(42, 7, SampleRequest());
    CHECK(Announce.size() == 98);

    ByteWriter Error;
    Error.WriteU32(3);
    Error.WriteU32(7);
    Error.WriteBytes(std::string_view("banned"));
    CHECK_THROWS_WITH_AS(UdpProtocol::ParseAnnounceResponse(Error.Data(), 7, false), "banned", TrackerError);
}

TEST_CASE("UdpAnnounce talks to a tracker over loopback") {
    FakeUdpTracker Tracker;
    const AnnounceResponse Response = UdpAnnounce(Tracker.Url(), SampleRequest(), {});
    CHECK(Response.IntervalSeconds == 900);
    CHECK(Response.Seeders == 7);
    CHECK(Response.Leechers == 3);
    REQUIRE(Response.Peers.size() == 2);
    CHECK(Response.Peers[1].ToString() == "10.0.0.2:51413");
    CHECK(Tracker.LastEvent == 2);  // started
    CHECK(Tracker.LastNumWant == 33);

    // The connection ID is reused within its lifetime
    UdpAnnounce(Tracker.Url(), SampleRequest(), {});
    CHECK(Tracker.ConnectsSeen == 1);
}

TEST_CASE("UdpAnnounce retries lost packets and reports errors") {
    const UdpTrackerTiming Fast {.BaseTimeout = 100ms, .MaxAttempts = 3};

    FakeUdpTracker Lossy(1);
    CHECK(UdpAnnounce(Lossy.Url(), SampleRequest(), {}, Fast).Peers.size() == 2);
    CHECK(Lossy.ConnectsSeen == 2);

    FakeUdpTracker Refusing(0, true);
    CHECK_THROWS_WITH_AS(UdpAnnounce(Refusing.Url(), SampleRequest(), {}, Fast), "go away", TrackerError);

    std::uint16_t ClosedPort = 0;
    {
        const UniqueSocket Probe = BindLoopback(SOCK_DGRAM, IPPROTO_UDP);
        ClosedPort               = LocalPort(Probe.Get());
    }
    CHECK_THROWS_AS(UdpAnnounce(std::format("udp://127.0.0.1:{}", ClosedPort), SampleRequest(), {}, Fast), TrackerError);

    std::stop_source Stop;
    Stop.request_stop();
    CHECK_THROWS_WITH_AS(UdpAnnounce(Lossy.Url(), SampleRequest(), Stop.get_token(), Fast), "cancelled", TrackerError);
}

TEST_CASE("HttpAnnounce talks to a tracker over loopback") {
    FakeHttpServer Server(200,
                          B::Encode(B::Dict {{"interval", 900}, {"peers", Bytes("\x0a\x00\x00\x01\x1a\xe1", 6)}}));
    const AnnounceResponse Response = HttpAnnounce(Server.Url(), SampleRequest(), {});
    CHECK(Response.IntervalSeconds == 900);
    REQUIRE(Response.Peers.size() == 1);
    CHECK(Response.Peers[0].ToString() == "10.0.0.1:6881");

    const std::string RequestLine = Server.RequestLine();
    CHECK(RequestLine.starts_with("GET /announce?info_hash=%00%01"));
    CHECK(RequestLine.find("&event=started") != std::string::npos);
}

TEST_CASE("HttpAnnounce surfaces tracker and HTTP errors") {
    FakeHttpServer Refusing(200, B::Encode(B::Dict {{"failure reason", "not registered"}}));
    CHECK_THROWS_WITH_AS(HttpAnnounce(Refusing.Url(), SampleRequest(), {}), "not registered", TrackerError);

    FakeHttpServer Missing(404, "nope");
    CHECK_THROWS_WITH_AS(HttpAnnounce(Missing.Url(), SampleRequest(), {}), "HTTP 404", TrackerError);
}

TEST_CASE("Announcer sends started, then announces on the tracker's interval") {
    FakeTrackers Fake;
    Announcer Target(TestConfig(), {{"http://a"}}, Fake.Function());
    const auto T0 = Announcer::Clock::now();

    Settle(Target, T0);
    REQUIRE(Fake.Calls().size() == 1);
    CHECK(Fake.Calls()[0].Event == AnnounceEvent::Started);
    CHECK(Target.TakePeers().size() == 1);
    CHECK(Target.TakePeers().empty());

    const auto Rows = Target.Status();
    REQUIRE(Rows.size() == 1);
    CHECK(Rows[0].State == TrackerState::Working);
    CHECK(Rows[0].Seeders == 5);
    CHECK(Rows[0].NextAnnounce == T0 + 1800s);

    Settle(Target, T0 + 1799s);
    CHECK(Fake.Calls().size() == 1);
    Settle(Target, T0 + 1800s);
    REQUIRE(Fake.Calls().size() == 2);
    CHECK(Fake.Calls()[1].Event == AnnounceEvent::None);

    Target.Completed(T0 + 1801s);
    Settle(Target, T0 + 1801s);
    REQUIRE(Fake.Calls().size() == 3);
    CHECK(Fake.Calls()[2].Event == AnnounceEvent::Completed);
}

TEST_CASE("Announcer fails over within a tier and promotes the tracker that answers") {
    FakeTrackers Fake;
    Fake.Fail("http://a");
    Announcer Target(TestConfig(), {{"http://a", "http://b"}}, Fake.Function());
    Settle(Target, Announcer::Clock::now());

    const auto Calls = Fake.Calls();
    REQUIRE(Calls.size() == 2);
    CHECK(Calls[0].Url == "http://a");
    CHECK(Calls[1].Url == "http://b");

    const auto Rows = Target.Status();
    CHECK(Rows[0].Url == "http://b");
    CHECK(Rows[0].State == TrackerState::Working);
    CHECK(Rows[1].State == TrackerState::Failed);
    CHECK(Rows[1].Message == "down");
}

TEST_CASE("Announcer backs off when a whole tier fails") {
    FakeTrackers Fake;
    Fake.Fail("http://a");
    Announcer Target(TestConfig(), {{"http://a"}}, Fake.Function());
    const auto T0 = Announcer::Clock::now();

    Settle(Target, T0);
    CHECK(Target.Status()[0].NextAnnounce == T0 + 30s);
    Settle(Target, T0 + 29s);
    CHECK(Fake.Calls().size() == 1);
    Settle(Target, T0 + 30s);
    CHECK(Fake.Calls().size() == 2);
    CHECK(Target.Status()[0].NextAnnounce == T0 + 90s);
    CHECK(Fake.Calls()[1].Event == AnnounceEvent::Started);  // still owed
}

TEST_CASE("Announcer announces to every tier and skips duplicate trackers") {
    FakeTrackers Fake;
    Announcer Target(TestConfig(), {{"http://a"}, {"http://b", "http://a"}, {"http://c"}}, Fake.Function());
    Settle(Target, Announcer::Clock::now());

    CHECK(Target.Status().size() == 3);
    CHECK(Fake.Calls().size() == 3);
    CHECK(Target.TakePeers().size() == 3);
}

TEST_CASE("Announcer sends stopped only to tiers that started") {
    FakeTrackers Fake;
    Fake.Fail("http://b");
    Announcer Target(TestConfig(), {{"http://a"}, {"http://b"}}, Fake.Function());
    const auto T0 = Announcer::Clock::now();
    Settle(Target, T0);
    CHECK_FALSE(Target.IsFinished());

    Target.Stop(T0 + 1s);
    SettleStop(Target, T0 + 1s);

    const auto Calls = Fake.Calls();
    REQUIRE(Calls.size() == 3);
    CHECK(Calls[2].Url == "http://a");
    CHECK(Calls[2].Event == AnnounceEvent::Stopped);
}

TEST_CASE("Announcer Stop cancels an announce in progress") {
    FakeTrackers Fake;
    Announcer Target(TestConfig(), {{"http://slow"}}, Fake.Function());
    const auto T0 = Announcer::Clock::now();
    Target.Tick(T0, {});
    CHECK(Target.Status()[0].State == TrackerState::Announcing);

    Target.Stop(T0);
    SettleStop(Target, T0);
    CHECK(Fake.Calls().size() == 1);  // never started, so no "stopped"
}

TEST_CASE("Announcer reports unsupported tracker schemes") {
    Announcer Target(TestConfig(), {{"wss://tracker.example"}});
    Settle(Target, Announcer::Clock::now());
    CHECK(Target.Status()[0].State == TrackerState::Failed);
    CHECK(Target.Status()[0].Message == "unsupported tracker protocol");
}
