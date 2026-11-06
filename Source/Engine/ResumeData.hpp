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
#include "Meta/Metainfo.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    /// @brief A file's size and last-write time, used to tell whether it changed while Tori wasn't running.
    struct FileStamp {
        std::int64_t Size     = -1;  // -1 when the file doesn't exist (or is a pad file, which never does)
        std::int64_t Modified = 0;   // FILETIME, 100 ns units

        bool operator==(const FileStamp&) const = default;
    };

    /// @brief Stamps each of a torrent's files, in order.
    std::vector<FileStamp> StampFiles(const std::filesystem::path& Destination, std::span<const TorrentFile> Files);

    /// @brief Everything needed to bring a torrent back after a restart, apart from its metadata, which is saved
    /// as a .torrent file beside it.
    struct ResumeData {
        InfoHash Hash;
        std::string Name;  // Shown for a magnet link until its metadata arrives
        std::filesystem::path Destination;
        std::vector<std::vector<std::string>> ExtraTrackers;  // A magnet link's trackers
        bool Paused             = false;
        std::int64_t Uploaded   = 0;  // Over the torrent's lifetime
        std::int64_t Downloaded = 0;
        std::int64_t AddedOn    = 0;  // Unix time
        std::int64_t Position   = 0;  // Restored torrents keep their relative order
        std::vector<int> SkippedFiles;

        /// @brief The verified pieces (wire-format bitfield) and the stamps the files had when that was true.
        /// Both are empty when nothing has been checked yet. If the files still match, the pieces are trusted
        /// and the full check is skipped.
        std::vector<std::uint8_t> Pieces;
        std::vector<FileStamp> Files;
    };

    std::string EncodeResumeData(const ResumeData& Data);

    /// @brief Throws ParseError when the data is malformed.
    ResumeData DecodeResumeData(std::string_view Encoded);
}  // namespace Tori
