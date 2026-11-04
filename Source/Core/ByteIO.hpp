#pragma once

#include "Core/Error.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Tori {
    /// @brief Appends big-endian (network order) integers and raw bytes.
    class ByteWriter {
    public:
        void WriteU8(const std::uint8_t Value) { _Data.push_back(Value); }
        void WriteU16(const std::uint16_t Value) { WriteBigEndian(Value, 2); }
        void WriteU32(const std::uint32_t Value) { WriteBigEndian(Value, 4); }
        void WriteU64(const std::uint64_t Value) { WriteBigEndian(Value, 8); }

        void WriteBytes(const std::span<const std::uint8_t> Bytes) { _Data.insert(_Data.end(), Bytes.begin(), Bytes.end()); }
        void WriteBytes(const std::string_view Bytes) { _Data.insert(_Data.end(), Bytes.begin(), Bytes.end()); }

        [[nodiscard]] const std::vector<std::uint8_t>& Data() const { return _Data; }
        [[nodiscard]] std::vector<std::uint8_t> Take() { return std::move(_Data); }

    private:
        void WriteBigEndian(const std::uint64_t Value, const int Bytes) {
            for (int Shift = (Bytes - 1) * 8; Shift >= 0; Shift -= 8) {
                _Data.push_back(static_cast<std::uint8_t>(Value >> Shift));
            }
        }

        std::vector<std::uint8_t> _Data;
    };

    /// @brief Reads big-endian integers and raw bytes. Throws ParseError when reading past the end.
    class ByteReader {
    public:
        explicit ByteReader(const std::span<const std::uint8_t> Data) : _Data(Data) {}

        [[nodiscard]] std::size_t Remaining() const { return _Data.size() - _Pos; }

        std::uint8_t ReadU8() { return static_cast<std::uint8_t>(ReadBigEndian(1)); }
        std::uint16_t ReadU16() { return static_cast<std::uint16_t>(ReadBigEndian(2)); }
        std::uint32_t ReadU32() { return static_cast<std::uint32_t>(ReadBigEndian(4)); }
        std::uint64_t ReadU64() { return ReadBigEndian(8); }

        std::span<const std::uint8_t> ReadBytes(const std::size_t Count) {
            Require(Count);
            const auto Bytes = _Data.subspan(_Pos, Count);
            _Pos += Count;
            return Bytes;
        }

        std::span<const std::uint8_t> ReadRest() { return ReadBytes(Remaining()); }

    private:
        void Require(const std::size_t Count) const {
            if (Count > Remaining()) { throw ParseError("unexpected end of message"); }
        }

        std::uint64_t ReadBigEndian(const std::size_t Bytes) {
            Require(Bytes);
            std::uint64_t Value = 0;
            for (std::size_t I = 0; I < Bytes; ++I) { Value = (Value << 8) | _Data[_Pos++]; }
            return Value;
        }

        std::span<const std::uint8_t> _Data;
        std::size_t _Pos = 0;
    };
}  // namespace Tori
