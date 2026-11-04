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
