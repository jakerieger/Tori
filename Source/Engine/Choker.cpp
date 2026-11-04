#include "Engine/Choker.hpp"

#include <algorithm>
#include <numeric>

namespace Tori {
    Choker::Choker(Settings Config, const std::uint32_t Seed) : _Config(Config), _Random(Seed) {}

    std::vector<bool> Choker::Decide(const std::span<const ChokeCandidate> Candidates,
                                     const bool Seeding,
                                     const Clock::time_point Now) {
        std::vector<bool> Unchoke(Candidates.size(), false);

        std::vector<std::size_t> Regular;
        for (std::size_t I = 0; I < Candidates.size(); ++I) {
            const ChokeCandidate& Peer = Candidates[I];
            if (Peer.Interested && (Seeding || !Peer.Snubbed)) { Regular.push_back(I); }
        }
        std::ranges::sort(Regular, [&](const std::size_t A, const std::size_t B) {
            const ChokeCandidate& Left  = Candidates[A];
            const ChokeCandidate& Right = Candidates[B];
            const double LeftRate       = Seeding ? Left.UploadRate : Left.DownloadRate;
            const double RightRate      = Seeding ? Right.UploadRate : Right.DownloadRate;
            if (LeftRate != RightRate) { return LeftRate > RightRate; }
            return Left.LastUnchoked < Right.LastUnchoked;  // Longest-waiting first
        });
        if (Regular.size() > static_cast<std::size_t>(_Config.Slots)) { Regular.resize(static_cast<std::size_t>(_Config.Slots)); }
        for (const std::size_t I : Regular) { Unchoke[I] = true; }

        // Keep the current optimistic peer until its time is up, unless it left, lost interest, or earned a
        // regular slot
        const auto Current = std::ranges::find_if(Candidates, [&](const ChokeCandidate& Peer) {
            return _Optimistic && Peer.Key == *_Optimistic;
        });
        const bool StillValid = Current != Candidates.end() && Current->Interested &&
                                !Unchoke[static_cast<std::size_t>(Current - Candidates.begin())];

        if (!StillValid || Now >= _NextOptimistic) {
            std::vector<std::size_t> Pool;
            std::vector<int> Weights;
            for (std::size_t I = 0; I < Candidates.size(); ++I) {
                if (!Candidates[I].Interested || Unchoke[I]) { continue; }
                Pool.push_back(I);
                Weights.push_back(Now - Candidates[I].Connected < _Config.NewPeerWindow ? 3 : 1);
            }

            _Optimistic.reset();
            if (!Pool.empty()) {
                std::discrete_distribution<std::size_t> Pick(Weights.begin(), Weights.end());
                _Optimistic = Candidates[Pool[Pick(_Random)]].Key;
            }
            _NextOptimistic = Now + _Config.OptimisticInterval;
        }

        for (std::size_t I = 0; I < Candidates.size(); ++I) {
            if (_Optimistic && Candidates[I].Key == *_Optimistic) { Unchoke[I] = true; }
        }
        return Unchoke;
    }
}  // namespace Tori
