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

#include "Engine/Torrent.hpp"

#include "Core/RateMeter.hpp"
#include "Core/Random.hpp"
#include "Core/Sha1.hpp"
#include "Engine/Choker.hpp"
#include "Engine/PiecePicker.hpp"
#include "Peer/Extensions.hpp"
#include "Peer/PeerConnection.hpp"
#include "Storage/FileStorage.hpp"
#include "Storage/PieceVerifier.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <tuple>

namespace Tori {
    namespace {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        constexpr auto kPollTimeout        = 50ms;
        constexpr auto kStatsInterval      = 250ms;
        constexpr auto kConnectTimeout     = 10s;
        constexpr auto kHandshakeTimeout   = 20s;
        constexpr auto kInactivityTimeout  = 150s;
        constexpr auto kKeepAliveInterval  = 90s;
        constexpr auto kRequestTimeout     = 30s;  // No block for this long: requests go back to the picker
        constexpr auto kSnubPenalty        = 30s;  // ...and that peer isn't asked again for this long
        constexpr auto kChokeInterval      = 10s;
        constexpr auto kUselessTimeout     = 2min;  // Neither side has wanted anything for this long: hang up
        constexpr auto kRetryAfterFailure  = 5min;
        constexpr auto kRetryAfterDrop     = 2min;
        constexpr auto kAnnouncerShutdown  = 5s;
        constexpr auto kMetadataTimeout    = 10s;   // Unanswered ut_metadata requests are forgotten after this
        constexpr auto kDhtLookupHungry    = 2min;  // While fetching metadata or short of peers
        constexpr auto kDhtLookupSatisfied = 15min;
        constexpr auto kResumeInterval     = 60s;  // Routine changes (transfer totals) are saved this often
        constexpr auto kDetailsInterval    = 1s;

        constexpr int kIncomingSlack             = 5;  // Incoming peers accepted beyond MaxPeers
        constexpr int kHungryPeerCount           = 10;
        constexpr int kMetadataRequestsPerPeer   = 2;
        constexpr int kMaxMetadataRejects        = 3;
        constexpr std::size_t kMaxCandidates     = 2000;
        constexpr std::size_t kMaxQueuedRequests = 256;
        constexpr std::size_t kMaxPendingHaves   = 100000;
        constexpr std::size_t kServeBufferLimit  = 512 * 1024;

        // What we report as "left" to trackers before the size is known; 0 would make us look like a seed
        constexpr std::int64_t kUnknownLeft = 16 * 1024;
    }  // namespace

    class TorrentWorker {
    public:
        TorrentWorker(Torrent& Owner, const std::stop_token Stop)
            : _Owner(Owner), _Params(Owner._Params), _Stop(Stop), _Assembler(Owner._Params.Hash),
              _Choker({.Slots = Owner._Params.Limits.UploadSlots}), _Key(RandomU32()) {
            if (_Params.Meta) { _Meta = std::make_shared<const Metainfo>(*_Params.Meta); }
            if (_Params.Resume) {
                _PriorUploaded   = _Params.Resume->Uploaded;
                _PriorDownloaded = _Params.Resume->Downloaded;
            }
            _SkippedFiles = _Params.SkippedFiles;
        }

        void Run() {
            EnsureWinsock();
            _Paused = _Params.StartPaused;

            try {
                if (_Meta) {
                    SetUpData();
                    if (!TryFastResume() && !CheckFiles()) {
                        Shutdown();
                        return;
                    }
                }
                if (_Paused) {
                    _State = TorrentState::Paused;
                } else {
                    StartRunning(Clock::now());
                }
            } catch (const StorageError& Ex) { EnterError(Ex.what(), Clock::now()); }

            while (!_Stop.stop_requested()) {
                const Clock::time_point Now = Clock::now();
                try {
                    ProcessCommands(Now);
                    if (IsRunning()) {
                        if (_Announcer) {
                            _Announcer->Tick(Now, Totals());
                            for (const Endpoint& Peer : _Announcer->TakePeers()) {
                                AddCandidate(Peer);
                            }
                        }
                        if (Now >= _NextHousekeeping) {
                            Housekeeping(Now);
                            _NextHousekeeping = Now + 1s;
                        }
                    }
                    TickRetiring(Now);
                    Poll(Now);
                    RemoveDropped(Now);
                    if (_ResumeUrgent || Now >= _NextResume) { SaveResume(Now); }
                    if (Now >= _NextDetails && _Owner.DetailsWanted()) {
                        PublishDetails(Now);
                        _NextDetails = Now + kDetailsInterval;
                    }
                } catch (const StorageError& Ex) { EnterError(Ex.what(), Clock::now()); }

                if (Now >= _NextStats) {
                    Publish(Now);
                    _NextStats = Now + kStatsInterval;
                }
            }
            Shutdown();
        }

    private:
        struct Peer {
            std::unique_ptr<PeerConnection> Connection;
            Endpoint Address;
            std::uint64_t Key = 0;  // Identifies the connection to the choker
            Clock::time_point Created;
            Clock::time_point LastUnchoked {};
            Clock::time_point LastUseful;  // Last time either side was interested
            Clock::time_point LastReceived;
            Clock::time_point LastSent;
            Clock::time_point WaitingSince;  // Since the last block arrived, while requests are outstanding
            Clock::time_point SnubbedUntil {};

            bool Active  = false;  // Handshake accepted
            bool Dropped = false;  // Removed at the end of the loop pass
            PeerId Id {};
            bool SupportsExtensions = false;
            bool SupportsDht        = false;
            bool Incoming           = false;
            std::string Client;  // From their extension handshake, else their peer ID

            bool AmChoking      = true;
            bool AmInterested   = false;
            bool PeerChoking    = true;
            bool PeerInterested = false;

            Bitfield Has;  // Sized once the metadata is known
            bool ReceivedBitfield = false;
            int Interesting       = 0;  // Pieces they have that we want and don't have
            std::vector<BlockRequest> Outstanding;
            std::deque<BlockRequest> Requests;  // Theirs, waiting to be served

            // Availability that arrived before we had the metadata, applied once we do
            std::vector<std::uint8_t> PendingBitfield;
            std::vector<std::uint32_t> PendingHaves;

            std::uint8_t UtMetadataId = 0;  // Their ID for ut_metadata, 0 if unsupported
            std::int64_t MetadataSize = 0;
            int MetadataRequests      = 0;
            int MetadataRejects       = 0;
            Clock::time_point LastMetadataRequest {};

            RateMeter Down;
            RateMeter Up;
        };

        // ---- Lifecycle -----------------------------------------------------------------------------------------

        [[nodiscard]] bool IsRunning() const {
            return _State == TorrentState::Downloading || _State == TorrentState::Seeding ||
                   _State == TorrentState::FetchingMetadata;
        }

        [[nodiscard]] bool DhtAllowed() const {
            return _Params.FindPeers && !(_Meta && _Meta->IsPrivate);  // BEP 27: private torrents use trackers only
        }

        [[nodiscard]] int PieceCount() const { return _Picker ? _Picker->PieceCount() : 0; }

        void SetUpData() {
            _Storage.emplace(_Params.Destination, _Meta->Files);
            _Picker.emplace(_Meta->PieceCount(), _Meta->PieceLength, _Meta->TotalSize);
            ApplyWanted();
        }

