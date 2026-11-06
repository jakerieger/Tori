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

#include "Core/Utf8.hpp"

#include <windows.h>

namespace Tori {
    std::wstring Utf8ToWide(const std::string_view Text) {
        if (Text.empty()) { return {}; }
        const int Length = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
        std::wstring Out(static_cast<std::size_t>(Length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Out.data(), Length);
        return Out;
    }

    std::string WideToUtf8(const std::wstring_view Text) {
        if (Text.empty()) { return {}; }
        const int Length =
          WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
        std::string Out(static_cast<std::size_t>(Length), '\0');
        WideCharToMultiByte(CP_UTF8,
                            0,
                            Text.data(),
                            static_cast<int>(Text.size()),
                            Out.data(),
                            Length,
                            nullptr,
                            nullptr);
        return Out;
    }

    std::filesystem::path PathFromUtf8(const std::string_view Text) {
        return std::filesystem::path(Utf8ToWide(Text));
    }

    std::string PathToUtf8(const std::filesystem::path& Path) {
        return WideToUtf8(Path.native());
    }
}  // namespace Tori
