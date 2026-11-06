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

#include "Core/TorrentTypes.hpp"
#include "Tracker/Announcer.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Tori {
    struct FileDetails {
        int Index = 0;  // In the metadata's file list, as SetSkippedFiles takes it
        std::filesystem::path Path;
        std::int64_t Size = 0;
        std::int64_t Done = 0;  // Bytes in verified pieces
        bool Wanted       = true;
    };

    struct PeerDetails {
        std::string Address;
        std::string Client;
        float Progress       = 0.0f;  // Fraction of the torrent the peer has
        double DownloadSpeed = 0.0;   // From them
        double UploadSpeed   = 0.0;   // To them
        /// @brief D/d: downloading from them / we want to but they choke us. U/u: uploading to them / they want to
        /// but we choke them. I: they connected to us.
        std::string Flags;
    };

    /// @brief What the details pane shows for one torrent. Workers only build this while someone is looking.
    struct TorrentDetails {
        TorrentId Id = kInvalidTorrentId;
        std::vector<FileDetails> Files;  // Empty until the metadata is known; pad files are left out
        std::vector<PeerDetails> Peers;
        std::vector<TrackerStatus> Trackers;
    };
}  // namespace Tori