        /// Marks the pieces that overlap any file the user wants, and recounts what's left of them.
        void ApplyWanted() {
            std::vector<bool> Skipped(_Meta->Files.size(), false);
            for (const int Index : _SkippedFiles) {
                if (Index >= 0 && static_cast<std::size_t>(Index) < Skipped.size()) {
                    Skipped[static_cast<std::size_t>(Index)] = true;
                }
            }

            Bitfield Wanted(PieceCount());
            for (std::size_t I = 0; I < _Meta->Files.size(); ++I) {
                const TorrentFile& File = _Meta->Files[I];
                if (Skipped[I] || File.IsPadding || File.Length == 0) { continue; }
                const auto First = static_cast<int>(File.Offset / _Meta->PieceLength);
                const auto Last  = static_cast<int>((File.Offset + File.Length - 1) / _Meta->PieceLength);
                for (int Piece = First; Piece <= Last; ++Piece) {
                    Wanted.Set(Piece);
                }
            }
            _Picker->SetWanted(Wanted);
            RecountProgress();
        }

        void RecountProgress() {
            _VerifiedBytes = 0;
            _WantedBytes   = 0;
            _WantedDone    = 0;
            for (int Piece = 0; Piece < PieceCount(); ++Piece) {
                const std::int64_t Size = _Picker->PieceSize(Piece);
                const bool Have         = _Picker->Have().Get(Piece);
                if (Have) { _VerifiedBytes += Size; }
                if (_Picker->Wanted(Piece)) {
                    _WantedBytes += Size;
                    if (Have) { _WantedDone += Size; }
                }
            }
        }

        bool CheckFiles() {
            _State        = TorrentState::Checking;
            _CheckedBytes = 0;
            _PiecesKnown  = false;
            Publish(Clock::now());

            Clock::time_point LastPublish      = Clock::now();
            const std::optional<Bitfield> Have = VerifyPieces(*_Storage, *_Meta, _Stop, [&](const int Checked, int) {
                _CheckedBytes = std::min(_Meta->TotalSize, static_cast<std::int64_t>(Checked) * _Meta->PieceLength);
                if (Clock::now() - LastPublish >= kStatsInterval) {
                    LastPublish = Clock::now();
                    Publish(LastPublish);
                }
            });
            _Storage->CloseAll();
            if (!Have) { return false; }

            SetHave(*Have);
            _Stamps.clear();  // Taken afresh when next saved
            return true;
        }

        /// Trusts the pieces saved last time if no file has changed since; otherwise the files must be checked.
        bool TryFastResume() {
            if (!_Params.Resume || _Params.Resume->Pieces.empty()) { return false; }
            const ResumeData& Saved            = *_Params.Resume;
            const std::optional<Bitfield> Have = Bitfield::FromBytes(Saved.Pieces, PieceCount());
            if (!Have || Saved.Files.size() != _Meta->Files.size()) { return false; }

            std::vector<FileStamp> Current = StampFiles(_Params.Destination, _Meta->Files);
            if (Current != Saved.Files) { return false; }

            SetHave(*Have);
            _Stamps = std::move(Current);
            return true;
        }

        void SetHave(const Bitfield& Have) {
            _Picker->SetHave(Have);
            _PiecesKnown = true;
            RecountProgress();
        }

        [[nodiscard]] TorrentState RunningState() const {
            if (!_Meta) { return TorrentState::FetchingMetadata; }
            // Seeding once every wanted piece is in, even if files the user skipped are missing
            return _Picker->WantedComplete() ? TorrentState::Seeding : TorrentState::Downloading;
        }

        void StartRunning(const Clock::time_point Now) {
            _State = RunningState();
            _Error.clear();
            _ResumeUrgent  = true;
            _NextChoke     = Now;
            _NextDhtLookup = Now;
            for (const Endpoint& Address : _Params.InitialPeers) {
                AddCandidate(Address);
            }

            if (!_Announcer) {
                Announcer::Config Config;
                Config.Hash = _Params.Hash;
                Config.Id   = _Params.LocalId;
                Config.Port = _Params.ListenPort;
                Config.Key  = _Key;
                _Announcer =
                  std::make_unique<Announcer>(Config,
                                              _Meta ? _Meta->TrackerTiers : std::vector<std::vector<std::string>> {},
                                              _Params.Announce);
                _Announcer->AddTrackers(_Params.ExtraTrackers);
            }
        }

        void StopRunning(const Clock::time_point Now, const TorrentState NewState) {
            for (const auto& Peer : _Peers) {
                Drop(*Peer);
            }
            RemoveDropped(Now);
            RetireAnnouncer(Now);
            _Candidates.clear();
            _CandidateSet.clear();
            _RetryAfter.clear();  // We hung up on them, not the other way round
            _State = NewState;
        }

        void EnterError(std::string Message, const Clock::time_point Now) {
            StopRunning(Now, TorrentState::Error);
            _Error = std::move(Message);
        }

        void Shutdown() {
            StopRunning(Clock::now(), _State);
            while (_RetiringAnnouncer) {
                TickRetiring(Clock::now());
                std::this_thread::sleep_for(20ms);
            }
            if (_Storage) { _Storage->CloseAll(); }
            Publish(Clock::now());
            _ResumeUrgent = true;
            SaveResume(Clock::now());
        }

        void RetireAnnouncer(const Clock::time_point Now) {
            if (!_Announcer) { return; }
            _Announcer->Stop(Now);
            _RetiringAnnouncer = std::move(_Announcer);
            _RetireDeadline    = Now + kAnnouncerShutdown;
        }

        // Keeps a stopped announcer alive just long enough to deliver "stopped" to trackers
        void TickRetiring(const Clock::time_point Now) {
            if (!_RetiringAnnouncer) { return; }
            _RetiringAnnouncer->Tick(Now, Totals());
            if (_RetiringAnnouncer->IsFinished() || Now >= _RetireDeadline) { _RetiringAnnouncer.reset(); }
        }

        [[nodiscard]] TransferTotals Totals() const {
            return {
              .Uploaded   = _Up.Total(),
              .Downloaded = _Down.Total(),
              .Left       = _Meta ? _WantedBytes - _WantedDone : kUnknownLeft,
            };
        }

        /// Publishes what to save if anything changed. Routine changes wait for kResumeInterval; setting
        /// _ResumeUrgent saves on the next loop pass.
        void SaveResume(const Clock::time_point Now) {
            const auto Key =
              std::tuple(_Picker ? _Picker->Have().Count() : -1, _Up.Total(), _Down.Total(), _Paused, _Meta != nullptr);
            _NextResume = Now + kResumeInterval;
            if (!_ResumeUrgent && Key == _LastResumeKey) { return; }
            _ResumeUrgent  = false;
            _LastResumeKey = Key;

            auto Data           = std::make_shared<ResumeData>();
            Data->Hash          = _Params.Hash;
            Data->Name          = _Meta ? _Meta->Name : _Params.DisplayName;
            Data->Destination   = _Params.Destination;
            Data->ExtraTrackers = _Params.ExtraTrackers;
            Data->Paused        = _Paused;
            Data->Uploaded      = _PriorUploaded + _Up.Total();
            Data->Downloaded    = _PriorDownloaded + _Down.Total();
            Data->AddedOn       = _Params.AddedOn;
            Data->SkippedFiles  = _SkippedFiles;

            if (_PiecesKnown) {
                // Closing first makes Windows settle each file's last-write time, so the stamps describe exactly
                // the data the pieces were verified from. Anything written after this changes them.
                if (_WroteSinceStamp || _Stamps.empty()) {
                    _Storage->CloseAll();
                    _Stamps          = StampFiles(_Params.Destination, _Meta->Files);
                    _WroteSinceStamp = false;
                }
                Data->Pieces = _Picker->Have().Bytes();
                Data->Files  = _Stamps;
            } else if (_Params.Resume) {
                // Still checking (or failed to): what was saved before is no less true than it was
                Data->Pieces = _Params.Resume->Pieces;
                Data->Files  = _Params.Resume->Files;
            }
            _Owner.PublishResume(std::move(Data));
        }

