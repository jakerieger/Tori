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
