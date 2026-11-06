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

#include "Core/Bitfield.hpp"
#include "Meta/Metainfo.hpp"
#include "Storage/FileStorage.hpp"

#include <functional>
#include <optional>
#include <stop_token>

namespace Tori {
    /// @brief Called after each piece is checked.
    using VerifyProgress = std::function<void(int Checked, int Total)>;

    /// @brief Hashes every piece on disk against the torrent's piece hashes. Missing data simply leaves a piece
    /// unset. Returns nullopt when Stop is requested; throws StorageError on I/O failures.
    std::optional<Bitfield> VerifyPieces(FileStorage& Storage,
                                         const Metainfo& Meta,
                                         std::stop_token Stop           = {},
                                         const VerifyProgress& Progress = {});
}  // namespace Tori
