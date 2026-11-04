#include "Engine/ResumeData.hpp"

#include "Core/Bencode.hpp"
#include "Core/ByteIO.hpp"
#include "Core/Error.hpp"
#include "Core/Utf8.hpp"
#include "Core/Win32.hpp"

#include <windows.h>

#include <cstring>

namespace Tori {
    namespace {
        constexpr Bencode::Integer kVersion = 1;
        constexpr std::size_t kStampSize    = 16;

        [[noreturn]] void Fail(const std::string_view Reason) {
            throw ParseError(std::string("resume data: ") + std::string(Reason));
        }

        Bencode::Integer RequireInteger(const Bencode::Value& Root, const std::string_view Key) {
            const Bencode::Integer* Value = Root.FindInteger(Key);
            if (!Value) { Fail(std::string(Key) + " is missing"); }
            return *Value;
        }

        const std::string& RequireString(const Bencode::Value& Root, const std::string_view Key) {
            const std::string* Value = Root.FindString(Key);
            if (!Value) { Fail(std::string(Key) + " is missing"); }
            return *Value;
        }
    }  // namespace

    std::vector<FileStamp> StampFiles(const std::filesystem::path& Destination, const std::span<const TorrentFile> Files) {
        std::vector<FileStamp> Stamps(Files.size());
        for (std::size_t I = 0; I < Files.size(); ++I) {
            if (Files[I].IsPadding) { continue; }
            WIN32_FILE_ATTRIBUTE_DATA Attributes {};
            if (!GetFileAttributesExW(LongPath(Destination / Files[I].Path).c_str(), GetFileExInfoStandard, &Attributes) ||
                (Attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }
            Stamps[I].Size = static_cast<std::int64_t>((static_cast<std::uint64_t>(Attributes.nFileSizeHigh) << 32) |
                                                       Attributes.nFileSizeLow);
            Stamps[I].Modified =
              static_cast<std::int64_t>((static_cast<std::uint64_t>(Attributes.ftLastWriteTime.dwHighDateTime) << 32) |
                                        Attributes.ftLastWriteTime.dwLowDateTime);
        }
        return Stamps;
    }

    std::string EncodeResumeData(const ResumeData& Data) {
        Bencode::List Tiers;
        for (const std::vector<std::string>& Tier : Data.ExtraTrackers) {
            Tiers.emplace_back(Bencode::List(Tier.begin(), Tier.end()));
        }

        Bencode::List Skipped(Data.SkippedFiles.begin(), Data.SkippedFiles.end());

        ByteWriter Stamps;
        for (const FileStamp& Stamp : Data.Files) {
            Stamps.WriteU64(static_cast<std::uint64_t>(Stamp.Size));
            Stamps.WriteU64(static_cast<std::uint64_t>(Stamp.Modified));
        }

        return Bencode::Encode(Bencode::Dict {
          {"version", kVersion},
          {"info hash", std::string_view(reinterpret_cast<const char*>(Data.Hash.Bytes.data()), Data.Hash.Bytes.size())},
          {"name", Data.Name},
          {"destination", PathToUtf8(Data.Destination)},
          {"trackers", std::move(Tiers)},
          {"paused", Data.Paused ? 1 : 0},
          {"uploaded", Data.Uploaded},
          {"downloaded", Data.Downloaded},
          {"added on", Data.AddedOn},
          {"position", Data.Position},
          {"skipped files", std::move(Skipped)},
          {"pieces", std::string_view(reinterpret_cast<const char*>(Data.Pieces.data()), Data.Pieces.size())},
          {"file stamps", std::string_view(reinterpret_cast<const char*>(Stamps.Data().data()), Stamps.Data().size())},
        });
    }

    ResumeData DecodeResumeData(const std::string_view Encoded) {
        const Bencode::Value Root = Bencode::Decode(Encoded);
        if (!Root.IsDict()) { Fail("not a dictionary"); }
        if (RequireInteger(Root, "version") != kVersion) { Fail("unsupported version"); }

        ResumeData Data;
        const std::string& Hash = RequireString(Root, "info hash");
        if (Hash.size() != Data.Hash.Bytes.size()) { Fail("info hash has the wrong length"); }
        std::memcpy(Data.Hash.Bytes.data(), Hash.data(), Hash.size());

        Data.Name        = RequireString(Root, "name");
        Data.Destination = PathFromUtf8(RequireString(Root, "destination"));
        if (Data.Destination.empty()) { Fail("destination is empty"); }

        if (const Bencode::List* Tiers = Root.FindList("trackers")) {
            for (const Bencode::Value& Tier : *Tiers) {
                std::vector<std::string>& Urls = Data.ExtraTrackers.emplace_back();
                for (const Bencode::Value& Url : Tier.AsList()) { Urls.push_back(Url.AsString()); }
            }
        }

        Data.Paused     = RequireInteger(Root, "paused") != 0;
        Data.Uploaded   = RequireInteger(Root, "uploaded");
        Data.Downloaded = RequireInteger(Root, "downloaded");
        Data.AddedOn    = RequireInteger(Root, "added on");
        Data.Position   = RequireInteger(Root, "position");

        if (const Bencode::List* Skipped = Root.FindList("skipped files")) {
            for (const Bencode::Value& Index : *Skipped) { Data.SkippedFiles.push_back(static_cast<int>(Index.AsInteger())); }
        }

        const std::string& Pieces = RequireString(Root, "pieces");
        Data.Pieces.assign(Pieces.begin(), Pieces.end());

        const std::string& Stamps = RequireString(Root, "file stamps");
        if (Stamps.size() % kStampSize != 0) { Fail("file stamps have the wrong length"); }
        ByteReader Reader(std::span(reinterpret_cast<const std::uint8_t*>(Stamps.data()), Stamps.size()));
        Data.Files.resize(Stamps.size() / kStampSize);
        for (FileStamp& Stamp : Data.Files) {
            Stamp.Size     = static_cast<std::int64_t>(Reader.ReadU64());
            Stamp.Modified = static_cast<std::int64_t>(Reader.ReadU64());
        }
        return Data;
    }
}  // namespace Tori
