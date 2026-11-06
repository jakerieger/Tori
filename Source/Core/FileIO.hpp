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
