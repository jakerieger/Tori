#include "Core/Error.hpp"
#include "Core/RateMeter.hpp"
#include "Engine/PiecePicker.hpp"
#include "Peer/Messages.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace Tori;
using namespace std::chrono_literals;

namespace {
    std::vector<std::uint8_t> Frame(const auto& Encode) {
        ByteWriter Out;
        Encode(Out);
        return Out.Take();
    }

    Bitfield AllOf(const int Size) {
        Bitfield Bits(Size);
        for (int I = 0; I < Size; ++I) { Bits.Set(I); }
        return Bits;
    }

    Bitfield Only(const int Size, std::initializer_list<int> Pieces) {
        Bitfield Bits(Size);
        for (const int Piece : Pieces) { Bits.Set(Piece); }
        return Bits;
    }
}  // namespace

TEST_CASE("Handshake round-trips and rejects other protocols") {
    Handshake Original;
    Original.Reserved[5] = 0x10;
    Original.Hash.Bytes.fill(0xAB);
    Original.Id.fill('x');

    const std::vector<std::uint8_t> Bytes = EncodeHandshake(Original);
    REQUIRE(Bytes.size() == kHandshakeSize);
    CHECK(Bytes[0] == 19);

    const auto Decoded = DecodeHandshake(Bytes);
    REQUIRE(Decoded.has_value());
    CHECK(Decoded->Reserved == Original.Reserved);
    CHECK(Decoded->Hash == Original.Hash);
    CHECK(Decoded->Id == Original.Id);

    std::vector<std::uint8_t> Wrong = Bytes;
    Wrong[1] = 'X';
    CHECK_FALSE(DecodeHandshake(Wrong).has_value());
    CHECK_FALSE(DecodeHandshake(std::span(Bytes).first(67)).has_value());
}

TEST_CASE("MessageParser reassembles messages fed a byte at a time") {
    std::vector<std::uint8_t> Stream;
    const auto Append = [&](const std::vector<std::uint8_t>& Bytes) { Stream.insert(Stream.end(), Bytes.begin(), Bytes.end()); };
    Append(Frame([](ByteWriter& Out) { EncodeKeepAlive(Out); }));
    Append(Frame([](ByteWriter& Out) { EncodeSimple(Out, MessageId::Unchoke); }));
    Append(Frame([](ByteWriter& Out) { EncodeHave(Out, 7); }));
    Append(Frame([](ByteWriter& Out) { EncodeBitfield(Out, std::vector<std::uint8_t> {0xF0}); }));
    Append(Frame([](ByteWriter& Out) { EncodeRequest(Out, {1, 16384, 16384}); }));
    Append(Frame([](ByteWriter& Out) { EncodePiece(Out, 2, 0, std::vector<std::uint8_t> {9, 8, 7}); }));
    Append(Frame([](ByteWriter& Out) { EncodeExtended(Out, 3, std::vector<std::uint8_t> {'d', 'e'}); }));
    Append({0, 0, 0, 2, 99, 0});  // unknown message id: skipped
    Append(Frame([](ByteWriter& Out) { EncodeCancel(Out, {1, 16384, 16384}); }));

    MessageParser Parser;
    std::vector<PeerMessage> Messages;
    for (const std::uint8_t Byte : Stream) {
        Parser.Feed(std::span(&Byte, 1));
        while (auto Message = Parser.Next()) { Messages.push_back(std::move(*Message)); }
    }

    REQUIRE(Messages.size() == 8);
    CHECK(Messages[0].IsKeepAlive);
    CHECK(Messages[1].Id == MessageId::Unchoke);
    CHECK((Messages[2].Id == MessageId::Have && Messages[2].Index == 7));
    CHECK(Messages[3].Payload == std::vector<std::uint8_t> {0xF0});
    CHECK((Messages[4].Id == MessageId::Request && Messages[4].Index == 1 && Messages[4].Begin == 16384 &&
           Messages[4].Length == 16384));
    CHECK((Messages[5].Id == MessageId::Piece && Messages[5].Index == 2));
    CHECK(Messages[5].Payload == std::vector<std::uint8_t> {9, 8, 7});
    CHECK((Messages[6].Id == MessageId::Extended && Messages[6].ExtendedId == 3));
    CHECK(Messages[7].Id == MessageId::Cancel);
    CHECK(Parser.Buffered() == 0);
}

TEST_CASE("MessageParser rejects oversized and malformed messages") {
    MessageParser Oversized;
    const std::vector<std::uint8_t> Huge = {0x10, 0, 0, 0};
    Oversized.Feed(Huge);
    CHECK_THROWS_AS(Oversized.Next(), ParseError);

    MessageParser BadHave;
    const std::vector<std::uint8_t> ShortHave = {0, 0, 0, 3, 4, 0, 1};
    BadHave.Feed(ShortHave);
    CHECK_THROWS_AS(BadHave.Next(), ParseError);
}

TEST_CASE("RateMeter averages over its window") {
    const auto T0 = RateMeter::Clock::time_point {} + 1000s;
    RateMeter Meter;
    Meter.Add(5000, T0);
    Meter.Add(5000, T0 + 1s);
    CHECK(Meter.Rate(T0 + 1s) == doctest::Approx(2000.0));
    CHECK(Meter.Total() == 10000);
    CHECK(Meter.Rate(T0 + 10s) == 0.0);
}

