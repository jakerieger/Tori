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

#include "Core/FileIO.hpp"

#include "Core/Utf8.hpp"
#include "Core/Win32.hpp"

#include <windows.h>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace Tori {
    namespace {
        struct FileHandle {
            HANDLE Native = INVALID_HANDLE_VALUE;
            ~FileHandle() {
                if (Native != INVALID_HANDLE_VALUE) { CloseHandle(Native); }
            }
        };
    }  // namespace

    std::optional<std::string> ReadWholeFile(const std::filesystem::path& File, const std::int64_t MaxSize) {
        FileHandle Handle {CreateFileW(LongPath(File).c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr)};
        if (Handle.Native == INVALID_HANDLE_VALUE) { return std::nullopt; }

        LARGE_INTEGER Size {};
        if (!GetFileSizeEx(Handle.Native, &Size) || Size.QuadPart > MaxSize) { return std::nullopt; }

        std::string Data(static_cast<std::size_t>(Size.QuadPart), '\0');
        std::size_t Done = 0;
        while (Done < Data.size()) {
            DWORD Read       = 0;
            const auto Chunk = static_cast<DWORD>(std::min<std::size_t>(Data.size() - Done, 1u << 30));
            if (!ReadFile(Handle.Native, Data.data() + Done, Chunk, &Read, nullptr) || Read == 0) {
                return std::nullopt;
            }
            Done += Read;
        }
        return Data;
    }

    void WriteFileAtomically(const std::filesystem::path& File, const std::string_view Contents) {
        const std::wstring Target    = LongPath(File);
        const std::wstring Temporary = Target + L".tmp";
        const auto Fail              = [&](const std::string_view What) {
            throw std::runtime_error(
              std::format("could not {} {}: {}", What, PathToUtf8(File), Win32ErrorMessage(GetLastError())));
        };

        {
            FileHandle Handle {
              CreateFileW(Temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (Handle.Native == INVALID_HANDLE_VALUE) { Fail("create"); }

            std::size_t Done = 0;
            while (Done < Contents.size()) {
                DWORD Written    = 0;
                const auto Chunk = static_cast<DWORD>(std::min<std::size_t>(Contents.size() - Done, 1u << 30));
                if (!WriteFile(Handle.Native, Contents.data() + Done, Chunk, &Written, nullptr)) { Fail("write"); }
                Done += Written;
            }
            // Without this the rename can reach the disk before the data does, leaving an empty file after a crash
            if (!FlushFileBuffers(Handle.Native)) { Fail("flush"); }
        }

        if (!MoveFileExW(Temporary.c_str(), Target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD Error = GetLastError();
            DeleteFileW(Temporary.c_str());
            SetLastError(Error);
            Fail("replace");
        }
    }
}  // namespace Tori
