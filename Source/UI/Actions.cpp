#include "UI/Actions.hpp"

#include "Core/Utf8.hpp"
#include "Engine/Session.hpp"
#include "Meta/MagnetUri.hpp"
#include "Meta/Metainfo.hpp"
#include "UI/Dialogs.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <format>

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        HWND OwnerWindow(const ToriState& State) {
            return State.MainWindow ? static_cast<HWND>(State.MainWindow->GetNativeHandle()) : nullptr;
        }

        /// Menu actions that open dialogs or message boxes run after the menu has closed.
        std::function<void()> Deferred(std::function<void()> Action) {
            return [Action = std::move(Action)] { App::Post(Action); };
        }
    }  // namespace

    std::filesystem::path DefaultDownloadFolder() {
        PWSTR Folder = nullptr;
        std::filesystem::path Result;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &Folder))) { Result = Folder; }
        CoTaskMemFree(Folder);
        if (Result.empty()) {
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &Folder))) { Result = Folder; }
            CoTaskMemFree(Folder);
        }
        return Result;
    }

    std::filesystem::path SuggestedDestination(const ToriState& State) {
        return State.LastDestination.empty() ? DefaultDownloadFolder() : State.LastDestination;
    }

    void RememberDestination(ToriState& State, const std::filesystem::path& Folder) {
        State.LastDestination = Folder;
    }

    void ShowError(const ToriState& State, const std::string_view Message) {
        MessageBoxW(OwnerWindow(State), Utf8ToWide(Message).c_str(), L"Tori", MB_OK | MB_ICONERROR);
    }

    void Confirm(ToriState& State,
                 std::string Title,
                 std::string Message,
                 std::string ConfirmLabel,
                 std::function<void()> OnConfirm) {
        if (!State.Overlay || State.ActiveDialog.IsShown()) { return; }

        const auto Close = [Target = &State] { Target->ActiveDialog.Reset(); };
        // Closing the dialog destroys this callback, so take what it needs out of it first
        const auto Accept = [Target = &State, OnConfirm = std::move(OnConfirm)] {
            const std::function<void()> Action = OnConfirm;
            ToriState* const Owner             = Target;
            Owner->ActiveDialog.Reset();
            if (Action) { Action(); }
        };

        State.ActiveDialog = ShowDialog(*State.Overlay,
                                        State.Popup,
                                        Dialog {
                                          .Title   = std::move(Title),
                                          .Content = Text {.Content = std::move(Message)},
                                          .Actions =
                                            {
                                              DialogAction {.Label = "Cancel", .OnPressed = Close},
                                              DialogAction {.Label = std::move(ConfirmLabel), .OnPressed = Accept, .Variant = ButtonVariant::Primary},
                                            },
                                          .OnDismiss = Close,
                                        });
    }

    void AddTorrentFiles(ToriState& State, const std::vector<std::filesystem::path>& Files) {
        std::string Errors;
        for (const std::filesystem::path& File : Files) {
            try {
                State.PendingAdds.push_back(Metainfo::Load(File));
            } catch (const std::exception& Ex) {
                Errors += std::format("{}: {}\n", PathToUtf8(File.filename()), Ex.what());
            }
        }
        if (!Errors.empty()) { ShowError(State, "Some torrents could not be opened:\n\n" + Errors); }
        ShowNextAddDialog(State);
    }

    void OpenTorrentFiles(ToriState& State) {
        const std::vector<std::filesystem::path> Files = FileDialog::OpenFiles({
          .Title   = "Open torrent",
          .Filters = {{"Torrent files", "*.torrent"}, {"All files", "*.*"}},
          .Owner   = State.MainWindow,
        });
        if (!Files.empty()) { AddTorrentFiles(State, Files); }
    }

    void AddMagnetLinks(ToriState& State, const std::vector<std::string>& Links) {
        std::string Errors;
        for (const std::string& Link : Links) {
            try {
                State.Engine->AddMagnet(MagnetUri::Parse(Link), SuggestedDestination(State));
            } catch (const std::exception& Ex) { Errors += std::format("{}\n", Ex.what()); }
        }
        if (!Errors.empty()) { ShowError(State, "Some links could not be added:\n\n" + Errors); }
    }

    void HandleArguments(ToriState& State, const std::vector<std::wstring>& Arguments) {
        std::vector<std::filesystem::path> Files;
        std::vector<std::string> Links;
        for (const std::wstring& Argument : Arguments) {
            if (Argument.starts_with(L"magnet:")) {
                Links.push_back(WideToUtf8(Argument));
            } else if (std::filesystem::path(Argument).extension() == L".torrent") {
                Files.emplace_back(Argument);
            }
        }
        if (!Links.empty()) { AddMagnetLinks(State, Links); }
        if (!Files.empty()) { AddTorrentFiles(State, Files); }
    }

    void PauseTorrent(ToriState& State, const TorrentId Id) {
        State.Engine->Pause(Id);
    }

    void ResumeTorrent(ToriState& State, const TorrentId Id) {
        State.Engine->Resume(Id);
    }

    void RecheckTorrent(ToriState& State, const TorrentId Id) {
        State.Engine->Recheck(Id);
    }

    void RemoveTorrent(ToriState& State, const TorrentId Id, const bool DeleteFiles) {
        if (Id == kInvalidTorrentId) { return; }
        if (!DeleteFiles) {
            State.Engine->RemoveTorrent(Id, false);
            return;
        }

        const std::shared_ptr<TorrentEntry> Entry = FindEntry(State, Id);
        const std::string Name                    = Entry ? Entry->Name.Peek() : std::string("this torrent");
        Confirm(State,
                "Delete downloaded files?",
                std::format("\"{}\" will be removed from Tori and its downloaded files deleted. This can't be undone.", Name),
                "Remove and Delete",
                [Engine = State.Engine, Id] { Engine->RemoveTorrent(Id, true); });
    }

    void OpenTorrentFolder(const ToriState& State, const TorrentId Id) {
        const std::shared_ptr<TorrentEntry> Entry = FindEntry(State, Id);
        if (!Entry) { return; }
        const std::filesystem::path Destination = Entry->Destination.Peek();
        const std::filesystem::path Content     = Destination / PathFromUtf8(Entry->Name.Peek());

        std::error_code Ec;
        if (std::filesystem::exists(Content, Ec)) {
            // Select the torrent's file or folder in its parent, as "Show in folder" does elsewhere
            const std::wstring Arguments = L"/select,\"" + Content.wstring() + L"\"";
            ShellExecuteW(OwnerWindow(State), L"open", L"explorer.exe", Arguments.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(OwnerWindow(State), L"open", Destination.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void CopyMagnetLink(const ToriState& State, const TorrentId Id) {
        const std::shared_ptr<TorrentEntry> Entry = FindEntry(State, Id);
        if (!Entry) { return; }
        MagnetUri Link;
        Link.Hash        = Entry->Hash;
        Link.DisplayName = Entry->Name.Peek();
        if (const std::shared_ptr<const TorrentDetails> Details = State.Details.Peek(); Details && Details->Id == Id) {
            for (const TrackerStatus& Tracker : Details->Trackers) { Link.Trackers.push_back(Tracker.Url); }
        }
        XenUI::Clipboard::SetText(Link.ToString());
    }

    std::vector<MenuItem> TorrentMenuItems(ToriState* State, const TorrentId Id) {
        const std::shared_ptr<TorrentEntry> Entry = FindEntry(*State, Id);
        const bool Exists                         = Entry != nullptr;
        const TorrentState Current                = Exists ? Entry->State.Peek() : TorrentState::Queued;
        const bool CanPause  = Exists && Current != TorrentState::Paused && Current != TorrentState::Checking;
        const bool CanResume = Exists && (Current == TorrentState::Paused || Current == TorrentState::Error);

        return {
          MenuItem {.Label = "Pause", .OnSelect = [State, Id] { PauseTorrent(*State, Id); }, .Enabled = CanPause},
          MenuItem {.Label = "Resume", .OnSelect = [State, Id] { ResumeTorrent(*State, Id); }, .Enabled = CanResume},
          MenuItem {.Label = "Force Recheck", .OnSelect = [State, Id] { RecheckTorrent(*State, Id); }, .Enabled = Exists},
          MenuItem::Divider(),
          MenuItem {.Label = "Open Containing Folder", .OnSelect = Deferred([State, Id] { OpenTorrentFolder(*State, Id); }), .Enabled = Exists},
          MenuItem {.Label = "Copy Magnet Link", .OnSelect = [State, Id] { CopyMagnetLink(*State, Id); }, .Enabled = Exists},
          MenuItem::Divider(),
          MenuItem {
            .Label        = "Remove",
            .ShortcutText = "Del",
            .OnSelect     = [State, Id] { RemoveTorrent(*State, Id, false); },
            .Enabled      = Exists,
          },
          MenuItem {
            .Label        = "Remove and Delete Files",
            .ShortcutText = "Shift+Del",
            .OnSelect     = Deferred([State, Id] { RemoveTorrent(*State, Id, true); }),
            .Enabled      = Exists,
          },
        };
    }
}  // namespace Tori::UI
