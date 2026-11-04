#pragma once

#include <string>

namespace Tori {
    /// @brief Formats a byte count using binary units, e.g. "1.50 MB".
    std::string FormatBytes(double Bytes);

    /// @brief Formats a transfer rate in bytes per second, e.g. "4.20 MB/s".
    std::string FormatRate(double BytesPerSecond);
}  // namespace Tori
