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

#include "Core/InfoHash.hpp"
#include "Core/PeerId.hpp"
#include "Net/Endpoint.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    enum class AnnounceEvent {
        None,
        Started,
        Completed,
        Stopped,
    };

    /// @brief The value of the HTTP "event" parameter; empty for None.
    std::string_view AnnounceEventName(AnnounceEvent Event);

    struct AnnounceRequest {
        InfoHash Hash;
        PeerId Id {};
        std::uint16_t Port      = 0;
        std::int64_t Uploaded   = 0;
        std::int64_t Downloaded = 0;
        std::int64_t Left       = 0;
        AnnounceEvent Event     = AnnounceEvent::None;
        int NumWant             = 50;
        std::uint32_t Key       = 0;  // Lets the tracker recognize us across IP changes
        std::string TrackerId;        // Echoed back if the tracker sent one
    };

    struct AnnounceResponse {
        std::vector<Endpoint> Peers;
        int IntervalSeconds = 1800;
        std::optional<int> MinIntervalSeconds;
        std::optional<int> Seeders;
        std::optional<int> Leechers;
        std::string WarningMessage;
        std::string TrackerId;
    };

    /// @brief The tracker refused the announce, sent garbage, or couldn't be reached. what() is shown to the user.
    class TrackerError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /// @brief Bounds a tracker-supplied interval so a misbehaving tracker can't make us hammer it or go silent.
    inline int ClampAnnounceInterval(const std::int64_t Seconds) {
        constexpr std::int64_t kMin = 30;
        constexpr std::int64_t kMax = 24 * 60 * 60;
        return static_cast<int>(std::clamp(Seconds, kMin, kMax));
    }
}  // namespace Tori
