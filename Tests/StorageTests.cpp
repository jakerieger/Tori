#include "Core/Bitfield.hpp"
#include "Core/Sha1.hpp"
#include "Storage/FileStorage.hpp"
#include "Storage/PieceVerifier.hpp"
#include "TestUtil.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace Tori;
using namespace Tori::Testing;

TEST_CASE("Bitfield tracks bits and count") {
    Bitfield Bits(10);
    CHECK(Bits.None());
    Bits.Set(0);
    Bits.Set(9);
    Bits.Set(9);  // idempotent
    CHECK(Bits.Count() == 2);
    CHECK(Bits.Get(0));
    CHECK_FALSE(Bits.Get(1));
    CHECK(Bits.Bytes() == std::vector<std::uint8_t> {0x80, 0x40});

    Bits.Set(0, false);
    CHECK(Bits.Count() == 1);
    CHECK_FALSE(Bits.All());
}

TEST_CASE("Bitfield parses the wire format strictly") {
    const std::vector<std::uint8_t> Wire = {0xFF, 0xC0};
    const auto Parsed                    = Bitfield::FromBytes(Wire, 10);
    REQUIRE(Parsed.has_value());
    CHECK(Parsed->All());
    CHECK(Parsed->Count() == 10);

    CHECK_FALSE(Bitfield::FromBytes(std::vector<std::uint8_t> {0xFF, 0xE0}, 10).has_value());  // spare bit set
    CHECK_FALSE(Bitfield::FromBytes(std::vector<std::uint8_t> {0xFF}, 10).has_value());        // too short
}

TEST_CASE("MapByteRange splits across files and skips empty ones") {
    const std::vector<TorrentFile> Files = {
      {.Path = L"a", .Length = 3, .Offset = 0},
      {.Path = L"empty", .Length = 0, .Offset = 3},
      {.Path = L"b", .Length = 4, .Offset = 3},
      {.Path = L"c", .Length = 2, .Offset = 7},
    };

    const auto Whole = MapByteRange(Files, 0, 9);
    REQUIRE(Whole.size() == 3);
    CHECK(Whole[0].FileIndex == 0);
    CHECK(Whole[1].FileIndex == 2);
    CHECK(Whole[2].FileIndex == 3);

    const auto Middle = MapByteRange(Files, 2, 6);
    REQUIRE(Middle.size() == 3);
    CHECK((Middle[0].FileIndex == 0 && Middle[0].FileOffset == 2 && Middle[0].Length == 1));
    CHECK((Middle[1].FileIndex == 2 && Middle[1].FileOffset == 0 && Middle[1].Length == 4));
    CHECK((Middle[2].FileIndex == 3 && Middle[2].FileOffset == 0 && Middle[2].Length == 1));

    const auto AtBoundary = MapByteRange(Files, 3, 1);
    REQUIRE(AtBoundary.size() == 1);
    CHECK(AtBoundary[0].FileIndex == 2);

    CHECK(MapByteRange(Files, 0, 0).empty());
}

