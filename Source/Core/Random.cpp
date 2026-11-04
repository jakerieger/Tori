#include "Core/Random.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <stdexcept>

namespace Tori {
    void RandomBytes(const std::span<std::uint8_t> Out) {
        if (Out.empty()) { return; }
        const NTSTATUS Status =
          BCryptGenRandom(nullptr, Out.data(), static_cast<ULONG>(Out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(Status)) { throw std::runtime_error("BCryptGenRandom failed"); }
    }

    std::uint32_t RandomU32() {
        std::uint32_t Value = 0;
        RandomBytes({reinterpret_cast<std::uint8_t*>(&Value), sizeof(Value)});
        return Value;
    }
}  // namespace Tori
