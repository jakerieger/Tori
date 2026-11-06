// Copyright 2026 Jake Rieger
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "Core/Sha1.hpp"

#include <compare>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Tori {
    /// @brief BitTorrent v1 info hash: the SHA-1 of a torrent's bencoded info dictionary.
    struct InfoHash {
        Sha1Digest Bytes {};

        [[nodiscard]] std::string ToHex() const;

        /// @brief Parses 40 hex characters.
        static std::optional<InfoHash> FromHex(std::string_view Text);

        /// @brief Parses 32 base32 characters (the older magnet link form).
        static std::optional<InfoHash> FromBase32(std::string_view Text);

        auto operator<=>(const InfoHash&) const = default;
    };
}  // namespace Tori

template<>
struct std::hash<Tori::InfoHash> {
    std::size_t operator()(const Tori::InfoHash& Hash) const noexcept {
        // Already uniformly distributed, so any slice of it makes a good hash
        std::size_t Value = 0;
        std::memcpy(&Value, Hash.Bytes.data(), sizeof(Value));
        return Value;
    }
};
