#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <vector>

namespace Tori {
    /// @brief What the choker needs to know about one connected peer.
    struct ChokeCandidate {
        std::uint64_t Key = 0;  // Stable for the life of the connection
        bool Interested   = false;
        bool Snubbed      = false;  // Stopped sending us blocks; earns no regular slot while downloading
        double DownloadRate = 0.0;  // From them, bytes/second
        double UploadRate   = 0.0;  // To them, bytes/second
        std::chrono::steady_clock::time_point Connected;
        std::chrono::steady_clock::time_point LastUnchoked;  // Epoch if never
    };

    /// @brief Decides who gets upload slots: tit-for-tat plus one optimistic unchoke (BEP 3).
    ///
    /// While downloading, the regular slots go to the interested peers that upload to us fastest, skipping snubbed
    /// ones. While seeding they go to the peers we upload to fastest, and ties go to whoever has waited longest, so
    /// equally fast peers take turns. One more "optimistic" slot rotates at random every OptimisticInterval among
    /// the rest, with recently connected peers three times as likely to be picked so they can prove themselves.
    class Choker {
    public:
        using Clock = std::chrono::steady_clock;

        struct Settings {
            int Slots                           = 4;
            Clock::duration OptimisticInterval = std::chrono::seconds(30);
            Clock::duration NewPeerWindow      = std::chrono::seconds(60);
        };

        explicit Choker(Settings Config = {}, std::uint32_t Seed = std::random_device {}());

        /// @brief Returns, for each candidate, whether it should be unchoked.
        std::vector<bool> Decide(std::span<const ChokeCandidate> Candidates, bool Seeding, Clock::time_point Now);

        [[nodiscard]] std::optional<std::uint64_t> Optimistic() const { return _Optimistic; }

    private:
        Settings _Config;
        std::mt19937 _Random;
        std::optional<std::uint64_t> _Optimistic;
        Clock::time_point _NextOptimistic {};
    };
}  // namespace Tori
