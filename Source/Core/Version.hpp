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

#include "ToriVersion.h"

#include <array>
#include <string_view>

namespace Tori {
    inline constexpr int kVersionMajor = TORI_VERSION_MAJOR;
    inline constexpr int kVersionMinor = TORI_VERSION_MINOR;
    inline constexpr int kVersionPatch = TORI_VERSION_PATCH;

    /// @brief This build's version, e.g. "0.1.0", from project() in CMakeLists.txt.
    inline constexpr std::string_view kToriVersion = TORI_VERSION_STRING;

    /// @brief How we name ourselves to peers in the extension handshake ("v").
    inline constexpr std::string_view kClientName = "Tori " TORI_VERSION_STRING;

    namespace Detail {
        /// One Azureus-style version character: 0-9, then A-Z for 10-35.
        constexpr char VersionCharacter(const int Part) {
            return static_cast<char>(Part < 10 ? '0' + Part : 'A' + (Part - 10));
        }
    }  // namespace Detail

    static_assert(kVersionMajor >= 0 && kVersionMajor < 36 && kVersionMinor >= 0 && kVersionMinor < 36 &&
                    kVersionPatch >= 0 && kVersionPatch < 36,
                  "the peer ID has one character (0-9, A-Z) for each version part");

    /// @brief The Azureus-style peer ID prefix: "-TO", one character per version part, a 0, then "-", so 0.1.0 is
    /// "-TO0100-".
    inline constexpr std::array<char, 8> kPeerIdPrefix = {
      '-',
      'T',
      'O',
      Detail::VersionCharacter(kVersionMajor),
      Detail::VersionCharacter(kVersionMinor),
      Detail::VersionCharacter(kVersionPatch),
      '0',
      '-',
    };
}  // namespace Tori
