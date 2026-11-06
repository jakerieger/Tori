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

#include "Core/Version.hpp"
#include "UI/Model.hpp"

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace Tori::UI {
    enum class UpdateStatus {
        UpdateAvailable,
        UpToDate,
        DevelopmentBuild,
        Error,
    };

    /// @brief Update info fetched from server.
    struct UpdateInfo {
        UpdateStatus Status;
        std::string Version;      // e.g. "0.2.0"
        std::string DownloadUrl;  // Where to get it
        std::string Notes;        // What changed, shown to the user. Doubles as error report if one occurred.
    };

    /// @brief Looks up the latest release. Runs on a worker thread, so it may block on the network; it should give
    /// up promptly once Stop is requested.
    UpdateInfo FetchLatestUpdate(const std::stop_token& Stop);

    /// @brief Help > Check for Updates: runs FetchLatestUpdate in the background, then shows what it found.
    void CheckForUpdates(ToriState& State);
}  // namespace Tori::UI
