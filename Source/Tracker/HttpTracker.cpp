#include "Tracker/HttpTracker.hpp"

#include "Core/Bencode.hpp"
#include "Core/Encoding.hpp"
#include "Core/Error.hpp"
#include "Core/Utf8.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <format>
#include <span>

namespace Tori {
    namespace {
        constexpr std::size_t kMaxResponseSize = 2 * 1024 * 1024;

        std::string_view AsChars(const std::span<const std::uint8_t> Bytes) {
            return {reinterpret_cast<const char*>(Bytes.data()), Bytes.size()};
        }

        // Atomic so a stop callback on another thread can close it to abort a blocking WinHTTP call
        class HttpHandle {
        public:
            explicit HttpHandle(const HINTERNET Handle = nullptr) : _Handle(Handle) {}
            ~HttpHandle() { Close(); }

            HttpHandle(const HttpHandle&)            = delete;
            HttpHandle& operator=(const HttpHandle&) = delete;

            [[nodiscard]] HINTERNET Get() const { return _Handle.load(); }
            explicit operator bool() const { return Get() != nullptr; }

            void Close() {
                if (const HINTERNET Handle = _Handle.exchange(nullptr)) { WinHttpCloseHandle(Handle); }
            }

        private:
            std::atomic<HINTERNET> _Handle;
        };

        std::string WinHttpErrorText(const DWORD Error) {
            switch (Error) {
                case ERROR_WINHTTP_TIMEOUT:
                    return "timed out";
                case ERROR_WINHTTP_NAME_NOT_RESOLVED:
                    return "could not resolve host";
                case ERROR_WINHTTP_CANNOT_CONNECT:
                    return "could not connect";
                case ERROR_WINHTTP_CONNECTION_ERROR:
                    return "connection was reset";
                case ERROR_WINHTTP_SECURE_FAILURE:
                    return "TLS certificate or handshake error";
                case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
                    return "invalid HTTP response";
                case ERROR_WINHTTP_REDIRECT_FAILED:
                    return "redirect failed";
                case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
                case ERROR_WINHTTP_INVALID_URL:
                    return "invalid tracker URL";
                default:
                    return std::format("WinHTTP error {}", Error);
            }
        }

        [[noreturn]] void FailWinHttp(const std::stop_token& Stop) {
            const DWORD Error = GetLastError();
            if (Stop.stop_requested()) { throw TrackerError("cancelled"); }
            throw TrackerError(WinHttpErrorText(Error));
        }

        std::optional<std::string> FailureReason(const std::string_view Body) {
            try {
                std::size_t Consumed = 0;
                const Bencode::Value Root = Bencode::DecodePrefix(Body, Consumed);
                if (const std::string* Reason = Root.FindString("failure reason")) { return *Reason; }
            } catch (const ParseError&) {}
            return std::nullopt;
        }
    }  // namespace

    std::string_view AnnounceEventName(const AnnounceEvent Event) {
        switch (Event) {
            case AnnounceEvent::Started:
                return "started";
            case AnnounceEvent::Completed:
                return "completed";
            case AnnounceEvent::Stopped:
                return "stopped";
            case AnnounceEvent::None:
                break;
        }
        return {};
    }

    std::string BuildAnnounceUrl(const std::string_view Base, const AnnounceRequest& Request) {
        std::string Url(Base);
        Url += Base.find('?') == std::string_view::npos ? '?' : '&';
        Url += "info_hash=" + PercentEncode(AsChars(Request.Hash.Bytes));
        Url += "&peer_id=" + PercentEncode(AsChars(Request.Id));
        Url += std::format("&port={}&uploaded={}&downloaded={}&left={}&compact=1&numwant={}&key={:08x}",
                           Request.Port,
                           Request.Uploaded,
                           Request.Downloaded,
                           Request.Left,
                           Request.NumWant,
                           Request.Key);
        if (const std::string_view Event = AnnounceEventName(Request.Event); !Event.empty()) {
            Url += "&event=";
            Url += Event;
        }
        if (!Request.TrackerId.empty()) { Url += "&trackerid=" + PercentEncode(Request.TrackerId); }
        return Url;
    }

