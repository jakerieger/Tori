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

#include <array>
#include <cstdint>
#include <string>

namespace Tori {
    using PeerId = std::array<std::uint8_t, 20>;

    /// @brief Azureus-style ID: kPeerIdPrefix ("-TO0100-" for Tori 0.1.0) followed by 12 random alphanumerics.
    PeerId GeneratePeerId();

    /// @brief Names the client from an Azureus-style peer ID, e.g. "qBittorrent 4.6.5". Unknown clients give their
    /// two-letter code and version; IDs in other styles give an empty string.
    std::string DescribePeerId(const PeerId& Id);
}  // namespace Tori
