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

#include "Core/Utf8.hpp"
#include "Engine/Session.hpp"
#include "UI/Actions.hpp"
#include "UI/CrashLog.hpp"
#include "UI/Dialogs.hpp"
#include "UI/Instance.hpp"
#include "UI/Model.hpp"
#include "UI/Root.hpp"

#include "Resource.h"

#include <XenUI/XenUI.hpp>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {
    using namespace XenUI;
    using namespace Tori;
    using namespace Tori::UI;

    Theme ToriTheme() {
        Theme Result;
        Result.Accent  = Color::FromHex(0x45d1d6);
        Result.Success = Color::FromHex(0xabd645);
        return Result;
    }

    /// .torrent paths and magnet links, e.g. from double-clicking a .torrent in Explorer or a browser opening a
    /// magnet: link once Tori is registered for them.
    std::vector<std::wstring> CommandLineArguments() {
        int Count         = 0;
        LPWSTR* Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
        std::vector<std::wstring> Result(Arguments + 1, Arguments + Count);
        LocalFree(Arguments);
        return Result;
    }

    /// %LOCALAPPDATA%\Tori: resume data names paths on this machine, so it shouldn't roam with the profile.
    /// TORI_STATE_FOLDER overrides it, so a test copy doesn't touch the real torrents.
    std::filesystem::path StateFolder() {
        if (const wchar_t* Override = _wgetenv(L"TORI_STATE_FOLDER"); Override && *Override) { return Override; }
        PWSTR Folder = nullptr;
        std::filesystem::path Result;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &Folder))) {
            Result = std::filesystem::path(Folder) / L"Tori";
        }
        CoTaskMemFree(Folder);
        return Result;
    }

    /// Keyboard equivalents of the menus. None of them fire while a dialog is open.
    std::vector<Shortcut> MainShortcuts(ToriState& State) {
        const auto UnlessDialog = [&State](void (*Action)(ToriState&)) {
            return [&State, Action] {
                if (!State.ActiveDialog.IsShown()) { Action(State); }
            };
        };
        return {
          {{KeyModifiers::Control + KeyCode::O}, UnlessDialog(OpenTorrentFiles)},
          {{(KeyModifiers::Control | KeyModifiers::Shift) + KeyCode::O},
           UnlessDialog([](ToriState& Target) { ShowOpenLinkDialog(Target); })},
          {{KeyModifiers::Control + KeyCode::N}, UnlessDialog(ShowCreateTorrentDialog)},
          {{KeyModifiers::Control + KeyCode::Q}, [&State] { State.MainWindow->Close(); }},
          {{KeyCombination {KeyCode::Delete}},
           UnlessDialog([](ToriState& Target) { RemoveTorrent(Target, Target.SelectedTorrent.Peek(), false); })},
          {{KeyModifiers::Shift + KeyCode::Delete},
           UnlessDialog([](ToriState& Target) { RemoveTorrent(Target, Target.SelectedTorrent.Peek(), true); })},
        };
    }

    Node BuildTori(ToriState* State) {
        return Provide<Theme> {
          .Value = ToriTheme(),
          .Child = Root {.State = State},
        };
    }
}  // namespace

int main() {
    const std::vector<std::wstring> Arguments = CommandLineArguments();
    InstallCrashLog(StateFolder() / L"crash.log");

    // A second launch passes its arguments to the copy already running, which also comes to the front
    InstanceChannel Instance;
    if (!Instance.IsFirst()) { return InstanceChannel::SendToFirst(Arguments) ? 0 : 1; }

    try {
        App Tori;
        const std::filesystem::path Folder = StateFolder();
        // Restores the torrents saved last time, and saves them again as it's destroyed after Run returns
        Session Engine({.StateFolder = Folder});

        Window MainWindow({.Title          = "Tori",
                           .ClientSize     = {820.f, 620.0f},
                           .Background     = Theme::Dark().Background,
                           .CustomTitleBar = true,
                           .Icon           = IDI_TORI});
        MainWindow.SetOnClosed([&Tori] { Tori.Quit(); });

        ToriState State {
          .Engine      = &Engine,
          .MainWindow  = &MainWindow,
          .StateFolder = Folder,
        };
        const UiSettings Saved = LoadUiSettings(Folder);
        State.LastDestination  = Saved.LastDestination;
        State.ShowDetails      = Saved.ShowDetails;
        State.DetailsSplit     = Saved.DetailsSplit;

        // The engine's threads never touch signals; the UI pulls snapshots instead
        const Timer Refresh = Timer::Every(0.5, [&] {
            SyncTorrents(State, Engine.Snapshot());
            SyncDetails(State);
        });

        MainWindow.SetShortcuts(MainShortcuts(State));
        MainWindow.SetContent(BuildTori(&State));
        MainWindow.Show();

        Instance.Listen([&State](std::vector<std::wstring> Received) {
            App::Post([&State, Received = std::move(Received)] {
                State.MainWindow->Activate();
                HandleArguments(State, Received);
            });
        });
        if (!Arguments.empty()) {
            App::Post([&State, &Arguments] { HandleArguments(State, Arguments); });
        }

        const int Result = Tori.Run();
        Instance.Listen({});
        SaveUiSettings(Folder,
                       {
                         .LastDestination = State.LastDestination,
                         .ShowDetails     = State.ShowDetails.Peek(),
                         .DetailsSplit    = State.DetailsSplit.Peek(),
                       });
        return Result;
    } catch (const std::exception& Ex) {
        MessageBoxW(nullptr, Utf8ToWide(Ex.what()).c_str(), L"Tori", MB_OK | MB_ICONERROR);
        return 1;
    }
}