        void Publish(const Clock::time_point Now) {
            TorrentStats Stats;
            Stats.Id          = _Params.Id;
            Stats.Hash        = _Params.Hash;
            Stats.Name        = _Meta ? _Meta->Name : _Params.DisplayName;
            Stats.Destination = _Params.Destination;
            if (_State == TorrentState::Checking) {
                Stats.TotalSize      = static_cast<double>(_Meta->TotalSize);
                Stats.DownloadedSize = static_cast<double>(_CheckedBytes);
            } else {
                // Only what the user chose to download counts toward progress
                Stats.TotalSize      = static_cast<double>(_WantedBytes);
                Stats.DownloadedSize = static_cast<double>(_WantedDone);
            }
            Stats.DownloadedTotal = static_cast<double>(_PriorDownloaded + _Down.Total());
            Stats.UploadedTotal   = static_cast<double>(_PriorUploaded + _Up.Total());
            Stats.DownloadSpeed   = _Down.Rate(Now);
            Stats.UploadSpeed     = _Up.Rate(Now);
            Stats.Peers           = ActivePeerCount();
            Stats.State           = _State;
            Stats.ErrorMessage    = _Error;
            _Owner.Publish(std::move(Stats));
        }

        void PublishDetails(const Clock::time_point Now) {
            auto Data = std::make_shared<TorrentDetails>();
            Data->Id  = _Params.Id;

            if (_Meta && _Picker) {
                const Bitfield& Have = _Picker->Have();
                for (std::size_t I = 0; I < _Meta->Files.size(); ++I) {
                    const TorrentFile& File = _Meta->Files[I];
                    if (File.IsPadding) { continue; }
                    FileDetails Entry {.Index = static_cast<int>(I), .Path = File.Path, .Size = File.Length};
                    Entry.Wanted = std::ranges::find(_SkippedFiles, static_cast<int>(I)) == _SkippedFiles.end();
                    if (File.Length > 0) {
                        const std::int64_t End = File.Offset + File.Length;
                        for (auto Piece = static_cast<int>(File.Offset / _Meta->PieceLength);
                             Piece < PieceCount() && static_cast<std::int64_t>(Piece) * _Meta->PieceLength < End;
                             ++Piece) {
                            if (!Have.Get(Piece)) { continue; }
                            const std::int64_t Start = static_cast<std::int64_t>(Piece) * _Meta->PieceLength;
                            Entry.Done +=
                              std::min(End, Start + _Picker->PieceSize(Piece)) - std::max(File.Offset, Start);
                        }
                    }
                    Data->Files.push_back(std::move(Entry));
                }
            }

            for (const auto& Pointer : _Peers) {
                const Peer& Target = *Pointer;
                if (!Target.Active || Target.Dropped) { continue; }
                PeerDetails Entry;
                Entry.Address       = Target.Address.ToString();
                Entry.Client        = Target.Client.empty() ? DescribePeerId(Target.Id) : Target.Client;
                Entry.Progress      = Target.Has.Size() > 0
                                        ? static_cast<float>(Target.Has.Count()) / static_cast<float>(Target.Has.Size())
                                        : 0.0f;
                Entry.DownloadSpeed = Target.Down.Rate(Now);
                Entry.UploadSpeed   = Target.Up.Rate(Now);
                if (Target.AmInterested) { Entry.Flags += Target.PeerChoking ? 'd' : 'D'; }
                if (Target.PeerInterested) { Entry.Flags += Target.AmChoking ? 'u' : 'U'; }
                if (Target.Incoming) { Entry.Flags += 'I'; }
                Data->Peers.push_back(std::move(Entry));
            }
            std::ranges::sort(Data->Peers, std::greater {}, [](const PeerDetails& Entry) {
                return Entry.DownloadSpeed + Entry.UploadSpeed;
            });

            if (_Announcer) {
                Data->Trackers = _Announcer->Status();
            } else {
                // Paused or stopped: list the trackers without any status
                std::vector<std::vector<std::string>> Tiers =
                  _Meta ? _Meta->TrackerTiers : std::vector<std::vector<std::string>> {};
                Tiers.insert(Tiers.end(), _Params.ExtraTrackers.begin(), _Params.ExtraTrackers.end());
                for (std::size_t Tier = 0; Tier < Tiers.size(); ++Tier) {
                    for (const std::string& Url : Tiers[Tier]) {
                        if (std::ranges::any_of(Data->Trackers,
                                                [&](const TrackerStatus& Known) { return Known.Url == Url; })) {
                            continue;
                        }
                        Data->Trackers.push_back({.Url = Url, .Tier = static_cast<int>(Tier)});
                    }
                }
            }
            _Owner.PublishDetails(std::move(Data));
        }

        [[nodiscard]] int ActivePeerCount() const {
            return static_cast<int>(
              std::ranges::count_if(_Peers, [](const auto& Peer) { return Peer->Active && !Peer->Dropped; }));
        }

        // ---- Commands ------------------------------------------------------------------------------------------

        void ProcessCommands(const Clock::time_point Now) {
            for (Torrent::Command& Item : _Owner.TakeCommands()) {
                if (std::holds_alternative<Torrent::PauseCommand>(Item)) {
                    _Paused       = true;
                    _ResumeUrgent = true;
                    if (IsRunning() || _State == TorrentState::Error) { StopRunning(Now, TorrentState::Paused); }
                } else if (std::holds_alternative<Torrent::ResumeCommand>(Item)) {
                    _Paused = false;
                    if (_State == TorrentState::Paused || _State == TorrentState::Error) { StartRunning(Now); }
                } else if (std::holds_alternative<Torrent::RecheckCommand>(Item)) {
                    if (!_Meta) { continue; }  // Nothing to check against yet
                    StopRunning(Now, TorrentState::Checking);
                    if (!CheckFiles()) { return; }
                    if (_Paused) {
                        _State = TorrentState::Paused;
                    } else {
                        StartRunning(Clock::now());
                    }
                } else if (auto* Skip = std::get_if<Torrent::SkipFilesCommand>(&Item)) {
                    SetSkippedFiles(std::move(Skip->Files), Now);
                } else if (auto* Add = std::get_if<Torrent::AddPeersCommand>(&Item)) {
                    // Peers the user, a magnet link or the DHT names are worth trying right away
                    for (const Endpoint& Address : Add->Peers) {
                        _RetryAfter.erase(Address.Unmapped());
                        AddCandidate(Address);
                    }
                } else if (auto* Incoming = std::get_if<Torrent::IncomingCommand>(&Item)) {
                    AcceptIncoming(*Incoming, Now);
                }
            }
        }

