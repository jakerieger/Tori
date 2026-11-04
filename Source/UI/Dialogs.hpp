#pragma once

#include "UI/Model.hpp"

#include <string>

namespace Tori::UI {
    /// @brief Shows the Add dialog for the next torrent in State.PendingAdds, if no dialog is open. Each one closes
    /// by showing the next, so a batch of .torrent files is gone through one at a time.
    void ShowNextAddDialog(ToriState& State);

    /// @brief File > Open URL: a magnet link (pre-filled from Link, else from the clipboard) and where to save it.
    void ShowOpenLinkDialog(ToriState& State, std::string Link = {});

    /// @brief File > New: builds a .torrent from a file or folder, optionally seeding it straight away.
    void ShowCreateTorrentDialog(ToriState& State);

    void ShowAboutDialog(ToriState& State);
}  // namespace Tori::UI