TEST_CASE("FileStorage writes and reads across files") {
    TempDir Dir;
    const std::vector<TorrentFile> Files = {
      {.Path = L"t/a.bin", .Length = 5, .Offset = 0},
      {.Path = L"t/.pad/0", .Length = 3, .Offset = 5, .IsPadding = true},
      {.Path = L"t/deep/er/b.bin", .Length = 8, .Offset = 8},
    };
    FileStorage Storage(Dir.Path, Files);
    CHECK(Storage.TotalSize() == 16);

    std::vector<std::uint8_t> Buffer(16);
    CHECK_FALSE(Storage.Read(0, Buffer));  // nothing on disk yet

    const std::vector<std::uint8_t> Data = {1, 2, 3, 4, 5, 9, 9, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    Storage.Write(0, Data);
    Storage.CloseAll();

    CHECK(std::filesystem::file_size(Dir.Path / L"t/a.bin") == 5);
    CHECK(std::filesystem::file_size(Dir.Path / L"t/deep/er/b.bin") == 8);
    CHECK_FALSE(std::filesystem::exists(Dir.Path / L"t/.pad/0"));

    REQUIRE(Storage.Read(0, Buffer));
    std::vector<std::uint8_t> Expected = Data;
    std::fill_n(Expected.begin() + 5, 3, std::uint8_t {0});  // padding reads as zeros
    CHECK(Buffer == Expected);

    std::vector<std::uint8_t> Partial(4);
    REQUIRE(Storage.Read(3, Partial));
    CHECK(Partial == std::vector<std::uint8_t> {4, 5, 0, 0});

    CHECK_THROWS_AS(Storage.Write(15, Data), StorageError);
    CHECK_THROWS_AS((void)Storage.Read(-1, Partial), StorageError);
}

TEST_CASE("FileStorage preallocates the whole file on first write") {
    TempDir Dir;
    FileStorage Storage(Dir.Path, {{.Path = L"big.bin", .Length = 1 << 20, .Offset = 0}});
    const std::vector<std::uint8_t> Tail = {7, 7, 7};
    Storage.Write((1 << 20) - 3, Tail);
    Storage.CloseAll();
    CHECK(std::filesystem::file_size(Dir.Path / L"big.bin") == (1u << 20));
}

TEST_CASE("FileStorage treats a short file as missing data") {
    TempDir Dir;
    WriteFileBytes(Dir.Path / L"short.bin", {1, 2});
    FileStorage Storage(Dir.Path, {{.Path = L"short.bin", .Length = 4, .Offset = 0}});
    std::vector<std::uint8_t> Buffer(4);
    CHECK_FALSE(Storage.Read(0, Buffer));
}

TEST_CASE("FileStorage handles more files than it keeps open") {
    TempDir Dir;
    std::vector<TorrentFile> Files;
    const int Count = static_cast<int>(FileStorage::kMaxOpenFiles) + 10;
    for (int I = 0; I < Count; ++I) {
        Files.push_back({.Path = std::format(L"many/{}.bin", I), .Length = 1, .Offset = I});
    }
    FileStorage Storage(Dir.Path, Files);

    std::vector<std::uint8_t> Data(static_cast<std::size_t>(Count));
    for (int I = 0; I < Count; ++I) { Data[static_cast<std::size_t>(I)] = static_cast<std::uint8_t>(I); }
    Storage.Write(0, Data);

    std::vector<std::uint8_t> Buffer(Data.size());
    REQUIRE(Storage.Read(0, Buffer));
    CHECK(Buffer == Data);
}

TEST_CASE("VerifyPieces finds good, corrupt and missing pieces") {
    TempDir Dir;
    const auto Content = RandomBytes(100, 1);

    Metainfo Meta;
    Meta.PieceLength = 32;
    Meta.TotalSize   = 100;
    Meta.Files       = {{.Path = L"data.bin", .Length = 100, .Offset = 0}};
    for (int I = 0; I < 4; ++I) {
        const auto Begin = Content.begin() + I * 32;
        const auto End   = Content.begin() + std::min(100, (I + 1) * 32);
        Meta.PieceHashes.push_back(Sha1::Hash({reinterpret_cast<const char*>(&*Begin), static_cast<std::size_t>(End - Begin)}));
    }

    FileStorage Storage(Dir.Path, Meta.Files);
    {
        const auto Nothing = VerifyPieces(Storage, Meta);
        REQUIRE(Nothing.has_value());
        CHECK(Nothing->None());
    }

    std::vector<std::uint8_t> Corrupt = Content;
    Corrupt[40] ^= 0xFF;
    Storage.Write(0, Corrupt);

    int LastProgress = 0;
    const auto Result = VerifyPieces(Storage, Meta, {}, [&](const int Checked, const int Total) {
        CHECK(Total == 4);
        LastProgress = Checked;
    });
    REQUIRE(Result.has_value());
    CHECK(LastProgress == 4);
    CHECK(Result->Get(0));
    CHECK_FALSE(Result->Get(1));
    CHECK(Result->Get(2));
    CHECK(Result->Get(3));  // the short last piece

    std::stop_source Stop;
    Stop.request_stop();
    CHECK_FALSE(VerifyPieces(Storage, Meta, Stop.get_token()).has_value());
}