        void SetSkippedFiles(std::vector<int> Files, const Clock::time_point Now) {
            std::ranges::sort(Files);
            Files.erase(std::ranges::unique(Files).begin(), Files.end());
            _SkippedFiles = std::move(Files);
            _ResumeUrgent = true;
            if (!_Picker) { return; }  // Applied when the metadata arrives

            ApplyWanted();
            for (const auto& Pointer : _Peers) {
                Peer& Target = *Pointer;
                if (!Target.Active || Target.Dropped || Target.Has.Size() != PieceCount()) { continue; }
                CountInteresting(Target);
            }

            if (_State != TorrentState::Downloading && _State != TorrentState::Seeding) { return; }
            const TorrentState Next = RunningState();
            if (Next == TorrentState::Seeding && _State == TorrentState::Downloading) {
                OnCompleted(Now);
            } else {
                const bool Reopened = _State == TorrentState::Seeding && Next == TorrentState::Downloading;
                _State              = Next;
                for (const auto& Pointer : _Peers) {
                    if (Pointer->Active && !Pointer->Dropped) { UpdateInterest(*Pointer, Now); }
                }
                if (Reopened) { FindPeersAgain(Now); }
            }
        }

        /// Seeding hung up on other seeds; now that there's something to download again, they're worth calling
        /// back, and trackers and the DHT worth asking.
        void FindPeersAgain(const Clock::time_point Now) {
            for (auto It = _RetryAfter.begin(); It != _RetryAfter.end();) {
                if (It->second == Clock::time_point::max()) {  // Ourselves
                    ++It;
                    continue;
                }
                AddCandidate(It->first);
                It = _RetryAfter.erase(It);
            }
            if (_Announcer) { _Announcer->ForceReannounce(Now); }
            _NextDhtLookup = Now;
        }

        // ---- Connections ---------------------------------------------------------------------------------------

        void AddCandidate(const Endpoint& Address) {
            const Endpoint Plain = Address.Unmapped();
            if (Plain.Port == 0 || _CandidateSet.size() >= kMaxCandidates || _CandidateSet.contains(Plain)) { return; }
            _CandidateSet.insert(Plain);
            _Candidates.push_back(Plain);
        }

        [[nodiscard]] bool ConnectedTo(const Endpoint& Address) const {
            return std::ranges::any_of(_Peers, [&](const auto& Peer) { return Peer->Address == Address; });
        }

        [[nodiscard]] Handshake LocalHandshake() const {
            Handshake Local {.Hash = _Params.Hash, .Id = _Params.LocalId};
            Local.Reserved[kExtensionReservedByte] |= kExtensionReservedBit;
            if (DhtAllowed()) { Local.Reserved[kDhtReservedByte] |= kDhtReservedBit; }
            return Local;
        }

        std::unique_ptr<Peer> NewPeer(std::unique_ptr<PeerConnection> Connection, const Clock::time_point Now) {
            auto Created          = std::make_unique<Peer>();
            Created->Address      = Connection->Remote();
            Created->Connection   = std::move(Connection);
            Created->Key          = ++_NextPeerKey;
            Created->Created      = Now;
            Created->LastUseful   = Now;
            Created->LastReceived = Now;
            Created->LastSent     = Now;
            Created->Connection->Send(EncodeHandshake(LocalHandshake()));
            return Created;
        }

        void ConnectToCandidates(const Clock::time_point Now) {
            int Connecting = static_cast<int>(
              std::ranges::count_if(_Peers, [](const auto& Peer) { return !Peer->Connection->IsConnected(); }));
            int Total = static_cast<int>(_Peers.size());

            while (Total < _Params.Limits.MaxPeers && Connecting < _Params.Limits.MaxHalfOpen && !_Candidates.empty()) {
                const Endpoint Address = _Candidates.front();
                _Candidates.pop_front();
                _CandidateSet.erase(Address);

                if (ConnectedTo(Address)) { continue; }
                if (const auto Retry = _RetryAfter.find(Address); Retry != _RetryAfter.end() && Now < Retry->second) {
                    continue;
                }
                std::unique_ptr<PeerConnection> Connection = PeerConnection::Connect(Address);
                if (!Connection) { continue; }

                _Peers.push_back(NewPeer(std::move(Connection), Now));
                ++Connecting;
                ++Total;
            }
        }

        void AcceptIncoming(Torrent::IncomingCommand& Incoming, const Clock::time_point Now) {
            if (!IsRunning() || static_cast<int>(_Peers.size()) >= _Params.Limits.MaxPeers + kIncomingSlack) { return; }
            const Endpoint Remote = Incoming.Remote.Unmapped();
            if (ConnectedTo(Remote)) { return; }

            _Peers.push_back(
              NewPeer(PeerConnection::Accept(std::move(Incoming.Socket), Remote, Incoming.Received), Now));
            _Peers.back()->Incoming = true;
            OnHandshake(*_Peers.back(), Now);
        }

        void OnHandshake(Peer& Target, const Clock::time_point Now) {
            const Handshake& Received = Target.Connection->RemoteHandshake();
            if (Received.Hash != _Params.Hash) { return Drop(Target); }
            if (Received.Id == _Params.LocalId) {
                _RetryAfter[Target.Address] = Clock::time_point::max();  // Our own listen address
                return Drop(Target);
            }
            const bool Duplicate = std::ranges::any_of(_Peers, [&](const auto& Other) {
                return Other.get() != &Target && Other->Active && !Other->Dropped && Other->Id == Received.Id;
            });
            if (Duplicate) { return Drop(Target); }

            Target.Id                 = Received.Id;
            Target.Active             = true;
            Target.SupportsExtensions = (Received.Reserved[kExtensionReservedByte] & kExtensionReservedBit) != 0;
            Target.SupportsDht        = (Received.Reserved[kDhtReservedByte] & kDhtReservedBit) != 0;
            if (_Picker) { Target.Has = Bitfield(_Picker->PieceCount()); }

            if (_Picker && !_Picker->Have().None()) {
                ByteWriter Out;
                EncodeBitfield(Out, _Picker->Have().Bytes());
                SendTo(Target, Out, Now);
            }
            if (Target.SupportsExtensions) {
                ByteWriter Out;
                const std::optional<std::int64_t> MetadataSize =
                  _Meta ? std::optional(static_cast<std::int64_t>(_Meta->InfoBytes.size())) : std::nullopt;
                EncodeExtended(Out, kExtensionHandshakeId, EncodeExtensionHandshake(MetadataSize, _Params.ListenPort));
                SendTo(Target, Out, Now);
            }
            if (Target.SupportsDht && DhtAllowed()) {
                ByteWriter Out;
                EncodePort(Out, _Params.ListenPort);
                SendTo(Target, Out, Now);
            }
        }

        static void Drop(Peer& Target) { Target.Dropped = true; }

        void RemoveDropped(const Clock::time_point Now) {
            for (auto It = _Peers.begin(); It != _Peers.end();) {
                Peer& Target = **It;
                if (!Target.Dropped) {
                    ++It;
                    continue;
                }
                AbortRequests(Target);
                if (Target.Active && _Picker && Target.Has.Size() == _Picker->PieceCount()) {
                    _Picker->RemovePeer(Target.Has);
                }

                Clock::time_point& Retry = _RetryAfter[Target.Address];
                Retry = std::max(Retry, Now + (Target.Active ? kRetryAfterDrop : kRetryAfterFailure));
                It    = _Peers.erase(It);
            }
        }

