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

#include "Core/InfoHash.hpp"
#include "Core/PeerId.hpp"
#include "Core/TorrentTypes.hpp"
#include "Engine/ResumeData.hpp"
#include "Engine/TorrentDetails.hpp"
#include "Meta/Metainfo.hpp"
#include "Net/Endpoint.hpp"
#include "Net/Socket.hpp"
#include "Peer/Messages.hpp"
#include "Tracker/Announcer.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace Tori {
    struct TorrentLimits {
        int MaxPeers        = 50;
        int MaxHalfOpen     = 8;   // Outgoing connects in progress at once
        int UploadSlots     = 4;   // Regular unchoke slots, plus one optimistic
        int RequestPipeline = 32;  // Outstanding block requests per peer
    };

    /// @brief One torrent and the worker thread that runs it: checking, trackers, DHT lookups, peers, metadata
    /// exchange, piece transfer and disk I/O all happen on that thread. Every public method is thread-safe;
    /// commands are queued for the worker.
    class Torrent {
    public:
        struct Params {
            TorrentId Id = kInvalidTorrentId;
            InfoHash Hash;
            std::string DisplayName;       // Shown until the metadata's name is known
            std::optional<Metainfo> Meta;  // Absent for a magnet link until peers send the metadata
            std::vector<std::vector<std::string>> ExtraTrackers;  // From a magnet link, alongside the metadata's
            std::vector<Endpoint> InitialPeers;                   // From a magnet link's x.pe
            std::filesystem::path Destination;
            PeerId LocalId {};
            std::uint16_t ListenPort = 0;
            bool StartPaused         = false;
            std::int64_t AddedOn     = 0;   // Unix time
            std::vector<int> SkippedFiles;  // Indices into the metadata's files not to download
            /// @brief State saved by the previous run. Its pieces are trusted without a full check if the files
            /// haven't changed since.
            std::optional<ResumeData> Resume;
            TorrentLimits Limits;
            AnnounceFunction Announce = AnnounceToTracker;

            /// @brief Starts a DHT lookup whose results come back through AddPeers. Empty when DHT is off.
            std::function<void(const InfoHash& Hash, std::uint16_t AnnouncePort)> FindPeers;
            /// @brief Tells the DHT about a node a peer advertised. Empty when DHT is off.
            std::function<void(const Endpoint& Node)> AddDhtNode;
        };

        /// @brief Starts the worker thread, which begins by checking any existing data.
        explicit Torrent(Params Settings);

        /// @brief Stops the worker (sending "stopped" to trackers, for at most a few seconds) and joins it.
        ~Torrent();

        Torrent(const Torrent&)            = delete;
        Torrent& operator=(const Torrent&) = delete;

        void Pause();
        void Resume();
        void Recheck();
        void AddPeers(std::vector<Endpoint> Peers);

        /// @brief Chooses which files to download, by index into the metadata's files. Applies once the metadata
        /// is known if it isn't yet.
        void SetSkippedFiles(std::vector<int> Files);

        /// @brief Hands over an incoming connection whose handshake named this torrent.
        void AddIncoming(UniqueSocket Socket, const Endpoint& Remote, const Handshake& Received);

        /// @brief Asks the worker to shut down without waiting for it.
        void RequestStop();

        /// @brief Waits for the worker to finish after RequestStop.
        void Join();
        [[nodiscard]] bool IsStopped() const { return _Stopped.load(); }

        [[nodiscard]] TorrentStats Stats() const;

        /// @brief The torrent's metadata, or nullptr while a magnet link is still fetching it.
        [[nodiscard]] std::shared_ptr<const Metainfo> Metadata() const;

        /// @brief The latest state worth saving, or nullptr before the worker has published any. The worker
        /// publishes a new object whenever something important changes, at most once a minute otherwise, and
        /// once more as it stops.
        [[nodiscard]] std::shared_ptr<const ResumeData> ResumeState() const;

        /// @brief Files, peers and trackers. The worker only builds these while they're being asked for, so the
        /// first call returns nullptr and later ones (a second or so apart) return fresh data.
        [[nodiscard]] std::shared_ptr<const TorrentDetails> Details();

        [[nodiscard]] const Params& Settings() const { return _Params; }
        [[nodiscard]] TorrentId Id() const { return _Params.Id; }
        [[nodiscard]] const InfoHash& Hash() const { return _Params.Hash; }

    private:
        friend class TorrentWorker;

        struct PauseCommand {};
        struct ResumeCommand {};
        struct RecheckCommand {};
        struct AddPeersCommand {
            std::vector<Endpoint> Peers;
        };
        struct SkipFilesCommand {
            std::vector<int> Files;
        };
        struct IncomingCommand {
            UniqueSocket Socket;
            Endpoint Remote;
            Handshake Received;
        };
        using Command =
          std::variant<PauseCommand, ResumeCommand, RecheckCommand, AddPeersCommand, SkipFilesCommand, IncomingCommand>;

        void Push(Command Item);
        std::vector<Command> TakeCommands();
        void Publish(TorrentStats Stats);
        void SetMetadata(std::shared_ptr<const Metainfo> Meta);
        void PublishResume(std::shared_ptr<const ResumeData> Data);
        void PublishDetails(std::shared_ptr<const TorrentDetails> Data);
        [[nodiscard]] bool DetailsWanted() const;

        const Params _Params;

        mutable std::mutex _Mutex;
        std::vector<Command> _Commands;
        TorrentStats _Stats;
        std::shared_ptr<const Metainfo> _Metadata;
        std::shared_ptr<const ResumeData> _Resume;
        std::shared_ptr<const TorrentDetails> _Details;
        std::atomic<std::int64_t> _DetailsWantedUntil {0};  // steady_clock ticks
        std::atomic<bool> _Stopped {false};

        std::jthread _Thread;  // Last, so it stops before anything it uses is destroyed
    };
}  // namespace Tori
