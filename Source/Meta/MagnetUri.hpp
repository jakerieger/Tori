#pragma once

#include "Core/InfoHash.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    /// @brief A magnet link (BEP 9) carrying a BitTorrent v1 info hash.
    struct MagnetUri {
        InfoHash Hash;
        std::string DisplayName;            // dn
        std::vector<std::string> Trackers;  // tr
        std::vector<std::string> Peers;     // x.pe, as "host:port"
        std::vector<std::string> WebSeeds;  // ws

        /// @brief Throws ParseError when the link is malformed or has no urn:btih hash.
        static MagnetUri Parse(std::string_view Uri);

        [[nodiscard]] std::string ToString() const;
    };
}  // namespace Tori
