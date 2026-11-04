#include "Core/Win32.hpp"

#include "Core/Utf8.hpp"

#include <windows.h>

#include <format>

namespace Tori {
    std::string Win32ErrorMessage(const unsigned long Code) {
        wchar_t* Buffer    = nullptr;
        const DWORD Length = FormatMessageW(
          FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
          nullptr,
          Code,
          0,
          reinterpret_cast<LPWSTR>(&Buffer),
          0,
          nullptr);

        std::string Message = Length ? WideToUtf8({Buffer, Length}) : std::format("error {}", Code);
        LocalFree(Buffer);

        while (!Message.empty() && (Message.back() == '\r' || Message.back() == '\n' || Message.back() == '.' ||
                                    Message.back() == ' ')) {
            Message.pop_back();
        }
        return Message;
    }

    std::wstring LongPath(const std::filesystem::path& Path) {
        if (Path.native().starts_with(L"\\\\?\\")) { return Path.native(); }

        // The \\?\ form disables all normalization, so it must already be absolute with backslashes only
        std::filesystem::path Normal = std::filesystem::absolute(Path).lexically_normal();
        Normal.make_preferred();
        const std::wstring& Native = Normal.native();

        if (Native.starts_with(L"\\\\")) { return L"\\\\?\\UNC\\" + Native.substr(2); }
        return L"\\\\?\\" + Native;
    }
}  // namespace Tori
