#pragma once

#include "Net/Endpoint.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <optional>
#include <utility>

namespace Tori {
    /// @brief Initializes Winsock once for the process. Safe to call from any thread.
    void EnsureWinsock();

    /// @brief Owns a SOCKET and closes it on destruction.
    class UniqueSocket {
    public:
        UniqueSocket() = default;
        explicit UniqueSocket(const SOCKET Handle) : _Handle(Handle) {}
        ~UniqueSocket() { Reset(); }

        UniqueSocket(UniqueSocket&& Other) noexcept : _Handle(std::exchange(Other._Handle, INVALID_SOCKET)) {}
        UniqueSocket& operator=(UniqueSocket&& Other) noexcept {
            if (this != &Other) {
                Reset();
                _Handle = std::exchange(Other._Handle, INVALID_SOCKET);
            }
            return *this;
        }

        UniqueSocket(const UniqueSocket&)            = delete;
        UniqueSocket& operator=(const UniqueSocket&) = delete;

        [[nodiscard]] SOCKET Get() const { return _Handle; }
        explicit operator bool() const { return _Handle != INVALID_SOCKET; }

        void Reset() {
            if (_Handle != INVALID_SOCKET) {
                closesocket(_Handle);
                _Handle = INVALID_SOCKET;
            }
        }

    private:
        SOCKET _Handle = INVALID_SOCKET;
    };

    /// @brief Fills Out and returns the address length to pass to Winsock.
    int ToSockaddr(const Endpoint& Address, sockaddr_storage& Out);

    /// @brief Returns nullopt for families other than IPv4 and IPv6.
    std::optional<Endpoint> FromSockaddr(const sockaddr* Address);
}  // namespace Tori
