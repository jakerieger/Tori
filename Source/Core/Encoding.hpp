#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    /// @brief Lowercase hexadecimal.
    std::string ToHex(std::span<const std::uint8_t> Bytes);

    /// @brief Accepts either case. Returns nullopt on odd length or non-hex characters.
    std::optional<std::vector<std::uint8_t>> FromHex(std::string_view Text);

    /// @brief RFC 4648 base32 with padding.
    std::string Base32Encode(std::span<const std::uint8_t> Bytes);

    /// @brief Accepts either case, with or without padding. Returns nullopt on invalid characters.
    std::optional<std::vector<std::uint8_t>> Base32Decode(std::string_view Text);

    /// @brief Percent-encodes every byte except RFC 3986 unreserved characters.
    std::string PercentEncode(std::string_view Bytes);

    /// @brief Decodes %XX escapes, and '+' as a space when PlusAsSpace is set. Returns nullopt on a bad escape.
    std::optional<std::string> PercentDecode(std::string_view Text, bool PlusAsSpace = false);

    /// @brief ASCII case-insensitive prefix test, for URL schemes and URNs.
    bool StartsWithIgnoreCase(std::string_view Text, std::string_view Prefix);
}  // namespace Tori
