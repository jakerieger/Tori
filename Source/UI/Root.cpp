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

#include "UI/Root.hpp"

#include "Core/Utf8.hpp"
#include "UI/Actions.hpp"
#include "UI/Dialogs.hpp"
#include "UI/Details.hpp"
#include "UI/Menus.hpp"
#include "UI/StatusBar.hpp"
#include "UI/TorrentList.hpp"

#include "Resource.h"

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        const SvgIcon& Logo() {
            static const SvgIcon Parsed = SvgIcon::FromResource(IDR_LOGO);
            return Parsed;
        }

        bool IsTorrentFile(const std::filesystem::path& File) {
            std::wstring Extension = File.extension().wstring();
            for (wchar_t& Character : Extension) {
                Character = static_cast<wchar_t>(towlower(Character));
            }
            return Extension == L".torrent";
        }

        /// What a drop from Explorer or a browser would add, as command-line style arguments.
        std::vector<std::wstring> DroppedArguments(const DragData& Data) {
            std::vector<std::wstring> Arguments;
            for (const std::filesystem::path& File : Data.Files) {
                if (IsTorrentFile(File)) { Arguments.push_back(File.wstring()); }
            }
            if (Data.Text && Data.Text->starts_with("magnet:")) { Arguments.push_back(Utf8ToWide(*Data.Text)); }
            return Arguments;
        }
    }  // namespace

    Node Root::Build(BuildContext& Context) const {
        State->Overlay = OverlayController::Of(Context);
        State->Popup   = PopupStyle::Capture(Context);
        // Files opened at startup (double-clicked in Explorer) can arrive before there was an overlay to show
        // their Add dialog in
        if (!State->PendingAdds.empty() && !State->ActiveDialog.IsShown()) {
            App::Post([State = State] { ShowNextAddDialog(*State); });
        }

        Node Body = TorrentListView {.State = State};
        if (State->ShowDetails()) {
            Body = SplitView {
              .Direction = Axis::Vertical,
              .Ratio     = &State->DetailsSplit,
              .MinFirst  = 120.0f,
              .MinSecond = 140.0f,
              .First     = Body,
              .Second    = DetailsPane {.State = State},
            };
        }

        return Provide<TextStyle> {
          .Value = {.Font = {.Size = 13.0f}},
          .Child =
            Container {
              .Child =
                Column {
                  .CrossAlignment = CrossAxisAlignment::Stretch,
                  .Children =
                    {
                      TitleBar {
                        .Title = "Tori",
                        .Logo  = Logo(),
                        .Menus = MainMenus(State),
                      },
                      Expanded {
                        .Child = DropTarget {
                          .OnOver =
                            [](const DropEvent& Event) {
                                return Event.Data->External && !DroppedArguments(*Event.Data).empty()
                                         ? DropEffect::Copy
                                         : DropEffect::None;
                            },
                          .OnDrop =
                            [State = State](const DropEvent& Event) {
                                // Dialogs open after the drop has finished
                                App::Post([State, Arguments = DroppedArguments(*Event.Data)] {
                                    HandleArguments(*State, Arguments);
                                });
                            },
                          .Behavior = HitTestBehavior::Opaque,
                          .Child    = Body,
                        },
                      },
                      StatusBar {.State = State},
                    },
                },
            },
        };
    }
}  // namespace Tori::UI
