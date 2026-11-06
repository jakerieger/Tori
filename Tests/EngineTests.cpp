#include "Core/Version.hpp"
#include "Dht/DhtNode.hpp"
#include "Engine/Session.hpp"
#include "Meta/MagnetUri.hpp"
#include "Meta/Metainfo.hpp"
#include "Meta/TorrentCreator.hpp"
#include "TestUtil.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace Tori;
using namespace Tori::Testing;
using namespace std::chrono_literals;

namespace {
    template<class TCondition>
    bool WaitUntil(TCondition&& Condition, const std::chrono::milliseconds Timeout) {
        const auto Deadline = std::chrono::steady_clock::now() + Timeout;
        while (std::chrono::steady_clock::now() < Deadline) {
            if (Condition()) { return true; }
            std::this_thread::sleep_for(20ms);
        }
        return Condition();
    }

    TorrentStats StatsOf(const Session& Target, const TorrentId Id) {
        for (const TorrentStats& Stats : Target.Snapshot()) {
            if (Stats.Id == Id) { return Stats; }
        }
        return {};
    }

    bool InState(const Session& Target, const TorrentId Id, const TorrentState State) {
        return StatsOf(Target, Id).State == State;
    }

    SessionSettings TestSettings() {
        SessionSettings Settings;
        Settings.ListenPort   = 0;
        Settings.LoopbackOnly = true;
        Settings.EnableDht    = false;  // DHT tests opt in, on loopback, without the public routers
        Settings.Announce     = [](const std::string&, const AnnounceRequest&, std::stop_token) -> AnnounceResponse {
            throw TrackerError("no trackers in tests");
        };
        return Settings;
    }

    Endpoint Loopback(const std::uint16_t Port) {
        return *Endpoint::FromAddress("127.0.0.1", Port);
    }

    /// A folder of random files and the torrent describing it.
    struct SharedFolder {
        explicit SharedFolder(const std::filesystem::path& Root) : Parent(Root / L"seed") {
            WriteFileBytes(Parent / L"Share/a.bin", RandomBytes(300000, 1));
            WriteFileBytes(Parent / L"Share/b.bin", RandomBytes(123457, 2));
            WriteFileBytes(Parent / L"Share/sub/c.bin", RandomBytes(1000, 3));

            TorrentCreatorOptions Options;
            Options.Source      = Parent / L"Share";
            Options.PieceLength = 32 * 1024;
            Meta                = Metainfo::Parse(*CreateTorrent(Options));
        }

        std::filesystem::path Parent;
        Metainfo Meta;
    };
}  // namespace

TEST_CASE("Session transfers a torrent between two clients over loopback") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);
    const auto LeechFolder = Dir.Path / L"leech";

    Session Seeder(TestSettings());
    Session Leecher(TestSettings());

    const TorrentId SeedId = Seeder.AddTorrent(Share.Meta, Share.Parent);
    REQUIRE(WaitUntil([&] { return InState(Seeder, SeedId, TorrentState::Seeding); }, 10s));
    CHECK(StatsOf(Seeder, SeedId).DownloadedSize == doctest::Approx(static_cast<double>(Share.Meta.TotalSize)));

    const TorrentId LeechId = Leecher.AddTorrent(Share.Meta, LeechFolder);
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Downloading); }, 10s));
    CHECK(StatsOf(Leecher, LeechId).DownloadedSize == 0.0);

    Leecher.AddPeers(LeechId, {Loopback(Seeder.ListenPort())});
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Seeding); }, 30s));

    const TorrentStats Done = StatsOf(Leecher, LeechId);
    CHECK(Done.DownloadedSize == doctest::Approx(static_cast<double>(Share.Meta.TotalSize)));
    // Stats are published every 250 ms, so the seeder's count can trail the leecher finishing
    CHECK(WaitUntil([&] { return StatsOf(Seeder, SeedId).UploadedTotal >= static_cast<double>(Share.Meta.TotalSize); }, 2s));

    for (const TorrentFile& File : Share.Meta.Files) {
        CHECK(ReadFileBytes(LeechFolder / File.Path) == ReadFileBytes(Share.Parent / File.Path));
    }
}

