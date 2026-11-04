#include "UI/TorrentList.hpp"

#include "Core/Format.hpp"
#include "UI/Actions.hpp"
#include "UI/Icons.hpp"

#include <format>
#include <optional>

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        Color ProgressFillColor(const Theme& Style, const TorrentState State) {
            switch (State) {
                case TorrentState::Downloading:
                    return Style.Accent;
                case TorrentState::Seeding:
                case TorrentState::Complete:
                    return Style.Success;
                default:
                    return Style.TextDisabled;
            }
        }

        // Indeterminate while metadata is still being fetched, since the total size isn't known yet
        std::optional<float> ProgressValue(const TorrentEntry& Torrent) {
            if (Torrent.State() == TorrentState::FetchingMetadata) { return std::nullopt; }
            return ComputeProgress(Torrent.DownloadedSize(), Torrent.TotalSize());
        }

        std::string StatusText(const TorrentEntry& Torrent) {
            switch (Torrent.State()) {
                case TorrentState::Error:
                    return std::format("{}: {}", TorrentStateString(Torrent.State()), Torrent.ErrorMessage());
                case TorrentState::Downloading:
                case TorrentState::Seeding:
                    return std::format("{} ({} {})",
                                       TorrentStateString(Torrent.State()),
                                       Torrent.Peers(),
                                       Torrent.Peers() == 1 ? "peer" : "peers");
                default:
                    return TorrentStateString(Torrent.State());
            }
        }

        Node TorrentNameAndPath(const Theme& Style, const TorrentEntry& Torrent) {
            return Row {
              .Spacing        = 8.0f,
              .MainAlignment  = MainAxisAlignment::Start,
              .CrossAlignment = CrossAxisAlignment::Center,
              .Children =
                {
                  Text {
                    .Content = Torrent.Name(),
                    .Font    = Font {.Size = 14.0f, .Weight = FontWeight::Bold},
                    .Color   = Style.Text,
                  },
                  Text {
                    .Content = "•",
                    .Color   = Style.TextMuted,
                  },
                  Text {
                    .Content = Torrent.Destination().string(),
                    .Color   = Style.TextMuted,
                  },
                },
            };
        }

        // Shows download progress in bytes as well as transfer speeds
        Node TorrentProgressStats(const Theme& Style, const TorrentEntry& Torrent) {
            const float Percent = ComputeProgress(Torrent.DownloadedSize(), Torrent.TotalSize()) * 100.0f;

            return Row {
              .Spacing        = 8.0f,
              .MainAlignment  = MainAxisAlignment::Start,
              .CrossAlignment = CrossAxisAlignment::Center,
              .Children =
                {
                  Text {
                    .Content = std::format("{} of {} ({:.1f}%)",
                                           FormatBytes(Torrent.DownloadedSize()),
                                           FormatBytes(Torrent.TotalSize()),
                                           Percent),
                    .Color   = Style.Text,
                  },
                  Text {
                    .Content = "—",
                    .Color   = Style.TextMuted,
                  },
                  Row {
                    .Spacing       = 4.0f,
                    .MainAlignment = MainAxisAlignment::Center,
                    .Children =
                      {
                        Icon {.Source = kDownIcon, .Size = 12.0f},
                        Text {.Content = FormatRate(Torrent.DownloadSpeed()), .Color = Style.TextMuted},
                        Icon {.Source = kUpIcon, .Size = 12.0f},
                        Text {.Content = FormatRate(Torrent.UploadSpeed()), .Color = Style.TextMuted},
                        Icon {.Source = kRatioIcon, .Size = 12.0f},
                        Text {
                          .Content = std::format(
                            "{:.2f}",
                            ComputeRatio(Torrent.UploadedTotal(), Torrent.DownloadedTotal(), Torrent.DownloadedSize())),
                          .Color = Style.TextMuted,
                        },
                      },
                  },
                  Text {
                    .Content = "—",
                    .Color   = Style.TextMuted,
                  },
                  Text {
                    .Content = StatusText(Torrent),
                    .Color   = Torrent.State() == TorrentState::Error ? Style.Danger : Style.TextMuted,
                  },
                },
            };
        }
    }  // namespace

    Node TorrentListItem::Build(BuildContext& Context) const {
        const Theme& Style          = Theme::Of(Context);
        const TorrentEntry& Torrent = *Entry;
        const TorrentId Id          = Torrent.Id;
        const bool Selected         = State->SelectedTorrent() == Id;

        // Reading the state subscribes this row, so the menu's Pause and Resume items stay current
        (void)Torrent.State();
        return ContextMenu {
          .Items = TorrentMenuItems(State, Id),
          .Child =
        GestureDetector {
          .OnTap    = [State = State, Id](const TapEvent&) { State->SelectedTorrent = Id; },
          .Behavior = HitTestBehavior::Opaque,
          .Child =
            Container {
              .Padding    = EdgeInsets::Symmetric(14.0f, 0.0f),
              .Background = Selected ? Style.Surface : Style.Background,
              .Child =
                Row {
                  .Spacing = 14.0f,
                  .Children =
                    {
                      SizedBox {
                        .Width  = 48.0f,
                        .Height = 48.0f,
                        .Child =
                          Icon {
                            .Source = kFolderIcon,
                            .Color  = Style.Text,
                          },
                      },
                      Expanded {
                        .Child =
                          Column {
                            .MainAlignment  = MainAxisAlignment::Center,
                            .CrossAlignment = CrossAxisAlignment::Start,
                            .Children =
                              {
                                TorrentNameAndPath(Style, Torrent),
                                Padding {
                                  .Insets = EdgeInsets::Symmetric(0.0f, 8.0f),
                                  .Child =
                                    ProgressBar {
                                      .Value  = ProgressValue(Torrent),
                                      .Min    = 0.0f,
                                      .Max    = 1.0f,
                                      .Height = 8.0f,
                                      .Fill   = ProgressFillColor(Style, Torrent.State()),
                                    },
                                },
                                TorrentProgressStats(Style, Torrent),
                              },
                          },
                      },
                    },
                },
            },
        },
        };
    }

    Node TorrentListView::Build(BuildContext& Context) const {
        (void)Context;
        return ListView {
          .ItemCount  = static_cast<int>(State->Torrents().size()),
          .ItemExtent = 100.0f,
          // Keyed by torrent, so a row's state (its open menu, say) stays with its torrent as others are removed
          .ItemBuilder =
            [State = State, Entries = State->Torrents()](const int Index) {
                const std::shared_ptr<TorrentEntry>& Entry = Entries[static_cast<std::size_t>(Index)];
                return Keyed(XenUI::Key(Entry->Id), TorrentListItem {.State = State, .Entry = Entry});
            },
          .Direction     = Axis::Vertical,
          .ShowScrollbar = true,
        };
    }
}  // namespace Tori::UI
