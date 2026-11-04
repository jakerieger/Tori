#include "Core/Format.hpp"

#include <format>

namespace Tori {
    std::string FormatBytes(double Bytes) {
        constexpr double KB = 1024.0;
        constexpr double MB = KB * 1024.0;
        constexpr double GB = MB * 1024.0;
        constexpr double TB = GB * 1024.0;

        if (Bytes < KB) { return std::format("{:.0f} B", Bytes); }
        if (Bytes < MB) { return std::format("{:.2f} KB", Bytes / KB); }
        if (Bytes < GB) { return std::format("{:.2f} MB", Bytes / MB); }
        if (Bytes < TB) { return std::format("{:.2f} GB", Bytes / GB); }
        return std::format("{:.2f} TB", Bytes / TB);
    }

    std::string FormatRate(double BytesPerSecond) {
        return FormatBytes(BytesPerSecond) + "/s";
    }
}  // namespace Tori
