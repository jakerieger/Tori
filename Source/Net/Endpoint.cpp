#include "Net/Endpoint.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace Tori {
    std::string Endpoint::AddressString() const {
        char Buffer[INET6_ADDRSTRLEN] = {};
        inet_ntop(IsV6 ? AF_INET6 : AF_INET, Address.data(), Buffer, sizeof(Buffer));
        return Buffer;
    }

    std::string Endpoint::ToString() const {
        return IsV6 ? std::format("[{}]:{}", AddressString(), Port) : std::format("{}:{}", AddressString(), Port);
    }

    Endpoint Endpoint::Unmapped() const {
        constexpr std::array<std::uint8_t, 12> kMappedPrefix = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (!IsV6 || !std::equal(kMappedPrefix.begin(), kMappedPrefix.end(), Address.begin())) { return *this; }

        Endpoint Plain;
        Plain.Port = Port;
        std::copy(Address.begin() + 12, Address.end(), Plain.Address.begin());
        return Plain;
    }

    std::optional<Endpoint> Endpoint::FromAddress(const std::string_view Ip, const std::uint16_t Port) {
        const std::string Text(Ip);  // inet_pton needs a terminated string
        Endpoint Result;
        Result.Port = Port;
        if (inet_pton(AF_INET, Text.c_str(), Result.Address.data()) == 1) { return Result; }
        if (inet_pton(AF_INET6, Text.c_str(), Result.Address.data()) == 1) {
            Result.IsV6 = true;
            return Result;
        }
        return std::nullopt;
    }

    std::optional<Endpoint> Endpoint::Parse(const std::string_view HostAndPort) {
        std::string_view Host;
        std::string_view PortText;

        if (HostAndPort.starts_with('[')) {
            const std::size_t Close = HostAndPort.find(']');
            if (Close == std::string_view::npos || Close + 1 >= HostAndPort.size() || HostAndPort[Close + 1] != ':') {
                return std::nullopt;
            }
            Host     = HostAndPort.substr(1, Close - 1);
            PortText = HostAndPort.substr(Close + 2);
        } else {
            const std::size_t Colon = HostAndPort.rfind(':');
            if (Colon == std::string_view::npos) { return std::nullopt; }
            Host     = HostAndPort.substr(0, Colon);
            PortText = HostAndPort.substr(Colon + 1);
            if (Host.find(':') != std::string_view::npos) { return std::nullopt; }  // IPv6 must be bracketed
        }

        std::uint32_t Port   = 0;
        const auto [Ptr, Ec] = std::from_chars(PortText.data(), PortText.data() + PortText.size(), Port);
        if (Ec != std::errc {} || Ptr != PortText.data() + PortText.size() || Port == 0 || Port > 65535) {
            return std::nullopt;
        }
        return FromAddress(Host, static_cast<std::uint16_t>(Port));
    }

    std::vector<Endpoint> ParseCompactPeers(const std::string_view Data, const bool IsV6) {
        const std::size_t AddressSize = IsV6 ? 16 : 4;
        const std::size_t EntrySize   = AddressSize + 2;

        std::vector<Endpoint> Peers;
        Peers.reserve(Data.size() / EntrySize);
        for (std::size_t Offset = 0; Offset + EntrySize <= Data.size(); Offset += EntrySize) {
            Endpoint Peer;
            Peer.IsV6 = IsV6;
            std::memcpy(Peer.Address.data(), Data.data() + Offset, AddressSize);
            const auto High = static_cast<std::uint8_t>(Data[Offset + AddressSize]);
            const auto Low  = static_cast<std::uint8_t>(Data[Offset + AddressSize + 1]);
            Peer.Port       = static_cast<std::uint16_t>((High << 8) | Low);
            if (Peer.Port != 0) { Peers.push_back(Peer); }
        }
        return Peers;
    }
}  // namespace Tori
