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

#include "Engine/PiecePicker.hpp"

#include "Core/Random.hpp"

#include <algorithm>
#include <climits>

namespace Tori {
    PiecePicker::PiecePicker(const int PieceCount, const std::int64_t PieceLength, const std::int64_t TotalSize)
        : _PieceCount(PieceCount), _PieceLength(PieceLength), _TotalSize(TotalSize), _Have(PieceCount),
          _Wanted(PieceCount), _Availability(static_cast<std::size_t>(PieceCount), 0), _Random(RandomU32()) {
        for (int Piece = 0; Piece < PieceCount; ++Piece) {
            _Wanted.Set(Piece);
        }
    }

    void PiecePicker::SetWanted(const Bitfield& Wanted) {
        if (Wanted.Size() == _PieceCount) { _Wanted = Wanted; }
    }

    bool PiecePicker::WantedComplete() const {
        for (int Piece = 0; Piece < _PieceCount; ++Piece) {
            if (Needs(Piece)) { return false; }
        }
        return true;
    }

    void PiecePicker::SetHave(const Bitfield& Have) {
        _Have = Have;
        _Partial.clear();
    }

    std::int64_t PiecePicker::PieceSize(const int Piece) const {
        if (Piece == _PieceCount - 1) { return _TotalSize - _PieceLength * Piece; }
        return _PieceLength;
    }

    int PiecePicker::BlocksInPiece(const int Piece) const {
        return static_cast<int>((PieceSize(Piece) + kBlockSize - 1) / kBlockSize);
    }

    int PiecePicker::BlockLength(const int Piece, const int Block) const {
        const std::int64_t Remaining = PieceSize(Piece) - static_cast<std::int64_t>(Block) * kBlockSize;
        return static_cast<int>(std::min<std::int64_t>(Remaining, kBlockSize));
    }

    void PiecePicker::AddPeer(const Bitfield& PeerHas) {
        for (int I = 0; I < _PieceCount; ++I) {
            if (PeerHas.Get(I)) { ++_Availability[static_cast<std::size_t>(I)]; }
        }
    }

    void PiecePicker::RemovePeer(const Bitfield& PeerHas) {
        for (int I = 0; I < _PieceCount; ++I) {
            if (PeerHas.Get(I)) { --_Availability[static_cast<std::size_t>(I)]; }
        }
    }

    void PiecePicker::PeerHasPiece(const int Piece) {
        ++_Availability[static_cast<std::size_t>(Piece)];
    }

    PiecePicker::Partial& PiecePicker::Start(const int Piece) {
        Partial& Entry = _Partial[Piece];
        if (Entry.Requests.empty()) {
            const auto Blocks = static_cast<std::size_t>(BlocksInPiece(Piece));
            Entry.Requests.assign(Blocks, 0);
            Entry.Received.assign(Blocks, false);
        }
        return Entry;
    }

    bool PiecePicker::Contains(const std::span<const BlockRequest> Requests, const BlockRequest& Block) {
        return std::ranges::find(Requests, Block) != Requests.end();
    }

    std::vector<BlockRequest>
    PiecePicker::Pick(const Bitfield& PeerHas, const int Count, const std::span<const BlockRequest> PeerOutstanding) {
        std::vector<BlockRequest> Picks;
        if (Count <= 0 || _PieceCount == 0) { return Picks; }

        const auto TakeFrom = [&](const int Piece, Partial& Entry, const bool Duplicates) {
            for (std::size_t Block = 0; Block < Entry.Requests.size(); ++Block) {
                if (static_cast<int>(Picks.size()) >= Count) { return; }
                if (Entry.Received[Block]) { continue; }

                const BlockRequest Request {Piece,
                                            static_cast<int>(Block) * kBlockSize,
                                            BlockLength(Piece, static_cast<int>(Block))};
                const bool Free = Entry.Requests[Block] == 0;
                if (Free || (Duplicates && !Contains(PeerOutstanding, Request))) {
                    ++Entry.Requests[Block];
                    Picks.push_back(Request);
                }
            }
        };

        // 1. Finish what's already started
        for (auto& [Piece, Entry] : _Partial) {
            if (PeerHas.Get(Piece) && _Wanted.Get(Piece)) { TakeFrom(Piece, Entry, false); }
        }

        // 2. Start new pieces, rarest first
        while (static_cast<int>(Picks.size()) < Count) {
            int Best          = -1;
            int BestAvailable = INT_MAX;
            const int Offset  = static_cast<int>(_Random() % static_cast<std::uint32_t>(_PieceCount));
            for (int Step = 0; Step < _PieceCount; ++Step) {
                const int Piece = (Offset + Step) % _PieceCount;
                if (!Needs(Piece) || !PeerHas.Get(Piece) || _Partial.contains(Piece)) { continue; }
                if (const int Available = Availability(Piece); Available < BestAvailable) {
                    Best          = Piece;
                    BestAvailable = Available;
                }
            }
            if (Best < 0) { break; }
            TakeFrom(Best, Start(Best), false);
        }

        // 3. Endgame: everything this peer could give us is already requested elsewhere
        if (Picks.empty()) {
            for (auto& [Piece, Entry] : _Partial) {
                if (PeerHas.Get(Piece) && _Wanted.Get(Piece)) { TakeFrom(Piece, Entry, true); }
            }
        }
        return Picks;
    }

    void PiecePicker::Abort(const BlockRequest& Block) {
        const auto It = _Partial.find(Block.Piece);
        if (It == _Partial.end()) { return; }
        const auto Index = static_cast<std::size_t>(Block.Offset / kBlockSize);
        if (Index >= It->second.Requests.size() || It->second.Received[Index]) { return; }
        if (It->second.Requests[Index] > 0) { --It->second.Requests[Index]; }
    }

    PiecePicker::BlockResult PiecePicker::Received(const BlockRequest& Block) {
        if (Block.Piece < 0 || Block.Piece >= _PieceCount || Block.Offset < 0 || Block.Offset % kBlockSize != 0) {
            return BlockResult::Ignored;
        }
        const auto It = _Partial.find(Block.Piece);
        if (It == _Partial.end()) { return BlockResult::Ignored; }

        Partial& Entry  = It->second;
        const int Index = Block.Offset / kBlockSize;
        if (Index >= static_cast<int>(Entry.Received.size()) || Block.Length != BlockLength(Block.Piece, Index) ||
            Entry.Received[static_cast<std::size_t>(Index)]) {
            return BlockResult::Ignored;
        }

        Entry.Received[static_cast<std::size_t>(Index)] = true;
        ++Entry.ReceivedCount;
        return Entry.ReceivedCount == static_cast<int>(Entry.Received.size()) ? BlockResult::PieceComplete
                                                                              : BlockResult::Accepted;
    }

    void PiecePicker::PieceVerified(const int Piece) {
        _Have.Set(Piece);
        _Partial.erase(Piece);
    }

    void PiecePicker::PieceFailed(const int Piece) {
        _Partial.erase(Piece);
    }
}  // namespace Tori
