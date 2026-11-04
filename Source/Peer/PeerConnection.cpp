#include "Peer/PeerConnection.hpp"

#include "Core/Win32.hpp"

#include <algorithm>
#include <climits>

namespace Tori {
    namespace {
        constexpr std::size_t kReadChunk     = 64 * 1024;
        constexpr int kMaxReadsPerPoll       = 8;  // So one fast peer can't starve the others in a loop pass

        void Configure(const SOCKET Socket) {
            u_long NonBlocking = 1;
            ioctlsocket(Socket, FIONBIO, &NonBlocking);
            BOOL NoDelay = TRUE;
            setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&NoDelay), sizeof(NoDelay));
        }
    }  // namespace

    std::unique_ptr<PeerConnection> PeerConnection::Connect(const Endpoint& Remote) {
        EnsureWinsock();
        UniqueSocket Socket(socket(Remote.IsV6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!Socket) { return nullptr; }
        Configure(Socket.Get());

        sockaddr_storage Address {};
        const int Length = ToSockaddr(Remote, Address);
        if (connect(Socket.Get(), reinterpret_cast<const sockaddr*>(&Address), Length) == SOCKET_ERROR &&
            WSAGetLastError() != WSAEWOULDBLOCK) {
            return nullptr;
        }
        return std::unique_ptr<PeerConnection>(new PeerConnection(std::move(Socket), Remote, false));
    }

    std::unique_ptr<PeerConnection> PeerConnection::Accept(UniqueSocket Socket,
                                                           const Endpoint& Remote,
                                                           const Handshake& Received) {
        Configure(Socket.Get());
        auto Connection = std::unique_ptr<PeerConnection>(new PeerConnection(std::move(Socket), Remote, true));
        Connection->_RemoteHandshake = Received;
        return Connection;
    }

    PeerConnection::PeerConnection(UniqueSocket Socket, const Endpoint& Remote, const bool Connected)
        : _Socket(std::move(Socket)), _Remote(Remote), _Connected(Connected), _ReadBuffer(kReadChunk) {}

    short PeerConnection::PollEvents() const {
        if (!_Connected) { return POLLWRNORM; }
        return static_cast<short>(POLLRDNORM | (PendingSendBytes() > 0 ? POLLWRNORM : 0));
    }

    bool PeerConnection::OnPoll(const short Events) {
        if (!_Connected) {
            if (Events & (POLLERR | POLLHUP)) { return FailWithSocketError(); }
            if (!(Events & POLLWRNORM)) { return true; }
            _Connected = true;
            return Flush();
        }

        if (Events & POLLERR) { return FailWithSocketError(); }
        if ((Events & (POLLRDNORM | POLLHUP)) && !Read()) { return false; }
        if ((Events & POLLWRNORM) && !Flush()) { return false; }
        return true;
    }

    void PeerConnection::Send(const std::span<const std::uint8_t> Data) {
        if (_OutStart > 0 && _OutStart == _Out.size()) {
            _Out.clear();
            _OutStart = 0;
        }
        _Out.insert(_Out.end(), Data.begin(), Data.end());
    }

    std::optional<PeerMessage> PeerConnection::NextMessage() {
        if (!_RemoteHandshake) { return std::nullopt; }
        return _Parser.Next();
    }

    bool PeerConnection::Read() {
        for (int Attempt = 0; Attempt < kMaxReadsPerPoll; ++Attempt) {
            const int Received = recv(_Socket.Get(), reinterpret_cast<char*>(_ReadBuffer.data()), static_cast<int>(_ReadBuffer.size()), 0);
            if (Received == 0) { return Fail("closed by peer"); }
            if (Received == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) { return true; }
                return FailWithSocketError();
            }

            std::span<const std::uint8_t> Data(_ReadBuffer.data(), static_cast<std::size_t>(Received));
            if (!_RemoteHandshake) {
                const std::size_t Take = std::min(kHandshakeSize - _HandshakeBuffer.size(), Data.size());
                _HandshakeBuffer.insert(_HandshakeBuffer.end(), Data.begin(), Data.begin() + static_cast<std::ptrdiff_t>(Take));
                Data = Data.subspan(Take);
                if (_HandshakeBuffer.size() == kHandshakeSize) {
                    _RemoteHandshake = DecodeHandshake(_HandshakeBuffer);
                    if (!_RemoteHandshake) { return Fail("not a BitTorrent peer"); }
                }
            }
            if (!Data.empty()) { _Parser.Feed(Data); }

            if (static_cast<std::size_t>(Received) < _ReadBuffer.size()) { return true; }
        }
        return true;
    }

    bool PeerConnection::Flush() {
        while (_OutStart < _Out.size()) {
            const int Chunk = static_cast<int>(std::min<std::size_t>(_Out.size() - _OutStart, INT_MAX));
            const int Sent  = send(_Socket.Get(), reinterpret_cast<const char*>(_Out.data() + _OutStart), Chunk, 0);
            if (Sent == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) { return true; }
                return FailWithSocketError();
            }
            _OutStart += static_cast<std::size_t>(Sent);
        }
        _Out.clear();
        _OutStart = 0;
        return true;
    }

    bool PeerConnection::Fail(std::string Reason) {
        _Error = std::move(Reason);
        return false;
    }

    bool PeerConnection::FailWithSocketError() {
        int Error       = 0;
        int ErrorLength = sizeof(Error);
        getsockopt(_Socket.Get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&Error), &ErrorLength);
        if (Error == 0) { Error = WSAGetLastError(); }
        return Fail(Error ? Win32ErrorMessage(static_cast<unsigned long>(Error)) : "connection failed");
    }
}  // namespace Tori
