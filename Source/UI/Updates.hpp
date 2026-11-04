#pragma once

#include "UI/Model.hpp"

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace Tori::UI {
    /// @brief This build's version, from the CMake project.
    inline constexpr std::string_view kToriVersion = TORI_VERSION;

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
