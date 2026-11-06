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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace Tori {
    struct TorrentCreatorOptions {
        std::filesystem::path Source;  // A file, or a folder to share recursively
        std::int64_t PieceLength = 0;  // 0 picks one with AutoPieceLength
        std::vector<std::vector<std::string>> TrackerTiers;
        std::vector<std::string> WebSeeds;
        std::string Comment;
        std::string CreatedBy    = "Tori";
        bool IsPrivate           = false;
        bool IncludeCreationDate = true;
    };

    /// @brief Called after each piece is hashed.
    using CreateProgress = std::function<void(int Hashed, int Total)>;

    /// @brief Piece length aiming for at most ~1500 pieces: a power of two from 16 KiB to 16 MiB.
    std::int64_t AutoPieceLength(std::int64_t TotalSize);

    /// @brief Hashes Source and returns the bencoded .torrent. Blocks for as long as hashing takes, so run it on a
    /// worker thread. Returns nullopt when Stop is requested. Throws std::runtime_error (or StorageError) when the
    /// source is missing, empty, or changes while being hashed.
    ///
    /// Folders are walked recursively in sorted order; symlinks are skipped.
    std::optional<std::string>
    CreateTorrent(const TorrentCreatorOptions& Options, std::stop_token Stop = {}, const CreateProgress& Progress = {});
}  // namespace Tori
