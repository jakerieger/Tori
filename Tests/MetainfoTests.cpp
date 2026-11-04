#include "Core/Bencode.hpp"
#include "Core/Error.hpp"
#include "Core/Sha1.hpp"
#include "Meta/Metainfo.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace Tori;
namespace B = Tori::Bencode;

namespace {
    std::string DigestBytes(const Sha1Digest& Digest) {
        return {reinterpret_cast<const char*>(Digest.data()), Digest.size()};
    }

    B::Dict SingleFileInfo() {
        return {
          {"name", "hello.txt"},
          {"length", 11},
          {"piece length", 16384},
          {"pieces", DigestBytes(Sha1::Hash("hello world"))},
        };
    }

    std::string Torrent(B::Dict Info, B::Dict Extra = {}) {
        Extra.emplace_back("info", std::move(Info));
        return B::Encode(Extra);
    }
}  // namespace

TEST_CASE("Metainfo parses a single-file torrent") {
    const std::string Data = Torrent(SingleFileInfo(),
                                     {
                                       {"announce", "http://tracker.example/announce"},
                                       {"comment", "test"},
                                       {"created by", "Tori"},
                                       {"creation date", 1700000000},
                                     });
    const Metainfo Meta = Metainfo::Parse(Data);

    CHECK(Meta.Name == "hello.txt");
    CHECK_FALSE(Meta.IsMultiFile);
    CHECK(Meta.TotalSize == 11);
    CHECK(Meta.PieceCount() == 1);
    CHECK(Meta.PieceSize(0) == 11);
    CHECK(Meta.PieceHashes[0] == Sha1::Hash("hello world"));
    REQUIRE(Meta.Files.size() == 1);
    CHECK(Meta.Files[0].Path == std::filesystem::path(L"hello.txt"));
    CHECK(Meta.TrackerTiers == std::vector<std::vector<std::string>> {{"http://tracker.example/announce"}});
    CHECK(Meta.Comment == "test");
    CHECK(Meta.CreatedBy == "Tori");
    CHECK(Meta.CreationDate == 1700000000);
    CHECK_FALSE(Meta.IsPrivate);

    const std::string InfoBytes = B::Encode(SingleFileInfo());
    CHECK(Meta.InfoBytes == InfoBytes);
    CHECK(Meta.Hash.Bytes == Sha1::Hash(InfoBytes));
}

TEST_CASE("Metainfo hashes the info dictionary's original bytes") {
    // Keys out of order: re-encoding would sort them and change the hash
    const std::string Pieces   = DigestBytes(Sha1::Hash("hello world"));
    const std::string RawInfo  = "d4:name9:hello.txt6:lengthi11e12:piece lengthi16384e6:pieces20:" + Pieces + "e";
    const std::string Data     = "d4:info" + RawInfo + "e";
    const Metainfo Meta        = Metainfo::Parse(Data);

    CHECK(Meta.Hash.Bytes == Sha1::Hash(RawInfo));
    CHECK(Meta.Hash.Bytes != Sha1::Hash(B::Encode(B::Decode(RawInfo))));
    CHECK(Meta.InfoBytes == RawInfo);
}

TEST_CASE("Metainfo parses a multi-file torrent") {
    const B::Dict Info = {
      {"name", "album"},
      {"piece length", 3},
      {"pieces", std::string(60, 'x')},
      {"files",
       B::List {
         B::Dict {{"length", 3}, {"path", B::List {"a.txt"}}},
         B::Dict {{"length", 1}, {"path", B::List {".pad", "1"}}, {"attr", "p"}},
         B::Dict {{"length", 4}, {"path", B::List {"sub", "b.txt"}}},
       }},
      {"private", 1},
    };
    const Metainfo Meta = Metainfo::Parse(Torrent(Info));

    CHECK(Meta.IsMultiFile);
    CHECK(Meta.IsPrivate);
    CHECK(Meta.TotalSize == 8);
    CHECK(Meta.PieceCount() == 3);
    CHECK(Meta.PieceSize(0) == 3);
    CHECK(Meta.PieceSize(2) == 2);
    REQUIRE(Meta.Files.size() == 3);
    CHECK(Meta.Files[0].Path == std::filesystem::path(L"album/a.txt"));
    CHECK(Meta.Files[1].IsPadding);
    CHECK(Meta.Files[2].Path == std::filesystem::path(L"album/sub/b.txt"));
    CHECK(Meta.Files[2].Offset == 4);
}

