#include "Core/Error.hpp"
#include "Core/Sha1.hpp"
#include "Core/Version.hpp"
#include "Peer/Extensions.hpp"
#include "TestUtil.hpp"

#include <doctest/doctest.h>

using namespace Tori;
using namespace Tori::Testing;
using namespace std::chrono_literals;

TEST_CASE("Extension handshake round-trips") {
    const ExtensionHandshake WithSize = DecodeExtensionHandshake(EncodeExtensionHandshake(1234, 6881));
    CHECK(WithSize.UtMetadataId == kLocalUtMetadataId);
    CHECK(WithSize.MetadataSize == 1234);
    CHECK(WithSize.ListenPort == 6881);
    CHECK(WithSize.Client == kClientName);

    const ExtensionHandshake WithoutSize = DecodeExtensionHandshake(EncodeExtensionHandshake(std::nullopt, 1));
    CHECK(WithoutSize.MetadataSize == 0);

    const std::string NotDict = "i5e";
    CHECK_THROWS_AS(DecodeExtensionHandshake(std::span(reinterpret_cast<const std::uint8_t*>(NotDict.data()), NotDict.size())),
                    ParseError);
}

TEST_CASE("ut_metadata messages round-trip") {
    const MetadataMessage Request = DecodeMetadataMessage(EncodeMetadataRequest(3));
    CHECK(Request.Type == MetadataMessageType::Request);
    CHECK(Request.Piece == 3);

    const std::vector<std::uint8_t> Payload = {1, 2, 3, 'e'};  // raw data may look like bencode
    const MetadataMessage Data              = DecodeMetadataMessage(EncodeMetadataData(1, 20000, Payload));
    CHECK(Data.Type == MetadataMessageType::Data);
    CHECK(Data.Piece == 1);
    CHECK(Data.TotalSize == 20000);
    CHECK(Data.Data == Payload);

    CHECK(DecodeMetadataMessage(EncodeMetadataReject(0)).Type == MetadataMessageType::Reject);

    const std::string Bad = "d8:msg_typei7e5:piecei0ee";
    CHECK_THROWS_AS(DecodeMetadataMessage(std::span(reinterpret_cast<const std::uint8_t*>(Bad.data()), Bad.size())),
                    ParseError);
}

TEST_CASE("MetadataAssembler collects, verifies and recovers from bad data") {
    const auto Bytes = RandomBytes(40000, 9);
    const std::string Info(Bytes.begin(), Bytes.end());
    InfoHash Hash;
    Hash.Bytes = Sha1::Hash(Info);

    MetadataAssembler Assembler(Hash);
    CHECK_FALSE(Assembler.OfferSize(kMaxMetadataSize + 1));
    CHECK(Assembler.OfferSize(40000));
    CHECK_FALSE(Assembler.OfferSize(41000));  // a peer that disagrees can't be used
    CHECK(Assembler.OfferSize(40000));

    const auto T0 = MetadataAssembler::Clock::now();
    CHECK(Assembler.NextRequest(T0) == 0);
    CHECK(Assembler.NextRequest(T0) == 1);
    CHECK(Assembler.NextRequest(T0) == 2);
    CHECK_FALSE(Assembler.NextRequest(T0).has_value());
    CHECK(Assembler.NextRequest(T0 + 11s) == 0);  // unanswered requests are retried

    const auto Piece = [&](const int Index) {
        const std::size_t Offset = static_cast<std::size_t>(Index) * kMetadataPieceSize;
        return std::span(Bytes).subspan(Offset, std::min<std::size_t>(kMetadataPieceSize, Bytes.size() - Offset));
    };
    Assembler.Received(0, Piece(0));
    Assembler.Received(1, Piece(1));
    Assembler.Received(2, Piece(1).first(100));  // wrong length: ignored
    CHECK_FALSE(Assembler.TryComplete().has_value());
    Assembler.Received(2, Piece(2));
    CHECK(Assembler.TryComplete() == Info);

    // Corrupt data fails verification and throws everything away, size included
    MetadataAssembler Corrupt(Hash);
    CHECK(Corrupt.OfferSize(40000));
    auto Wrong = Bytes;
    Wrong[5] ^= 1;
    for (int Index = 0; Index < 3; ++Index) {
        const std::size_t Offset = static_cast<std::size_t>(Index) * kMetadataPieceSize;
        Corrupt.Received(Index, std::span(Wrong).subspan(Offset, std::min<std::size_t>(kMetadataPieceSize, Wrong.size() - Offset)));
    }
    CHECK_FALSE(Corrupt.TryComplete().has_value());
    CHECK_FALSE(Corrupt.HasSize());
}
