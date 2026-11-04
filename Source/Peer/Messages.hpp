#pragma once

#include "Core/ByteIO.hpp"
#include "Core/InfoHash.hpp"
#include "Core/PeerId.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Tori {
    /// @brief The unit pieces are requested in. Every client accepts 16 KiB; many reject anything larger.
    inline constexpr int kBlockSize = 16 * 1024;

    /// @brief Largest request we serve. Larger ones get the peer disconnected.
    inline constexpr std::uint32_t kMaxRequestLength = 128 * 1024;

    inline constexpr std::size_t kHandshakeSize = 68;

    /// @brief Largest message accepted: a bitfield for ~8 million pieces, or a 128 KiB block with headroom.
    inline constexpr std::uint32_t kMaxMessageSize = 1024 * 1024;

    enum class MessageId : std::uint8_t {
        Choke         = 0,
        Unchoke       = 1,
        Interested    = 2,
        NotInterested = 3,
        Have          = 4,
        Bitfield      = 5,
        Request       = 6,
        Piece         = 7,
        Cancel        = 8,
        Port          = 9,   // BEP 5: the peer's DHT port
        Extended      = 20,  // BEP 10
    };

    struct BlockRequest {
        int Piece  = 0;
        int Offset = 0;
        int Length = 0;

        auto operator<=>(const BlockRequest&) const = default;
    };

    struct Handshake {
        std::array<std::uint8_t, 8> Reserved {};
        InfoHash Hash;
        PeerId Id {};
    };

    std::vector<std::uint8_t> EncodeHandshake(const Handshake& Value);

    /// @brief Returns nullopt unless Data is exactly a BitTorrent protocol handshake.
    std::optional<Handshake> DecodeHandshake(std::span<const std::uint8_t> Data);

    struct PeerMessage {
        bool IsKeepAlive     = false;
        MessageId Id         = MessageId::Choke;
        std::uint32_t Index  = 0;  // Have, Request, Cancel, Piece
        std::uint32_t Begin  = 0;  // Request, Cancel, Piece
        std::uint32_t Length = 0;  // Request, Cancel
        std::uint16_t Port   = 0;  // Port
        std::uint8_t ExtendedId = 0;
        std::vector<std::uint8_t> Payload;  // Bitfield bytes, block data, or extended message body
    };

    /// @brief Each appends one complete length-prefixed message.
    void EncodeKeepAlive(ByteWriter& Out);
    void EncodeSimple(ByteWriter& Out, MessageId Id);  // Choke, Unchoke, Interested, NotInterested
    void EncodeHave(ByteWriter& Out, std::uint32_t Piece);
    void EncodeBitfield(ByteWriter& Out, std::span<const std::uint8_t> Bits);
    void EncodeRequest(ByteWriter& Out, const BlockRequest& Block);
    void EncodeCancel(ByteWriter& Out, const BlockRequest& Block);
    void EncodePiece(ByteWriter& Out, std::uint32_t Piece, std::uint32_t Begin, std::span<const std::uint8_t> Data);
    void EncodeExtended(ByteWriter& Out, std::uint8_t ExtendedId, std::span<const std::uint8_t> Payload);
    void EncodePort(ByteWriter& Out, std::uint16_t DhtPort);

    /// @brief Splits the byte stream after the handshake into messages. Unknown message IDs are skipped.
    class MessageParser {
    public:
        void Feed(std::span<const std::uint8_t> Data);

        /// @brief The next complete message, or nullopt until more data arrives. Throws ParseError when a message
        /// is oversized or has the wrong length for its type.
        std::optional<PeerMessage> Next();

        [[nodiscard]] std::size_t Buffered() const { return _Buffer.size() - _Start; }

    private:
        std::vector<std::uint8_t> _Buffer;
        std::size_t _Start = 0;
    };
}  // namespace Tori
