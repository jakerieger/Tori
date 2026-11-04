#pragma once

#include "Core/InfoHash.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Tori {
    /// @brief Handshake reserved bits we set and look for.
    inline constexpr std::size_t kExtensionReservedByte = 5;
    inline constexpr std::uint8_t kExtensionReservedBit = 0x10;  // BEP 10
    inline constexpr std::size_t kDhtReservedByte       = 7;
    inline constexpr std::uint8_t kDhtReservedBit       = 0x01;  // BEP 5

    /// @brief Extended message 0 is always the extension handshake.
    inline constexpr std::uint8_t kExtensionHandshakeId = 0;

    /// @brief The ID we ask peers to use when sending us ut_metadata messages.
    inline constexpr std::uint8_t kLocalUtMetadataId = 1;

    inline constexpr int kMetadataPieceSize          = 16 * 1024;
    inline constexpr std::int64_t kMaxMetadataSize   = 16 * 1024 * 1024;

    /// @brief What a peer told us in its BEP 10 handshake.
    struct ExtensionHandshake {
        std::uint8_t UtMetadataId = 0;  // The peer's ID for ut_metadata; 0 if it doesn't support it
        std::int64_t MetadataSize = 0;  // 0 if unknown
        std::uint16_t ListenPort  = 0;
        std::string Client;
    };

    /// @brief Our handshake. MetadataSize is the size of the info dictionary, when we have it.
    std::vector<std::uint8_t> EncodeExtensionHandshake(std::optional<std::int64_t> MetadataSize, std::uint16_t ListenPort);

    /// @brief Throws ParseError if the payload isn't a bencoded dictionary.
    ExtensionHandshake DecodeExtensionHandshake(std::span<const std::uint8_t> Payload);

    /// @brief BEP 9 messages, carried in extended messages.
    enum class MetadataMessageType {
        Request = 0,
        Data    = 1,
        Reject  = 2,
    };

    struct MetadataMessage {
        MetadataMessageType Type = MetadataMessageType::Request;
        int Piece                = 0;
        std::int64_t TotalSize   = 0;  // Data only
        std::vector<std::uint8_t> Data;
    };

    std::vector<std::uint8_t> EncodeMetadataRequest(int Piece);
    std::vector<std::uint8_t> EncodeMetadataData(int Piece, std::int64_t TotalSize, std::span<const std::uint8_t> Data);
    std::vector<std::uint8_t> EncodeMetadataReject(int Piece);

    /// @brief Throws ParseError on a malformed message.
    MetadataMessage DecodeMetadataMessage(std::span<const std::uint8_t> Payload);

    /// @brief Number of 16 KiB pieces in metadata of the given size.
    int MetadataPieceCount(std::int64_t Size);

    /// @brief Reassembles a torrent's info dictionary from ut_metadata pieces and checks it against the info hash.
    class MetadataAssembler {
    public:
        using Clock = std::chrono::steady_clock;

        explicit MetadataAssembler(InfoHash Hash) : _Hash(Hash) {}

        [[nodiscard]] bool HasSize() const { return _Size > 0; }
        [[nodiscard]] std::int64_t Size() const { return _Size; }

        /// @brief Adopts the size a peer reported, unless one is already set or it's implausible. Returns whether
        /// Size() now equals it, i.e. whether that peer can be asked for pieces.
        bool OfferSize(std::int64_t Size);

        /// @brief A piece nobody has been asked for recently, marked as requested now.
        std::optional<int> NextRequest(Clock::time_point Now);

        void Received(int Piece, std::span<const std::uint8_t> Data);
        void Rejected(int Piece);

        /// @brief Once every piece is in, verifies the result. On a hash mismatch everything is discarded,
        /// including the size, since a peer may have lied about it.
        std::optional<std::string> TryComplete();

    private:
        InfoHash _Hash;
        std::int64_t _Size = 0;
        std::string _Buffer;
        std::vector<bool> _Have;
        std::vector<Clock::time_point> _RequestedAt;
    };
}  // namespace Tori
