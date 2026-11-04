#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Tori {
    /// @brief Reads a whole file. Returns nullopt when it doesn't exist, can't be read, or is larger than MaxSize.
    std::optional<std::string> ReadWholeFile(const std::filesystem::path& File, std::int64_t MaxSize);

    /// @brief Replaces File's contents in one step: writes a temporary file beside it, flushes it, then renames it
    /// over the original, so a crash leaves either the old or the new contents. Throws std::runtime_error.
    void WriteFileAtomically(const std::filesystem::path& File, std::string_view Contents);
}  // namespace Tori
