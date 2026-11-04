#pragma once

#include <filesystem>
#include <string>

namespace Tori {
    /// @brief The system's description of a Win32 error code, in UTF-8.
    std::string Win32ErrorMessage(unsigned long Code);

    /// @brief Absolute path with the \\?\ prefix, so torrents with deep folder trees aren't limited to MAX_PATH.
    std::wstring LongPath(const std::filesystem::path& Path);
}  // namespace Tori
