#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Tori {
    /// @brief Invalid UTF-8 sequences become U+FFFD rather than failing, since torrent metadata is untrusted.
    std::wstring Utf8ToWide(std::string_view Text);
    std::string WideToUtf8(std::wstring_view Text);

    std::filesystem::path PathFromUtf8(std::string_view Text);
    std::string PathToUtf8(const std::filesystem::path& Path);
}  // namespace Tori
