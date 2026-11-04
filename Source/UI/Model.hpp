#pragma once

#include "Core/TorrentTypes.hpp"
#include "Engine/TorrentDetails.hpp"
#include "Meta/Metainfo.hpp"

#include <XenUI/XenUI.hpp>

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Tori {
    class Session;
}

namespace Tori::UI {
    /// @brief UI-side mirror of a torrent. Only ever written on the UI thread; the engine publishes
    /// TorrentStats snapshots which are copied in here.
    struct TorrentEntry {
        TorrentId Id = kInvalidTorrentId;
        InfoHash Hash;
        XenUI::Signal<std::string> Name;                   // Torrent name, fetched from metadata
        XenUI::Signal<std::filesystem::path> Destination;  // Path on disk torrent is being downloaded to
        XenUI::Signal<double> TotalSize;                   // Bytes chosen for download, 0 if unknown
        XenUI::Signal<double> DownloadedSize;              // Verified bytes of those on disk
        XenUI::Signal<double> DownloadedTotal;             // Payload bytes received from peers
        XenUI::Signal<double> UploadedTotal;               // Total number of bytes uploaded so far
        XenUI::Signal<double> DownloadSpeed;               // Current download speed in bytes/second
        XenUI::Signal<double> UploadSpeed;                 // Current upload speed in bytes/second
        XenUI::Signal<int> Peers;                          // Connected peers
        XenUI::Signal<TorrentState> State;                 // Current state torrent is in
        XenUI::Signal<std::string> ErrorMessage;           // Set when State is Error
    };

    /// @brief Preferences kept between runs in the state folder.
    struct UiSettings {
        std::filesystem::path LastDestination;  // Where the last torrent was saved; offered for the next one
        bool ShowDetails   = true;
        float DetailsSplit = 0.55f;
    };

    struct ToriState {
        XenUI::Signal<TorrentId> SelectedTorrent = kInvalidTorrentId;  // Currently selected torrent in the list view
        // Shared so entries stay individually writable while the list signal hands out const access
        XenUI::Signal<std::vector<std::shared_ptr<TorrentEntry>>> Torrents;

        Session* Engine           = nullptr;
        XenUI::Window* MainWindow = nullptr;
        std::filesystem::path StateFolder;  // Where UiSettings are saved; empty saves nothing

        // In-window dialogs. Root captures the overlay and style each build so menu actions can open dialogs.
        XenUI::OverlayController* Overlay = nullptr;
        XenUI::PopupStyle Popup;
        XenUI::OverlayEntry ActiveDialog;
        std::deque<Metainfo> PendingAdds;  // Torrents waiting for their turn in the Add dialog

        // Details pane
        XenUI::Signal<bool> ShowDetails    = true;
        XenUI::Signal<float> DetailsSplit  = 0.55f;
        XenUI::Signal<int> DetailsTab      = 0;
        XenUI::Signal<std::shared_ptr<const TorrentDetails>> Details;  // For the selected torrent, refreshed by main

        std::filesystem::path LastDestination;

        bool CheckingForUpdates = false;
        std::jthread UpdateCheck;  // Last, so it's stopped and joined before anything it could post to goes away
    };

    /// @brief Copies an engine snapshot into the UI model: updates entries in place, adds new torrents and drops
    /// removed ones. Call on the UI thread.
    void SyncTorrents(ToriState& State, const std::vector<TorrentStats>& Snapshot);

    /// @brief Fetches the selected torrent's details while the pane is shown. Call on the UI thread, about once
    /// a second.
    void SyncDetails(ToriState& State);

    [[nodiscard]] UiSettings LoadUiSettings(const std::filesystem::path& StateFolder);
    void SaveUiSettings(const std::filesystem::path& StateFolder, const UiSettings& Settings);

    /// @brief The entry for a torrent, or nullptr. Reads the list without subscribing.
    [[nodiscard]] std::shared_ptr<TorrentEntry> FindEntry(const ToriState& State, TorrentId Id);
}  // namespace Tori::UI
