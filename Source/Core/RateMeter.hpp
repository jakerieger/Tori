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

#include <array>
#include <chrono>
#include <cstdint>

namespace Tori {
    /// @brief Bytes per second averaged over the last few whole seconds.
    class RateMeter {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr int kWindowSeconds = 5;

        void Add(std::int64_t Bytes, Clock::time_point Now);
        [[nodiscard]] double Rate(Clock::time_point Now) const;
        [[nodiscard]] std::int64_t Total() const { return _Total; }

    private:
        struct Slot {
            std::int64_t Second = -1;
            std::int64_t Bytes  = 0;
        };

        std::array<Slot, kWindowSeconds> _Slots {};
        std::int64_t _Total = 0;
    };
}  // namespace Tori