TEST_CASE("Three clients form a swarm and both leechers finish") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);

    Session Seeder(TestSettings());
    Session First(TestSettings());
    Session Second(TestSettings());

    const TorrentId SeedId = Seeder.AddTorrent(Share.Meta, Share.Parent);
    REQUIRE(WaitUntil([&] { return InState(Seeder, SeedId, TorrentState::Seeding); }, 10s));

    const TorrentId FirstId  = First.AddTorrent(Share.Meta, Dir.Path / L"first");
    const TorrentId SecondId = Second.AddTorrent(Share.Meta, Dir.Path / L"second");
    First.AddPeers(FirstId, {Loopback(Seeder.ListenPort()), Loopback(Second.ListenPort())});
    Second.AddPeers(SecondId, {Loopback(Seeder.ListenPort()), Loopback(First.ListenPort())});

    REQUIRE(WaitUntil([&] { return InState(First, FirstId, TorrentState::Seeding); }, 30s));
    REQUIRE(WaitUntil([&] { return InState(Second, SecondId, TorrentState::Seeding); }, 30s));

    for (const TorrentFile& File : Share.Meta.Files) {
        const auto Original = ReadFileBytes(Share.Parent / File.Path);
        CHECK(ReadFileBytes(Dir.Path / L"first" / File.Path) == Original);
        CHECK(ReadFileBytes(Dir.Path / L"second" / File.Path) == Original);
    }

    // Every byte that arrived was asked for, and nothing was received twice except in endgame
    const TorrentStats Done = StatsOf(First, FirstId);
    CHECK(Done.DownloadedTotal >= static_cast<double>(Share.Meta.TotalSize));
    CHECK(Done.DownloadedTotal < 1.25 * static_cast<double>(Share.Meta.TotalSize));
}

TEST_CASE("A magnet link fetches its metadata from a peer, then downloads") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);
    const auto LeechFolder = Dir.Path / L"leech";

    Session Seeder(TestSettings());
    Session Leecher(TestSettings());
    const TorrentId SeedId = Seeder.AddTorrent(Share.Meta, Share.Parent);
    REQUIRE(WaitUntil([&] { return InState(Seeder, SeedId, TorrentState::Seeding); }, 10s));

    MagnetUri Magnet;
    Magnet.Hash        = Share.Meta.Hash;
    Magnet.DisplayName = "from a magnet";
    const TorrentId LeechId = Leecher.AddMagnet(Magnet, LeechFolder);
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::FetchingMetadata); }, 5s));
    CHECK(StatsOf(Leecher, LeechId).Name == "from a magnet");
    CHECK_THROWS(Leecher.AddTorrent(Share.Meta, Dir.Path / L"elsewhere"));  // same info hash

    Leecher.AddPeers(LeechId, {Loopback(Seeder.ListenPort())});
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Seeding); }, 30s));

    const TorrentStats Done = StatsOf(Leecher, LeechId);
    CHECK(Done.Name == Share.Meta.Name);  // the real name, from the metadata
    CHECK(Done.TotalSize == doctest::Approx(static_cast<double>(Share.Meta.TotalSize)));
    for (const TorrentFile& File : Share.Meta.Files) {
        CHECK(ReadFileBytes(LeechFolder / File.Path) == ReadFileBytes(Share.Parent / File.Path));
    }
}