TEST_CASE("PiecePicker splits pieces into blocks") {
    const PiecePicker Picker(3, 40000, 90000);
    CHECK(Picker.BlocksInPiece(0) == 3);
    CHECK(Picker.BlockLength(0, 2) == 40000 - 2 * kBlockSize);
    CHECK(Picker.PieceSize(2) == 10000);
    CHECK(Picker.BlocksInPiece(2) == 1);
    CHECK(Picker.BlockLength(2, 0) == 10000);
}

TEST_CASE("PiecePicker picks the rarest piece first") {
    PiecePicker Picker(4, kBlockSize, 4 * kBlockSize);
    Picker.AddPeer(AllOf(4));
    Picker.AddPeer(Only(4, {0, 1, 3}));
    Picker.AddPeer(Only(4, {0, 3}));
    // Availability: piece 2 = 1, piece 1 = 2, pieces 0 and 3 = 3

    const auto First = Picker.Pick(AllOf(4), 1, {});
    REQUIRE(First.size() == 1);
    CHECK(First[0].Piece == 2);

    const auto Second = Picker.Pick(AllOf(4), 1, {});
    REQUIRE(Second.size() == 1);
    CHECK(Second[0].Piece == 1);
}

TEST_CASE("PiecePicker finishes partial pieces before starting new ones") {
    PiecePicker Picker(3, 4 * kBlockSize, 12 * kBlockSize);
    Picker.AddPeer(AllOf(3));

    const auto Started = Picker.Pick(AllOf(3), 2, {});
    REQUIRE(Started.size() == 2);
    const int Piece = Started[0].Piece;
    CHECK(Started[1].Piece == Piece);

    const auto Next = Picker.Pick(AllOf(3), 2, {});
    REQUIRE(Next.size() == 2);
    CHECK(Next[0].Piece == Piece);
    CHECK(Next[1].Piece == Piece);
    CHECK(Picker.PartialCount() == 1);
}

TEST_CASE("PiecePicker tracks received blocks, failures and aborts") {
    PiecePicker Picker(1, 2 * kBlockSize, 2 * kBlockSize);
    Picker.AddPeer(AllOf(1));

    const auto Blocks = Picker.Pick(AllOf(1), 10, {});
    REQUIRE(Blocks.size() == 2);

    CHECK(Picker.Received({0, 0, 100}) == PiecePicker::BlockResult::Ignored);  // wrong length
    CHECK(Picker.Received(Blocks[0]) == PiecePicker::BlockResult::Accepted);
    CHECK(Picker.Received(Blocks[0]) == PiecePicker::BlockResult::Ignored);  // duplicate
    CHECK(Picker.Received(Blocks[1]) == PiecePicker::BlockResult::PieceComplete);

    // Hash failure: everything is handed out again
    Picker.PieceFailed(0);
    CHECK(Picker.Pick(AllOf(1), 10, {}).size() == 2);

    // Aborted requests become available to the next peer
    Picker.Abort(Blocks[0]);
    const auto Retry = Picker.Pick(AllOf(1), 10, {});
    REQUIRE(Retry.size() == 1);
    CHECK(Retry[0] == Blocks[0]);

    Picker.PieceVerified(0);
    CHECK(Picker.Have().All());
    CHECK(Picker.Pick(AllOf(1), 10, {}).empty());
}

TEST_CASE("PiecePicker endgame duplicates requests, never to the same peer twice") {
    PiecePicker Picker(1, 2 * kBlockSize, 2 * kBlockSize);
    Picker.AddPeer(AllOf(1));
    Picker.AddPeer(AllOf(1));

    const auto FirstPeer = Picker.Pick(AllOf(1), 10, {});
    REQUIRE(FirstPeer.size() == 2);

    // Everything is requested, so a second peer gets duplicates...
    const auto SecondPeer = Picker.Pick(AllOf(1), 10, {});
    CHECK(SecondPeer.size() == 2);

    // ...but the first peer is never asked for what it already has pending
    CHECK(Picker.Pick(AllOf(1), 10, FirstPeer).empty());

    // A peer without the piece gets nothing
    CHECK(Picker.Pick(Bitfield(1), 10, {}).empty());
}

TEST_CASE("PiecePicker only picks wanted pieces") {
    PiecePicker Picker(4, kBlockSize, 4 * kBlockSize);
    Picker.AddPeer(AllOf(4));
    Picker.SetWanted(Only(4, {1, 3}));
    CHECK(Picker.Needs(1));
    CHECK_FALSE(Picker.Needs(0));

    std::vector<int> Picked;
    for (const BlockRequest& Block : Picker.Pick(AllOf(4), 10, {})) { Picked.push_back(Block.Piece); }
    std::ranges::sort(Picked);
    CHECK(Picked == std::vector<int> {1, 3});

    CHECK_FALSE(Picker.WantedComplete());
    for (const int Piece : {1, 3}) {
        CHECK(Picker.Received({Piece, 0, kBlockSize}) == PiecePicker::BlockResult::PieceComplete);
        Picker.PieceVerified(Piece);
    }
    CHECK(Picker.WantedComplete());
    CHECK_FALSE(Picker.Have().All());
}
