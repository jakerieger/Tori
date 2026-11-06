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

#include <functional>
#include <string>
#include <vector>

namespace Tori::UI {
    /// @brief Keeps Tori to one running copy per user session. A second launch (double-clicking a .torrent, a
    /// browser opening a magnet link) hands its arguments to the first copy over WM_COPYDATA and exits.
    class InstanceChannel {
    public:
        InstanceChannel();
        ~InstanceChannel();

        InstanceChannel(const InstanceChannel&)            = delete;
        InstanceChannel& operator=(const InstanceChannel&) = delete;

        /// @brief False when another copy of Tori is already running.
        [[nodiscard]] bool IsFirst() const { return _IsFirst; }

        /// @brief Sends Arguments to the running copy, which also comes to the front. Waits a few seconds for it to
        /// finish starting up. Returns false if it couldn't be reached.
        static bool SendToFirst(const std::vector<std::wstring>& Arguments);

        /// @brief In the first copy: receive arguments from later launches. Call on the UI thread; OnArguments
        /// runs there, from inside the message loop.
        void Listen(std::function<void(std::vector<std::wstring>)> OnArguments);

    private:
        friend struct InstanceWindow;

        void* _Mutex  = nullptr;
        void* _Window = nullptr;
        bool _IsFirst = true;
        std::function<void(std::vector<std::wstring>)> _OnArguments;
    };
}  // namespace Tori::UI
