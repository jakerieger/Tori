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

#include "Tracker/Announcer.hpp"

#include "Core/Encoding.hpp"
#include "Core/Random.hpp"
#include "Tracker/HttpTracker.hpp"
#include "Tracker/UdpTracker.hpp"

#include <algorithm>
#include <random>

namespace Tori {
    namespace {
        // After every tracker in a tier fails: 30s, 60s, 2m, ... capped at 30 minutes
        std::chrono::seconds RetryDelay(const int Failures) {
            const int Exponent = std::clamp(Failures - 1, 0, 6);
            return std::min(std::chrono::seconds(30 << Exponent), std::chrono::seconds(30 * 60));
        }
    }  // namespace

    AnnounceResponse
    AnnounceToTracker(const std::string& Url, const AnnounceRequest& Request, const std::stop_token Stop) {
        if (StartsWithIgnoreCase(Url, "http://") || StartsWithIgnoreCase(Url, "https://")) {
            return HttpAnnounce(Url, Request, Stop);
        }
        if (StartsWithIgnoreCase(Url, "udp://")) { return UdpAnnounce(Url, Request, Stop); }
        throw TrackerError("unsupported tracker protocol");
    }

    Announcer::Announcer(Config Settings, const std::vector<std::vector<std::string>>& Tiers, AnnounceFunction Announce)
        : _Config(Settings), _Announce(std::move(Announce)) {
        AddTrackers(Tiers);
    }

    void Announcer::AddTrackers(const std::vector<std::vector<std::string>>& Tiers) {
        if (_Stopping) { return; }
        std::mt19937 Shuffler(RandomU32());

        for (const std::vector<std::string>& Urls : Tiers) {
            Tier Entry;
            for (const std::string& Url : Urls) {
                // The same tracker listed in several tiers would only be announced to twice
                if (Url.empty() || std::ranges::find(_KnownUrls, Url) != _KnownUrls.end()) { continue; }
                _KnownUrls.push_back(Url);
                Entry.Trackers.push_back({.Url = Url});
            }
            if (Entry.Trackers.empty()) { continue; }
            if (_Config.ShuffleTiers) { std::ranges::shuffle(Entry.Trackers, Shuffler); }
            _Tiers.push_back(std::move(Entry));
        }
    }

    // Destroying in-flight jobs requests stop on their threads and joins them
    Announcer::~Announcer() = default;

    void Announcer::Tick(const Clock::time_point Now, const TransferTotals& Totals) {
        for (Tier& Target : _Tiers) {
            Collect(Target, Now);
        }

        int InFlight =
          static_cast<int>(std::ranges::count_if(_Tiers, [](const Tier& T) { return T.InFlight.has_value(); }));
        for (Tier& Target : _Tiers) {
            if (InFlight >= _Config.MaxConcurrent) { break; }
            if (Target.InFlight || Now < Target.NextAnnounce) { continue; }
            if (const std::optional<AnnounceEvent> Event = NextEvent(Target)) {
                Launch(Target, *Event, Totals);
                ++InFlight;
            }
        }
    }

    void Announcer::Completed(const Clock::time_point Now) {
        if (_Stopping) { return; }
        for (Tier& Target : _Tiers) {
            if (!Target.Started) { continue; }
            Target.CompletedPending = true;
            Target.NextAnnounce     = Now;
        }
    }

    void Announcer::ForceReannounce(const Clock::time_point Now) {
        if (_Stopping) { return; }
        for (Tier& Target : _Tiers) {
            Target.NextAnnounce = std::max(Now, Target.EarliestForced);
        }
    }

    void Announcer::Stop(const Clock::time_point Now) {
        if (_Stopping) { return; }
        _Stopping = true;
        for (Tier& Target : _Tiers) {
            Target.StopPending  = Target.Started;
            Target.NextAnnounce = Now;
            if (Target.InFlight) { Target.InFlight->Thread.request_stop(); }
        }
    }

    bool Announcer::IsFinished() const {
        return _Stopping &&
               std::ranges::none_of(_Tiers, [](const Tier& T) { return T.InFlight.has_value() || T.StopPending; });
    }

    std::vector<Endpoint> Announcer::TakePeers() {
        return std::exchange(_Peers, {});
    }

    std::vector<TrackerStatus> Announcer::Status() const {
        std::vector<TrackerStatus> Rows;
        for (std::size_t TierIndex = 0; TierIndex < _Tiers.size(); ++TierIndex) {
            const Tier& Source = _Tiers[TierIndex];
            for (std::size_t I = 0; I < Source.Trackers.size(); ++I) {
                const Tracker& Entry = Source.Trackers[I];
                Rows.push_back({
                  .Url          = Entry.Url,
                  .Tier         = static_cast<int>(TierIndex),
                  .State        = Entry.State,
                  .Message      = Entry.Message,
                  .Seeders      = Entry.Seeders,
                  .Leechers     = Entry.Leechers,
                  .NextAnnounce = I == Source.Current && !Source.InFlight && !_Stopping
                                    ? std::optional(Source.NextAnnounce)
                                    : std::nullopt,
                });
            }
        }
        return Rows;
    }

