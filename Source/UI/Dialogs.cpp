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

#include "UI/Dialogs.hpp"

#include "Core/FileIO.hpp"
#include "Core/Format.hpp"
#include "Core/Utf8.hpp"
#include "Engine/Session.hpp"
#include "Meta/MagnetUri.hpp"
#include "Meta/TorrentCreator.hpp"
#include "UI/Actions.hpp"
#include "UI/Updates.hpp"

#include <atomic>
#include <format>
#include <mutex>
#include <sstream>
#include <thread>

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        constexpr float kDialogWidth = 520.0f;

        Node FieldLabel(const Theme& Style, std::string Content) {
            return Text {.Content = std::move(Content), .Color = Style.TextMuted};
        }

        Node ErrorText(const Theme& Style, std::string Content) {
            return Text {.Content = std::move(Content), .Color = Style.Danger};
        }

        Node
        CellLabel(const Theme& Style, std::string Content, const TextAlignment Alignment = TextAlignment::Leading) {
            return Text {
              .Content   = std::move(Content),
              .Font      = Style.BodyFont,
              .Color     = Style.Text,
              .Alignment = Alignment,
              .Wrapping  = TextWrapping::NoWrap,
              .Trimming  = TextTrimming::Ellipsis,
            };
        }

        /// A file's path without the torrent's own folder, which every file of a multi-file torrent starts with.
        std::string PathInTorrent(const Metainfo& Meta, const TorrentFile& File) {
            if (!Meta.IsMultiFile) { return PathToUtf8(File.Path); }
            std::filesystem::path Inner;
            for (auto It = std::next(File.Path.begin()); It != File.Path.end(); ++It) {
                Inner /= *It;
            }
            return PathToUtf8(Inner);
        }

        /// A folder path that can be typed or chosen with Browse.
        struct FolderField {
            Signal<std::string>* Bind = nullptr;
            const Window* Owner       = nullptr;

            [[nodiscard]] Node Build(BuildContext& Context) const {
                (void)Context;
                return Row {
                  .Spacing        = 8.0f,
                  .CrossAlignment = CrossAxisAlignment::Center,
                  .Children =
                    {
                      Expanded {.Child = TextField {.Bind = Bind, .Placeholder = "Folder"}},
                      Button {
                        .Label = "Browse...",
                        .OnPressed =
                          [Bind = Bind, Owner = Owner] {
                              const std::optional<std::filesystem::path> Chosen = FileDialog::PickFolder({
                                .Title         = "Save to",
                                .InitialFolder = PathFromUtf8(Bind->Peek()),
                                .Owner         = Owner,
                              });
                              if (Chosen) { *Bind = PathToUtf8(*Chosen); }
                          },
                      },
                    },
                };
            }
        };

        /// The folder typed or chosen, if it's usable.
        std::optional<std::filesystem::path> ParseFolder(const std::string& Text) {
            std::filesystem::path Folder = PathFromUtf8(Text);
            if (Folder.empty() || !Folder.is_absolute()) { return std::nullopt; }
            return Folder;
        }

        // ---- Add torrent -----------------------------------------------------------------------------------------

        struct AddModel {
            ToriState* State = nullptr;
            Metainfo Meta;
            Signal<std::string> Destination;
            Signal<std::vector<bool>> Wanted;  // By index into Meta.Files
            Signal<bool> Paused = false;
            Signal<std::string> Error;
        };

        struct AddTorrentForm {
            std::shared_ptr<AddModel> Model;

            [[nodiscard]] Node Build(BuildContext& Context) const {
                const Theme& Style            = Theme::Of(Context);
                const Metainfo& Meta          = Model->Meta;
                const std::vector<bool>& Want = Model->Wanted();

                std::vector<int> Rows;  // File index of each table row; pad files aren't shown
                std::int64_t Selected = 0;
                for (std::size_t I = 0; I < Meta.Files.size(); ++I) {
                    if (Meta.Files[I].IsPadding) { continue; }
                    Rows.push_back(static_cast<int>(I));
                    if (Want[I]) { Selected += Meta.Files[I].Length; }
                }
                const auto Total = static_cast<double>(Meta.TotalSize);

                std::vector<Node> Children = {
                  Text {
                    .Content  = Meta.Name,
                    .Font     = Font {.Size = 14.0f, .Weight = FontWeight::Bold},
                    .Color    = Style.Text,
                    .Wrapping = TextWrapping::NoWrap,
                    .Trimming = TextTrimming::Ellipsis,
                  },
                  Text {
                    .Content = std::format("{} in {} {}{}",
                                           FormatBytes(Total),
                                           Rows.size(),
                                           Rows.size() == 1 ? "file" : "files",
                                           Meta.IsPrivate ? " · private" : ""),
                    .Color   = Style.TextMuted,
                  },
                  FieldLabel(Style, "Save to"),
                  FolderField {.Bind = &Model->Destination, .Owner = Model->State->MainWindow},
                };

                if (Rows.size() > 1) {
                    const auto SetAll = [Model = Model](const bool On) {
                        Model->Wanted = std::vector<bool>(Model->Meta.Files.size(), On);
                    };
                    Children.push_back(Row {
                      .Spacing        = 8.0f,
                      .CrossAlignment = CrossAxisAlignment::Center,
                      .Children =
                        {
                          FieldLabel(Style,
                                     std::format("Files: {} of {} selected",
                                                 FormatBytes(static_cast<double>(Selected)),
                                                 FormatBytes(Total))),
                          Expanded {},
                          Button {.Label     = "All",
                                  .OnPressed = [SetAll] { SetAll(true); },
                                  .Variant   = ButtonVariant::Ghost},
                          Button {.Label     = "None",
                                  .OnPressed = [SetAll] { SetAll(false); },
                                  .Variant   = ButtonVariant::Ghost},
                        },
                    });
                    Children.push_back(SizedBox {
                      .Height = 220.0f,
                      .Child =
                        Table {
                          .Columns =
                            {
                              TableColumn {.Title = "", .Width = 36.0f, .MinWidth = 36.0f, .Sortable = false},
                              TableColumn {.Title = "Name", .Width = 330.0f, .Sortable = false},
                              TableColumn {.Title = "Size", .Alignment = TextAlignment::Trailing, .Sortable = false},
                            },
                          .RowCount    = static_cast<int>(Rows.size()),
                          .CellBuilder = [Model = Model, Rows, Style](const int Row, const int Column) -> Node {
                              const int Index         = Rows[static_cast<std::size_t>(Row)];
                              const TorrentFile& File = Model->Meta.Files[static_cast<std::size_t>(Index)];
                              if (Column == 0) {
                                  return Checkbox {
                                    .Value = Model->Wanted.Peek()[static_cast<std::size_t>(Index)],
                                    .OnChanged =
                                      [Model, Index](const bool On) {
                                          std::vector<bool> Next                = Model->Wanted.Peek();
                                          Next[static_cast<std::size_t>(Index)] = On;
                                          Model->Wanted                         = std::move(Next);
                                      },
                                  };
                              }
                              if (Column == 1) { return CellLabel(Style, PathInTorrent(Model->Meta, File)); }
                              return CellLabel(Style,
                                               FormatBytes(static_cast<double>(File.Length)),
                                               TextAlignment::Trailing);
                          },
                          .Label = "Files",
                        },
                    });
                }

                Children.push_back(Checkbox {
                  .Value     = Model->Paused(),
                  .OnChanged = [Model = Model](const bool On) { Model->Paused = On; },
                  .Label     = "Add paused",
                });
                if (!Model->Error().empty()) { Children.push_back(ErrorText(Style, Model->Error())); }

                return Column {
                  .Spacing        = 10.0f,
                  .CrossAlignment = CrossAxisAlignment::Stretch,
                  .Children       = std::move(Children),
                };
            }
        };

        // ---- Open link -------------------------------------------------------------------------------------------

        struct LinkModel {
            ToriState* State = nullptr;
            Signal<std::string> Link;
            Signal<std::string> Destination;
            Signal<std::string> Error;
        };

        struct OpenLinkForm {
            std::shared_ptr<LinkModel> Model;
            std::function<void()> Submit;

            [[nodiscard]] Node Build(BuildContext& Context) const {
                const Theme& Style         = Theme::Of(Context);
                std::vector<Node> Children = {
                  TextField {
                    .Bind             = &Model->Link,
                    .OnSubmit         = [Submit = Submit](const std::string&) { Submit(); },
                    .Placeholder      = "magnet:?xt=urn:btih:...",
                    .AutoFocus        = true,
                    .SelectAllOnFocus = true,
                  },
                  FieldLabel(Style, "Save to"),
                  FolderField {.Bind = &Model->Destination, .Owner = Model->State->MainWindow},
                };
                if (!Model->Error().empty()) { Children.push_back(ErrorText(Style, Model->Error())); }
                return Column {
                  .Spacing        = 10.0f,
                  .CrossAlignment = CrossAxisAlignment::Stretch,
                  .Children       = std::move(Children),
                };
            }
        };

        // ---- Create torrent --------------------------------------------------------------------------------------

        /// Shared between the UI and the hashing thread.
        struct CreateJob {
            std::atomic<int> Hashed {0};
            std::atomic<int> Total {0};
            std::atomic<bool> Done {false};
            std::filesystem::path Source;
            std::mutex Mutex;
            std::optional<std::string> Torrent;  // Set when it was created and saved
            std::string Error;
        };

        struct CreateModel : std::enable_shared_from_this<CreateModel> {
            ToriState* State = nullptr;
            Signal<std::string> Source;
            Signal<std::string> Trackers;
            Signal<std::string> Comment;
            Signal<int> PieceSize     = 0;  // Index into kPieceSizes
            Signal<bool> Private      = false;
            Signal<bool> StartSeeding = true;
            Signal<std::string> Error;
            Signal<std::optional<float>> Progress;  // Set while hashing

            std::shared_ptr<CreateJob> Job;
            bool Finishing = false;
            bool Closed    = false;  // Cancelled after hashing finished but before the result was handled
            Timer Poll;
            std::jthread Worker;  // Last, so it's stopped and joined first
        };

        const std::vector<std::string> kPieceSizes = {
          "Auto",
          "16 KiB",
          "32 KiB",
          "64 KiB",
          "128 KiB",
          "256 KiB",
          "512 KiB",
          "1 MiB",
          "2 MiB",
          "4 MiB",
          "8 MiB",
          "16 MiB",
        };

        std::int64_t PieceLengthFor(const int Index) {
            return Index <= 0 ? 0 : (16LL * 1024) << (Index - 1);
        }

        /// One tracker per line; a blank line starts the next tier.
        std::vector<std::vector<std::string>> ParseTrackerTiers(const std::string& Text) {
            std::vector<std::vector<std::string>> Tiers(1);
            std::istringstream Lines(Text);
            std::string Line;
            while (std::getline(Lines, Line)) {
                const auto First = Line.find_first_not_of(" \t\r");
                if (First == std::string::npos) {
                    if (!Tiers.back().empty()) { Tiers.emplace_back(); }
                    continue;
                }
                const auto Last = Line.find_last_not_of(" \t\r");
                Tiers.back().push_back(Line.substr(First, Last - First + 1));
            }
            if (Tiers.back().empty()) { Tiers.pop_back(); }
            return Tiers;
        }

        struct CreateTorrentForm {
            std::shared_ptr<CreateModel> Model;

            [[nodiscard]] Node Build(BuildContext& Context) const {
                const Theme& Style  = Theme::Of(Context);
                const Window* Owner = Model->State->MainWindow;
                const bool Busy     = Model->Progress().has_value();

                std::vector<Node> Children = {
                  FieldLabel(Style, "File or folder to share"),
                  Row {
                    .Spacing        = 8.0f,
                    .CrossAlignment = CrossAxisAlignment::Center,
                    .Children =
                      {
                        Expanded {.Child = TextField {.Bind = &Model->Source, .Placeholder = "Path", .Enabled = !Busy}},
                        Button {
                          .Label = "File...",
                          .OnPressed =
                            [Model = Model, Owner] {
                                if (const auto File = FileDialog::OpenFile({.Title = "Share a file", .Owner = Owner})) {
                                    Model->Source = PathToUtf8(*File);
                                }
                            },
                          .Enabled = !Busy,
                        },
                        Button {
                          .Label = "Folder...",
                          .OnPressed =
                            [Model = Model, Owner] {
                                if (const auto Folder =
                                      FileDialog::PickFolder({.Title = "Share a folder", .Owner = Owner})) {
                                    Model->Source = PathToUtf8(*Folder);
                                }
                            },
                          .Enabled = !Busy,
                        },
                      },
                  },
                  FieldLabel(Style, "Trackers, one per line (a blank line starts a new tier)"),
                  TextArea {.Bind        = &Model->Trackers,
                            .Placeholder = "udp://tracker.example.org:1337/announce",
                            .Enabled     = !Busy,
                            .Lines       = 4},
                  FieldLabel(Style, "Comment"),
                  TextField {.Bind = &Model->Comment, .Enabled = !Busy},
                  Row {
                    .Spacing        = 10.0f,
                    .CrossAlignment = CrossAxisAlignment::Center,
                    .Children =
                      {
                        FieldLabel(Style, "Piece size"),
                        Select {
                          .Options       = kPieceSizes,
                          .SelectedIndex = Model->PieceSize(),
                          .OnChanged     = [Model = Model](const int Index) { Model->PieceSize = Index; },
                          .Width         = 110.0f,
                          .Label         = "Piece size",
                        },
                        Expanded {},
                        Checkbox {
                          .Value     = Model->Private(),
                          .OnChanged = [Model = Model](const bool On) { Model->Private = On; },
                          .Label     = "Private",
                          .Enabled   = !Busy,
                        },
                      },
                  },
                  Checkbox {
                    .Value     = Model->StartSeeding(),
                    .OnChanged = [Model = Model](const bool On) { Model->StartSeeding = On; },
                    .Label     = "Start seeding it",
                    .Enabled   = !Busy,
                  },
                };
                if (Busy) {
                    Children.push_back(ProgressBar {.Value = *Model->Progress(), .Height = 8.0f, .Label = "Hashing"});
                }
                if (!Model->Error().empty()) { Children.push_back(ErrorText(Style, Model->Error())); }

                return Column {
                  .Spacing        = 8.0f,
                  .CrossAlignment = CrossAxisAlignment::Stretch,
                  .Children       = std::move(Children),
                };
            }
        };

        void FinishCreate(const std::shared_ptr<CreateModel>& Model) {
            if (Model->Worker.joinable()) { Model->Worker.join(); }
            const std::shared_ptr<CreateJob> Job = std::exchange(Model->Job, nullptr);
            Model->Finishing                     = false;
            Model->Progress                      = std::nullopt;

            std::optional<std::string> Torrent;
            {
                std::scoped_lock Lock(Job->Mutex);
                if (!Job->Error.empty()) {
                    Model->Error = Job->Error;
                    return;
                }
                Torrent = std::move(Job->Torrent);
            }
            if (!Torrent || Model->Closed) { return; }  // Cancelled

            ToriState& Owner = *Model->State;
            const bool Seed  = Model->StartSeeding.Peek();
            Owner.ActiveDialog.Reset();
            if (!Seed) { return; }
            try {
                // The data is already where the torrent says, so it's checked and then seeded
                Owner.Engine->AddTorrent(Metainfo::Parse(*Torrent), Job->Source.parent_path());
            } catch (const std::exception& Ex) {
                ShowError(Owner, std::string("The torrent was saved but couldn't be seeded: ") + Ex.what());
            }
        }

        void StartCreate(const std::shared_ptr<CreateModel>& Model) {
            if (Model->Job) { return; }  // Already hashing
            Model->Error = std::string {};

            const std::filesystem::path Source = PathFromUtf8(Model->Source.Peek());
            std::error_code Ec;
            if (Source.empty() || !Source.is_absolute() || !std::filesystem::exists(Source, Ec)) {
                Model->Error = std::string("Choose a file or folder to share.");
                return;
            }

            const std::optional<std::filesystem::path> Output = FileDialog::SaveFile({
              .Title            = "Save torrent as",
              .InitialFolder    = Source.parent_path(),
              .FileName         = PathToUtf8(Source.filename()) + ".torrent",
              .Filters          = {{"Torrent files", "*.torrent"}},
              .DefaultExtension = "torrent",
              .Owner            = Model->State->MainWindow,
            });
            if (!Output) { return; }

            TorrentCreatorOptions Options;
            Options.Source       = Source;
            Options.PieceLength  = PieceLengthFor(Model->PieceSize.Peek());
            Options.TrackerTiers = ParseTrackerTiers(Model->Trackers.Peek());
            Options.Comment      = Model->Comment.Peek();
            Options.IsPrivate    = Model->Private.Peek();

            auto Job        = std::make_shared<CreateJob>();
            Job->Source     = Source;
            Model->Job      = Job;
            Model->Progress = 0.0f;
            Model->Worker   = std::jthread([Job, Options, Output = *Output](const std::stop_token Stop) {
                try {
                    std::optional<std::string> Created =
                      CreateTorrent(Options, Stop, [&](const int Hashed, const int Total) {
                          Job->Hashed = Hashed;
                          Job->Total  = Total;
                      });
                    if (Created) { WriteFileAtomically(Output, *Created); }
                    std::scoped_lock Lock(Job->Mutex);
                    Job->Torrent = std::move(Created);
                } catch (const std::exception& Ex) {
                    std::scoped_lock Lock(Job->Mutex);
                    Job->Error = Ex.what();
                }
                Job->Done = true;
            });

            // Signals are UI-thread only, so progress is copied over by a timer
            Model->Poll = Timer::Every(0.1, [Raw = Model.get()] {
                if (!Raw->Job || Raw->Finishing) { return; }
                const int Total = Raw->Job->Total.load();
                Raw->Progress =
                  Total > 0 ? static_cast<float>(Raw->Job->Hashed.load()) / static_cast<float>(Total) : 0.0f;
                if (Raw->Job->Done.load()) {
                    // Finishing may close the dialog, which destroys this timer, so do it outside the timer callback
                    Raw->Finishing = true;
                    App::Post([Self = Raw->shared_from_this()] { FinishCreate(Self); });
                }
            });
        }
    }  // namespace

    void ShowNextAddDialog(ToriState& State) {
        if (!State.Overlay || State.ActiveDialog.IsShown() || State.PendingAdds.empty()) { return; }

        auto Model         = std::make_shared<AddModel>();
        Model->State       = &State;
        Model->Meta        = std::move(State.PendingAdds.front());
        Model->Destination = PathToUtf8(SuggestedDestination(State));
        Model->Wanted      = std::vector<bool>(Model->Meta.Files.size(), true);
        State.PendingAdds.pop_front();

        // Closing the dialog destroys these callbacks, so they copy what they need into locals first
        const auto Close = [Model] {
            ToriState& Owner = *Model->State;
            Owner.ActiveDialog.Reset();
            ShowNextAddDialog(Owner);
        };
        const auto Add = [Model] {
            const std::shared_ptr<AddModel> Self                   = Model;
            const std::optional<std::filesystem::path> Destination = ParseFolder(Self->Destination.Peek());
            if (!Destination) {
                Self->Error = std::string("Choose a folder to save the torrent in.");
                return;
            }

            std::vector<int> Skipped;
            std::size_t Shown = 0;
            for (std::size_t I = 0; I < Self->Meta.Files.size(); ++I) {
                if (Self->Meta.Files[I].IsPadding) { continue; }
                ++Shown;
                if (!Self->Wanted.Peek()[I]) { Skipped.push_back(static_cast<int>(I)); }
            }
            if (Skipped.size() == Shown) {
                Self->Error = std::string("Choose at least one file to download.");
                return;
            }

            try {
                Self->State->Engine->AddTorrent(Self->Meta, *Destination, Self->Paused.Peek(), std::move(Skipped));
            } catch (const std::exception& Ex) {
                Self->Error = std::string(Ex.what());
                return;
            }
            RememberDestination(*Self->State, *Destination);
            ToriState& Owner = *Self->State;
            Owner.ActiveDialog.Reset();
            ShowNextAddDialog(Owner);
        };

        State.ActiveDialog =
          ShowDialog(*State.Overlay,
                     State.Popup,
                     Dialog {
                       .Title   = "Add torrent",
                       .Content = AddTorrentForm {.Model = Model},
                       .Actions =
                         {
                           DialogAction {.Label = "Cancel", .OnPressed = Close},
                           DialogAction {.Label = "Add", .OnPressed = Add, .Variant = ButtonVariant::Primary},
                         },
                       .OnDismiss = Close,
                       .Width     = kDialogWidth,
                     });
    }

    void ShowOpenLinkDialog(ToriState& State, std::string Link) {
        if (!State.Overlay || State.ActiveDialog.IsShown()) { return; }

        if (Link.empty()) {
            // Most people copy the link first, so offer it straight away
            const std::optional<std::string> Clipboard = XenUI::Clipboard::GetText();
            if (Clipboard && Clipboard->starts_with("magnet:")) { Link = *Clipboard; }
        }

        auto Model         = std::make_shared<LinkModel>();
        Model->State       = &State;
        Model->Link        = std::move(Link);
        Model->Destination = PathToUtf8(SuggestedDestination(State));

        const auto Close  = [Model] { Model->State->ActiveDialog.Reset(); };
        const auto Submit = [Model] {
            const std::shared_ptr<LinkModel> Self                  = Model;
            const std::optional<std::filesystem::path> Destination = ParseFolder(Self->Destination.Peek());
            if (!Destination) {
                Self->Error = std::string("Choose a folder to save the torrent in.");
                return;
            }
            try {
                Self->State->Engine->AddMagnet(MagnetUri::Parse(Self->Link.Peek()), *Destination);
            } catch (const std::exception& Ex) {
                Self->Error = std::string(Ex.what());
                return;
            }
            RememberDestination(*Self->State, *Destination);
            Self->State->ActiveDialog.Reset();
        };

        State.ActiveDialog =
          ShowDialog(*State.Overlay,
                     State.Popup,
                     Dialog {
                       .Title   = "Open magnet link",
                       .Content = OpenLinkForm {.Model = Model, .Submit = Submit},
                       .Actions =
                         {
                           DialogAction {.Label = "Cancel", .OnPressed = Close},
                           DialogAction {.Label = "Add", .OnPressed = Submit, .Variant = ButtonVariant::Primary},
                         },
                       .OnDismiss = Close,
                       .Width     = kDialogWidth,
                     });
    }

    void ShowCreateTorrentDialog(ToriState& State) {
        if (!State.Overlay || State.ActiveDialog.IsShown()) { return; }

        auto Model   = std::make_shared<CreateModel>();
        Model->State = &State;

        // Closing while hashing destroys the model, which stops and joins the hashing thread
        const auto Close = [Model] {
            Model->Closed = true;
            Model->State->ActiveDialog.Reset();
        };
        const auto Create = [Model] { StartCreate(Model); };

        State.ActiveDialog =
          ShowDialog(*State.Overlay,
                     State.Popup,
                     Dialog {
                       .Title   = "Create torrent",
                       .Content = CreateTorrentForm {.Model = Model},
                       .Actions =
                         {
                           DialogAction {.Label = "Cancel", .OnPressed = Close},
                           DialogAction {.Label = "Create...", .OnPressed = Create, .Variant = ButtonVariant::Primary},
                         },
                       .OnDismiss = Close,
                       .Width     = kDialogWidth,
                     },
                     false);  // Don't lose typed trackers to a stray click
    }

    void ShowAboutDialog(ToriState& State) {
        if (!State.Overlay || State.ActiveDialog.IsShown()) { return; }
        const auto Close   = [Target = &State] { Target->ActiveDialog.Reset(); };
        State.ActiveDialog = ShowDialog(
          *State.Overlay,
          State.Popup,
          Dialog {
            .Title     = "About Tori",
            .Content   = Text {.Content = std::format("Tori v{}\nA BitTorrent client for Windows, built on XenUI.\n\n"
                                                      "Supports .torrent files, magnet links, the DHT, HTTP and "
                                                      "UDP trackers, and creating torrents.",
                                                    kToriVersion)},
            .Actions   = {DialogAction {.Label = "Close", .OnPressed = Close, .Variant = ButtonVariant::Primary}},
            .OnDismiss = Close,
          });
    }
}  // namespace Tori::UI
