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

#include "Storage/FileStorage.hpp"

#include "Core/Utf8.hpp"
#include "Core/Win32.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <format>
#include <limits>

namespace Tori {
    namespace {
        // Keeps each ReadFile/WriteFile call within a DWORD byte count
        constexpr std::size_t kMaxIoChunk = 1u << 30;

        OVERLAPPED OffsetOverlapped(const std::int64_t Offset) {
            OVERLAPPED Overlapped {};
            Overlapped.Offset     = static_cast<DWORD>(static_cast<std::uint64_t>(Offset) & 0xFFFFFFFFu);
            Overlapped.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(Offset) >> 32);
            return Overlapped;
        }
    }  // namespace

    std::vector<FileSlice>
    MapByteRange(const std::span<const TorrentFile> Files, std::int64_t Offset, const std::int64_t Length) {
        std::vector<FileSlice> Slices;
        if (Length <= 0 || Files.empty()) { return Slices; }

        // Last file starting at or before Offset. Any zero-length files sharing that offset come after the file
        // that actually holds the byte, so the loop below skips past them.
        const auto First  = std::ranges::upper_bound(Files, Offset, {}, &TorrentFile::Offset);
        std::size_t Index = First == Files.begin() ? 0 : static_cast<std::size_t>(First - Files.begin() - 1);

        const std::int64_t End = Offset + Length;
        for (; Index < Files.size() && Offset < End; ++Index) {
            const TorrentFile& File    = Files[Index];
            const std::int64_t FileEnd = File.Offset + File.Length;
            if (FileEnd <= Offset) { continue; }

            const std::int64_t SliceLength = std::min(End, FileEnd) - Offset;
            Slices.push_back({
              .FileIndex  = static_cast<int>(Index),
              .FileOffset = Offset - File.Offset,
              .Length     = SliceLength,
            });
            Offset += SliceLength;
        }
        return Slices;
    }

    FileStorage::FileStorage(std::filesystem::path Destination, std::vector<TorrentFile> Files)
        : _Destination(std::move(Destination)), _Files(std::move(Files)), _Handles(_Files.size()) {
        for (const TorrentFile& File : _Files) {
            _TotalSize += File.Length;
        }
    }

    FileStorage::~FileStorage() {
        CloseAll();
    }

    std::filesystem::path FileStorage::FilePath(const int Index) const {
        return _Destination / _Files[static_cast<std::size_t>(Index)].Path;
    }

    bool FileStorage::Read(const std::int64_t Offset, const std::span<std::uint8_t> Buffer) {
        CheckRange(Offset, Buffer.size());

        std::size_t Done = 0;
        for (const FileSlice& Slice : MapByteRange(_Files, Offset, static_cast<std::int64_t>(Buffer.size()))) {
            std::span<std::uint8_t> Out = Buffer.subspan(Done, static_cast<std::size_t>(Slice.Length));
            Done += Out.size();

            if (_Files[static_cast<std::size_t>(Slice.FileIndex)].IsPadding) {
                std::ranges::fill(Out, std::uint8_t {0});
                continue;
            }

            void* File = Acquire(Slice.FileIndex, false);
            if (!File) { return false; }

            std::int64_t Position = Slice.FileOffset;
            while (!Out.empty()) {
                const auto Chunk      = static_cast<DWORD>(std::min(Out.size(), kMaxIoChunk));
                OVERLAPPED Overlapped = OffsetOverlapped(Position);
                DWORD Transferred     = 0;
                if (!ReadFile(File, Out.data(), Chunk, &Transferred, &Overlapped)) {
                    const DWORD Error = GetLastError();
                    if (Error == ERROR_HANDLE_EOF) { return false; }
                    throw StorageError(std::format("could not read {}: {}",
                                                   PathToUtf8(FilePath(Slice.FileIndex)),
                                                   Win32ErrorMessage(Error)));
                }
                if (Transferred != Chunk) { return false; }  // file is shorter than the torrent says
                Out = Out.subspan(Chunk);
                Position += Chunk;
            }
        }
        return true;
    }

    void FileStorage::Write(const std::int64_t Offset, const std::span<const std::uint8_t> Data) {
        CheckRange(Offset, Data.size());

        std::size_t Done = 0;
        for (const FileSlice& Slice : MapByteRange(_Files, Offset, static_cast<std::int64_t>(Data.size()))) {
            std::span<const std::uint8_t> In = Data.subspan(Done, static_cast<std::size_t>(Slice.Length));
            Done += In.size();

            if (_Files[static_cast<std::size_t>(Slice.FileIndex)].IsPadding) { continue; }

            void* File            = Acquire(Slice.FileIndex, true);
            std::int64_t Position = Slice.FileOffset;
            while (!In.empty()) {
                const auto Chunk      = static_cast<DWORD>(std::min(In.size(), kMaxIoChunk));
                OVERLAPPED Overlapped = OffsetOverlapped(Position);
                DWORD Transferred     = 0;
                if (!WriteFile(File, In.data(), Chunk, &Transferred, &Overlapped) || Transferred != Chunk) {
                    throw StorageError(std::format("could not write {}: {}",
                                                   PathToUtf8(FilePath(Slice.FileIndex)),
                                                   Win32ErrorMessage(GetLastError())));
                }
                In = In.subspan(Chunk);
                Position += Chunk;
            }
        }
    }

    void FileStorage::CloseAll() {
        for (std::size_t I = 0; I < _Handles.size(); ++I) {
            Close(static_cast<int>(I));
        }
    }

    void* FileStorage::Acquire(const int Index, const bool ForWrite) {
        Handle& Entry = _Handles[static_cast<std::size_t>(Index)];
        Entry.LastUse = ++_UseClock;
        if (Entry.Native && (Entry.Writable || !ForWrite)) { return Entry.Native; }

        Close(Index);
        if (_OpenCount >= kMaxOpenFiles) { EvictLeastRecentlyUsed(); }

        const std::filesystem::path Path = FilePath(Index);
        const std::wstring Native        = LongPath(Path);
        HANDLE File                      = INVALID_HANDLE_VALUE;

        if (ForWrite) {
            std::error_code Ec;
            std::filesystem::create_directories(std::filesystem::path(LongPath(Path.parent_path())), Ec);
            if (Ec) {
                throw StorageError(
                  std::format("could not create folder for {}: {}", PathToUtf8(Path), Win32ErrorMessage(Ec.value())));
            }

            File                  = CreateFileW(Native.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
            const DWORD OpenError = GetLastError();
            if (File == INVALID_HANDLE_VALUE) {
                throw StorageError(
                  std::format("could not open {}: {}", PathToUtf8(Path), Win32ErrorMessage(OpenError)));
            }

            // Sparse so preallocating a large torrent doesn't write zeros to disk. Best effort: FAT32 can't do it.
            if (OpenError != ERROR_ALREADY_EXISTS) {
                DWORD Unused = 0;
                DeviceIoControl(File, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &Unused, nullptr);
            }

            // A file already at this path is the torrent's file; size it to match, which truncates any excess
            const std::int64_t Length = _Files[static_cast<std::size_t>(Index)].Length;
            LARGE_INTEGER Size {};
            if (!GetFileSizeEx(File, &Size) || Size.QuadPart != Length) {
                FILE_END_OF_FILE_INFO EndOfFile {};
                EndOfFile.EndOfFile.QuadPart = Length;
                if (!SetFileInformationByHandle(File, FileEndOfFileInfo, &EndOfFile, sizeof(EndOfFile))) {
                    const DWORD Error = GetLastError();
                    CloseHandle(File);
                    throw StorageError(
                      std::format("could not allocate {}: {}", PathToUtf8(Path), Win32ErrorMessage(Error)));
                }
            }
        } else {
            File = CreateFileW(Native.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
            if (File == INVALID_HANDLE_VALUE) {
                const DWORD Error = GetLastError();
                if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND) { return nullptr; }
                throw StorageError(std::format("could not open {}: {}", PathToUtf8(Path), Win32ErrorMessage(Error)));
            }
        }

        Entry.Native   = File;
        Entry.Writable = ForWrite;
        ++_OpenCount;
        return File;
    }

    void FileStorage::Close(const int Index) {
        Handle& Entry = _Handles[static_cast<std::size_t>(Index)];
        if (!Entry.Native) { return; }
        CloseHandle(Entry.Native);
        Entry.Native   = nullptr;
        Entry.Writable = false;
        --_OpenCount;
    }

    void FileStorage::EvictLeastRecentlyUsed() {
        int Oldest              = -1;
        std::uint64_t OldestUse = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t I = 0; I < _Handles.size(); ++I) {
            if (_Handles[I].Native && _Handles[I].LastUse < OldestUse) {
                Oldest    = static_cast<int>(I);
                OldestUse = _Handles[I].LastUse;
            }
        }
        if (Oldest >= 0) { Close(Oldest); }
    }

    void FileStorage::CheckRange(const std::int64_t Offset, const std::size_t Length) const {
        if (Offset < 0 || Offset > _TotalSize || static_cast<std::int64_t>(Length) > _TotalSize - Offset) {
            throw StorageError(
              std::format("range [{}, +{}) is outside the torrent's {} bytes", Offset, Length, _TotalSize));
        }
    }
}  // namespace Tori
