#pragma once

#include "UI/Model.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Tori::UI {
    /// @brief The user's Downloads folder, falling back to their profile folder.
    std::filesystem::path DefaultDownloadFolder();

    /// @brief Where a new torrent is offered to go: the last folder used, else Downloads.
    std::filesystem::path SuggestedDestination(const ToriState& State);
    void RememberDestination(ToriState& State, const std::filesystem::path& Folder);

    void ShowError(const ToriState& State, std::string_view Message);

    /// @brief Asks before doing something that can't be undone, in an in-window dialog. OnConfirm runs only if the
    /// user presses ConfirmLabel; Cancel, Escape and clicking outside do nothing.
    void Confirm(ToriState& State,
                 std::string Title,
                 std::string Message,
                 std::string ConfirmLabel,
                 std::function<void()> OnConfirm);

    /// @brief Loads each .torrent file and queues it for the Add dialog, reporting any that can't be read.
    void AddTorrentFiles(ToriState& State, const std::vector<std::filesystem::path>& Files);

    /// @brief File > Open: pick .torrent files, then confirm each in the Add dialog.
    void OpenTorrentFiles(ToriState& State);

    /// @brief Adds magnet links to the suggested folder without asking, reporting any that are invalid or already
    /// added. Used for links that arrive from outside, such as a browser.
    void AddMagnetLinks(ToriState& State, const std::vector<std::string>& Links);

    /// @brief Command-line style arguments: .torrent paths and magnet links. Anything else is ignored.
    void HandleArguments(ToriState& State, const std::vector<std::wstring>& Arguments);

    void PauseTorrent(ToriState& State, TorrentId Id);
    void ResumeTorrent(ToriState& State, TorrentId Id);
    void RecheckTorrent(ToriState& State, TorrentId Id);
    void RemoveTorrent(ToriState& State, TorrentId Id, bool DeleteFiles);

    /// @brief Shows the torrent's file or folder in Explorer, or its destination if nothing is there yet.
    void OpenTorrentFolder(const ToriState& State, TorrentId Id);
    void CopyMagnetLink(const ToriState& State, TorrentId Id);

    /// @brief Pause, Resume, Recheck, Open folder, Copy magnet link and the Remove items for one torrent; shared
    /// by the Torrent menu and the list's right-click menu. Reads the torrent's state without subscribing.
    [[nodiscard]] std::vector<XenUI::MenuItem> TorrentMenuItems(ToriState* State, TorrentId Id);
}  // namespace Tori::UI
