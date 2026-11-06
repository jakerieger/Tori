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
