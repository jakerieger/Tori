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
        WideCharToMultiByte(
          CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Out.data(), Length, nullptr, nullptr);
        return Out;
    }

    std::filesystem::path PathFromUtf8(const std::string_view Text) {
        return std::filesystem::path(Utf8ToWide(Text));
    }

    std::string PathToUtf8(const std::filesystem::path& Path) {
        return WideToUtf8(Path.native());
    }
}  // namespace Tori
