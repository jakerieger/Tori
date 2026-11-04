#include "Core/Encoding.hpp"
#include "Core/Error.hpp"
#include "Meta/MagnetUri.hpp"

#include <doctest/doctest.h>

using namespace Tori;

namespace {
    constexpr std::string_view kHash = "c12fe1c06bba254a9dc9f519b335aa7c1367a88a";
}  // namespace

TEST_CASE("Magnet parses a typical link") {
    const MagnetUri Magnet = MagnetUri::Parse(
      "magnet:?xt=urn:btih:C12FE1C06BBA254A9DC9F519B335AA7C1367A88A"
      "&dn=Big+Buck%20Bunny"
      "&tr=udp%3A%2F%2Ftracker.example%3A1337%2Fannounce"
      "&tr.1=http://other.example/announce?key=a+b"
      "&tr=udp%3A%2F%2Ftracker.example%3A1337%2Fannounce"
      "&x.pe=10.0.0.1:6881"
      "&ws=https%3A%2F%2Fseed.example%2F");

    CHECK(Magnet.Hash.ToHex() == kHash);
    CHECK(Magnet.DisplayName == "Big Buck Bunny");
    CHECK(Magnet.Trackers ==
          std::vector<std::string> {"udp://tracker.example:1337/announce", "http://other.example/announce?key=a+b"});
    CHECK(Magnet.Peers == std::vector<std::string> {"10.0.0.1:6881"});
    CHECK(Magnet.WebSeeds == std::vector<std::string> {"https://seed.example/"});
}

TEST_CASE("Magnet accepts base32 hashes and mixed-case schemes") {
    const auto Hash        = InfoHash::FromHex(kHash);
    const std::string Uri  = "MAGNET:?xt=URN:BTIH:" + Base32Encode(Hash->Bytes);
    const MagnetUri Magnet = MagnetUri::Parse(Uri);
    CHECK(Magnet.Hash == *Hash);
}

TEST_CASE("Magnet ignores v2 hashes when a v1 hash is present") {
    const MagnetUri Magnet = MagnetUri::Parse(std::string("magnet:?xt=urn:btmh:1220abcdef&xt=urn:btih:") +
                                              std::string(kHash));
    CHECK(Magnet.Hash.ToHex() == kHash);
}

TEST_CASE("Magnet rejects invalid links") {
    CHECK_THROWS_AS(MagnetUri::Parse("http://example.com"), ParseError);
    CHECK_THROWS_AS(MagnetUri::Parse("magnet:?dn=nothing"), ParseError);
    CHECK_THROWS_AS(MagnetUri::Parse("magnet:?xt=urn:btmh:1220abcdef"), ParseError);
    CHECK_THROWS_AS(MagnetUri::Parse("magnet:?xt=urn:btih:1234"), ParseError);
    CHECK_THROWS_AS(MagnetUri::Parse("magnet:?xt=urn:btih:zz2fe1c06bba254a9dc9f519b335aa7c1367a88a"), ParseError);
    CHECK_THROWS_AS(MagnetUri::Parse(std::string("magnet:?xt=urn:btih:") + std::string(kHash) + "&dn=%zz"),
                    ParseError);
    CHECK_THROWS_AS(MagnetUri::Parse(std::string("magnet:?xt=urn:btih:") + std::string(kHash) +
                                     "&xt=urn:btih:0000000000000000000000000000000000000000"),
                    ParseError);
}

TEST_CASE("Magnet ToString round-trips") {
    MagnetUri Original;
    Original.Hash        = *InfoHash::FromHex(kHash);
    Original.DisplayName = "name with spaces & symbols";
    Original.Trackers    = {"udp://tracker.example:1337/announce"};
    Original.Peers       = {"[::1]:6881"};

    const MagnetUri Parsed = MagnetUri::Parse(Original.ToString());
    CHECK(Parsed.Hash == Original.Hash);
    CHECK(Parsed.DisplayName == Original.DisplayName);
    CHECK(Parsed.Trackers == Original.Trackers);
    CHECK(Parsed.Peers == Original.Peers);
}
