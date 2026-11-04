#include "Core/Bitfield.hpp"

#include <bit>
#include <cassert>

namespace Tori {
    namespace {
        std::size_t ByteCount(const int Size) {
            return (static_cast<std::size_t>(Size) + 7) / 8;
        }

        std::uint8_t Mask(const int Index) {
            return static_cast<std::uint8_t>(0x80u >> (Index % 8));
        }
    }  // namespace

    Bitfield::Bitfield(const int Size) : _Bytes(ByteCount(Size), 0), _Size(Size) {}

    bool Bitfield::Get(const int Index) const {
        assert(Index >= 0 && Index < _Size);
        return (_Bytes[static_cast<std::size_t>(Index / 8)] & Mask(Index)) != 0;
    }

    void Bitfield::Set(const int Index, const bool Value) {
        assert(Index >= 0 && Index < _Size);
        if (Get(Index) == Value) { return; }
        std::uint8_t& Byte = _Bytes[static_cast<std::size_t>(Index / 8)];
        if (Value) {
            Byte |= Mask(Index);
            ++_Count;
        } else {
            Byte &= static_cast<std::uint8_t>(~Mask(Index));
            --_Count;
        }
    }

    std::optional<Bitfield> Bitfield::FromBytes(const std::span<const std::uint8_t> Bytes, const int Size) {
        if (Size < 0 || Bytes.size() != ByteCount(Size)) { return std::nullopt; }

        const int SpareBits = static_cast<int>(Bytes.size() * 8) - Size;
        if (SpareBits > 0 && (Bytes.back() & ((1u << SpareBits) - 1)) != 0) { return std::nullopt; }

        Bitfield Result(Size);
        Result._Bytes.assign(Bytes.begin(), Bytes.end());
        for (const std::uint8_t Byte : Bytes) { Result._Count += std::popcount(Byte); }
        return Result;
    }
}  // namespace Tori
