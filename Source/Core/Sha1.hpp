#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Tori {
    using Sha1Digest = std::array<std::uint8_t, 20>;

    /// @brief Incremental SHA-1 backed by Windows CNG. Reusable: Finish() resets it for the next message.
    class Sha1 {
    public:
        Sha1();
        ~Sha1();

        Sha1(const Sha1&)            = delete;
        Sha1& operator=(const Sha1&) = delete;
        Sha1(Sha1&& Other) noexcept;
        Sha1& operator=(Sha1&& Other) noexcept;

        void Update(const void* Data, std::size_t Size);
        void Update(std::string_view Data) { Update(Data.data(), Data.size()); }

        Sha1Digest Finish();

        static Sha1Digest Hash(std::string_view Data);

    private:
        void* _Handle = nullptr;  // BCRYPT_HASH_HANDLE
    };
}  // namespace Tori
