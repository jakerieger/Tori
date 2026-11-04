#include "Core/TorrentTypes.hpp"

#include <algorithm>

namespace Tori {
    std::string TorrentStateString(const TorrentState State) {
        switch (State) {
            case TorrentState::Queued:
                return "Queued";
            case TorrentState::Checking:
                return "Checking";
            case TorrentState::FetchingMetadata:
                return "Fetching metadata";
            case TorrentState::Downloading:
                return "Downloading";
            case TorrentState::Paused:
                return "Paused";
            case TorrentState::Seeding:
                return "Seeding";
            case TorrentState::Complete:
                return "Complete";
            case TorrentState::Error:
                return "Error";
        }
        return {};
    }

    float ComputeProgress(const double DownloadedSize, const double TotalSize) {
        if (TotalSize <= 0.0) { return 0.0f; }
        return static_cast<float>(std::clamp(DownloadedSize / TotalSize, 0.0, 1.0));
    }

    double ComputeRatio(const double Uploaded, const double Downloaded, const double Done) {
        const double Basis = std::max(Downloaded, Done);
        if (Basis <= 0.0) { return 0.0; }
        return Uploaded / Basis;
    }
}  // namespace Tori
