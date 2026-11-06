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

#include <cstdint>
#include <filesystem>
#include <string>

namespace Tori {
    /// @brief Stable identifier for a torrent within a session. Never reused while the app is running.
    using TorrentId = std::uint64_t;

    inline constexpr TorrentId kInvalidTorrentId = 0;

    enum class TorrentState {
        Queued,
        Checking,
        FetchingMetadata,
        Downloading,
        Paused,
        Seeding,
        Complete,
        Error,
    };

    std::string TorrentStateString(TorrentState State);

    /// @brief Snapshot of a torrent published by its worker thread for the UI to read.
    struct TorrentStats {
        TorrentId Id = kInvalidTorrentId;
        InfoHash Hash;
        std::string Name;
        std::filesystem::path Destination;
        double TotalSize       = 0.0;  // Bytes; 0 until metadata is known
        double DownloadedSize  = 0.0;  // Verified bytes on disk
        double DownloadedTotal = 0.0;  // Payload bytes received from peers, including discarded data
        double UploadedTotal   = 0.0;  // Bytes uploaded over the torrent's lifetime
        double DownloadSpeed   = 0.0;  // Bytes/second
        double UploadSpeed     = 0.0;  // Bytes/second
        int Peers              = 0;
        TorrentState State     = TorrentState::Queued;
        std::string ErrorMessage;
    };

    /// @brief Fraction of the torrent downloaded, in [0, 1]. Returns 0 when the total size is not yet known.
    float ComputeProgress(double DownloadedSize, double TotalSize);

    /// @brief Share ratio: uploaded over downloaded. Data that was already on disk (Done) counts as downloaded, as
    /// in qBittorrent, so seeding files you made yourself doesn't show an endless ratio. Returns 0 when both are 0.
    double ComputeRatio(double Uploaded, double Downloaded, double Done = 0.0);
}  // namespace Tori
