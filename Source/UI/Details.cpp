#include "UI/Details.hpp"

#include "Core/Format.hpp"
#include "Core/Utf8.hpp"
#include "Engine/Session.hpp"

#include <chrono>
#include <format>

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        Node CellLabel(const Theme& Style, std::string Content, const TextAlignment Alignment = TextAlignment::Leading) {
            return Text {
              .Content   = std::move(Content),
              .Font      = Style.BodyFont,
              .Color     = Style.Text,
              .Alignment = Alignment,
              .Wrapping  = TextWrapping::NoWrap,
              .Trimming  = TextTrimming::Ellipsis,
            };
        }

        Node Message(const Theme& Style, std::string Content) {
            return Align {
              .Position = Alignment::Center(),
              .Child    = Text {.Content = std::move(Content), .Color = Style.TextMuted},
            };
        }

        std::string Percent(const double Fraction) {
            return std::format("{:.1f}%", std::clamp(Fraction, 0.0, 1.0) * 100.0);
        }

        /// Files of a multi-file torrent all start with the torrent's folder; leave it out.
        std::string DisplayPath(const TorrentDetails& Details, const FileDetails& File) {
            if (Details.Files.size() <= 1) { return PathToUtf8(File.Path); }
            std::filesystem::path Inner;
            for (auto It = std::next(File.Path.begin()); It != File.Path.end(); ++It) { Inner /= *It; }
            return PathToUtf8(Inner);
        }

        void SetWanted(ToriState* State, const int Index, const bool Wanted) {
            const std::shared_ptr<const TorrentDetails> Current = State->Details.Peek();
            if (!Current) { return; }

            // Show the change straight away rather than when the worker next reports
            auto Updated = std::make_shared<TorrentDetails>(*Current);
            std::vector<int> Skipped;
            for (FileDetails& File : Updated->Files) {
                if (File.Index == Index) { File.Wanted = Wanted; }
                if (!File.Wanted) { Skipped.push_back(File.Index); }
            }
            State->Engine->SetSkippedFiles(Current->Id, std::move(Skipped));
            State->Details = std::shared_ptr<const TorrentDetails>(std::move(Updated));
        }

        Node FilesTab(ToriState* State, const Theme& Style, const std::shared_ptr<const TorrentDetails>& Details) {
            if (Details->Files.empty()) { return Message(Style, "The file list appears once the metadata arrives."); }
            return Table {
              .Columns =
                {
                  TableColumn {.Title = "", .Width = 36.0f, .MinWidth = 36.0f, .Sortable = false},
                  TableColumn {.Title = "Name", .Width = 280.0f, .Sortable = false},
                  TableColumn {.Title = "Size", .Width = 90.0f, .Alignment = TextAlignment::Trailing, .Sortable = false},
                  TableColumn {.Title = "Done", .Alignment = TextAlignment::Trailing, .Sortable = false},
                },
              .RowCount = static_cast<int>(Details->Files.size()),
              .CellBuilder =
                [State, Details, Style](const int Row, const int Column) -> Node {
                    const FileDetails& File = Details->Files[static_cast<std::size_t>(Row)];
                    switch (Column) {
                        case 0:
                            return Checkbox {
                              .Value     = File.Wanted,
                              .OnChanged = [State, Index = File.Index](const bool On) { SetWanted(State, Index, On); },
                            };
                        case 1:
                            return CellLabel(Style, DisplayPath(*Details, File));
                        case 2:
                            return CellLabel(Style, FormatBytes(static_cast<double>(File.Size)), TextAlignment::Trailing);
                        default:
                            return CellLabel(Style,
                                             Percent(File.Size > 0 ? static_cast<double>(File.Done) / static_cast<double>(File.Size) : 1.0),
                                             TextAlignment::Trailing);
                    }
                },
              .Label = "Files",
            };
        }

        Node PeersTab(const Theme& Style, const std::shared_ptr<const TorrentDetails>& Details) {
            if (Details->Peers.empty()) { return Message(Style, "Not connected to any peers."); }
            return Table {
              .Columns =
                {
                  TableColumn {.Title = "Address", .Width = 150.0f, .Sortable = false},
                  TableColumn {.Title = "Client", .Width = 130.0f, .Sortable = false},
                  TableColumn {.Title = "Has", .Width = 60.0f, .Alignment = TextAlignment::Trailing, .Sortable = false},
                  TableColumn {.Title = "Down", .Width = 80.0f, .Alignment = TextAlignment::Trailing, .Sortable = false},
                  TableColumn {.Title = "Up", .Width = 80.0f, .Alignment = TextAlignment::Trailing, .Sortable = false},
                  TableColumn {.Title = "Flags", .Sortable = false},
                },
              .RowCount = static_cast<int>(Details->Peers.size()),
              .CellText =
                [Details](const int Row, const int Column) -> std::string {
                    const PeerDetails& Peer = Details->Peers[static_cast<std::size_t>(Row)];
                    switch (Column) {
                        case 0: return Peer.Address;
                        case 1: return Peer.Client;
                        case 2: return Percent(Peer.Progress);
                        case 3: return Peer.DownloadSpeed > 0.0 ? FormatRate(Peer.DownloadSpeed) : std::string {};
                        case 4: return Peer.UploadSpeed > 0.0 ? FormatRate(Peer.UploadSpeed) : std::string {};
                        default: return Peer.Flags;
                    }
                },
              .Label = "Peers",
            };
        }

        std::string TrackerStateText(const TrackerStatus& Tracker) {
            switch (Tracker.State) {
                case TrackerState::NotContacted: return "Not contacted";
                case TrackerState::Announcing: return "Announcing...";
                case TrackerState::Working: return Tracker.Message.empty() ? "Working" : "Working: " + Tracker.Message;
                case TrackerState::Failed: return Tracker.Message.empty() ? "Failed" : Tracker.Message;
            }
            return {};
        }

        std::string NextAnnounceText(const TrackerStatus& Tracker) {
            if (!Tracker.NextAnnounce) { return {}; }
            const auto Seconds =
              std::chrono::duration_cast<std::chrono::seconds>(*Tracker.NextAnnounce - std::chrono::steady_clock::now()).count();
            if (Seconds <= 0) { return "now"; }
            return Seconds < 60 ? std::format("in {}s", Seconds) : std::format("in {}m {:02}s", Seconds / 60, Seconds % 60);
        }

        Node TrackersTab(const Theme& Style, const std::shared_ptr<const TorrentDetails>& Details) {
            if (Details->Trackers.empty()) { return Message(Style, "No trackers; peers come from the DHT."); }
            return Table {
              .Columns =
                {
                  TableColumn {.Title = "URL", .Width = 230.0f, .Sortable = false},
                  TableColumn {.Title = "Status", .Width = 170.0f, .Sortable = false},
                  TableColumn {.Title = "Seeds / Peers", .Width = 100.0f, .Alignment = TextAlignment::Trailing, .Sortable = false},
                  TableColumn {.Title = "Next", .Alignment = TextAlignment::Trailing, .Sortable = false},
                },
              .RowCount = static_cast<int>(Details->Trackers.size()),
              .CellText =
                [Details](const int Row, const int Column) -> std::string {
                    const TrackerStatus& Tracker = Details->Trackers[static_cast<std::size_t>(Row)];
                    switch (Column) {
                        case 0: return Tracker.Url;
                        case 1: return TrackerStateText(Tracker);
                        case 2:
                            if (!Tracker.Seeders && !Tracker.Leechers) { return {}; }
                            return std::format("{} / {}",
                                               Tracker.Seeders ? std::to_string(*Tracker.Seeders) : "?",
                                               Tracker.Leechers ? std::to_string(*Tracker.Leechers) : "?");
                        default: return NextAnnounceText(Tracker);
                    }
                },
              .Label = "Trackers",
            };
        }
    }  // namespace

    Node DetailsPane::Build(BuildContext& Context) const {
        const Theme& Style       = Theme::Of(Context);
        const TorrentId Selected = State->SelectedTorrent();
        const std::shared_ptr<const TorrentDetails> Details = State->Details();

        if (Selected == kInvalidTorrentId) { return Message(Style, "Select a torrent to see its files, peers and trackers."); }
        if (!Details || Details->Id != Selected) { return Message(Style, "Loading..."); }

        return Container {
          .Padding = EdgeInsets::Symmetric(8.0f, 6.0f),
          .Child =
            TabView {
              .Tabs =
                {
                  Tab {.Label = std::format("Files ({})", Details->Files.size()), .Content = FilesTab(State, Style, Details)},
                  Tab {.Label = std::format("Peers ({})", Details->Peers.size()), .Content = PeersTab(Style, Details)},
                  Tab {.Label = std::format("Trackers ({})", Details->Trackers.size()), .Content = TrackersTab(Style, Details)},
                },
              .Selected = &State->DetailsTab,
            },
        };
    }
}  // namespace Tori::UI