        // ---- I/O -----------------------------------------------------------------------------------------------

        void Poll(const Clock::time_point Now) {
            if (_Peers.empty()) {
                std::this_thread::sleep_for(kPollTimeout);
                return;
            }

            std::vector<WSAPOLLFD> Fds;
            Fds.reserve(_Peers.size());
            for (const auto& Target : _Peers) {
                Fds.push_back({Target->Connection->Socket(), Target->Connection->PollEvents(), 0});
            }
            if (WSAPoll(Fds.data(), static_cast<ULONG>(Fds.size()), static_cast<INT>(kPollTimeout.count())) <= 0) {
                return;
            }

            for (std::size_t I = 0; I < Fds.size(); ++I) {
                if (Fds[I].revents == 0) { continue; }
                Peer& Target = *_Peers[I];
                if (Target.Dropped) { continue; }
                if (!Target.Connection->OnPoll(Fds[I].revents)) {
                    Drop(Target);
                    continue;
                }
                HandleInput(Target, Now);
            }
        }

        void SendTo(Peer& Target, const ByteWriter& Out, const Clock::time_point Now) {
            Target.Connection->Send(Out.Data());
            Target.LastSent = Now;
        }

        void HandleInput(Peer& Target, const Clock::time_point Now) {
            if (!Target.Active) {
                if (!Target.Connection->HasHandshake()) { return; }
                OnHandshake(Target, Now);
                if (Target.Dropped) { return; }
            }

            try {
                while (std::optional<PeerMessage> Message = Target.Connection->NextMessage()) {
                    Target.LastReceived = Now;
                    HandleMessage(Target, *Message, Now);
                    // Metadata may have just arrived and been checked, which can take a while and changes state
                    if (Target.Dropped || !IsRunning()) { return; }
                }
            } catch (const ParseError&) { return Drop(Target); }

            RequestMore(Target, Now);
            ServeRequests(Target, Now);
        }

        void HandleMessage(Peer& Target, const PeerMessage& Message, const Clock::time_point Now) {
            if (Message.IsKeepAlive) { return; }

            switch (Message.Id) {
                case MessageId::Choke:
                    Target.PeerChoking = true;
                    AbortRequests(Target);  // Without the fast extension, a choke discards everything pending
                    break;
                case MessageId::Unchoke:
                    Target.PeerChoking = false;
                    break;
                case MessageId::Interested:
                    Target.PeerInterested = true;
                    // Don't make a new peer wait for the next choke round if a slot is free
                    if (Target.AmChoking && _Picker && UnchokedCount() < _Params.Limits.UploadSlots) {
                        SetChoked(Target, false, Now);
                    }
                    break;
                case MessageId::NotInterested:
                    Target.PeerInterested = false;
                    break;
                case MessageId::Have:
                    OnHave(Target, Message.Index, Now);
                    break;
                case MessageId::Bitfield:
                    if (Target.ReceivedBitfield || Target.Has.Count() > 0) {
                        return Drop(Target);
                    }  // Must come first, once
                    Target.ReceivedBitfield = true;
                    if (!_Picker) {
                        Target.PendingBitfield = Message.Payload;
                    } else if (!ApplyBitfield(Target, Message.Payload)) {
                        return Drop(Target);
                    } else {
                        if (IsSeedPair(Target)) { return Drop(Target); }
                        UpdateInterest(Target, Now);
                    }
                    break;
                case MessageId::Request:
                    OnRequest(Target, Message);
                    break;
                case MessageId::Piece:
                    OnBlock(Target, Message, Now);
                    break;
                case MessageId::Cancel:
                    std::erase(Target.Requests,
                               BlockRequest {static_cast<int>(Message.Index),
                                             static_cast<int>(Message.Begin),
                                             static_cast<int>(Message.Length)});
                    break;
                case MessageId::Port:
                    if (DhtAllowed() && _Params.AddDhtNode && Message.Port != 0) {
                        Endpoint Node = Target.Address;
                        Node.Port     = Message.Port;
                        _Params.AddDhtNode(Node);
                    }
                    break;
                case MessageId::Extended:
                    OnExtended(Target, Message, Now);
                    break;
            }
        }

        // ---- Availability --------------------------------------------------------------------------------------

        [[nodiscard]] bool IsSeedPair(const Peer& Target) const {
            return _State == TorrentState::Seeding && Target.Has.Size() > 0 && Target.Has.All();
        }

        void OnHave(Peer& Target, const std::uint32_t Index, const Clock::time_point Now) {
            if (!_Picker) {
                if (Target.PendingHaves.size() < kMaxPendingHaves) { Target.PendingHaves.push_back(Index); }
                return;
            }
            if (Index >= static_cast<std::uint32_t>(PieceCount())) { return Drop(Target); }

            const int Piece = static_cast<int>(Index);
            if (!Target.Has.Get(Piece)) {
                Target.Has.Set(Piece);
                _Picker->PeerHasPiece(Piece);
                if (_Picker->Needs(Piece)) {
                    ++Target.Interesting;
                    UpdateInterest(Target, Now);
                }
            }
            if (IsSeedPair(Target)) { Drop(Target); }
        }

        bool ApplyBitfield(Peer& Target, const std::vector<std::uint8_t>& Bytes) {
            std::optional<Bitfield> Bits = Bitfield::FromBytes(Bytes, PieceCount());
            if (!Bits) { return false; }
            Target.Has = std::move(*Bits);
            _Picker->AddPeer(Target.Has);

            CountInteresting(Target);
            return true;
        }

        void CountInteresting(Peer& Target) const {
            Target.Interesting = 0;
            for (int Piece = 0; Piece < PieceCount(); ++Piece) {
                if (Target.Has.Get(Piece) && _Picker->Needs(Piece)) { ++Target.Interesting; }
            }
        }

        /// Replays a peer's bitfield and haves that arrived before we knew how many pieces there are.
        void ApplyPendingAvailability(Peer& Target, const Clock::time_point Now) {
            Target.Has         = Bitfield(PieceCount());
            Target.Interesting = 0;
            if (!Target.PendingBitfield.empty() && !ApplyBitfield(Target, Target.PendingBitfield)) {
                return Drop(Target);
            }
            for (const std::uint32_t Index : Target.PendingHaves) {
                if (Index >= static_cast<std::uint32_t>(PieceCount())) { return Drop(Target); }
                const int Piece = static_cast<int>(Index);
                if (Target.Has.Get(Piece)) { continue; }
                Target.Has.Set(Piece);
                _Picker->PeerHasPiece(Piece);
                if (_Picker->Needs(Piece)) { ++Target.Interesting; }
            }
            Target.PendingBitfield.clear();
            Target.PendingHaves.clear();

            if (IsSeedPair(Target)) { return Drop(Target); }
            UpdateInterest(Target, Now);
        }

        // ---- Metadata (BEP 9) ----------------------------------------------------------------------------------

