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
                                         std::stop_token Stop          = {},
                                         const VerifyProgress& Progress = {});
}  // namespace Tori