    std::optional<AnnounceEvent> Announcer::NextEvent(const Tier& Target) const {
        if (_Stopping) { return Target.StopPending ? std::optional(AnnounceEvent::Stopped) : std::nullopt; }
        if (!Target.Started) { return AnnounceEvent::Started; }
        if (Target.CompletedPending) { return AnnounceEvent::Completed; }
        return AnnounceEvent::None;
    }

    void Announcer::Launch(Tier& Target, const AnnounceEvent Event, const TransferTotals& Totals) {
        Tracker& Destination = Target.Trackers[Target.Current];
        Destination.State    = TrackerState::Announcing;

        AnnounceRequest Request {
          .Hash       = _Config.Hash,
          .Id         = _Config.Id,
          .Port       = _Config.Port,
          .Uploaded   = Totals.Uploaded,
          .Downloaded = Totals.Downloaded,
          .Left       = Totals.Left,
          .Event      = Event,
          .NumWant    = Event == AnnounceEvent::Stopped ? 0 : _Config.NumWant,
          .Key        = _Config.Key,
          .TrackerId  = Destination.TrackerId,
        };

        auto Result = std::make_shared<JobResult>();
        std::jthread Thread([Result, Request = std::move(Request), Url = Destination.Url, Announce = _Announce](
                              const std::stop_token Stop) {
            std::optional<AnnounceResponse> Response;
            std::string Error;
            try {
                Response = Announce(Url, Request, Stop);
            } catch (const std::exception& Ex) { Error = Ex.what(); }

            std::scoped_lock Lock(Result->Mutex);
            Result->Response = std::move(Response);
            Result->Error    = std::move(Error);
            Result->Done     = true;
        });

        Target.InFlight = Job {
          .Result       = std::move(Result),
          .Thread       = std::move(Thread),
          .Event        = Event,
          .TrackerIndex = Target.Current,
        };
    }

    void Announcer::Collect(Tier& Target, const Clock::time_point Now) {
        if (!Target.InFlight) { return; }
        {
            std::scoped_lock Lock(Target.InFlight->Result->Mutex);
            if (!Target.InFlight->Result->Done) { return; }
        }

        // The thread has published its result, so joining it here is immediate
        const Job Finished = std::move(*Target.InFlight);
        Target.InFlight.reset();

        if (Finished.Result->Response) {
            OnSuccess(Target, Finished, *Finished.Result->Response, Now);
        } else {
            OnFailure(Target, Finished, Finished.Result->Error, Now);
        }
    }

    void Announcer::OnSuccess(Tier& Target,
                              const Job& Finished,
                              const AnnounceResponse& Response,
                              const Clock::time_point Now) {
        Tracker& Source = Target.Trackers[Finished.TrackerIndex];
        Source.State    = TrackerState::Working;
        Source.Message  = Response.WarningMessage;
        Source.Seeders  = Response.Seeders;
        Source.Leechers = Response.Leechers;
        if (!Response.TrackerId.empty()) { Source.TrackerId = Response.TrackerId; }

        switch (Finished.Event) {
            case AnnounceEvent::Started:
                Target.Started = true;
                break;
            case AnnounceEvent::Completed:
                Target.CompletedPending = false;
                break;
            case AnnounceEvent::Stopped:
                Target.Started     = false;
                Target.StopPending = false;
                break;
            case AnnounceEvent::None:
                break;
        }

        // BEP 12: a tracker that answers moves to the front of its tier
        std::rotate(Target.Trackers.begin(),
                    Target.Trackers.begin() + static_cast<std::ptrdiff_t>(Finished.TrackerIndex),
                    Target.Trackers.begin() + static_cast<std::ptrdiff_t>(Finished.TrackerIndex) + 1);
        Target.Current  = 0;
        Target.Failures = 0;

        if (_Stopping) {
            // An announce that was already running when Stop() was called; the tier may now owe a "stopped"
            if (Finished.Event != AnnounceEvent::Stopped) { Target.StopPending = Target.Started; }
            Target.NextAnnounce = Now;
            return;
        }

        _Peers.insert(_Peers.end(), Response.Peers.begin(), Response.Peers.end());
        Target.NextAnnounce   = Now + std::chrono::seconds(Response.IntervalSeconds);
        Target.EarliestForced = Now + std::chrono::seconds(Response.MinIntervalSeconds.value_or(0));
    }

    void
    Announcer::OnFailure(Tier& Target, const Job& Finished, const std::string& Error, const Clock::time_point Now) {
        Tracker& Source = Target.Trackers[Finished.TrackerIndex];

        if (_Stopping) {
            // Cancelled by Stop(), or the "stopped" announce itself failed; either way don't retry
            Source.State = TrackerState::NotContacted;
            if (Finished.Event == AnnounceEvent::Stopped) { Target.StopPending = false; }
            Target.NextAnnounce = Now;
            return;
        }

        Source.State   = TrackerState::Failed;
        Source.Message = Error;

        if (Target.Current + 1 < Target.Trackers.size()) {
            ++Target.Current;
            Target.NextAnnounce = Now;
        } else {
            Target.Current = 0;
            ++Target.Failures;
            Target.NextAnnounce = Now + RetryDelay(Target.Failures);
        }
    }
}  // namespace Tori
