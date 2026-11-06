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

#include "Core/RateMeter.hpp"

namespace Tori {
    namespace {
        std::int64_t SecondOf(const RateMeter::Clock::time_point Time) {
            return std::chrono::duration_cast<std::chrono::seconds>(Time.time_since_epoch()).count();
        }
    }  // namespace

    void RateMeter::Add(const std::int64_t Bytes, const Clock::time_point Now) {
        const std::int64_t Second = SecondOf(Now);
        Slot& Target              = _Slots[static_cast<std::size_t>(Second % kWindowSeconds)];
        if (Target.Second != Second) { Target = {Second, 0}; }
        Target.Bytes += Bytes;
        _Total += Bytes;
    }

    double RateMeter::Rate(const Clock::time_point Now) const {
        const std::int64_t Second = SecondOf(Now);
        std::int64_t Sum          = 0;
        for (const Slot& Entry : _Slots) {
            if (Entry.Second > Second - kWindowSeconds && Entry.Second <= Second) { Sum += Entry.Bytes; }
        }
        return static_cast<double>(Sum) / kWindowSeconds;
    }
}  // namespace Tori
