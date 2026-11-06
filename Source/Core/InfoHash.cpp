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

#include "Core/InfoHash.hpp"

#include "Core/Encoding.hpp"

#include <algorithm>

namespace Tori {
    namespace {
        std::optional<InfoHash> FromBytes(const std::optional<std::vector<std::uint8_t>>& Bytes) {
            if (!Bytes || Bytes->size() != Sha1Digest {}.size()) { return std::nullopt; }
            InfoHash Hash;
            std::ranges::copy(*Bytes, Hash.Bytes.begin());
            return Hash;
        }
    }  // namespace

    std::string InfoHash::ToHex() const {
        return Tori::ToHex(Bytes);
    }

    std::optional<InfoHash> InfoHash::FromHex(const std::string_view Text) {
        if (Text.size() != 40) { return std::nullopt; }
        return FromBytes(Tori::FromHex(Text));
    }

    std::optional<InfoHash> InfoHash::FromBase32(const std::string_view Text) {
        if (Text.size() != 32) { return std::nullopt; }
        return FromBytes(Base32Decode(Text));
    }
}  // namespace Tori
