#include "Core/PeerId.hpp"

#include "Core/Random.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace Tori {
    PeerId GeneratePeerId() {
        constexpr std::string_view kPrefix   = "-TO0100-";
        constexpr std::string_view kAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

        PeerId Id {};
        std::ranges::copy(kPrefix, Id.begin());

        std::array<std::uint8_t, Id.size() - kPrefix.size()> Random {};
        RandomBytes(Random);
        for (std::size_t I = 0; I < Random.size(); ++I) {
            Id[kPrefix.size() + I] = static_cast<std::uint8_t>(kAlphabet[Random[I] % kAlphabet.size()]);
        }
        return Id;
    }

    std::string DescribePeerId(const PeerId& Id) {
        // -XXvvvv-: two letters for the client, four characters of version
        if (Id[0] != '-' || Id[7] != '-') { return {}; }
        for (int I = 1; I < 7; ++I) {
            if (!std::isalnum(Id[static_cast<std::size_t>(I)])) { return {}; }
        }

        static constexpr std::pair<std::string_view, std::string_view> kClients[] = {
          {"AZ", "Vuze"},        {"BI", "BiglyBT"},      {"BT", "BitTorrent"},   {"DE", "Deluge"},
          {"FW", "FrostWire"},   {"KT", "KTorrent"},     {"LT", "libtorrent"},   {"lt", "rTorrent"},
          {"PI", "PicoTorrent"}, {"qB", "qBittorrent"},  {"TO", "Tori"},         {"TR", "Transmission"},
          {"UT", "µTorrent"}, {"UW", "µTorrent Web"}, {"WW", "WebTorrent"},
        };
        const std::string_view Code(reinterpret_cast<const char*>(&Id[1]), 2);
        std::string Name(Code);
        for (const auto& [Known, Client] : kClients) {
            if (Known == Code) { Name = Client; }
        }

        // Digits are versions as-is; letters count on from 10 (A = 10), as libtorrent writes them
        std::string Version;
        for (std::size_t I = 3; I < 7; ++I) {
            const char Digit = static_cast<char>(Id[I]);
            if (!Version.empty()) { Version += '.'; }
            Version += std::isdigit(static_cast<unsigned char>(Digit)) ? std::string(1, Digit)
                                                                        : std::to_string(std::toupper(Digit) - 'A' + 10);
        }
        // Trailing zero components say nothing ("4.6.5.0" is 4.6.5)
        while (Version.size() > 2 && Version.ends_with(".0")) { Version.resize(Version.size() - 2); }
        return Name + " " + Version;
    }
}  // namespace Tori
