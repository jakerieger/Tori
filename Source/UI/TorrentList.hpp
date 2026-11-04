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
