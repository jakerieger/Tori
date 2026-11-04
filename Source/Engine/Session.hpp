#pragma once

#include "Core/InfoHash.hpp"
#include "Core/PeerId.hpp"
#include "Core/TorrentTypes.hpp"
#include "Engine/ResumeData.hpp"
#include "Engine/TorrentDetails.hpp"
#include "Meta/MagnetUri.hpp"
#include "Meta/Metainfo.hpp"
#include "Net/Endpoint.hpp"
#include "Tracker/Announcer.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace Tori {
    class DhtNode;
    class Torrent;

    struct SessionSettings {
        std::uint16_t ListenPort = 6881;  // Falls back to a random port if taken; 0 always picks one
        bool LoopbackOnly        = false; // Listen on 127.0.0.1 only (tests)
        int MaxPeersPerTorrent   = 50;
        AnnounceFunction Announce = AnnounceToTracker;

        bool EnableDht           = true;  // Uses the listen port's number for UDP
        bool DhtLoopbackOnly     = false; // Separate from LoopbackOnly: a DHT on loopback can't reach the internet
        bool DhtUsePublicRouters = true;
        std::vector<Endpoint> DhtBootstrapNodes;

        /// @brief Where torrents, their progress and the DHT's nodes are saved, to be restored on the next start.
        /// Empty saves nothing.
        std::filesystem::path StateFolder;
    };

    /// @brief Owns every torrent, plus the one listening socket all of them share. Incoming connections are
    /// routed to a torrent by the info hash in their handshake. All methods are thread-safe.
    ///
    /// With a StateFolder, the torrents saved there are restored by the constructor, changes are saved within a
    /// few seconds (transfer totals within a minute), and everything is saved once more by the destructor:
    ///   resume\<info hash>.dat     ResumeData
    ///   torrents\<info hash>.torrent  The metadata, including any a magnet link fetched
    ///   dht.dat                     The DHT's node ID and some good nodes
    class Session {
    public:
        /// @brief Opens the listener and DHT, then restores any torrents saved in the state folder. Saved torrents
        /// that can't be read are skipped. Throws std::runtime_error if the listener or state folder can't be set up.
        explicit Session(SessionSettings Settings = {});

        /// @brief Stops every torrent in parallel, waiting for them to tell trackers they're leaving, then saves
        /// their final state.
        ~Session();

        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;

        /// @brief Starts a torrent. SkippedFiles are indices into Meta.Files not to download. Throws
        /// std::runtime_error if one with the same info hash is already added.
        TorrentId AddTorrent(Metainfo Meta,
                             std::filesystem::path Destination,
                             bool Paused                   = false,
                             std::vector<int> SkippedFiles = {});

        /// @brief Starts a torrent from a magnet link. Its metadata is fetched from peers found through the link's
        /// trackers and peers and the DHT. Throws std::runtime_error if the info hash is already added.
        TorrentId AddMagnet(const MagnetUri& Magnet, std::filesystem::path Destination, bool Paused = false);

        /// @brief Removes a torrent without blocking. It finishes stopping in the background, after which its
        /// files are deleted if requested.
        void RemoveTorrent(TorrentId Id, bool DeleteFiles = false);

        /// @brief Writes whatever changed since the last save now, rather than waiting for the next save pass.
        void SaveState();

        void Pause(TorrentId Id);
        void Resume(TorrentId Id);
        void Recheck(TorrentId Id);
        void AddPeers(TorrentId Id, std::vector<Endpoint> Peers);
        void SetSkippedFiles(TorrentId Id, std::vector<int> Files);

        /// @brief Files, peers and trackers for the details pane; see Torrent::Details. Poll it about once a second
        /// while it's shown. Returns nullptr for an unknown torrent and until the first details are built.
        [[nodiscard]] std::shared_ptr<const TorrentDetails> Details(TorrentId Id);

        [[nodiscard]] std::vector<TorrentStats> Snapshot() const;
        [[nodiscard]] std::optional<TorrentId> Find(const InfoHash& Hash) const;
        [[nodiscard]] std::uint16_t ListenPort() const { return _ListenPort; }
        [[nodiscard]] const PeerId& LocalId() const { return _LocalId; }

        /// @brief Nodes in the DHT routing table; 0 when DHT is off.
        [[nodiscard]] std::size_t DhtNodeCount() const;

        /// @brief The DHT's UDP port, for tests that bootstrap one session from another; 0 when DHT is off.
        [[nodiscard]] std::uint16_t DhtPort() const;

    private:
        struct Listener;
        struct Retiring;

        /// What has been written to the state folder for one torrent.
        struct Persisted {
            std::shared_ptr<const ResumeData> Resume;
            bool HasTorrentFile = false;
        };

        template<class TAction>
        void WithTorrent(TorrentId Id, TAction&& Action);

        template<class TParams>
        TorrentId Launch(TParams Params);

        void OnDhtPeers(const InfoHash& Hash, std::vector<Endpoint> Peers);

        void RestoreTorrents();
        void SaveTorrents(std::span<const std::pair<TorrentId, const Torrent*>> Torrents);
        void SaveDht();
        void ForgetSaved(const InfoHash& Hash);
        [[nodiscard]] std::filesystem::path ResumePath(const InfoHash& Hash) const;
        [[nodiscard]] std::filesystem::path TorrentPath(const InfoHash& Hash) const;

        void ListenLoop(const std::stop_token& Stop);
        void ReapRetired();

        SessionSettings _Settings;
        PeerId _LocalId;
        std::uint16_t _ListenPort = 0;

        mutable std::mutex _Mutex;
        std::map<TorrentId, std::unique_ptr<Torrent>> _Torrents;
        std::vector<Retiring> _Retiring;
        TorrentId _NextId = 1;

        std::unique_ptr<Listener> _Listener;

        mutable std::mutex _DhtMutex;  // Torrent threads reach the DHT through this
        std::unique_ptr<DhtNode> _Dht;

        // Taken before _Mutex. Held while saving, so a removed torrent's files can't be written back afterwards.
        std::mutex _SaveMutex;
        std::map<InfoHash, Persisted> _Persisted;

        std::jthread _Thread;  // Last, so it stops first
    };

    /// @brief Deletes a torrent's files from Destination, then any folders that were left empty.
    void DeleteTorrentFiles(const std::filesystem::path& Destination, const std::vector<TorrentFile>& Files);
}  // namespace Tori
