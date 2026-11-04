#include "Meta/MagnetUri.hpp"

#include "Core/Encoding.hpp"
#include "Core/Error.hpp"

#include <algorithm>
#include <format>

namespace Tori {
    namespace {
        [[noreturn]] void Fail(const std::string_view Reason) {
            throw ParseError(std::format("magnet: {}", Reason));
        }

        // Numbered parameters ("tr.1", "xt.2") behave like their base key
        std::string_view BaseKey(std::string_view Key) {
            const std::size_t Dot = Key.rfind('.');
            if (Dot == std::string_view::npos || Dot + 1 == Key.size()) { return Key; }
            const std::string_view Suffix = Key.substr(Dot + 1);
            if (!std::ranges::all_of(Suffix, [](const char C) { return C >= '0' && C <= '9'; })) { return Key; }
            return Key.substr(0, Dot);
        }

        void AppendUnique(std::vector<std::string>& Items, std::string Item) {
            if (!Item.empty() && std::ranges::find(Items, Item) == Items.end()) { Items.push_back(std::move(Item)); }
        }
    }  // namespace

    MagnetUri MagnetUri::Parse(const std::string_view Uri) {
        constexpr std::string_view kScheme = "magnet:?";
        if (!StartsWithIgnoreCase(Uri, kScheme)) { Fail("not a magnet link"); }

        MagnetUri Result;
        bool HasHash               = false;
        std::string_view Remaining = Uri.substr(kScheme.size());

        while (!Remaining.empty()) {
            const std::size_t Amp       = Remaining.find('&');
            const std::string_view Pair = Remaining.substr(0, Amp);
            Remaining = Amp == std::string_view::npos ? std::string_view {} : Remaining.substr(Amp + 1);

            const std::size_t Equals = Pair.find('=');
            if (Equals == std::string_view::npos) { continue; }

            const std::string_view Key = BaseKey(Pair.substr(0, Equals));
            // '+' means space only in the display name; tracker URLs may legitimately contain it
            const std::optional<std::string> Decoded = PercentDecode(Pair.substr(Equals + 1), Key == "dn");
            if (!Decoded) { Fail(std::format("bad percent-encoding in '{}'", Key)); }

            if (Key == "xt") {
                constexpr std::string_view kBtih = "urn:btih:";
                if (!StartsWithIgnoreCase(*Decoded, kBtih)) { continue; }  // e.g. urn:btmh (v2), which we skip

                const std::string_view Encoded    = std::string_view(*Decoded).substr(kBtih.size());
                const std::optional<InfoHash> Hash = Encoded.size() == 40   ? InfoHash::FromHex(Encoded)
                                                     : Encoded.size() == 32 ? InfoHash::FromBase32(Encoded)
                                                                            : std::nullopt;
                if (!Hash) { Fail("invalid btih info hash"); }
                if (HasHash && *Hash != Result.Hash) { Fail("conflicting info hashes"); }
                Result.Hash = *Hash;
                HasHash     = true;
            } else if (Key == "dn") {
                Result.DisplayName = *Decoded;
            } else if (Key == "tr") {
                AppendUnique(Result.Trackers, *Decoded);
            } else if (Key == "x.pe") {
                AppendUnique(Result.Peers, *Decoded);
            } else if (Key == "ws") {
                AppendUnique(Result.WebSeeds, *Decoded);
            }
        }

        if (!HasHash) { Fail("no BitTorrent v1 info hash (xt=urn:btih:...)"); }
        return Result;
    }

    std::string MagnetUri::ToString() const {
        std::string Out = "magnet:?xt=urn:btih:" + Hash.ToHex();
        if (!DisplayName.empty()) { Out += "&dn=" + PercentEncode(DisplayName); }
        for (const std::string& Tracker : Trackers) { Out += "&tr=" + PercentEncode(Tracker); }
        for (const std::string& Peer : Peers) { Out += "&x.pe=" + PercentEncode(Peer); }
        for (const std::string& Seed : WebSeeds) { Out += "&ws=" + PercentEncode(Seed); }
        return Out;
    }
}  // namespace Tori
