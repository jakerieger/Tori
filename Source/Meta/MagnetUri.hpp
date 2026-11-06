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
