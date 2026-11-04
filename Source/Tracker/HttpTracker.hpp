#pragma once

#include "Tracker/TrackerTypes.hpp"

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

namespace Tori {
    /// @brief The full announce URL: Base plus the BEP 3 query parameters (compact peers requested).
    std::string BuildAnnounceUrl(std::string_view Base, const AnnounceRequest& Request);

    /// @brief Parses a tracker's bencoded reply. Throws TrackerError with the tracker's "failure reason", or when
    /// the reply is malformed.
    AnnounceResponse ParseHttpAnnounceResponse(std::string_view Body);

    /// @brief Announces over HTTP(S) with WinHTTP. Blocks until the tracker answers, Timeout passes, or Stop is
    /// requested. Throws TrackerError.
    AnnounceResponse HttpAnnounce(const std::string& Url,
                                  const AnnounceRequest& Request,
                                  std::stop_token Stop,
                                  std::chrono::milliseconds Timeout = std::chrono::seconds(20));
}  // namespace Tori
