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

#include "Tracker/TrackerTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    struct UdpTrackerAddress {
        std::string Host;
        std::uint16_t Port = 0;
    };

    /// @brief Parses "udp://host:port[/anything]", including bracketed IPv6 hosts. The port is required.
    std::optional<UdpTrackerAddress> ParseUdpTrackerUrl(std::string_view Url);

    struct UdpTrackerTiming {
        /// @brief BEP 15 waits 15 * 2^n seconds for attempt n. Its full 8 attempts would take over an hour, so
        /// we stop sooner and let the announcer move on to the next tracker.
        std::chrono::milliseconds BaseTimeout = std::chrono::seconds(15);
        int MaxAttempts                       = 3;
    };

    /// @brief Announces over UDP (BEP 15). Blocks until the tracker answers, every resolved address times out, or
    /// Stop is requested. Connection IDs are cached for a minute per tracker. Throws TrackerError.
    AnnounceResponse UdpAnnounce(const std::string& Url,
                                 const AnnounceRequest& Request,
                                 std::stop_token Stop,
                                 const UdpTrackerTiming& Timing = {});

    /// @brief BEP 15 packet encoding, exposed for tests. The Parse functions return nullopt for packets that
    /// aren't the expected reply (wrong transaction or action), and throw TrackerError for tracker error replies.
    namespace UdpProtocol {
        inline constexpr std::uint64_t kProtocolId = 0x41727101980ULL;

        std::vector<std::uint8_t> BuildConnect(std::uint32_t TransactionId);
        std::optional<std::uint64_t> ParseConnectResponse(std::span<const std::uint8_t> Packet,
                                                          std::uint32_t TransactionId);

        std::vector<std::uint8_t>
        BuildAnnounce(std::uint64_t ConnectionId, std::uint32_t TransactionId, const AnnounceRequest& Request);
        std::optional<AnnounceResponse>
        ParseAnnounceResponse(std::span<const std::uint8_t> Packet, std::uint32_t TransactionId, bool IsV6);
    }  // namespace UdpProtocol
}  // namespace Tori