        void OnExtended(Peer& Target, const PeerMessage& Message, const Clock::time_point Now) {
            try {
                if (Message.ExtendedId == kExtensionHandshakeId) {
                    const ExtensionHandshake Handshake = DecodeExtensionHandshake(Message.Payload);
                    Target.UtMetadataId                = Handshake.UtMetadataId;
                    Target.MetadataSize                = Handshake.MetadataSize;
                    Target.Client                      = Handshake.Client.substr(0, 64);
                    RequestMetadata(Target, Now);
                    return;
                }
                if (Message.ExtendedId != kLocalUtMetadataId) { return; }  // Extensions we never advertised

                const MetadataMessage Metadata = DecodeMetadataMessage(Message.Payload);
                switch (Metadata.Type) {
                    case MetadataMessageType::Request:
                        ServeMetadata(Target, Metadata.Piece, Now);
                        break;
                    case MetadataMessageType::Data:
                        if (_Meta) { break; }
                        Target.MetadataRequests = std::max(0, Target.MetadataRequests - 1);
                        _Assembler.Received(Metadata.Piece, Metadata.Data);
                        if (std::optional<std::string> Info = _Assembler.TryComplete()) {
                            OnMetadata(std::move(*Info), Now);
                        } else {
                            RequestMetadata(Target, Now);
                        }
                        break;
                    case MetadataMessageType::Reject:
                        if (_Meta) { break; }
                        Target.MetadataRequests = std::max(0, Target.MetadataRequests - 1);
                        ++Target.MetadataRejects;
                        _Assembler.Rejected(Metadata.Piece);
                        break;
                }
            } catch (const ParseError&) { Drop(Target); }
        }

        void RequestMetadata(Peer& Target, const Clock::time_point Now) {
            if (_Meta || Target.Dropped || Target.UtMetadataId == 0 || Target.MetadataRejects >= kMaxMetadataRejects) {
                return;
            }
            // The first plausible size wins; peers that disagree with it aren't asked
            if (!_Assembler.OfferSize(Target.MetadataSize)) { return; }

            while (Target.MetadataRequests < kMetadataRequestsPerPeer) {
                const std::optional<int> Piece = _Assembler.NextRequest(Now);
                if (!Piece) { break; }
                ByteWriter Out;
                EncodeExtended(Out, Target.UtMetadataId, EncodeMetadataRequest(*Piece));
                SendTo(Target, Out, Now);
                ++Target.MetadataRequests;
                Target.LastMetadataRequest = Now;
            }
        }

        void ServeMetadata(Peer& Target, const int Piece, const Clock::time_point Now) {
            if (Target.UtMetadataId == 0) { return; }

            ByteWriter Out;
            const auto Size = _Meta ? static_cast<std::int64_t>(_Meta->InfoBytes.size()) : 0;
            if (!_Meta || Piece >= MetadataPieceCount(Size)) {
                EncodeExtended(Out, Target.UtMetadataId, EncodeMetadataReject(Piece));
            } else {
                const std::int64_t Offset = static_cast<std::int64_t>(Piece) * kMetadataPieceSize;
                const auto Length = static_cast<std::size_t>(std::min<std::int64_t>(kMetadataPieceSize, Size - Offset));
                const auto* Data  = reinterpret_cast<const std::uint8_t*>(_Meta->InfoBytes.data()) + Offset;
                EncodeExtended(Out, Target.UtMetadataId, EncodeMetadataData(Piece, Size, std::span(Data, Length)));
            }
            SendTo(Target, Out, Now);
        }

        void OnMetadata(std::string InfoBytes, const Clock::time_point Now) {
            Metainfo Parsed;
            try {
                Parsed = Metainfo::ParseInfoDictionary(InfoBytes);
            } catch (const ParseError& Ex) {
                // The hash matched, so this really is the torrent's metadata; it's just unusable
                return EnterError(std::string("invalid metadata: ") + Ex.what(), Now);
            }
            Parsed.TrackerTiers = _Params.ExtraTrackers;
            _Meta               = std::make_shared<const Metainfo>(std::move(Parsed));
            _Owner.SetMetadata(_Meta);

            // Data may already be in the destination, e.g. when a finished download's magnet is added again
            SetUpData();
            if (!CheckFiles()) { return; }  // Stop requested
            _State = RunningState();

            for (const auto& Target : _Peers) {
                if (!Target->Active || Target->Dropped) { continue; }
                ApplyPendingAvailability(*Target, Now);
                if (Target->Dropped) { continue; }
                // Too late for a bitfield, so announce whatever the check found piece by piece
                ByteWriter Out;
                for (int Piece = 0; Piece < PieceCount(); ++Piece) {
                    if (_Picker->Have().Get(Piece)) { EncodeHave(Out, static_cast<std::uint32_t>(Piece)); }
                }
                if (!Out.Data().empty()) { SendTo(*Target, Out, Now); }
            }
            _NextChoke    = Now;
            _ResumeUrgent = true;
        }

        // ---- Downloading ---------------------------------------------------------------------------------------

        void UpdateInterest(Peer& Target, const Clock::time_point Now) {
            const bool Want = Target.Interesting > 0 && _State == TorrentState::Downloading;
            if (Want == Target.AmInterested) { return; }
            Target.AmInterested = Want;
            ByteWriter Out;
            EncodeSimple(Out, Want ? MessageId::Interested : MessageId::NotInterested);
            SendTo(Target, Out, Now);
        }

        void RequestMore(Peer& Target, const Clock::time_point Now) {
            if (!_Picker || !Target.Active || Target.Dropped || Target.PeerChoking || !Target.AmInterested ||
                _State != TorrentState::Downloading || Now < Target.SnubbedUntil) {
                return;
            }

            // Refill in batches rather than one request per block received
            const int Pipeline = _Params.Limits.RequestPipeline;
            const int Want     = Pipeline - static_cast<int>(Target.Outstanding.size());
            if (Want <= 0 || (!Target.Outstanding.empty() && Want < Pipeline / 4)) { return; }

            const std::vector<BlockRequest> Blocks = _Picker->Pick(Target.Has, Want, Target.Outstanding);
            if (Blocks.empty()) { return; }
            if (Target.Outstanding.empty()) { Target.WaitingSince = Now; }

            ByteWriter Out;
            for (const BlockRequest& Block : Blocks) {
                EncodeRequest(Out, Block);
                Target.Outstanding.push_back(Block);
            }
            SendTo(Target, Out, Now);
        }

        void AbortRequests(Peer& Target) {
            if (_Picker) {
                for (const BlockRequest& Block : Target.Outstanding) {
                    _Picker->Abort(Block);
                }
            }
            Target.Outstanding.clear();
        }

        void OnBlock(Peer& Target, const PeerMessage& Message, const Clock::time_point Now) {
            if (!_Picker || Message.Index >= static_cast<std::uint32_t>(PieceCount())) { return Drop(Target); }

            const BlockRequest Block {static_cast<int>(Message.Index),
                                      static_cast<int>(Message.Begin),
                                      static_cast<int>(Message.Payload.size())};
            std::erase(Target.Outstanding, Block);

            const auto Bytes = static_cast<std::int64_t>(Message.Payload.size());
            Target.Down.Add(Bytes, Now);
            _Down.Add(Bytes, Now);
            Target.WaitingSince = Now;

            const PiecePicker::BlockResult Result = _Picker->Received(Block);
            if (Result == PiecePicker::BlockResult::Ignored) { return; }

            _Storage->Write(static_cast<std::int64_t>(Block.Piece) * _Meta->PieceLength + Block.Offset,
                            Message.Payload);
            _WroteSinceStamp = true;

            // In endgame the same block may still be pending from other peers
            for (const auto& Other : _Peers) {
                if (Other.get() == &Target || std::erase(Other->Outstanding, Block) == 0) { continue; }
                ByteWriter Out;
                EncodeCancel(Out, Block);
                SendTo(*Other, Out, Now);
            }

            if (Result == PiecePicker::BlockResult::PieceComplete) { VerifyPiece(Block.Piece, Now); }
        }

