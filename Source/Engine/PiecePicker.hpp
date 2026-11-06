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
#include "Peer/Messages.hpp"

#include <cstdint>
#include <map>
#include <random>
#include <span>
#include <vector>

namespace Tori {
    /// @brief Decides which blocks to request from which peer.
    ///
    /// Partially downloaded pieces are finished first, so data can be verified and shared sooner. New pieces are
    /// chosen rarest-first among peers we know of, with ties broken randomly. When every missing block a peer can
    /// give us has already been requested from someone else, the picker enters endgame and hands out duplicates,
    /// so a few slow peers can't hold up the last pieces.
    class PiecePicker {
    public:
        PiecePicker(int PieceCount, std::int64_t PieceLength, std::int64_t TotalSize);

        /// @brief Replaces what we have, e.g. after a recheck. Forgets all partial pieces.
        void SetHave(const Bitfield& Have);
        [[nodiscard]] const Bitfield& Have() const { return _Have; }

        /// @brief Limits downloading to these pieces, e.g. those of the files the user chose. Every piece is
        /// wanted until this is called. Partial pieces that are no longer wanted stop being requested.
        void SetWanted(const Bitfield& Wanted);
        [[nodiscard]] bool Wanted(int Piece) const { return _Wanted.Get(Piece); }

        /// @brief A wanted piece we don't have yet.
        [[nodiscard]] bool Needs(int Piece) const { return _Wanted.Get(Piece) && !_Have.Get(Piece); }

        /// @brief Every wanted piece has been verified.
        [[nodiscard]] bool WantedComplete() const;

        [[nodiscard]] int PieceCount() const { return _PieceCount; }
        [[nodiscard]] std::int64_t PieceSize(int Piece) const;
        [[nodiscard]] int BlocksInPiece(int Piece) const;
        [[nodiscard]] int BlockLength(int Piece, int Block) const;

        /// @brief Availability bookkeeping as peers come, go, and announce pieces.
        void AddPeer(const Bitfield& PeerHas);
        void RemovePeer(const Bitfield& PeerHas);
        void PeerHasPiece(int Piece);
        [[nodiscard]] int Availability(int Piece) const { return _Availability[static_cast<std::size_t>(Piece)]; }

        /// @brief Up to Count blocks to request from a peer, marked as requested. PeerOutstanding lists what that
        /// peer already has pending, so endgame never asks one peer for the same block twice.
        std::vector<BlockRequest>
        Pick(const Bitfield& PeerHas, int Count, std::span<const BlockRequest> PeerOutstanding);

        /// @brief A request was dropped (choke, disconnect, timeout); the block can be handed out again.
        void Abort(const BlockRequest& Block);

        enum class BlockResult {
            Ignored,  // Not a block we still need: duplicate, unknown, or malformed
            Accepted,
            PieceComplete,  // Accepted, and the piece now has every block; verify it next
        };

        BlockResult Received(const BlockRequest& Block);

        void PieceVerified(int Piece);
        void PieceFailed(int Piece);

        [[nodiscard]] std::size_t PartialCount() const { return _Partial.size(); }

    private:
        struct Partial {
            std::vector<std::uint8_t> Requests;  // Outstanding requests per block
            std::vector<bool> Received;
            int ReceivedCount = 0;
        };

        Partial& Start(int Piece);
        static bool Contains(std::span<const BlockRequest> Requests, const BlockRequest& Block);

        int _PieceCount;
        std::int64_t _PieceLength;
        std::int64_t _TotalSize;
        Bitfield _Have;
        Bitfield _Wanted;
        std::vector<int> _Availability;
        std::map<int, Partial> _Partial;
        std::mt19937 _Random;
    };
}  // namespace Tori
