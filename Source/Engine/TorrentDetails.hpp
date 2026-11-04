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
