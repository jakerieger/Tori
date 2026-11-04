#pragma once

#include "Tracker/TrackerTypes.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Tori {
    /// @brief Performs one blocking announce. Must return promptly once Stop is requested.
    using AnnounceFunction =
      std::function<AnnounceResponse(const std::string& Url, const AnnounceRequest& Request, std::stop_token Stop)>;

    /// @brief Picks HttpAnnounce or UdpAnnounce from the URL's scheme. Throws TrackerError for other schemes.
    AnnounceResponse AnnounceToTracker(const std::string& Url, const AnnounceRequest& Request, std::stop_token Stop);

    struct TransferTotals {
        std::int64_t Uploaded   = 0;
        std::int64_t Downloaded = 0;
        std::int64_t Left       = 0;
    };

    enum class TrackerState {
        NotContacted,
        Announcing,
        Working,
        Failed,
    };

    /// @brief One row of the Trackers tab.
    struct TrackerStatus {
        std::string Url;
        int Tier           = 0;
        TrackerState State = TrackerState::NotContacted;
        std::string Message;  // Error text, or the tracker's warning message
        std::optional<int> Seeders;
        std::optional<int> Leechers;
        std::optional<std::chrono::steady_clock::time_point> NextAnnounce;  // Only for the tracker each tier uses next
    };

    /// @brief Schedules a torrent's tracker announces. Owned and driven by the torrent's worker thread.
    ///
    /// Every tier is announced to independently and concurrently (as qBittorrent does), since many torrents put
    /// each tracker in its own tier. Within a tier, trackers are tried in order and the first that answers moves to
    /// the front (BEP 12). Each announce runs on its own short-lived thread so the torrent's loop never blocks.
    class Announcer {
    public:
        using Clock = std::chrono::steady_clock;

        struct Config {
            InfoHash Hash;
            PeerId Id {};
            std::uint16_t Port  = 0;
            std::uint32_t Key   = 0;
            int NumWant         = 50;
            bool ShuffleTiers   = true;  // BEP 12 asks for this; tests turn it off for a fixed order
            int MaxConcurrent   = 8;     // Announce threads in flight at once
        };

        Announcer(Config Settings,
                  const std::vector<std::vector<std::string>>& Tiers,
                  AnnounceFunction Announce = AnnounceToTracker);
        ~Announcer();

        Announcer(const Announcer&)            = delete;
        Announcer& operator=(const Announcer&) = delete;

        /// @brief Collects finished announces and starts any that are due. Call regularly from the torrent loop.
        void Tick(Clock::time_point Now, const TransferTotals& Totals);

        /// @brief The download finished: tell every tier that has seen "started".
        void Completed(Clock::time_point Now);

        /// @brief Adds tiers, skipping trackers already known. New ones are announced to on the next Tick.
        void AddTrackers(const std::vector<std::vector<std::string>>& Tiers);

        /// @brief Announce again now, except to trackers whose minimum interval hasn't passed.
        void ForceReannounce(Clock::time_point Now);

        /// @brief Begins shutdown: cancels in-flight announces and sends "stopped" to every tier that saw "started".
        /// Keep calling Tick until IsFinished, or give up and destroy the announcer.
        void Stop(Clock::time_point Now);

        [[nodiscard]] bool IsFinished() const;

        /// @brief Peers received since the last call. May contain duplicates across trackers.
        [[nodiscard]] std::vector<Endpoint> TakePeers();

        [[nodiscard]] std::vector<TrackerStatus> Status() const;

    private:
        struct Tracker {
            std::string Url;
            TrackerState State = TrackerState::NotContacted;
            std::string Message;
            std::optional<int> Seeders;
            std::optional<int> Leechers;
            std::string TrackerId;
        };

        struct JobResult {
            std::mutex Mutex;
            bool Done = false;
            std::optional<AnnounceResponse> Response;
            std::string Error;
        };

        struct Job {
            std::shared_ptr<JobResult> Result;
            std::jthread Thread;
            AnnounceEvent Event      = AnnounceEvent::None;
            std::size_t TrackerIndex = 0;
        };

        struct Tier {
            std::vector<Tracker> Trackers;
            std::size_t Current = 0;            // Tracker to use next
            Clock::time_point NextAnnounce {};  // Epoch: as soon as possible
            Clock::time_point EarliestForced {};
            bool Started          = false;  // A "started" announce succeeded and "stopped" hasn't been sent
            bool CompletedPending = false;
            bool StopPending      = false;
            int Failures          = 0;  // Consecutive passes through the whole tier without an answer
            std::optional<Job> InFlight;
        };

        void Collect(Tier& Target, Clock::time_point Now);
        void Launch(Tier& Target, AnnounceEvent Event, const TransferTotals& Totals);
        void OnSuccess(Tier& Target, const Job& Finished, const AnnounceResponse& Response, Clock::time_point Now);
        void OnFailure(Tier& Target, const Job& Finished, const std::string& Error, Clock::time_point Now);
        [[nodiscard]] std::optional<AnnounceEvent> NextEvent(const Tier& Target) const;

        Config _Config;
        AnnounceFunction _Announce;
        std::vector<Tier> _Tiers;
        std::vector<std::string> _KnownUrls;
        std::vector<Endpoint> _Peers;
        bool _Stopping = false;
    };
}  // namespace Tori
