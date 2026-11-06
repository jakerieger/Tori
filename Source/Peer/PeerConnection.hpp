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

#include "Net/Endpoint.hpp"
#include "Net/Socket.hpp"
#include "Peer/Messages.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Tori {
    /// @brief One peer's TCP connection: non-blocking connect, handshake, message framing and send buffering.
    ///
    /// It knows nothing about the torrent; the owner drives it from a WSAPoll loop by passing PollEvents() in and
    /// the returned events to OnPoll(), then draining NextMessage().
    class PeerConnection {
    public:
        /// @brief Starts a non-blocking connect. Returns nullptr when the socket can't be created.
        static std::unique_ptr<PeerConnection> Connect(const Endpoint& Remote);

        /// @brief Wraps an accepted socket whose handshake the listener has already read.
        static std::unique_ptr<PeerConnection>
        Accept(UniqueSocket Socket, const Endpoint& Remote, const Handshake& Received);

        PeerConnection(const PeerConnection&)            = delete;
        PeerConnection& operator=(const PeerConnection&) = delete;

        [[nodiscard]] SOCKET Socket() const { return _Socket.Get(); }
        [[nodiscard]] const Endpoint& Remote() const { return _Remote; }
        [[nodiscard]] bool IsConnected() const { return _Connected; }

        [[nodiscard]] bool HasHandshake() const { return _RemoteHandshake.has_value(); }
        [[nodiscard]] const Handshake& RemoteHandshake() const { return *_RemoteHandshake; }

        [[nodiscard]] short PollEvents() const;

        /// @brief Handles WSAPoll results: completes the connect, reads, and flushes. Returns false once the
        /// connection has failed or closed; Error() then says why.
        bool OnPoll(short Events);

        /// @brief Queues bytes; they go out when the socket is next writable.
        void Send(std::span<const std::uint8_t> Data);

        /// @brief Throws ParseError on a protocol violation.
        std::optional<PeerMessage> NextMessage();

        [[nodiscard]] std::size_t PendingSendBytes() const { return _Out.size() - _OutStart; }
        [[nodiscard]] const std::string& Error() const { return _Error; }

    private:
        PeerConnection(UniqueSocket Socket, const Endpoint& Remote, bool Connected);

        bool Read();
        bool Flush();
        bool Fail(std::string Reason);
        bool FailWithSocketError();

        UniqueSocket _Socket;
        Endpoint _Remote;
        bool _Connected = false;

        std::optional<Handshake> _RemoteHandshake;
        std::vector<std::uint8_t> _HandshakeBuffer;
        MessageParser _Parser;

        std::vector<std::uint8_t> _ReadBuffer;
        std::vector<std::uint8_t> _Out;
        std::size_t _OutStart = 0;
        std::string _Error;
    };
}  // namespace Tori
