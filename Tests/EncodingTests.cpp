#include "Core/Encoding.hpp"
#include "Core/InfoHash.hpp"
#include "Core/Sha1.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Tori;

namespace {
    std::vector<std::uint8_t> Bytes(const std::string_view Text) {
        return {Text.begin(), Text.end()};
    }
}  // namespace

TEST_CASE("SHA-1 matches FIPS 180 test vectors") {
    CHECK(ToHex(Sha1::Hash("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(ToHex(Sha1::Hash("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(ToHex(Sha1::Hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("SHA-1 is incremental and reusable") {
    Sha1 Hasher;
    Hasher.Update("ab");
    Hasher.Update("c");
    CHECK(Hasher.Finish() == Sha1::Hash("abc"));

    Hasher.Update("abc");
    CHECK(Hasher.Finish() == Sha1::Hash("abc"));
}

TEST_CASE("Hex round-trips and rejects junk") {
    CHECK(ToHex(Bytes("\x01\xab\xff")) == "01abff");
    CHECK(FromHex("01ABff") == Bytes("\x01\xab\xff"));
    CHECK_FALSE(FromHex("abc").has_value());
    CHECK_FALSE(FromHex("zz").has_value());
}

TEST_CASE("Base32 matches RFC 4648 test vectors") {
    CHECK(Base32Encode(Bytes("")) == "");
    CHECK(Base32Encode(Bytes("f")) == "MY======");
    CHECK(Base32Encode(Bytes("fo")) == "MZXQ====");
    CHECK(Base32Encode(Bytes("foo")) == "MZXW6===");
    CHECK(Base32Encode(Bytes("foob")) == "MZXW6YQ=");
    CHECK(Base32Encode(Bytes("fooba")) == "MZXW6YTB");
    CHECK(Base32Encode(Bytes("foobar")) == "MZXW6YTBOI======");

    CHECK(Base32Decode("MZXW6YTBOI======") == Bytes("foobar"));
    CHECK(Base32Decode("mzxw6ytb") == Bytes("fooba"));
    CHECK_FALSE(Base32Decode("MZXW1").has_value());
}

TEST_CASE("Percent encoding") {
    CHECK(PercentEncode("a b/c~") == "a%20b%2Fc~");
    CHECK(PercentEncode(std::string("\x00\xff", 2)) == "%00%FF");
    CHECK(PercentDecode("a%20b%2fc") == "a b/c");
    CHECK(PercentDecode("a+b") == "a+b");
    CHECK(PercentDecode("a+b", true) == "a b");
    CHECK_FALSE(PercentDecode("%2").has_value());
    CHECK_FALSE(PercentDecode("%zz").has_value());
}

TEST_CASE("InfoHash parses hex and base32 forms") {
    const auto FromHex = InfoHash::FromHex("C12FE1C06BBA254A9DC9F519B335AA7C1367A88A");
    REQUIRE(FromHex.has_value());
    CHECK(FromHex->ToHex() == "c12fe1c06bba254a9dc9f519b335aa7c1367a88a");

    const std::string Base32 = Base32Encode(FromHex->Bytes);
    CHECK(Base32.size() == 32);
    CHECK(InfoHash::FromBase32(Base32) == FromHex);

    CHECK_FALSE(InfoHash::FromHex("c12f").has_value());
    CHECK_FALSE(InfoHash::FromBase32("MZXW6YTB").has_value());
}
