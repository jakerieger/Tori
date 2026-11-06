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

#include "UI/StatusBar.hpp"

#include "Core/Format.hpp"
#include "UI/Icons.hpp"

#include <format>

namespace Tori::UI {
    using namespace XenUI;

    namespace {
        Node Stat(const Theme& Style, const SvgIcon& Glyph, std::string Value) {
            return Row {
              .Spacing = 4.0f,
              .Children =
                {
                  Icon {.Source = Glyph, .Size = 12.0f},
                  Text {.Content = std::move(Value), .Color = Style.TextMuted},
                },
            };
        }
    }  // namespace

    Node StatusBar::Build(BuildContext& Context) const {
        const Theme& Style = Theme::Of(Context);

        double DownloadSpeed = 0.0;
        double UploadSpeed   = 0.0;
        double Downloaded    = 0.0;
        double Done          = 0.0;
        double Uploaded      = 0.0;
        for (const auto& Torrent : State->Torrents()) {
            DownloadSpeed += Torrent->DownloadSpeed();
            UploadSpeed += Torrent->UploadSpeed();
            Downloaded += Torrent->DownloadedTotal();
            Done += Torrent->DownloadedSize();
            Uploaded += Torrent->UploadedTotal();
        }

        return Container {
          .Height     = 36.0f,
          .Padding    = EdgeInsets::Symmetric(14.0f, 0.0f),
          .Background = Style.Chrome,
          .Child =
            Align {
              .Position = Alignment::CenterRight(),
              .Child =
                Row {
                  .Spacing       = 14.0f,
                  .MainAlignment = MainAxisAlignment::Center,
                  .Children =
                    {
                      Expanded {},
                      Stat(Style, kDownIcon, FormatRate(DownloadSpeed)),
                      Stat(Style, kUpIcon, FormatRate(UploadSpeed)),
                      Stat(Style, kRatioIcon, std::format("{:.2f}", ComputeRatio(Uploaded, Downloaded, Done))),
                    },
                },
            },
        };
    }
}  // namespace Tori::UI
