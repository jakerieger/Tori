#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    /// @brief An IPv4 or IPv6 address and port.
    struct Endpoint {
        std::array<std::uint8_t, 16> Address {};  // IPv4 uses the first 4 bytes
        std::uint16_t Port = 0;
        bool IsV6          = false;

        /// @brief "1.2.3.4" or "2001:db8::1".
        [[nodiscard]] std::string AddressString() const;

        /// @brief "1.2.3.4:6881" or "[2001:db8::1]:6881".
        [[nodiscard]] std::string ToString() const;

        /// @brief Converts an IPv4-mapped IPv6 address (::ffff:1.2.3.4, as a dual-stack listener reports IPv4
        /// clients) to plain IPv4. Other addresses are returned unchanged.
        [[nodiscard]] Endpoint Unmapped() const;

        /// @brief Builds an endpoint from a literal IP address. Host names are rejected.
        static std::optional<Endpoint> FromAddress(std::string_view Ip, std::uint16_t Port);

        /// @brief Parses "1.2.3.4:6881" or "[::1]:6881". Host names and port 0 are rejected.
        static std::optional<Endpoint> Parse(std::string_view HostAndPort);

        auto operator<=>(const Endpoint&) const = default;
    };

    /// @brief Parses packed peers: 6 bytes each for IPv4, 18 for IPv6. Entries with port 0 and any trailing
    /// partial entry are skipped.
    std::vector<Endpoint> ParseCompactPeers(std::string_view Data, bool IsV6);
}  // namespace Tori
