#include "UI/Model.hpp"

#include "Core/Bencode.hpp"
#include "Core/Error.hpp"
#include "Core/FileIO.hpp"
#include "Core/Utf8.hpp"
#include "Engine/Session.hpp"

#include <algorithm>
#include <map>

namespace Tori::UI {
    namespace {
        void CopyStats(TorrentEntry& Entry, const TorrentStats& Stats) {
            Entry.Hash            = Stats.Hash;
            Entry.Name            = Stats.Name;
            Entry.Destination     = Stats.Destination;
            Entry.TotalSize       = Stats.TotalSize;
            Entry.DownloadedSize  = Stats.DownloadedSize;
            Entry.DownloadedTotal = Stats.DownloadedTotal;
            Entry.UploadedTotal   = Stats.UploadedTotal;
            Entry.DownloadSpeed   = Stats.DownloadSpeed;
            Entry.UploadSpeed     = Stats.UploadSpeed;
            Entry.Peers           = Stats.Peers;
            Entry.State           = Stats.State;
            Entry.ErrorMessage    = Stats.ErrorMessage;
        }

        std::filesystem::path SettingsPath(const std::filesystem::path& StateFolder) {
            return StateFolder / L"ui.dat";
        }
    }  // namespace

    void SyncTorrents(ToriState& State, const std::vector<TorrentStats>& Snapshot) {
        XenUI::Batch([&] {
            const auto& Current     = State.Torrents.Peek();
            const bool SameTorrents = Current.size() == Snapshot.size() &&
                                      std::ranges::equal(Current, Snapshot, {}, &TorrentEntry::Id, &TorrentStats::Id);

            if (!SameTorrents) {
                std::map<TorrentId, std::shared_ptr<TorrentEntry>> Existing;
                for (const auto& Entry : Current) {
                    Existing.emplace(Entry->Id, Entry);
                }

                std::vector<std::shared_ptr<TorrentEntry>> Rebuilt;
                for (const TorrentStats& Stats : Snapshot) {
                    const auto Found = Existing.find(Stats.Id);
                    if (Found != Existing.end()) {
                        Rebuilt.push_back(Found->second);
                    } else {
                        auto Entry = std::make_shared<TorrentEntry>();
                        Entry->Id  = Stats.Id;
                        Rebuilt.push_back(std::move(Entry));
                    }
                }
                State.Torrents = std::move(Rebuilt);

                const TorrentId Selected = State.SelectedTorrent.Peek();
                if (std::ranges::none_of(Snapshot, [&](const TorrentStats& Stats) { return Stats.Id == Selected; })) {
                    State.SelectedTorrent = kInvalidTorrentId;
                }
            }

            const auto& Entries = State.Torrents.Peek();
            for (std::size_t I = 0; I < Snapshot.size(); ++I) {
                CopyStats(*Entries[I], Snapshot[I]);
            }
        });
    }

    void SyncDetails(ToriState& State) {
        const TorrentId Selected = State.SelectedTorrent.Peek();
        std::shared_ptr<const TorrentDetails> Latest;
        if (State.ShowDetails.Peek() && Selected != kInvalidTorrentId) { Latest = State.Engine->Details(Selected); }
        // The engine hands back the same object until it builds a new one, so this only rebuilds the pane on change
        if (Latest != State.Details.Peek()) { State.Details = std::move(Latest); }
    }

    UiSettings LoadUiSettings(const std::filesystem::path& StateFolder) {
        UiSettings Settings;
        if (StateFolder.empty()) { return Settings; }
        const std::optional<std::string> Data = ReadWholeFile(SettingsPath(StateFolder), 1024 * 1024);
        if (!Data) { return Settings; }
        try {
            const Bencode::Value Root = Bencode::Decode(*Data);
            if (const std::string* Folder = Root.FindString("last destination")) {
                Settings.LastDestination = PathFromUtf8(*Folder);
            }
            if (const Bencode::Integer* Show = Root.FindInteger("show details")) { Settings.ShowDetails = *Show != 0; }
            if (const Bencode::Integer* Split = Root.FindInteger("details split")) {
                Settings.DetailsSplit = std::clamp(static_cast<float>(*Split) / 1000.0f, 0.1f, 0.9f);
            }
        } catch (const ParseError&) {}
        return Settings;
    }

    void SaveUiSettings(const std::filesystem::path& StateFolder, const UiSettings& Settings) {
        if (StateFolder.empty()) { return; }
        try {
            WriteFileAtomically(SettingsPath(StateFolder),
                                Bencode::Encode(Bencode::Dict {
                                  {"last destination", PathToUtf8(Settings.LastDestination)},
                                  {"show details", Settings.ShowDetails ? 1 : 0},
                                  {"details split", static_cast<Bencode::Integer>(Settings.DetailsSplit * 1000.0f)},
                                }));
        } catch (const std::runtime_error&) {}
    }

    std::shared_ptr<TorrentEntry> FindEntry(const ToriState& State, const TorrentId Id) {
        for (const auto& Entry : State.Torrents.Peek()) {
            if (Entry->Id == Id) { return Entry; }
        }
        return nullptr;
    }
}  // namespace Tori::UI
