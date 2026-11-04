#include "Meta/Metainfo.hpp"
#include "Meta/TorrentCreator.hpp"
#include "Storage/FileStorage.hpp"
#include "Storage/PieceVerifier.hpp"
#include "TestUtil.hpp"

#include <doctest/doctest.h>

using namespace Tori;
using namespace Tori::Testing;

namespace {
    Metainfo CreateAndParse(const TorrentCreatorOptions& Options) {
        const std::optional<std::string> Data = CreateTorrent(Options);
        REQUIRE(Data.has_value());
        return Metainfo::Parse(*Data);
    }
}  // namespace

TEST_CASE("AutoPieceLength scales with size within bounds") {
    CHECK(AutoPieceLength(1) == 16 * 1024);
    CHECK(AutoPieceLength(700LL * 1024 * 1024) == 512 * 1024);
    CHECK(AutoPieceLength(4LL * 1024 * 1024 * 1024) == 4 * 1024 * 1024);
    CHECK(AutoPieceLength(1LL << 50) == 16 * 1024 * 1024);
}

TEST_CASE("CreateTorrent round-trips a folder through parse and recheck") {
    TempDir Dir;
    const auto Root = Dir.Path / L"My Share";
    WriteFileBytes(Root / L"b.bin", RandomBytes(40000, 1));
    WriteFileBytes(Root / L"a.txt", RandomBytes(123, 2));
    WriteFileBytes(Root / L"sub/c.bin", RandomBytes(70000, 3));
    WriteFileBytes(Root / L"sub/empty.txt", {});
    WriteFileBytes(Root / L"日本/d.bin", RandomBytes(5, 4));

    TorrentCreatorOptions Options;
    Options.Source       = Root;
    Options.PieceLength  = 16 * 1024;
    Options.TrackerTiers = {{"udp://a:80", "udp://b:80"}, {"http://c/announce"}};
    Options.WebSeeds     = {"http://seed/"};
    Options.Comment      = "hello";

    int LastProgress = 0;
    const std::optional<std::string> Data =
      CreateTorrent(Options, {}, [&](const int Hashed, const int) { LastProgress = Hashed; });
    REQUIRE(Data.has_value());
    const Metainfo Meta = Metainfo::Parse(*Data);

    CHECK(Meta.Name == "My Share");
    CHECK(Meta.IsMultiFile);
    CHECK(Meta.TotalSize == 40000 + 123 + 70000 + 5);
    CHECK(Meta.PieceCount() == LastProgress);
    CHECK(Meta.TrackerTiers == Options.TrackerTiers);
    CHECK(Meta.WebSeeds == Options.WebSeeds);
    CHECK(Meta.Comment == "hello");
    CHECK(Meta.CreatedBy == "Tori");
    CHECK(Meta.CreationDate.has_value());
    CHECK_FALSE(Meta.IsPrivate);

    // Sorted, with the torrent name as the root folder
    REQUIRE(Meta.Files.size() == 5);
    CHECK(Meta.Files[0].Path == std::filesystem::path(L"My Share/a.txt"));
    CHECK(Meta.Files[1].Path == std::filesystem::path(L"My Share/b.bin"));
    CHECK(Meta.Files[2].Path == std::filesystem::path(L"My Share/sub/c.bin"));
    CHECK(Meta.Files[3].Path == std::filesystem::path(L"My Share/sub/empty.txt"));
    CHECK(Meta.Files[4].Path == std::filesystem::path(L"My Share/日本/d.bin"));

    FileStorage Storage(Dir.Path, Meta.Files);
    const auto Verified = VerifyPieces(Storage, Meta);
    REQUIRE(Verified.has_value());
    CHECK(Verified->All());
    Storage.CloseAll();

    // Corrupting a file breaks exactly the pieces that cover it
    auto Changed = ReadFileBytes(Root / L"a.txt");
    Changed[0] ^= 0xFF;
    WriteFileBytes(Root / L"a.txt", Changed);
    const auto AfterCorruption = VerifyPieces(Storage, Meta);
    CHECK_FALSE(AfterCorruption->Get(0));
    CHECK(AfterCorruption->Count() == Meta.PieceCount() - 1);
    Storage.CloseAll();

    // Deleting the last file loses the final piece
    std::filesystem::remove(Root / L"日本/d.bin");
    const auto AfterDelete = VerifyPieces(Storage, Meta);
    CHECK_FALSE(AfterDelete->Get(Meta.PieceCount() - 1));
}

TEST_CASE("CreateTorrent makes a single-file private torrent") {
    TempDir Dir;
    const auto File = Dir.Path / L"movie.mkv";
    WriteFileBytes(File, RandomBytes(50000, 5));

    TorrentCreatorOptions Options;
    Options.Source              = File;
    Options.TrackerTiers        = {{"http://only/announce"}};
    Options.IsPrivate           = true;
    Options.IncludeCreationDate = false;

    const Metainfo Meta = CreateAndParse(Options);
    CHECK(Meta.Name == "movie.mkv");
    CHECK_FALSE(Meta.IsMultiFile);
    CHECK(Meta.IsPrivate);
    CHECK(Meta.PieceLength == 16 * 1024);
    CHECK(Meta.PieceCount() == 4);
    CHECK(Meta.TrackerTiers == std::vector<std::vector<std::string>> {{"http://only/announce"}});
    CHECK_FALSE(Meta.CreationDate.has_value());

    FileStorage Storage(Dir.Path, Meta.Files);
    CHECK(VerifyPieces(Storage, Meta)->All());
}

TEST_CASE("CreateTorrent output is deterministic without a creation date") {
    TempDir Dir;
    WriteFileBytes(Dir.Path / L"s/x", RandomBytes(1000, 6));
    WriteFileBytes(Dir.Path / L"s/y", RandomBytes(1000, 7));

    TorrentCreatorOptions Options;
    Options.Source              = Dir.Path / L"s/";  // trailing separator is fine
    Options.IncludeCreationDate = false;
    CHECK(CreateTorrent(Options) == CreateTorrent(Options));
    CHECK(CreateAndParse(Options).Name == "s");
}

TEST_CASE("CreateTorrent rejects empty or missing sources and can be cancelled") {
    TempDir Dir;
    std::filesystem::create_directories(Dir.Path / L"empty");
    WriteFileBytes(Dir.Path / L"zero/file", {});

    TorrentCreatorOptions Options;
    Options.Source = Dir.Path / L"empty";
    CHECK_THROWS(CreateTorrent(Options));
    Options.Source = Dir.Path / L"zero";
    CHECK_THROWS(CreateTorrent(Options));
    Options.Source = Dir.Path / L"missing";
    CHECK_THROWS(CreateTorrent(Options));

    WriteFileBytes(Dir.Path / L"real.bin", RandomBytes(10, 8));
    Options.Source = Dir.Path / L"real.bin";
    std::stop_source Stop;
    Stop.request_stop();
    CHECK_FALSE(CreateTorrent(Options, Stop.get_token()).has_value());
}