        void VerifyPiece(const int Piece, const Clock::time_point Now) {
            const auto Size = static_cast<std::size_t>(_Picker->PieceSize(Piece));
            _PieceBuffer.resize(Size);
            const bool Read = _Storage->Read(static_cast<std::int64_t>(Piece) * _Meta->PieceLength, _PieceBuffer);
            const std::string_view Data(reinterpret_cast<const char*>(_PieceBuffer.data()), Size);

            if (!Read || Sha1::Hash(Data) != _Meta->PieceHashes[static_cast<std::size_t>(Piece)]) {
                _Picker->PieceFailed(Piece);  // Someone sent bad data; the piece will be downloaded again
                return;
            }

            const bool WasNeeded = _Picker->Needs(Piece);
            _Picker->PieceVerified(Piece);
            _VerifiedBytes += static_cast<std::int64_t>(Size);
            if (_Picker->Wanted(Piece)) { _WantedDone += static_cast<std::int64_t>(Size); }

            ByteWriter Have;
            EncodeHave(Have, static_cast<std::uint32_t>(Piece));
            for (const auto& Other : _Peers) {
                if (!Other->Active || Other->Dropped) { continue; }
                SendTo(*Other, Have, Now);
                if (WasNeeded && Other->Has.Size() > 0 && Other->Has.Get(Piece)) {
                    --Other->Interesting;
                    UpdateInterest(*Other, Now);
                }
            }

            if (_State == TorrentState::Downloading && _Picker->WantedComplete()) { OnCompleted(Now); }
        }

        void OnCompleted(const Clock::time_point Now) {
            _State        = TorrentState::Seeding;
            _ResumeUrgent = true;
            // Reopen files read-only, so other programs can open them freely while we seed
            _Storage->CloseAll();
            // Trackers count completed downloads, so only tell them when we have all of it
            if (_Announcer && _Picker->Have().All()) { _Announcer->Completed(Now); }
            for (const auto& Other : _Peers) {
                if (Other->Active && IsSeedPair(*Other)) {
                    Drop(*Other);
                } else {
                    UpdateInterest(*Other, Now);
                }
            }
        }

        // ---- Uploading -----------------------------------------------------------------------------------------

        void OnRequest(Peer& Target, const PeerMessage& Message) {
            if (!_Picker) { return; }
            if (Message.Index >= static_cast<std::uint32_t>(PieceCount()) || Message.Length == 0 ||
                Message.Length > kMaxRequestLength ||
                static_cast<std::int64_t>(Message.Begin) + Message.Length >
                  _Picker->PieceSize(static_cast<int>(Message.Index))) {
                return Drop(Target);
            }
            if (Target.AmChoking || !_Picker->Have().Get(static_cast<int>(Message.Index)) ||
                Target.Requests.size() >= kMaxQueuedRequests) {
                return;
            }
            Target.Requests.push_back(
              {static_cast<int>(Message.Index), static_cast<int>(Message.Begin), static_cast<int>(Message.Length)});
        }

        void ServeRequests(Peer& Target, const Clock::time_point Now) {
            while (_Storage && !Target.Requests.empty() && Target.Connection->PendingSendBytes() < kServeBufferLimit) {
                const BlockRequest Request = Target.Requests.front();
                Target.Requests.pop_front();

                _BlockBuffer.resize(static_cast<std::size_t>(Request.Length));
                if (!_Storage->Read(static_cast<std::int64_t>(Request.Piece) * _Meta->PieceLength + Request.Offset,
                                    _BlockBuffer)) {
                    continue;
                }
                ByteWriter Out;
                EncodePiece(Out,
                            static_cast<std::uint32_t>(Request.Piece),
                            static_cast<std::uint32_t>(Request.Offset),
                            _BlockBuffer);
                SendTo(Target, Out, Now);
                Target.Up.Add(Request.Length, Now);
                _Up.Add(Request.Length, Now);
            }
        }

        [[nodiscard]] int UnchokedCount() const {
            return static_cast<int>(std::ranges::count_if(_Peers, [](const auto& Peer) {
                return Peer->Active && !Peer->Dropped && !Peer->AmChoking;
            }));
        }

        void SetChoked(Peer& Target, const bool Choked, const Clock::time_point Now) {
            if (Target.AmChoking == Choked) { return; }
            Target.AmChoking = Choked;
            if (Choked) {
                Target.Requests.clear();
            } else {
                Target.LastUnchoked = Now;
            }
            ByteWriter Out;
            EncodeSimple(Out, Choked ? MessageId::Choke : MessageId::Unchoke);
            SendTo(Target, Out, Now);
        }

        void RunChoker(const Clock::time_point Now) {
            if (!_Picker) { return; }  // Nothing to upload yet

            std::vector<Peer*> Active;
            std::vector<ChokeCandidate> Candidates;
            for (const auto& Target : _Peers) {
                if (!Target->Active || Target->Dropped) { continue; }
                Active.push_back(Target.get());
                Candidates.push_back({
                  .Key          = Target->Key,
                  .Interested   = Target->PeerInterested,
                  .Snubbed      = Now < Target->SnubbedUntil,
                  .DownloadRate = Target->Down.Rate(Now),
                  .UploadRate   = Target->Up.Rate(Now),
                  .Connected    = Target->Created,
                  .LastUnchoked = Target->LastUnchoked,
                });
            }

            const std::vector<bool> Unchoke = _Choker.Decide(Candidates, _State == TorrentState::Seeding, Now);
            for (std::size_t I = 0; I < Active.size(); ++I) {
                SetChoked(*Active[I], !Unchoke[I], Now);
            }
        }

        // ---- Housekeeping --------------------------------------------------------------------------------------

        void Housekeeping(const Clock::time_point Now) {
            for (const auto& Pointer : _Peers) {
                Peer& Target = *Pointer;
                if (Target.Dropped) { continue; }

                if (!Target.Connection->IsConnected()) {
                    if (Now - Target.Created > kConnectTimeout) { Drop(Target); }
                    continue;
                }
                if (!Target.Active) {
                    if (Now - Target.Created > kHandshakeTimeout) { Drop(Target); }
                    continue;
                }
                if (Now - Target.LastReceived > kInactivityTimeout) {
                    Drop(Target);
                    continue;
                }
                // Free the slot for a peer that might actually trade with us. Without metadata we can't tell
                // yet, so everyone stays.
                if (!_Meta || Target.AmInterested || Target.PeerInterested) {
                    Target.LastUseful = Now;
                } else if (Now - Target.LastUseful > kUselessTimeout) {
                    Drop(Target);
                    continue;
                }
                if (!Target.Outstanding.empty() && Now - Target.WaitingSince > kRequestTimeout) {
                    AbortRequests(Target);
                    Target.SnubbedUntil = Now + kSnubPenalty;
                }
                if (!_Meta) {
                    if (Now - Target.LastMetadataRequest > kMetadataTimeout) { Target.MetadataRequests = 0; }
                    RequestMetadata(Target, Now);
                }
                if (Now - Target.LastSent > kKeepAliveInterval) {
                    ByteWriter Out;
                    EncodeKeepAlive(Out);
                    SendTo(Target, Out, Now);
                }
                RequestMore(Target, Now);
                ServeRequests(Target, Now);
            }

            if (Now >= _NextChoke) {
                RunChoker(Now);
                _NextChoke = Now + kChokeInterval;
            }
            if (DhtAllowed() && Now >= _NextDhtLookup) {
                _Params.FindPeers(_Params.Hash, _Params.ListenPort);
                _NextDhtLookup =
                  Now + (!_Meta || ActivePeerCount() < kHungryPeerCount ? kDhtLookupHungry : kDhtLookupSatisfied);
            }
            ConnectToCandidates(Now);
        }

