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

#include "Meta/Metainfo.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace Tori {
    /// @brief Thrown when reading or writing torrent data fails for a reason other than the data being absent.
    class StorageError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /// @brief The part of one file covered by a range of the torrent's concatenated data.
    struct FileSlice {
        int FileIndex           = 0;
        std::int64_t FileOffset = 0;
        std::int64_t Length     = 0;
    };

    /// @brief Splits [Offset, Offset + Length) of the torrent's data into per-file slices, in order. Zero-length
    /// files never appear. Files must be in torrent order with contiguous offsets, as Metainfo produces them.
    std::vector<FileSlice> MapByteRange(std::span<const TorrentFile> Files, std::int64_t Offset, std::int64_t Length);

    /// @brief Reads and writes a torrent's data as one contiguous byte range spread over its files.
    ///
    /// Files are opened lazily: for writing they are created sparse and sized to their final length; for reading a
    /// missing file just means the data isn't there yet. At most kMaxOpenFiles handles are held at once. BEP 47 pad
    /// files read as zeros and are never created. Not thread-safe; each torrent's worker thread owns its storage.
    class FileStorage {
    public:
        static constexpr std::size_t kMaxOpenFiles = 64;

        FileStorage(std::filesystem::path Destination, std::vector<TorrentFile> Files);
        ~FileStorage();

        FileStorage(const FileStorage&)            = delete;
        FileStorage& operator=(const FileStorage&) = delete;

        /// @brief Fills Buffer from the torrent's data at Offset. Returns false when any of it is missing (a file
        /// doesn't exist or is too short). Throws StorageError on other I/O failures.
        [[nodiscard]] bool Read(std::int64_t Offset, std::span<std::uint8_t> Buffer);

        /// @brief Writes Data at Offset, creating folders and files as needed. Throws StorageError on failure.
        void Write(std::int64_t Offset, std::span<const std::uint8_t> Data);

        /// @brief Releases every open handle, e.g. before files are moved or deleted.
        void CloseAll();

        [[nodiscard]] std::filesystem::path FilePath(int Index) const;
        [[nodiscard]] const std::vector<TorrentFile>& Files() const { return _Files; }
        [[nodiscard]] std::int64_t TotalSize() const { return _TotalSize; }

    private:
        struct Handle {
            void* Native          = nullptr;  // HANDLE, or nullptr while closed
            bool Writable         = false;
            std::uint64_t LastUse = 0;
        };

        void* Acquire(int Index, bool ForWrite);
        void Close(int Index);
        void EvictLeastRecentlyUsed();
        void CheckRange(std::int64_t Offset, std::size_t Length) const;

        std::filesystem::path _Destination;
        std::vector<TorrentFile> _Files;
        std::vector<Handle> _Handles;
        std::int64_t _TotalSize = 0;
        std::uint64_t _UseClock = 0;
        std::size_t _OpenCount  = 0;
    };
}  // namespace Tori
