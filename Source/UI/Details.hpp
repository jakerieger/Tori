#pragma once

#include "UI/Model.hpp"

namespace Tori::UI {
    /// @brief Files, Peers and Trackers tabs for the selected torrent.
    struct DetailsPane {
        ToriState* State = nullptr;

        XenUI::Node Build(XenUI::BuildContext& Context) const;
    };
}  // namespace Tori::UI