TEST_CASE("A trackerless magnet link finds its seeder through the DHT") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);

    // A bare DHT node both sessions bootstrap from, standing in for router.bittorrent.com
    DhtSettings RouterSettings;
    RouterSettings.LoopbackOnly     = true;
    RouterSettings.UsePublicRouters = false;
    DhtNode Router(RouterSettings, {});

    const auto WithDht = [&] {
        SessionSettings Settings      = TestSettings();
        Settings.EnableDht            = true;
        Settings.DhtLoopbackOnly      = true;
        Settings.DhtUsePublicRouters  = false;
        Settings.DhtBootstrapNodes    = {Loopback(Router.Port())};
        return Settings;
    };

    Session Seeder(WithDht());
    const TorrentId SeedId = Seeder.AddTorrent(Share.Meta, Share.Parent);
    REQUIRE(WaitUntil([&] { return InState(Seeder, SeedId, TorrentState::Seeding); }, 10s));

    // Wait until the seeder's announce is visible in the DHT before starting the download
    std::atomic<bool> Announced = false;
    DhtSettings ProbeSettings   = RouterSettings;
    ProbeSettings.BootstrapNodes = {Loopback(Router.Port())};
    DhtNode Probe(ProbeSettings, [&](const InfoHash& Hash, const std::vector<Endpoint>& Peers) {
        if (Hash == Share.Meta.Hash && std::ranges::find(Peers, Loopback(Seeder.ListenPort())) != Peers.end()) { Announced = true; }
    });
    REQUIRE(WaitUntil([&] {
        Probe.GetPeers(Share.Meta.Hash, 0);
        std::this_thread::sleep_for(200ms);
        return Announced.load();
    }, 15s));

    Session Leecher(WithDht());
    MagnetUri Magnet;
    Magnet.Hash             = Share.Meta.Hash;  // no trackers, no peers: the DHT is the only way
    const TorrentId LeechId = Leecher.AddMagnet(Magnet, Dir.Path / L"leech");
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Seeding); }, 30s));
    CHECK(Leecher.DhtNodeCount() >= 1);

    for (const TorrentFile& File : Share.Meta.Files) {
        CHECK(ReadFileBytes(Dir.Path / L"leech" / File.Path) == ReadFileBytes(Share.Parent / File.Path));
    }
}

TEST_CASE("Session repairs corrupted data after a recheck") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);
    const auto LeechFolder = Dir.Path / L"leech";

    Session Seeder(TestSettings());
    Session Leecher(TestSettings());
    const TorrentId SeedId = Seeder.AddTorrent(Share.Meta, Share.Parent);
    REQUIRE(WaitUntil([&] { return InState(Seeder, SeedId, TorrentState::Seeding); }, 10s));

    // Start from a corrupted copy
    std::filesystem::create_directories(LeechFolder);
    std::filesystem::copy(Share.Parent / L"Share", LeechFolder / L"Share", std::filesystem::copy_options::recursive);
    auto Corrupted = ReadFileBytes(LeechFolder / L"Share/b.bin");
    Corrupted[5000] ^= 0xFF;
    WriteFileBytes(LeechFolder / L"Share/b.bin", Corrupted);

    const TorrentId LeechId = Leecher.AddTorrent(Share.Meta, LeechFolder);
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Downloading); }, 10s));
    const double Missing = Share.Meta.TotalSize - StatsOf(Leecher, LeechId).DownloadedSize;
    CHECK(Missing == doctest::Approx(32 * 1024.0));  // exactly one piece failed

    Leecher.AddPeers(LeechId, {Loopback(Seeder.ListenPort())});
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Seeding); }, 30s));
    CHECK(ReadFileBytes(LeechFolder / L"Share/b.bin") == ReadFileBytes(Share.Parent / L"Share/b.bin"));

    // A recheck of good data lands straight back in seeding (too quickly to reliably observe Checking)
    Leecher.Recheck(LeechId);
    std::this_thread::sleep_for(300ms);
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Seeding); }, 10s));
    CHECK(StatsOf(Leecher, LeechId).DownloadedSize == doctest::Approx(static_cast<double>(Share.Meta.TotalSize)));
}

TEST_CASE("Session pauses, resumes, rejects duplicates and deletes files on removal") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);
    Session Engine(TestSettings());

    const TorrentId Id = Engine.AddTorrent(Share.Meta, Share.Parent, true);
    REQUIRE(WaitUntil([&] { return InState(Engine, Id, TorrentState::Paused); }, 10s));
    CHECK_THROWS(Engine.AddTorrent(Share.Meta, Dir.Path / L"elsewhere"));

    Engine.Resume(Id);
    REQUIRE(WaitUntil([&] { return InState(Engine, Id, TorrentState::Seeding); }, 5s));
    Engine.Pause(Id);
    REQUIRE(WaitUntil([&] { return InState(Engine, Id, TorrentState::Paused); }, 5s));

    // Keep an unrelated file in the torrent's folder: deletion must leave it alone
    WriteFileBytes(Share.Parent / L"Share/sub/mine.txt", {1, 2, 3});

    Engine.RemoveTorrent(Id, true);
    CHECK(Engine.Snapshot().empty());
    REQUIRE(WaitUntil([&] { return !std::filesystem::exists(Share.Parent / L"Share/a.bin"); }, 10s));
    CHECK_FALSE(std::filesystem::exists(Share.Parent / L"Share/b.bin"));
    CHECK(std::filesystem::exists(Share.Parent / L"Share/sub/mine.txt"));
}

