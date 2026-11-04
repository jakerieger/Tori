#include "Core/Bencode.hpp"
#include "Core/Error.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace Tori;
using namespace Tori::Bencode;

TEST_CASE("Bencode decodes integers") {
    CHECK(Decode("i0e").AsInteger() == 0);
    CHECK(Decode("i42e").AsInteger() == 42);
    CHECK(Decode("i-42e").AsInteger() == -42);
    CHECK(Decode("i9223372036854775807e").AsInteger() == INT64_MAX);
}

TEST_CASE("Bencode rejects malformed integers") {
    CHECK_THROWS_AS(Decode("ie"), ParseError);
    CHECK_THROWS_AS(Decode("i-e"), ParseError);
    CHECK_THROWS_AS(Decode("i-0e"), ParseError);
    CHECK_THROWS_AS(Decode("i03e"), ParseError);
    CHECK_THROWS_AS(Decode("i1.5e"), ParseError);
    CHECK_THROWS_AS(Decode("i12"), ParseError);
    CHECK_THROWS_AS(Decode("i9223372036854775808e"), ParseError);
}

TEST_CASE("Bencode decodes strings including binary data") {
    CHECK(Decode("4:spam").AsString() == "spam");
    CHECK(Decode("0:").AsString().empty());
    const std::string Binary("3:\0\xff\n", 5);
    CHECK(Decode(Binary).AsString() == std::string("\0\xff\n", 3));
}

TEST_CASE("Bencode rejects malformed strings") {
    CHECK_THROWS_AS(Decode("5:spam"), ParseError);
    CHECK_THROWS_AS(Decode("04:spam"), ParseError);
    CHECK_THROWS_AS(Decode("4spam"), ParseError);
    CHECK_THROWS_AS(Decode("99999999999999999999999:x"), ParseError);
}

TEST_CASE("Bencode decodes lists and dictionaries") {
    const Value Root = Decode("d3:bar4:spam3:fooi42e4:listl1:ai1eee");
    REQUIRE(Root.IsDict());
    CHECK(*Root.FindString("bar") == "spam");
    CHECK(*Root.FindInteger("foo") == 42);
    REQUIRE(Root.FindList("list") != nullptr);
    CHECK(Root.FindList("list")->size() == 2);
    CHECK(Root.Find("missing") == nullptr);
    CHECK(Root.FindString("foo") == nullptr);  // wrong type
}

TEST_CASE("Bencode rejects bad structure") {
    CHECK_THROWS_AS(Decode(""), ParseError);
    CHECK_THROWS_AS(Decode("l"), ParseError);
    CHECK_THROWS_AS(Decode("di1ei2ee"), ParseError);          // non-string key
    CHECK_THROWS_AS(Decode("d1:ai1e1:ai2ee"), ParseError);    // duplicate key
    CHECK_THROWS_AS(Decode("i1ei2e"), ParseError);            // trailing data
    CHECK_THROWS_AS(Decode("x"), ParseError);
    CHECK_THROWS_AS(Decode(std::string(1000, 'l') + std::string(1000, 'e')), ParseError);  // too deep
}

TEST_CASE("Bencode accepts unsorted dictionary keys") {
    const Value Root = Decode("d1:bi2e1:ai1ee");
    CHECK(*Root.FindInteger("a") == 1);
    CHECK(Encode(Root) == "d1:ai1e1:bi2ee");
}

TEST_CASE("Bencode records source spans") {
    const std::string Input = "d4:infod1:xi1ee4:name3:abce";
    const Value Root        = Decode(Input);
    const Value* Info       = Root.Find("info");
    REQUIRE(Info != nullptr);
    CHECK(Input.substr(Info->SourceOffset(), Info->SourceLength()) == "d1:xi1ee");
}

TEST_CASE("Bencode DecodePrefix reports consumed length") {
    std::size_t Consumed = 0;
    const Value Root     = DecodePrefix("d1:ai1eeRAWDATA", Consumed);
    CHECK(Consumed == 8);
    CHECK(*Root.FindInteger("a") == 1);
}

TEST_CASE("Bencode round-trips values built in code") {
    const Value Root = Dict {
      {"zeta", List {"x", 1, -2}},
      {"alpha", Dict {{"nested", "value"}}},
      {"empty", ""},
    };
    const std::string Encoded = Encode(Root);
    CHECK(Encoded == "d5:alphad6:nested5:valuee5:empty0:4:zetal1:xi1ei-2eee");
    CHECK(Encode(Decode(Encoded)) == Encoded);
}
