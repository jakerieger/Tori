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
