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

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <windows.h>
#include <winhttp.h>

namespace Tori::Http {
    struct HandleCloser {
        void operator()(void* H) const noexcept {
            if (H) WinHttpCloseHandle(H);
        }
    };

    using Handle = std::unique_ptr<void, HandleCloser>;

    struct HttpResponse {
        DWORD Status {0};
        std::string Body;
    };

    /// The request handle, closable from another thread: closing it makes a WinHTTP call blocked on it return,
    /// which is how a stop request cuts a slow connection short.
    class CancellableRequest {
    public:
        explicit CancellableRequest(const HINTERNET Request) : _Request(Request) {}
        ~CancellableRequest() { Close(); }

        CancellableRequest(const CancellableRequest&)            = delete;
        CancellableRequest& operator=(const CancellableRequest&) = delete;

        [[nodiscard]] HINTERNET Get() const { return _Request; }

        void Close() {
            std::scoped_lock Lock(_Mutex);
            if (_Request) { WinHttpCloseHandle(std::exchange(_Request, nullptr)); }
        }

    private:
        std::mutex _Mutex;
        HINTERNET _Request;
    };

    /// GETs https://Host/Path. On failure returns nothing, with the WinHTTP error code in Error.
    std::optional<HttpResponse> HttpsGet(const std::stop_token& Stop,
                                         std::wstring_view Host,
                                         const std::wstring& Path,
                                         const wchar_t* UserAgent,
                                         const size_t MaxResponseSize,
                                         DWORD& Error);
}  // namespace Tori::Http