#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Tori {
    /// @brief One bit per piece, stored in the BEP 3 wire format: most significant bit first, spare bits zero.
    class Bitfield {
    public:
        Bitfield() = default;
        explicit Bitfield(int Size);

        [[nodiscard]] int Size() const { return _Size; }
        [[nodiscard]] int Count() const { return _Count; }
        [[nodiscard]] bool All() const { return _Count == _Size; }
        [[nodiscard]] bool None() const { return _Count == 0; }

        [[nodiscard]] bool Get(int Index) const;
        void Set(int Index, bool Value = true);

        [[nodiscard]] const std::vector<std::uint8_t>& Bytes() const { return _Bytes; }

        /// @brief Parses a wire-format bitfield. Returns nullopt when the length is wrong or spare bits are set,
        /// both of which BEP 3 says should drop the peer.
        static std::optional<Bitfield> FromBytes(std::span<const std::uint8_t> Bytes, int Size);

        bool operator==(const Bitfield&) const = default;

    private:
        std::vector<std::uint8_t> _Bytes;
        int _Size  = 0;
        int _Count = 0;
    };
}  // namespace Tori
