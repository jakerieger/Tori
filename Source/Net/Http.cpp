
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

#include "Http.hpp"

namespace Tori::Http {
    std::optional<HttpResponse> HttpsGet(const std::stop_token& Stop,
                                         std::wstring_view Host,
                                         const std::wstring& Path,
                                         const wchar_t* UserAgent,
                                         const size_t MaxResponseSize,
                                         DWORD& Error) {
        // Read the error at once: closing handles on the way out can overwrite it
        const auto Fail = [&Error] {
            Error = GetLastError();
            return std::nullopt;
        };

        Handle Session {WinHttpOpen(UserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
        if (!Session) return Fail();

        WinHttpSetTimeouts(Session.get(), 5000, 5000, 10000, 10000);

        Handle Connection {WinHttpConnect(Session.get(), std::wstring {Host}.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!Connection) return Fail();

        CancellableRequest Request {
          WinHttpOpenRequest(Connection.get(), L"GET", Path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)};
        if (!Request.Get()) return Fail();
        // Declared after Request, so it's unregistered before Request closes the handle itself
        const std::stop_callback Cancel(Stop, [&Request] { Request.Close(); });
        if (Stop.stop_requested()) { return std::nullopt; }

        auto Headers = L"Accept: application/vnd.github+json\r\n"
                       L"X-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(Request.Get(), Headers, static_cast<DWORD>(-1L), nullptr, 0, 0, 0)) { return Fail(); }
        if (!WinHttpReceiveResponse(Request.Get(), nullptr)) return Fail();

        HttpResponse Response;
        DWORD Size = sizeof(Response.Status);
        if (!WinHttpQueryHeaders(Request.Get(),
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 nullptr,
                                 &Response.Status,
                                 &Size,
                                 nullptr)) {
            return Fail();
        }

        for (;;) {
            if (Stop.stop_requested()) { return std::nullopt; }

            DWORD Avail = 0;
            if (!WinHttpQueryDataAvailable(Request.Get(), &Avail)) return Fail();
            if (Avail == 0) break;
            if (Response.Body.size() + Avail > MaxResponseSize) {
                Error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
                return std::nullopt;
            }

            std::string Chunk(Avail, '\0');
            DWORD Read = 0;
            if (!WinHttpReadData(Request.Get(), Chunk.data(), Avail, &Read)) return Fail();
            Response.Body.append(Chunk, 0, Read);
        }

        return Response;
    }
}  // namespace Tori::Http