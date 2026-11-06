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

#include "Meta/TorrentCreator.hpp"

#include "Core/Bencode.hpp"
#include "Core/Sha1.hpp"
#include "Core/Utf8.hpp"
#include "Meta/Metainfo.hpp"
#include "Storage/FileStorage.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <span>
#include <stdexcept>

namespace Tori {
    namespace fs = std::filesystem;

    namespace {
        constexpr std::int64_t kMinAutoPieceLength = 16 * 1024;
        constexpr std::int64_t kMaxAutoPieceLength = 16 * 1024 * 1024;
        constexpr std::int64_t kTargetPieceCount   = 1500;

        [[noreturn]] void Fail(const std::string& Reason) {
            throw std::runtime_error(std::format("create torrent: {}", Reason));
        }

        struct SourceLayout {
            std::vector<TorrentFile> Files;
            std::vector<Bencode::List> PathComponents;  // UTF-8 path inside the torrent, per file
            std::int64_t TotalSize = 0;
            bool IsMultiFile       = false;
        };

        SourceLayout ScanSource(const fs::path& Source) {
            SourceLayout Layout;
            std::error_code Ec;
            const fs::file_status Status = fs::symlink_status(Source, Ec);
            const fs::path Name          = Source.filename();

            if (fs::is_regular_file(Status)) {
                Layout.TotalSize = static_cast<std::int64_t>(fs::file_size(Source));
                Layout.Files.push_back({.Path = Name, .Length = Layout.TotalSize});
                return Layout;
            }
            if (!fs::is_directory(Status)) { Fail(std::format("{} is not a file or folder", PathToUtf8(Source))); }

            Layout.IsMultiFile = true;
            std::vector<fs::path> Relative;
            for (const fs::directory_entry& Entry :
                 fs::recursive_directory_iterator(Source, fs::directory_options::skip_permission_denied)) {
                // Directory symlinks aren't followed by the iterator; file symlinks are skipped here
                if (Entry.is_symlink() || !Entry.is_regular_file()) { continue; }
                Relative.push_back(Entry.path().lexically_relative(Source));
            }
            // Sorted so the same folder always produces the same torrent
            std::ranges::sort(Relative);

            for (const fs::path& Path : Relative) {
                const auto Length = static_cast<std::int64_t>(fs::file_size(Source / Path));
                Bencode::List Components;
                for (const fs::path& Component : Path) {
                    Components.emplace_back(PathToUtf8(Component));
                }

                Layout.Files.push_back({.Path = Name / Path, .Length = Length, .Offset = Layout.TotalSize});
                Layout.PathComponents.push_back(std::move(Components));
                Layout.TotalSize += Length;
            }
            return Layout;
        }

        std::optional<std::string> HashPieces(const fs::path& Root,
                                              const SourceLayout& Layout,
                                              const std::int64_t PieceLength,
                                              const std::stop_token& Stop,
                                              const CreateProgress& Progress) {
            FileStorage Storage(Root, Layout.Files);
            const auto Count = static_cast<int>((Layout.TotalSize - 1) / PieceLength + 1);
            std::vector<std::uint8_t> Buffer(static_cast<std::size_t>(PieceLength));
            std::string Pieces;
            Pieces.reserve(static_cast<std::size_t>(Count) * sizeof(Sha1Digest));
            Sha1 Hasher;

            for (int Index = 0; Index < Count; ++Index) {
                if (Stop.stop_requested()) { return std::nullopt; }

                const std::int64_t Offset = PieceLength * Index;
                const std::span<std::uint8_t> Piece =
                  std::span(Buffer).first(static_cast<std::size_t>(std::min(PieceLength, Layout.TotalSize - Offset)));
                if (!Storage.Read(Offset, Piece)) { Fail("files changed while they were being hashed"); }

                Hasher.Update(Piece.data(), Piece.size());
                const Sha1Digest Digest = Hasher.Finish();
                Pieces.append(reinterpret_cast<const char*>(Digest.data()), Digest.size());

                if (Progress) { Progress(Index + 1, Count); }
            }
            return Pieces;
        }
    }  // namespace

    std::int64_t AutoPieceLength(const std::int64_t TotalSize) {
        std::int64_t Length = kMinAutoPieceLength;
        while (Length < kMaxAutoPieceLength && TotalSize / Length > kTargetPieceCount) {
            Length *= 2;
        }
        return Length;
    }

    std::optional<std::string>
    CreateTorrent(const TorrentCreatorOptions& Options, const std::stop_token Stop, const CreateProgress& Progress) {
        fs::path Source = fs::absolute(Options.Source).lexically_normal();
        if (!Source.has_filename()) { Source = Source.parent_path(); }  // "C:/folder/" normalizes with a trailing slash
        if (Source.filename().empty() || Source == Source.root_path()) { Fail("cannot share a whole drive"); }

        const SourceLayout Layout = ScanSource(Source);
        if (Layout.TotalSize == 0) { Fail("there is no data to share"); }

        const std::int64_t PieceLength =
          Options.PieceLength > 0 ? Options.PieceLength : AutoPieceLength(Layout.TotalSize);
        if (PieceLength > kMaxPieceLength) { Fail("piece length is too large"); }

        std::optional<std::string> Pieces = HashPieces(Source.parent_path(), Layout, PieceLength, Stop, Progress);
        if (!Pieces) { return std::nullopt; }

        Bencode::Dict Info = {
          {"name", PathToUtf8(Source.filename())},
          {"piece length", PieceLength},
          {"pieces", std::move(*Pieces)},
        };
        if (Layout.IsMultiFile) {
            Bencode::List Files;
            for (std::size_t I = 0; I < Layout.Files.size(); ++I) {
                Files.emplace_back(Bencode::Dict {
                  {"length", Layout.Files[I].Length},
                  {"path", Layout.PathComponents[I]},
                });
            }
            Info.emplace_back("files", std::move(Files));
        } else {
            Info.emplace_back("length", Layout.TotalSize);
        }
        if (Options.IsPrivate) { Info.emplace_back("private", 1); }

        Bencode::Dict Root;
        Bencode::List Tiers;
        std::size_t TrackerCount = 0;
        for (const std::vector<std::string>& Tier : Options.TrackerTiers) {
            Bencode::List Trackers;
            for (const std::string& Tracker : Tier) {
                if (!Tracker.empty()) { Trackers.emplace_back(Tracker); }
            }
            TrackerCount += Trackers.size();
            if (!Trackers.empty()) { Tiers.emplace_back(std::move(Trackers)); }
        }
        if (!Tiers.empty()) {
            // Older clients only read "announce", so it carries the first tracker either way
            Root.emplace_back("announce", Tiers.front().AsList().front());
            if (TrackerCount > 1) { Root.emplace_back("announce-list", std::move(Tiers)); }
        }

        Bencode::List WebSeeds;
        for (const std::string& Seed : Options.WebSeeds) {
            if (!Seed.empty()) { WebSeeds.emplace_back(Seed); }
        }
        if (!WebSeeds.empty()) { Root.emplace_back("url-list", std::move(WebSeeds)); }

        if (!Options.Comment.empty()) { Root.emplace_back("comment", Options.Comment); }
        if (!Options.CreatedBy.empty()) { Root.emplace_back("created by", Options.CreatedBy); }
        if (Options.IncludeCreationDate) {
            Root.emplace_back("creation date", static_cast<Bencode::Integer>(std::time(nullptr)));
        }
        Root.emplace_back("info", std::move(Info));

        return Bencode::Encode(Root);
    }
}  // namespace Tori