TEST_CASE("Metainfo prefers announce-list and reads url-list") {
    const std::string Data = Torrent(SingleFileInfo(),
                                     {
                                       {"announce", "http://ignored/announce"},
                                       {"announce-list",
                                        B::List {
                                          B::List {"udp://a:80", "udp://b:80"},
                                          B::List {},
                                          B::List {"http://c/announce"},
                                        }},
                                       {"url-list", "http://seed/file"},
                                     });
    const Metainfo Meta = Metainfo::Parse(Data);

    CHECK(Meta.TrackerTiers ==
          std::vector<std::vector<std::string>> {{"udp://a:80", "udp://b:80"}, {"http://c/announce"}});
    CHECK(Meta.WebSeeds == std::vector<std::string> {"http://seed/file"});
}

TEST_CASE("Metainfo sanitizes hostile file paths") {
    const B::Dict Info = {
      {"name", "../evil"},
      {"piece length", 16384},
      {"pieces", std::string(20, 'x')},
      {"files",
       B::List {
         B::Dict {{"length", 1}, {"path", B::List {"..", "..", "CON.txt"}}},
         B::Dict {{"length", 1}, {"path", B::List {".", "a:b?", "trail. "}}},
       }},
    };
    const Metainfo Meta = Metainfo::Parse(Torrent(Info));

    CHECK(Meta.Name == ".._evil");
    CHECK(Meta.Files[0].Path == std::filesystem::path(L".._evil/_/_/_CON.txt"));
    CHECK(Meta.Files[1].Path == std::filesystem::path(L".._evil/a_b_/trail"));
    for (const TorrentFile& File : Meta.Files) {
        for (const auto& Component : File.Path) { CHECK(Component != L".."); }
    }
}

TEST_CASE("SanitizePathComponent") {
    CHECK(SanitizePathComponent("") == "");
    CHECK(SanitizePathComponent(".") == "");
    CHECK(SanitizePathComponent("..") == "_");
    CHECK(SanitizePathComponent("...") == "_");
    CHECK(SanitizePathComponent("normal name.mkv") == "normal name.mkv");
    CHECK(SanitizePathComponent("a/b\\c") == "a_b_c");
    CHECK(SanitizePathComponent(std::string("tab\there", 8)) == "tab_here");
    CHECK(SanitizePathComponent("nul") == "_nul");
    CHECK(SanitizePathComponent("Lpt9.log") == "_Lpt9.log");
    CHECK(SanitizePathComponent("COM0") == "COM0");
    CHECK(SanitizePathComponent("CONSOLE") == "CONSOLE");
    CHECK(SanitizePathComponent("日本語") == "日本語");
}

TEST_CASE("Metainfo rejects invalid torrents") {
    CHECK_THROWS_AS(Metainfo::Parse("le"), ParseError);
    CHECK_THROWS_AS(Metainfo::Parse(B::Encode(B::Dict {{"announce", "x"}})), ParseError);

    auto Mutated = [](const std::string& Key, B::Value Replacement) {
        B::Dict Info = SingleFileInfo();
        for (auto& [EntryKey, EntryValue] : Info) {
            if (EntryKey == Key) { EntryValue = std::move(Replacement); }
        }
        return Torrent(std::move(Info));
    };
    CHECK_THROWS_AS(Metainfo::Parse(Mutated("pieces", std::string(19, 'x'))), ParseError);
    CHECK_THROWS_AS(Metainfo::Parse(Mutated("pieces", std::string(40, 'x'))), ParseError);  // count mismatch
    CHECK_THROWS_AS(Metainfo::Parse(Mutated("piece length", 0)), ParseError);
    CHECK_THROWS_AS(Metainfo::Parse(Mutated("length", -1)), ParseError);
    CHECK_THROWS_AS(Metainfo::Parse(Mutated("length", 0)), ParseError);
    CHECK_THROWS_AS(Metainfo::Parse(Mutated("name", "")), ParseError);

    const B::Dict V2Only = {
      {"name", "x"}, {"piece length", 16384}, {"meta version", 2}, {"file tree", B::Dict {}}};
    CHECK_THROWS_WITH_AS(Metainfo::Parse(Torrent(V2Only)), doctest::Contains("v2-only"), ParseError);
}

TEST_CASE("Metainfo parses a bare info dictionary from ut_metadata") {
    const std::string InfoBytes = B::Encode(SingleFileInfo());
    const Metainfo Meta         = Metainfo::ParseInfoDictionary(InfoBytes);
    CHECK(Meta.Name == "hello.txt");
    CHECK(Meta.Hash.Bytes == Sha1::Hash(InfoBytes));
    CHECK(Meta.TrackerTiers.empty());
}

TEST_CASE("Metainfo::Load reads from disk") {
    const auto File = std::filesystem::temp_directory_path() / L"tori-test-日本.torrent";
    {
        std::ofstream Out(File, std::ios::binary);
        Out << Torrent(SingleFileInfo());
    }
    CHECK(Metainfo::Load(File).Name == "hello.txt");
    std::filesystem::remove(File);

    CHECK_THROWS(Metainfo::Load(File));
}
