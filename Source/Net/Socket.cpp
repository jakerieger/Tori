#include "Net/Socket.hpp"

#include <cstring>
#include <format>
#include <mutex>
#include <stdexcept>

namespace Tori {
    void EnsureWinsock() {
        static std::once_flag Once;
        std::call_once(Once, [] {
            WSADATA Data {};
            if (const int Error = WSAStartup(MAKEWORD(2, 2), &Data); Error != 0) {
                throw std::runtime_error(std::format("WSAStartup failed: {}", Error));
            }
        });
    }

    int ToSockaddr(const Endpoint& Address, sockaddr_storage& Out) {
        Out = {};
        if (Address.IsV6) {
            auto& V6       = reinterpret_cast<sockaddr_in6&>(Out);
            V6.sin6_family = AF_INET6;
            V6.sin6_port   = htons(Address.Port);
            std::memcpy(&V6.sin6_addr, Address.Address.data(), 16);
            return sizeof(sockaddr_in6);
        }
        auto& V4      = reinterpret_cast<sockaddr_in&>(Out);
        V4.sin_family = AF_INET;
        V4.sin_port   = htons(Address.Port);
        std::memcpy(&V4.sin_addr, Address.Address.data(), 4);
        return sizeof(sockaddr_in);
    }

    std::optional<Endpoint> FromSockaddr(const sockaddr* Address) {
        Endpoint Result;
        if (Address->sa_family == AF_INET) {
            const auto* V4 = reinterpret_cast<const sockaddr_in*>(Address);
            std::memcpy(Result.Address.data(), &V4->sin_addr, 4);
            Result.Port = ntohs(V4->sin_port);
            return Result;
        }
        if (Address->sa_family == AF_INET6) {
            const auto* V6 = reinterpret_cast<const sockaddr_in6*>(Address);
            std::memcpy(Result.Address.data(), &V6->sin6_addr, 16);
            Result.Port = ntohs(V6->sin6_port);
            Result.IsV6 = true;
            return Result;
        }
        return std::nullopt;
    }
}  // namespace Tori
