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

#include <XenUI/XenUI.hpp>

namespace Tori::UI {
    inline const XenUI::SvgIcon kFolderIcon = XenUI::SvgIcon::FromPathData(
      "M10,4H4C2.89,4 2,4.89 2,6V18A2,2 0 0,0 4,20H20A2,2 0 0,0 22,18V8C22,6.89 21.1,6 20,6H12L10,4Z");

    inline const XenUI::SvgIcon kUpIcon =
      XenUI::SvgIcon::FromPathData("M7.41,15.41L12,10.83L16.59,15.41L18,14L12,8L6,14L7.41,15.41Z");

    inline const XenUI::SvgIcon kDownIcon =
      XenUI::SvgIcon::FromPathData("M7.41,8.58L12,13.17L16.59,8.58L18,10L12,16L6,10L7.41,8.58Z");

    inline const XenUI::SvgIcon kRatioIcon = XenUI::SvgIcon::FromPathData(
      "M12,2A10,10 0 0,1 22,12A10,10 0 0,1 12,22A10,10 0 0,1 2,12A10,10 0 0,1 12,2M12,4A8,8 0 0,0 4,12A8,8 0 0,0 "
      "12,20A4,4 0 0,1 8,16A4,4 0 0,1 12,12A4,4 0 0,0 16,8A4,4 0 0,0 12,4M12,6.5A1.5,1.5 0 0,1 13.5,8A1.5,1.5 0 0,1 "
      "12,9.5A1.5,1.5 0 0,1 10.5,8A1.5,1.5 0 0,1 12,6.5M12,14.5A1.5,1.5 0 0,0 10.5,16A1.5,1.5 0 0,0 12,17.5A1.5,1.5 0 "
      "0,0 13.5,16A1.5,1.5 0 0,0 12,14.5Z");
}  // namespace Tori::UI
