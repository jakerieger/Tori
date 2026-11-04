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
