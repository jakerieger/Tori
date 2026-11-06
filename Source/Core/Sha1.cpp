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

#include "Core/Sha1.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Tori {
    namespace {
        void Check(const NTSTATUS Status, const char* What) {
            if (!BCRYPT_SUCCESS(Status)) {
                throw std::runtime_error(std::format("{} failed: 0x{:08X}", What, static_cast<unsigned long>(Status)));
            }
        }

        BCRYPT_ALG_HANDLE Sha1Provider() {
            // Algorithm handles are thread-safe, so one is shared by every hasher for the life of the process
            static const BCRYPT_ALG_HANDLE Provider = [] {
                BCRYPT_ALG_HANDLE Handle = nullptr;
                Check(BCryptOpenAlgorithmProvider(&Handle, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_HASH_REUSABLE_FLAG),
                      "BCryptOpenAlgorithmProvider(SHA1)");
                return Handle;
            }();
            return Provider;
        }
    }  // namespace

    Sha1::Sha1() {
        BCRYPT_HASH_HANDLE Handle = nullptr;
        Check(BCryptCreateHash(Sha1Provider(), &Handle, nullptr, 0, nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG),
              "BCryptCreateHash");
        _Handle = Handle;
    }

    Sha1::~Sha1() {
        if (_Handle) { BCryptDestroyHash(_Handle); }
    }

    Sha1::Sha1(Sha1&& Other) noexcept : _Handle(std::exchange(Other._Handle, nullptr)) {}

    Sha1& Sha1::operator=(Sha1&& Other) noexcept {
        std::swap(_Handle, Other._Handle);
        return *this;
    }

    void Sha1::Update(const void* Data, std::size_t Size) {
        // CNG takes a non-const pointer but never writes through it
        auto* Bytes = static_cast<PUCHAR>(const_cast<void*>(Data));
        while (Size > 0) {
            const auto Chunk = static_cast<ULONG>(std::min<std::size_t>(Size, std::numeric_limits<ULONG>::max()));
            Check(BCryptHashData(_Handle, Bytes, Chunk, 0), "BCryptHashData");
            Bytes += Chunk;
            Size -= Chunk;
        }
    }

    Sha1Digest Sha1::Finish() {
        Sha1Digest Digest {};
        Check(BCryptFinishHash(_Handle, Digest.data(), static_cast<ULONG>(Digest.size()), 0), "BCryptFinishHash");
        return Digest;
    }

    Sha1Digest Sha1::Hash(const std::string_view Data) {
        Sha1 Hasher;
        Hasher.Update(Data);
        return Hasher.Finish();
    }
}  // namespace Tori
