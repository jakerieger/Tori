#pragma once

#include "UI/Model.hpp"

namespace Tori::UI {
    /// @brief Bottom bar showing total transfer rates and the overall share ratio.
    struct StatusBar {
        ToriState* State = nullptr;

        XenUI::Node Build(XenUI::BuildContext& Context) const;
    };
}  // namespace Tori::UI
