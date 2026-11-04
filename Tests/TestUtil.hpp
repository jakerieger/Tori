#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace Tori::Testing {
    /// @brief A fresh folder under %TEMP%, deleted with everything in it when the test ends.
    class TempDir {
    public:
        TempDir() {
            Path = std::filesystem::temp_directory_path() / std::format(L"tori-test-{:08x}", std::random_device {}());
            std::filesystem::create_directories(Path);
        }

        ~TempDir() {
            std::error_code Ec;
            std::filesystem::remove_all(Path, Ec);
        }

        TempDir(const TempDir&)            = delete;
        TempDir& operator=(const TempDir&) = delete;

        std::filesystem::path Path;
    };

    inline std::vector<std::uint8_t> RandomBytes(const std::size_t Size, const std::uint32_t Seed) {
        std::mt19937 Engine(Seed);
        std::vector<std::uint8_t> Out(Size);
        for (std::uint8_t& Byte : Out) { Byte = static_cast<std::uint8_t>(Engine()); }
        return Out;
    }

    inline void WriteFileBytes(const std::filesystem::path& Path, const std::vector<std::uint8_t>& Bytes) {
        std::filesystem::create_directories(Path.parent_path());
        std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
        Out.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
    }

    inline std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& Path) {
        std::ifstream In(Path, std::ios::binary);
        return {std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()};
    }
}  // namespace Tori::Testing
