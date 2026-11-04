#include "Meta/Metainfo.hpp"

#include "Core/Bencode.hpp"
#include "Core/Error.hpp"
#include "Core/Utf8.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace Tori {
    namespace {
        [[noreturn]] void Fail(const std::string_view Reason) {
            throw ParseError(std::format("torrent: {}", Reason));
        }

        // Many clients write a ".utf-8" variant alongside fields that may be in a legacy encoding
        const Bencode::Value* FindPreferUtf8(const Bencode::Value& Dict, const std::string_view Key) {
            if (const Bencode::Value* Utf8 = Dict.Find(std::string(Key) + ".utf-8")) { return Utf8; }
            return Dict.Find(Key);
        }

        std::string StringOrEmpty(const Bencode::Value& Dict, const std::string_view Key) {
            const Bencode::Value* Item = FindPreferUtf8(Dict, Key);
            return Item && Item->IsString() ? Item->AsString() : std::string {};
        }

        std::int64_t CheckedAdd(const std::int64_t Total, const std::int64_t Length) {
            if (Length > std::numeric_limits<std::int64_t>::max() - Total) { Fail("total size overflows"); }
            return Total + Length;
        }

        std::filesystem::path SanitizedFilePath(const std::string& Root, const Bencode::List& Components) {
            std::filesystem::path Path = PathFromUtf8(Root);
            bool HasComponent          = false;
            for (const Bencode::Value& Component : Components) {
                if (!Component.IsString()) { Fail("file path component is not a string"); }
                const std::string Safe = SanitizePathComponent(Component.AsString());
                if (Safe.empty()) { continue; }
                Path /= PathFromUtf8(Safe);
                HasComponent = true;
            }
            if (!HasComponent) { Fail("file has an empty path"); }
            return Path;
        }

        Metainfo FromInfo(const Bencode::Value& Info, const std::string_view RawInfo) {
            if (!Info.IsDict()) { Fail("info is not a dictionary"); }

            Metainfo Result;
            Result.InfoBytes = std::string(RawInfo);
            Result.Hash      = InfoHash {Sha1::Hash(RawInfo)};

            const std::string* Pieces = Info.FindString("pieces");
            if (!Pieces) {
                const Bencode::Integer* MetaVersion = Info.FindInteger("meta version");
                if (MetaVersion && *MetaVersion == 2) { Fail("BitTorrent v2-only torrents are not supported"); }
                Fail("missing pieces");
            }
            if (Pieces->size() % Sha1Digest {}.size() != 0) { Fail("pieces length is not a multiple of 20"); }

            const Bencode::Integer* PieceLength = Info.FindInteger("piece length");
            if (!PieceLength || *PieceLength <= 0 || *PieceLength > kMaxPieceLength) { Fail("invalid piece length"); }
            Result.PieceLength = *PieceLength;

            const std::string RawName = StringOrEmpty(Info, "name");
            if (RawName.empty()) { Fail("missing name"); }
            Result.Name = SanitizePathComponent(RawName);
            if (Result.Name.empty()) { Result.Name = "_"; }

            if (const Bencode::List* Files = Info.FindList("files")) {
                Result.IsMultiFile = true;
                if (Files->empty()) { Fail("files list is empty"); }

                for (const Bencode::Value& Entry : *Files) {
                    if (!Entry.IsDict()) { Fail("file entry is not a dictionary"); }

                    const Bencode::Integer* Length = Entry.FindInteger("length");
                    if (!Length || *Length < 0) { Fail("file has an invalid length"); }

                    const Bencode::Value* PathValue = FindPreferUtf8(Entry, "path");
                    if (!PathValue || !PathValue->IsList()) { Fail("file is missing its path"); }

                    const std::string* Attributes = Entry.FindString("attr");
                    Result.Files.push_back({
                      .Path      = SanitizedFilePath(Result.Name, PathValue->AsList()),
                      .Length    = *Length,
                      .Offset    = Result.TotalSize,
                      .IsPadding = Attributes && Attributes->find('p') != std::string::npos,
                    });
                    Result.TotalSize = CheckedAdd(Result.TotalSize, *Length);
                }
            } else if (const Bencode::Integer* Length = Info.FindInteger("length")) {
                if (*Length < 0) { Fail("invalid length"); }
                Result.Files.push_back({.Path = PathFromUtf8(Result.Name), .Length = *Length});
                Result.TotalSize = *Length;
            } else {
                Fail("info has neither length nor files");
            }

            if (Result.TotalSize == 0) { Fail("torrent contains no data"); }

            const std::int64_t ExpectedPieces = (Result.TotalSize - 1) / Result.PieceLength + 1;
            const auto PieceCount             = static_cast<std::int64_t>(Pieces->size() / Sha1Digest {}.size());
            if (PieceCount != ExpectedPieces) {
                Fail(std::format("expected {} piece hashes but found {}", ExpectedPieces, PieceCount));
            }

            Result.PieceHashes.resize(static_cast<std::size_t>(PieceCount));
            for (std::size_t I = 0; I < Result.PieceHashes.size(); ++I) {
                std::memcpy(Result.PieceHashes[I].data(), Pieces->data() + I * 20, 20);
            }

            const Bencode::Integer* Private = Info.FindInteger("private");
            Result.IsPrivate                = Private && *Private == 1;

            return Result;
        }

        std::vector<std::string> NonEmptyStrings(const Bencode::List& Items) {
            std::vector<std::string> Out;
            for (const Bencode::Value& Item : Items) {
                if (Item.IsString() && !Item.AsString().empty()) { Out.push_back(Item.AsString()); }
            }
            return Out;
        }
    }  // namespace

    std::int64_t Metainfo::PieceSize(const int Index) const {
        if (Index == PieceCount() - 1) { return TotalSize - PieceLength * Index; }
        return PieceLength;
    }

    Metainfo Metainfo::Parse(const std::string_view TorrentData) {
        const Bencode::Value Root = Bencode::Decode(TorrentData);
        if (!Root.IsDict()) { Fail("root is not a dictionary"); }

        const Bencode::Value* Info = Root.FindDict("info");
        if (!Info) { Fail("missing info dictionary"); }

        Metainfo Result = FromInfo(*Info, TorrentData.substr(Info->SourceOffset(), Info->SourceLength()));

        if (const Bencode::List* AnnounceList = Root.FindList("announce-list")) {
            for (const Bencode::Value& Tier : *AnnounceList) {
                if (!Tier.IsList()) { continue; }
                std::vector<std::string> Trackers = NonEmptyStrings(Tier.AsList());
                if (!Trackers.empty()) { Result.TrackerTiers.push_back(std::move(Trackers)); }
            }
        }
        // BEP 12: announce is ignored when announce-list is present
        if (Result.TrackerTiers.empty()) {
            if (const std::string* Announce = Root.FindString("announce"); Announce && !Announce->empty()) {
                Result.TrackerTiers.push_back({*Announce});
            }
        }

        if (const Bencode::Value* UrlList = Root.Find("url-list")) {
            if (UrlList->IsString() && !UrlList->AsString().empty()) {
                Result.WebSeeds.push_back(UrlList->AsString());
            } else if (UrlList->IsList()) {
                Result.WebSeeds = NonEmptyStrings(UrlList->AsList());
            }
        }

        Result.Comment   = StringOrEmpty(Root, "comment");
        Result.CreatedBy = StringOrEmpty(Root, "created by");
        if (const Bencode::Integer* Created = Root.FindInteger("creation date")) { Result.CreationDate = *Created; }

        return Result;
    }

    Metainfo Metainfo::ParseInfoDictionary(const std::string_view InfoBytes) {
        return FromInfo(Bencode::Decode(InfoBytes), InfoBytes);
    }

    Metainfo Metainfo::Load(const std::filesystem::path& File) {
        std::ifstream Stream(File, std::ios::binary | std::ios::ate);
        if (!Stream) { throw std::runtime_error(std::format("could not open {}", PathToUtf8(File))); }

        const std::streamoff Size = Stream.tellg();
        if (Size < 0 || Size > kMaxTorrentFileSize) {
            throw ParseError(std::format("torrent: {} is too large", PathToUtf8(File)));
        }

        std::string Data(static_cast<std::size_t>(Size), '\0');
        Stream.seekg(0);
        if (!Stream.read(Data.data(), Size)) {
            throw std::runtime_error(std::format("could not read {}", PathToUtf8(File)));
        }
        return Parse(Data);
    }

    std::string Metainfo::ToTorrentFile() const {
        // Every key sorts either before or after "info", so encode the two halves and splice the raw info between
        Bencode::Dict Before;
        Bencode::Dict After;

        Bencode::List Tiers;
        for (const std::vector<std::string>& Tier : TrackerTiers) {
            Bencode::List Trackers(Tier.begin(), Tier.end());
            if (!Trackers.empty()) { Tiers.emplace_back(std::move(Trackers)); }
        }
        if (!Tiers.empty()) {
            Before.emplace_back("announce", Tiers.front().AsList().front());
            Before.emplace_back("announce-list", std::move(Tiers));
        }
        if (!Comment.empty()) { Before.emplace_back("comment", Comment); }
        if (!CreatedBy.empty()) { Before.emplace_back("created by", CreatedBy); }
        if (CreationDate) { Before.emplace_back("creation date", *CreationDate); }
        if (!WebSeeds.empty()) { After.emplace_back("url-list", Bencode::List(WebSeeds.begin(), WebSeeds.end())); }

        std::string Head = Bencode::Encode(Before);
        Head.pop_back();  // "e"
        const std::string Tail = Bencode::Encode(After);
        return Head + "4:info" + InfoBytes + Tail.substr(1);  // Tail without its "d"
    }

    std::string SanitizePathComponent(const std::string_view Component) {
        if (Component.empty() || Component == ".") { return {}; }
        if (Component == "..") { return "_"; }

        constexpr std::string_view kForbidden = "<>:\"/\\|?*";
        std::string Out;
        Out.reserve(Component.size());
        for (const char C : Component) {
            const bool IsControl = static_cast<unsigned char>(C) < 0x20;
            Out += IsControl || kForbidden.find(C) != std::string_view::npos ? '_' : C;
        }

        // Windows silently strips trailing dots and spaces, which would let two entries collide
        while (!Out.empty() && (Out.back() == '.' || Out.back() == ' ')) { Out.pop_back(); }
        if (Out.empty()) { return "_"; }

        // Device names are reserved with or without an extension ("CON", "con.txt")
        std::string Stem = Out.substr(0, Out.find('.'));
        std::ranges::transform(Stem, Stem.begin(), [](const char C) {
            return C >= 'a' && C <= 'z' ? static_cast<char>(C - 'a' + 'A') : C;
        });
        constexpr std::array<std::string_view, 4> kReserved = {"CON", "PRN", "AUX", "NUL"};
        const bool IsNumberedDevice = Stem.size() == 4 && (Stem.starts_with("COM") || Stem.starts_with("LPT")) &&
                                      Stem[3] >= '1' && Stem[3] <= '9';
        if (IsNumberedDevice || std::ranges::find(kReserved, Stem) != kReserved.end()) { Out.insert(0, "_"); }

        return Out;
    }
}  // namespace Tori
