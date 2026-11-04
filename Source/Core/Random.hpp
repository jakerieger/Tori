#pragma once

#include <cstdint>
#include <span>

namespace Tori {
    /// @brief Fills Out from the system CSPRNG.
    void RandomBytes(std::span<std::uint8_t> Out);

    std::uint32_t RandomU32();
}  // namespace Tori
