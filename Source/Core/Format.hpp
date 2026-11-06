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

#include <string>

namespace Tori {
    /// @brief Formats a byte count using binary units, e.g. "1.50 MB".
    std::string FormatBytes(double Bytes);

    /// @brief Formats a transfer rate in bytes per second, e.g. "4.20 MB/s".
    std::string FormatRate(double BytesPerSecond);
}  // namespace Tori