TEST_CASE("Skipped files aren't downloaded until they're wanted again") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);
    const auto LeechFolder = Dir.Path / L"leech";
    REQUIRE(Share.Meta.Files.size() == 3);  // a.bin, b.bin, sub/c.bin

    Session Seeder(TestSettings());
    Session Leecher(TestSettings());
    const TorrentId SeedId = Seeder.AddTorrent(Share.Meta, Share.Parent);
    REQUIRE(WaitUntil([&] { return InState(Seeder, SeedId, TorrentState::Seeding); }, 10s));

    const TorrentId LeechId = Leecher.AddTorrent(Share.Meta, LeechFolder, false, {1});
    Leecher.AddPeers(LeechId, {Loopback(Seeder.ListenPort())});
    REQUIRE(WaitUntil([&] { return InState(Leecher, LeechId, TorrentState::Seeding); }, 30s));

    // Progress counts only the pieces of the chosen files, which overlap b.bin at its ends
    const TorrentStats Partial = StatsOf(Leecher, LeechId);
    CHECK(Partial.TotalSize < static_cast<double>(Share.Meta.TotalSize));
    CHECK(Partial.DownloadedSize == Partial.TotalSize);
    CHECK(ReadFileBytes(LeechFolder / Share.Meta.Files[0].Path) == ReadFileBytes(Share.Parent / Share.Meta.Files[0].Path));
    CHECK(ReadFileBytes(LeechFolder / Share.Meta.Files[2].Path) == ReadFileBytes(Share.Parent / Share.Meta.Files[2].Path));
    CHECK(ReadFileBytes(LeechFolder / Share.Meta.Files[1].Path) != ReadFileBytes(Share.Parent / Share.Meta.Files[1].Path));

    // Details are only built while asked for, so the first call may come back empty
    std::shared_ptr<const TorrentDetails> Details;
    REQUIRE(WaitUntil([&] { return (Details = Leecher.Details(LeechId)) != nullptr; }, 5s));
    REQUIRE(Details->Files.size() == 3);
    CHECK(Details->Files[0].Done == Details->Files[0].Size);
    CHECK_FALSE(Details->Files[1].Wanted);
    CHECK(Details->Files[1].Done < Details->Files[1].Size);

    Leecher.SetSkippedFiles(LeechId, {});
    REQUIRE(WaitUntil([&] { return StatsOf(Leecher, LeechId).DownloadedSize == static_cast<double>(Share.Meta.TotalSize); }, 30s));
    CHECK(InState(Leecher, LeechId, TorrentState::Seeding));
    for (const TorrentFile& File : Share.Meta.Files) {
        CHECK(ReadFileBytes(LeechFolder / File.Path) == ReadFileBytes(Share.Parent / File.Path));
    }
}

TEST_CASE("Details list the peers a torrent is connected to") {
    TempDir Dir;
    const SharedFolder Share(Dir.Path);
    // A seeder missing its first piece, so neither side ever finishes and the connection stays up
    auto Damaged = ReadFileBytes(Share.Parent / L"Share/a.bin");
    Damaged[0] ^= 0xFF;
    WriteFileBytes(Share.Parent / L"Share/a.bin", Damaged);

    Session Seeder(TestSettings());
    Session Leecher(TestSettings());
    const TorrentId SeedId = Seeder.AddTorrent(Share.Meta, Share.Parent);
    REQUIRE(WaitUntil([&] { return InState(Seeder, SeedId, TorrentState::Downloading); }, 10s));
    const TorrentId LeechId = Leecher.AddTorrent(Share.Meta, Dir.Path / L"leech");
    Leecher.AddPeers(LeechId, {Loopback(Seeder.ListenPort())});

    std::shared_ptr<const TorrentDetails> Details;
    REQUIRE(WaitUntil(
      [&] {
          Details = Seeder.Details(SeedId);
          return Details && !Details->Peers.empty();
      },
      10s));
    CHECK(Details->Peers.front().Client == kClientName);
    CHECK(Details->Peers.front().Flags.find('I') != std::string::npos);  // The leecher connected to us
    CHECK(Details->Files.size() == 3);
}
