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

#include "Core/Encoding.hpp"

namespace Tori {
    namespace {
        constexpr char kHexDigits[]      = "0123456789abcdef";
        constexpr char kUpperHexDigits[] = "0123456789ABCDEF";  // RFC 3986 recommends uppercase escapes
        constexpr char kBase32Digits[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

        int HexValue(const char C) {
            if (C >= '0' && C <= '9') { return C - '0'; }
            if (C >= 'a' && C <= 'f') { return C - 'a' + 10; }
            if (C >= 'A' && C <= 'F') { return C - 'A' + 10; }
            return -1;
        }

        int Base32Value(const char C) {
            if (C >= 'A' && C <= 'Z') { return C - 'A'; }
            if (C >= 'a' && C <= 'z') { return C - 'a'; }
            if (C >= '2' && C <= '7') { return C - '2' + 26; }
            return -1;
        }

        bool IsUnreserved(const unsigned char C) {
            return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '-' || C == '.' ||
                   C == '_' || C == '~';
        }
    }  // namespace

    std::string ToHex(const std::span<const std::uint8_t> Bytes) {
        std::string Out;
        Out.reserve(Bytes.size() * 2);
        for (const std::uint8_t Byte : Bytes) {
            Out += kHexDigits[Byte >> 4];
            Out += kHexDigits[Byte & 0x0F];
        }
        return Out;
    }

    std::optional<std::vector<std::uint8_t>> FromHex(const std::string_view Text) {
        if (Text.size() % 2 != 0) { return std::nullopt; }

        std::vector<std::uint8_t> Out;
        Out.reserve(Text.size() / 2);
        for (std::size_t I = 0; I < Text.size(); I += 2) {
            const int High = HexValue(Text[I]);
            const int Low  = HexValue(Text[I + 1]);
            if (High < 0 || Low < 0) { return std::nullopt; }
            Out.push_back(static_cast<std::uint8_t>((High << 4) | Low));
        }
        return Out;
    }

    std::string Base32Encode(const std::span<const std::uint8_t> Bytes) {
        std::string Out;
        std::uint32_t Buffer = 0;
        int Bits             = 0;
        for (const std::uint8_t Byte : Bytes) {
            Buffer = (Buffer << 8) | Byte;
            Bits += 8;
            while (Bits >= 5) {
                Out += kBase32Digits[(Buffer >> (Bits - 5)) & 0x1F];
                Bits -= 5;
            }
        }
        if (Bits > 0) { Out += kBase32Digits[(Buffer << (5 - Bits)) & 0x1F]; }
        while (Out.size() % 8 != 0) {
            Out += '=';
        }
        return Out;
    }

    std::optional<std::vector<std::uint8_t>> Base32Decode(std::string_view Text) {
        while (!Text.empty() && Text.back() == '=') {
            Text.remove_suffix(1);
        }

        std::vector<std::uint8_t> Out;
        std::uint32_t Buffer = 0;
        int Bits             = 0;
        for (const char C : Text) {
            const int Digit = Base32Value(C);
            if (Digit < 0) { return std::nullopt; }
            Buffer = (Buffer << 5) | static_cast<std::uint32_t>(Digit);
            Bits += 5;
            if (Bits >= 8) {
                Out.push_back(static_cast<std::uint8_t>(Buffer >> (Bits - 8)));
                Bits -= 8;
            }
        }
        return Out;
    }

    std::string PercentEncode(const std::string_view Bytes) {
        std::string Out;
        for (const char C : Bytes) {
            const auto Byte = static_cast<unsigned char>(C);
            if (IsUnreserved(Byte)) {
                Out += C;
            } else {
                Out += '%';
                Out += kUpperHexDigits[Byte >> 4];
                Out += kUpperHexDigits[Byte & 0x0F];
            }
        }
        return Out;
    }

    std::optional<std::string> PercentDecode(const std::string_view Text, const bool PlusAsSpace) {
        std::string Out;
        Out.reserve(Text.size());
        for (std::size_t I = 0; I < Text.size(); ++I) {
            const char C = Text[I];
            if (C == '%') {
                if (I + 2 >= Text.size()) { return std::nullopt; }
                const int High = HexValue(Text[I + 1]);
                const int Low  = HexValue(Text[I + 2]);
                if (High < 0 || Low < 0) { return std::nullopt; }
                Out += static_cast<char>((High << 4) | Low);
                I += 2;
            } else if (C == '+' && PlusAsSpace) {
                Out += ' ';
            } else {
                Out += C;
            }
        }
        return Out;
    }

    bool StartsWithIgnoreCase(const std::string_view Text, const std::string_view Prefix) {
        if (Text.size() < Prefix.size()) { return false; }
        const auto Lower = [](const char C) { return C >= 'A' && C <= 'Z' ? static_cast<char>(C + 32) : C; };
        for (std::size_t I = 0; I < Prefix.size(); ++I) {
            if (Lower(Text[I]) != Lower(Prefix[I])) { return false; }
        }
        return true;
    }
}  // namespace Tori
