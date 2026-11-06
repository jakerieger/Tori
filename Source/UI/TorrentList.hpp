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

namespace Tori::UI {
    struct TorrentListItem {
        ToriState* State = nullptr;
        /// @brief Held rather than looked up by position: after a removal a row can rebuild before the list has
        /// shrunk, and its position would then be past the end.
        std::shared_ptr<TorrentEntry> Entry;

        XenUI::Node Build(XenUI::BuildContext& Context) const;
    };

    struct TorrentListView {
        ToriState* State = nullptr;

        XenUI::Node Build(XenUI::BuildContext& Context) const;
    };
}  // namespace Tori::UI
