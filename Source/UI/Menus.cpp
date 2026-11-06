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

#include "UI/Menus.hpp"

#include "UI/Actions.hpp"
#include "UI/Dialogs.hpp"
#include "UI/Updates.hpp"

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        // Dialogs and message boxes run after the menu has closed
        std::function<void()> Deferred(ToriState* State, void (*Action)(ToriState&)) {
            return [State, Action] { App::Post([State, Action] { Action(*State); }); };
        }
    }  // namespace

    std::vector<Menu> MainMenus(ToriState* State) {
        // Subscribe to what the Torrent menu's items depend on, so it's rebuilt when they change
        const TorrentId Selected = State->SelectedTorrent();
        for (const auto& Entry : State->Torrents()) {
            if (Entry->Id == Selected) { (void)Entry->State(); }
        }

        return {
          Menu {
            .Label = "File",
            .Items =
              {
                MenuItem {
                  .Label        = "Open...",
                  .ShortcutText = "Ctrl+O",
                  .OnSelect     = Deferred(State, OpenTorrentFiles),
                },
                MenuItem {
                  .Label        = "Open URL...",
                  .ShortcutText = "Ctrl+Shift+O",
                  .OnSelect     = Deferred(State, [](ToriState& Target) { ShowOpenLinkDialog(Target); }),
                },
                MenuItem {
                  .Label        = "New...",
                  .ShortcutText = "Ctrl+N",
                  .OnSelect     = Deferred(State, ShowCreateTorrentDialog),
                },
                MenuItem::Divider(),
                MenuItem {
                  .Label        = "Exit",
                  .ShortcutText = "Ctrl+Q",
                  .OnSelect     = [State] { State->MainWindow->Close(); },
                },
              },
          },
          Menu {
            .Label = "Torrent",
            .Items = TorrentMenuItems(State, Selected),
          },
          Menu {
            .Label = "View",
            .Items =
              {
                MenuItem {
                  .Label    = "Details Pane",
                  .OnSelect = [State] { State->ShowDetails = !State->ShowDetails.Peek(); },
                  .Checked  = State->ShowDetails(),
                },
              },
          },
          Menu {
            .Label = "Help",
            .Items =
              {
                MenuItem {
                  .Label    = "Check for Updates...",
                  .OnSelect = Deferred(State, CheckForUpdates),
                },
                MenuItem::Divider(),
                MenuItem {
                  .Label    = "About Tori",
                  .OnSelect = Deferred(State, ShowAboutDialog),
                },
              },
          },
        };
    }
}  // namespace Tori::UI
