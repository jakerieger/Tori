#pragma once

#include "UI/Model.hpp"

namespace Tori::UI {
    struct Root {
        ToriState* State = nullptr;

        XenUI::Node Build(XenUI::BuildContext& Context) const;
    };
}  // namespace Tori::UI
