#pragma once

#include <filesystem>

namespace Tori::UI {
    /// @brief On a crash, a failed debug assertion or an uncaught exception, appends the reason and a symbolized
    /// stack trace to File before the process ends.
    void InstallCrashLog(const std::filesystem::path& File);
}  // namespace Tori::UI
