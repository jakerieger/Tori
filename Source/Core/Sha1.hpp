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