        Torrent& _Owner;
        const Torrent::Params& _Params;
        std::stop_token _Stop;

        std::shared_ptr<const Metainfo> _Meta;  // Null until a magnet link's metadata arrives
        std::optional<FileStorage> _Storage;
        std::optional<PiecePicker> _Picker;
        MetadataAssembler _Assembler;

        Choker _Choker;
        std::vector<std::unique_ptr<Peer>> _Peers;
        std::uint64_t _NextPeerKey = 0;

        std::unique_ptr<Announcer> _Announcer;
        std::unique_ptr<Announcer> _RetiringAnnouncer;
        Clock::time_point _RetireDeadline;

        std::deque<Endpoint> _Candidates;
        std::set<Endpoint> _CandidateSet;
        std::map<Endpoint, Clock::time_point> _RetryAfter;

        TorrentState _State = TorrentState::Queued;
        bool _Paused        = false;
        std::string _Error;

        RateMeter _Down;
        RateMeter _Up;
        std::int64_t _VerifiedBytes = 0;
        std::int64_t _WantedBytes   = 0;  // Size of the pieces the chosen files cover
        std::int64_t _WantedDone    = 0;  // ...and how much of that is verified
        std::vector<int> _SkippedFiles;
        std::int64_t _CheckedBytes = 0;
        std::uint32_t _Key         = 0;

        // Resume data
        std::int64_t _PriorUploaded   = 0;  // Totals from earlier runs
        std::int64_t _PriorDownloaded = 0;
        bool _PiecesKnown             = false;  // The picker's pieces come from a finished check or fast resume
        bool _WroteSinceStamp         = false;
        bool _ResumeUrgent            = true;  // Publish on the first loop pass
        std::vector<FileStamp> _Stamps;
        std::tuple<int, std::int64_t, std::int64_t, bool, bool> _LastResumeKey {};
        Clock::time_point _NextResume {};
        Clock::time_point _NextDetails {};

        Clock::time_point _NextHousekeeping {};
        Clock::time_point _NextChoke {};
        Clock::time_point _NextDhtLookup {};
        Clock::time_point _NextStats {};

        std::vector<std::uint8_t> _PieceBuffer;
        std::vector<std::uint8_t> _BlockBuffer;
    };

    Torrent::Torrent(Params Settings) : _Params(std::move(Settings)) {
        _Stats.Id          = _Params.Id;
        _Stats.Hash        = _Params.Hash;
        _Stats.Name        = _Params.Meta ? _Params.Meta->Name : _Params.DisplayName;
        _Stats.Destination = _Params.Destination;
        _Stats.TotalSize   = _Params.Meta ? static_cast<double>(_Params.Meta->TotalSize) : 0.0;
        _Stats.State       = _Params.Meta ? TorrentState::Checking : TorrentState::FetchingMetadata;
        if (_Params.Meta) { _Metadata = std::make_shared<const Metainfo>(*_Params.Meta); }
        if (_Params.Resume) {
            _Stats.UploadedTotal   = static_cast<double>(_Params.Resume->Uploaded);
            _Stats.DownloadedTotal = static_cast<double>(_Params.Resume->Downloaded);
        }

        _Thread = std::jthread([this](const std::stop_token Stop) {
            try {
                TorrentWorker(*this, Stop).Run();
            } catch (const std::exception& Ex) {
                TorrentStats Failed  = Stats();
                Failed.State         = TorrentState::Error;
                Failed.ErrorMessage  = Ex.what();
                Failed.DownloadSpeed = Failed.UploadSpeed = 0;
                Publish(std::move(Failed));
            }
            _Stopped = true;
        });
    }

    Torrent::~Torrent() = default;

    void Torrent::Pause() {
        Push(PauseCommand {});
    }

    void Torrent::Resume() {
        Push(ResumeCommand {});
    }

    void Torrent::Recheck() {
        Push(RecheckCommand {});
    }

    void Torrent::AddPeers(std::vector<Endpoint> Peers) {
        Push(AddPeersCommand {std::move(Peers)});
    }

    void Torrent::SetSkippedFiles(std::vector<int> Files) {
        Push(SkipFilesCommand {std::move(Files)});
    }

    std::shared_ptr<const TorrentDetails> Torrent::Details() {
        using namespace std::chrono_literals;
        _DetailsWantedUntil = (Clock::now() + 3s).time_since_epoch().count();
        std::scoped_lock Lock(_Mutex);
        return _Details;
    }

    bool Torrent::DetailsWanted() const {
        return Clock::now().time_since_epoch().count() < _DetailsWantedUntil.load();
    }

    void Torrent::PublishDetails(std::shared_ptr<const TorrentDetails> Data) {
        std::scoped_lock Lock(_Mutex);
        _Details = std::move(Data);
    }

    void Torrent::AddIncoming(UniqueSocket Socket, const Endpoint& Remote, const Handshake& Received) {
        Push(IncomingCommand {std::move(Socket), Remote, Received});
    }

    void Torrent::RequestStop() {
        _Thread.request_stop();
    }

    void Torrent::Join() {
        if (_Thread.joinable()) { _Thread.join(); }
    }

    TorrentStats Torrent::Stats() const {
        std::scoped_lock Lock(_Mutex);
        return _Stats;
    }

    std::shared_ptr<const Metainfo> Torrent::Metadata() const {
        std::scoped_lock Lock(_Mutex);
        return _Metadata;
    }

    std::shared_ptr<const ResumeData> Torrent::ResumeState() const {
        std::scoped_lock Lock(_Mutex);
        return _Resume;
    }

    void Torrent::PublishResume(std::shared_ptr<const ResumeData> Data) {
        std::scoped_lock Lock(_Mutex);
        _Resume = std::move(Data);
    }

    void Torrent::Push(Command Item) {
        std::scoped_lock Lock(_Mutex);
        _Commands.push_back(std::move(Item));
    }

    std::vector<Torrent::Command> Torrent::TakeCommands() {
        std::scoped_lock Lock(_Mutex);
        return std::exchange(_Commands, {});
    }

    void Torrent::Publish(TorrentStats Stats) {
        std::scoped_lock Lock(_Mutex);
        _Stats = std::move(Stats);
    }

    void Torrent::SetMetadata(std::shared_ptr<const Metainfo> Meta) {
        std::scoped_lock Lock(_Mutex);
        _Metadata = std::move(Meta);
    }
}  // namespace Tori
