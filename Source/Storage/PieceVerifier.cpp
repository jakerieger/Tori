#include "Storage/PieceVerifier.hpp"

#include "Core/Sha1.hpp"

#include <span>
#include <vector>

namespace Tori {
    std::optional<Bitfield> VerifyPieces(FileStorage& Storage,
                                         const Metainfo& Meta,
                                         const std::stop_token Stop,
                                         const VerifyProgress& Progress) {
        const int Count = Meta.PieceCount();
        Bitfield Result(Count);
        std::vector<std::uint8_t> Buffer(static_cast<std::size_t>(Meta.PieceLength));
        Sha1 Hasher;

        for (int Index = 0; Index < Count; ++Index) {
            if (Stop.stop_requested()) { return std::nullopt; }

            const std::span<std::uint8_t> Piece =
              std::span(Buffer).first(static_cast<std::size_t>(Meta.PieceSize(Index)));
            if (Storage.Read(Meta.PieceLength * Index, Piece)) {
                Hasher.Update(Piece.data(), Piece.size());
                if (Hasher.Finish() == Meta.PieceHashes[static_cast<std::size_t>(Index)]) { Result.Set(Index); }
            }

            if (Progress) { Progress(Index + 1, Count); }
        }
        return Result;
    }
}  // namespace Tori