    AnnounceResponse ParseHttpAnnounceResponse(const std::string_view Body) {
        Bencode::Value Root;
        try {
            // Some trackers append a newline after the dictionary
            std::size_t Consumed = 0;
            Root                 = Bencode::DecodePrefix(Body, Consumed);
        } catch (const ParseError&) { throw TrackerError("tracker sent an invalid response"); }
        if (!Root.IsDict()) { throw TrackerError("tracker sent an invalid response"); }

        if (const std::string* Reason = Root.FindString("failure reason")) {
            throw TrackerError(Reason->empty() ? "tracker refused the announce" : *Reason);
        }

        AnnounceResponse Response;
        if (const auto* Interval = Root.FindInteger("interval")) {
            Response.IntervalSeconds = ClampAnnounceInterval(*Interval);
        }
        if (const auto* MinInterval = Root.FindInteger("min interval")) {
            Response.MinIntervalSeconds = ClampAnnounceInterval(*MinInterval);
        }
        if (const auto* Complete = Root.FindInteger("complete"); Complete && *Complete >= 0) {
            Response.Seeders = static_cast<int>(std::min<Bencode::Integer>(*Complete, INT32_MAX));
        }
        if (const auto* Incomplete = Root.FindInteger("incomplete"); Incomplete && *Incomplete >= 0) {
            Response.Leechers = static_cast<int>(std::min<Bencode::Integer>(*Incomplete, INT32_MAX));
        }
        if (const std::string* Warning = Root.FindString("warning message")) { Response.WarningMessage = *Warning; }
        if (const std::string* TrackerId = Root.FindString("tracker id")) { Response.TrackerId = *TrackerId; }

        if (const std::string* Compact = Root.FindString("peers")) {
            Response.Peers = ParseCompactPeers(*Compact, false);
        } else if (const Bencode::List* Peers = Root.FindList("peers")) {
            // Non-compact form: a list of {ip, port} dictionaries
            for (const Bencode::Value& Peer : *Peers) {
                const std::string* Ip        = Peer.FindString("ip");
                const Bencode::Integer* Port = Peer.FindInteger("port");
                if (!Ip || !Port || *Port <= 0 || *Port > 65535) { continue; }
                if (const auto Address = Endpoint::FromAddress(*Ip, static_cast<std::uint16_t>(*Port))) {
                    Response.Peers.push_back(*Address);
                }
            }
        }
        if (const std::string* Compact6 = Root.FindString("peers6")) {
            const std::vector<Endpoint> V6 = ParseCompactPeers(*Compact6, true);
            Response.Peers.insert(Response.Peers.end(), V6.begin(), V6.end());
        }
        return Response;
    }

    AnnounceResponse HttpAnnounce(const std::string& Url,
                                  const AnnounceRequest& Request,
                                  const std::stop_token Stop,
                                  const std::chrono::milliseconds Timeout) {
        const std::wstring FullUrl = Utf8ToWide(BuildAnnounceUrl(Url, Request));

        URL_COMPONENTS Parts {};
        Parts.dwStructSize      = sizeof(Parts);
        Parts.dwSchemeLength    = static_cast<DWORD>(-1);
        Parts.dwHostNameLength  = static_cast<DWORD>(-1);
        Parts.dwUrlPathLength   = static_cast<DWORD>(-1);
        Parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(FullUrl.c_str(), static_cast<DWORD>(FullUrl.size()), 0, &Parts)) {
            throw TrackerError("invalid tracker URL");
        }
        const std::wstring Host(Parts.lpszHostName, Parts.dwHostNameLength);
        const std::wstring Object = std::wstring(Parts.lpszUrlPath, Parts.dwUrlPathLength) +
                                    std::wstring(Parts.lpszExtraInfo, Parts.dwExtraInfoLength);
        const bool Secure = Parts.nScheme == INTERNET_SCHEME_HTTPS;

        HttpHandle Session(
          WinHttpOpen(L"Tori/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!Session) { FailWinHttp(Stop); }

        const int TimeoutMs = static_cast<int>(Timeout.count());
        WinHttpSetTimeouts(Session.Get(), TimeoutMs, TimeoutMs, TimeoutMs, TimeoutMs);
        DWORD Decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(Session.Get(), WINHTTP_OPTION_DECOMPRESSION, &Decompression, sizeof(Decompression));

        HttpHandle Connection(WinHttpConnect(Session.Get(), Host.c_str(), Parts.nPort, 0));
        if (!Connection) { FailWinHttp(Stop); }

        // The query is already percent-encoded; WinHTTP must not escape it again
        const DWORD Flags = (Secure ? WINHTTP_FLAG_SECURE : 0) | WINHTTP_FLAG_ESCAPE_DISABLE |
                            WINHTTP_FLAG_ESCAPE_DISABLE_QUERY;
        HttpHandle Handle(WinHttpOpenRequest(
          Connection.Get(), L"GET", Object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, Flags));
        if (!Handle) { FailWinHttp(Stop); }

        // Closing the request handle from another thread aborts a blocking send or receive
        std::stop_callback Cancel(Stop, [&Handle] { Handle.Close(); });

        if (!WinHttpSendRequest(Handle.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(Handle.Get(), nullptr)) {
            FailWinHttp(Stop);
        }

        DWORD Status     = 0;
        DWORD StatusSize = sizeof(Status);
        if (!WinHttpQueryHeaders(Handle.Get(),
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &Status,
                                 &StatusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            FailWinHttp(Stop);
        }

        std::string Body;
        while (true) {
            DWORD Available = 0;
            if (!WinHttpQueryDataAvailable(Handle.Get(), &Available)) { FailWinHttp(Stop); }
            if (Available == 0) { break; }
            if (Body.size() + Available > kMaxResponseSize) { throw TrackerError("tracker response is too large"); }

            const std::size_t Offset = Body.size();
            Body.resize(Offset + Available);
            DWORD Read = 0;
            if (!WinHttpReadData(Handle.Get(), Body.data() + Offset, Available, &Read)) { FailWinHttp(Stop); }
            Body.resize(Offset + Read);
            if (Read == 0) { break; }
        }

        if (Status != 200) {
            // Some trackers pair an error status with a bencoded failure reason
            if (const auto Reason = FailureReason(Body)) { throw TrackerError(*Reason); }
            throw TrackerError(std::format("HTTP {}", Status));
        }
        return ParseHttpAnnounceResponse(Body);
    }
}  // namespace Tori
