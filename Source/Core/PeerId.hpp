#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Tori {
    using PeerId = std::array<std::uint8_t, 20>;

    /// @brief Azureus-style ID: "-TO0100-" (Tori 0.1.0.0) followed by 12 random alphanumerics.
    PeerId GeneratePeerId();

    /// @brief Names the client from an Azureus-style peer ID, e.g. "qBittorrent 4.6.5". Unknown clients give their
    /// two-letter code and version; IDs in other styles give an empty string.
    std::string DescribePeerId(const PeerId& Id);
}  // namespace Tori
