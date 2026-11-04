#pragma once

#include "Core/InfoHash.hpp"
#include "Core/Sha1.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    struct TorrentFile {
        /// @brief Relative to the download destination and already sanitized. For multi-file torrents this
        /// starts with the torrent's name as the root folder.
        std::filesystem::path Path;
        std::int64_t Length = 0;
        std::int64_t Offset = 0;    // Byte offset of the file within the torrent's concatenated data
        bool IsPadding      = false;  // BEP 47 pad file; occupies piece space but is never written to disk
    };

    /// @brief A parsed BitTorrent v1 .torrent file (BEP 3). Hybrid v1/v2 torrents are read as v1.
    struct Metainfo {
        InfoHash Hash;
        std::string Name;  // UTF-8, sanitized for use as a file or folder name
        bool IsMultiFile         = false;
        std::int64_t PieceLength = 0;
        std::vector<Sha1Digest> PieceHashes;
        std::vector<TorrentFile> Files;
        std::int64_t TotalSize = 0;
        bool IsPrivate         = false;  // BEP 27: no DHT or peer exchange

        std::vector<std::vector<std::string>> TrackerTiers;  // BEP 12; a lone "announce" becomes one tier
        std::vector<std::string> WebSeeds;                   // BEP 19 url-list
        std::string Comment;
        std::string CreatedBy;
        std::optional<std::int64_t> CreationDate;  // Unix time

        /// @brief The info dictionary exactly as it appeared in the source. Hash is its SHA-1, and it is what
        /// ut_metadata serves to peers.
        std::string InfoBytes;

        [[nodiscard]] int PieceCount() const { return static_cast<int>(PieceHashes.size()); }
        [[nodiscard]] std::int64_t PieceSize(int Index) const;

        /// @brief Parses a whole .torrent file. Throws ParseError.
        static Metainfo Parse(std::string_view TorrentData);

        /// @brief Parses a bare info dictionary, as received over ut_metadata for a magnet link. Throws ParseError.
        static Metainfo ParseInfoDictionary(std::string_view InfoBytes);

        /// @brief Reads and parses a .torrent file from disk. Throws ParseError or std::runtime_error.
        static Metainfo Load(const std::filesystem::path& File);

        /// @brief Encodes a .torrent file: InfoBytes unchanged, so the info hash is preserved, plus the trackers,
        /// web seeds and descriptive fields.
        [[nodiscard]] std::string ToTorrentFile() const;
    };

    /// @brief Largest .torrent file Load() will read.
    inline constexpr std::int64_t kMaxTorrentFileSize = 64LL * 1024 * 1024;

    /// @brief Largest piece length accepted. Each piece is buffered in memory while it downloads.
    inline constexpr std::int64_t kMaxPieceLength = 256LL * 1024 * 1024;

    /// @brief Makes one UTF-8 path component from torrent metadata safe to create on Windows. Returns an empty
    /// string for components that should be dropped ("" and "."). Neutralizes "..", characters Windows forbids,
    /// trailing dots and spaces, and reserved device names such as CON or LPT1.
    std::string SanitizePathComponent(std::string_view Component);
}  // namespace Tori
