#include "Core/Error.hpp"
#include "Core/FileIO.hpp"
#include "Core/Win32.hpp"
#include "Engine/ResumeData.hpp"
#include "Engine/Session.hpp"
#include "Meta/MagnetUri.hpp"
#include "Meta/Metainfo.hpp"
#include "Meta/TorrentCreator.hpp"
#include "TestUtil.hpp"

#include <doctest/doctest.h>

#include <windows.h>

#include <thread>

using namespace Tori;
using namespace Tori::Testing;
using namespace std::chrono_literals;

namespace {
    template<class TCondition>
    bool WaitUntil(TCondition&& Condition, const std::chrono::milliseconds Timeout) {
        const auto Deadline = std::chrono::steady_clock::now() + Timeout;
        while (std::chrono::steady_clock::now() < Deadline) {
            if (Condition()) { return true; }
            std::this_thread::sleep_for(20ms);
        }
        return Condition();
    }

    SessionSettings StateSettings(const std::filesystem::path& Folder) {
        SessionSettings Settings;
        Settings.ListenPort   = 0;
        Settings.LoopbackOnly = true;
        Settings.EnableDht    = false;
        Settings.StateFolder  = Folder;
        Settings.Announce     = [](const std::string&, const AnnounceRequest&, std::stop_token) -> AnnounceResponse {
            throw TrackerError("no trackers in tests");
        };
        return Settings;
    }

    TorrentStats Only(const Session& Target) {
        const std::vector<TorrentStats> All = Target.Snapshot();
        REQUIRE(All.size() == 1);
        return All.front();
    }

    /// Flips one byte of a file. With KeepTime the last-write time is put back, so only a full check notices.
    void CorruptByte(const std::filesystem::path& File, const bool KeepTime, const LONG Offset = 0) {
        HANDLE Handle = CreateFileW(LongPath(File).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(Handle != INVALID_HANDLE_VALUE);
        FILETIME Written {};
        GetFileTime(Handle, nullptr, nullptr, &Written);

        std::uint8_t Byte = 0;
        DWORD Done        = 0;
        SetFilePointer(Handle, Offset, nullptr, FILE_BEGIN);
        REQUIRE(ReadFile(Handle, &Byte, 1, &Done, nullptr));
        Byte ^= 0xFF;
        SetFilePointer(Handle, Offset, nullptr, FILE_BEGIN);
        REQUIRE(WriteFile(Handle, &Byte, 1, &Done, nullptr));
        if (KeepTime) { SetFileTime(Handle, nullptr, nullptr, &Written); }
        CloseHandle(Handle);
    }
}  // namespace

TEST_CASE("Resume data round-trips") {
    ResumeData Data;
    Data.Hash.Bytes.fill(0xAB);
    Data.Name          = "Ubuntu \xE2\x9C\x93";
    Data.Destination   = L"C:\\Downloads\\Fol\u00e9der";
    Data.ExtraTrackers = {{"udp://a:1"}, {"http://b/announce", "http://c/announce"}};
    Data.Paused        = true;
    Data.Uploaded      = 1LL << 40;
    Data.Downloaded    = 12345;
    Data.AddedOn       = 1700000000;
    Data.Position      = 7;
    Data.SkippedFiles  = {2, 5};
    Data.Pieces        = {0xF0, 0x80};
    Data.Files         = {{100, 133000000000000000LL}, {-1, 0}};

    const ResumeData Back = DecodeResumeData(EncodeResumeData(Data));
    CHECK(Back.Hash == Data.Hash);
    CHECK(Back.Name == Data.Name);
    CHECK(Back.Destination == Data.Destination);
    CHECK(Back.ExtraTrackers == Data.ExtraTrackers);
    CHECK(Back.Paused);
    CHECK(Back.Uploaded == Data.Uploaded);
    CHECK(Back.Downloaded == Data.Downloaded);
    CHECK(Back.AddedOn == Data.AddedOn);
    CHECK(Back.Position == Data.Position);
    CHECK(Back.SkippedFiles == Data.SkippedFiles);
    CHECK(Back.Pieces == Data.Pieces);
    CHECK(Back.Files == Data.Files);

    CHECK_THROWS_AS(DecodeResumeData("de"), ParseError);
    CHECK_THROWS_AS(DecodeResumeData("i3e"), ParseError);
}

TEST_CASE("File stamps notice changes and missing files") {
    TempDir Dir;
    WriteFileBytes(Dir.Path / L"a.bin", RandomBytes(1000, 1));
    const std::vector<TorrentFile> Files = {
      {.Path = L"a.bin", .Length = 1000},
      {.Path = L"pad", .Length = 24, .Offset = 1000, .IsPadding = true},
      {.Path = L"b.bin", .Length = 10, .Offset = 1024},
    };

    const std::vector<FileStamp> Before = StampFiles(Dir.Path, Files);
    CHECK(Before[0].Size == 1000);
    CHECK(Before[1].Size == -1);
    CHECK(Before[2].Size == -1);
    CHECK(StampFiles(Dir.Path, Files) == Before);

    CorruptByte(Dir.Path / L"a.bin", true);
    CHECK(StampFiles(Dir.Path, Files) == Before);  // Same size and time: stamps can't tell
    std::this_thread::sleep_for(50ms);  // Write times only move on with the system clock's tick
    CorruptByte(Dir.Path / L"a.bin", false);
    CHECK(StampFiles(Dir.Path, Files) != Before);
}

TEST_CASE("WriteFileAtomically replaces contents") {
    TempDir Dir;
    const auto File = Dir.Path / L"state.dat";
    WriteFileAtomically(File, "first");
    WriteFileAtomically(File, "second");
    CHECK(ReadWholeFile(File, 100) == "second");
    CHECK_FALSE(ReadWholeFile(File, 3).has_value());  // Over the size limit
    CHECK_FALSE(std::filesystem::exists(Dir.Path / L"state.dat.tmp"));
    CHECK_FALSE(ReadWholeFile(Dir.Path / L"missing", 100).has_value());
}

TEST_CASE("ToTorrentFile keeps the info hash and trackers") {
    TempDir Dir;
    WriteFileBytes(Dir.Path / L"x.bin", RandomBytes(5000, 4));
    TorrentCreatorOptions Options;
    Options.Source       = Dir.Path / L"x.bin";
    Options.PieceLength  = 16 * 1024;
    Options.TrackerTiers = {{"http://t/announce"}};
    Options.Comment      = "hello";
    Options.WebSeeds     = {"http://w/x.bin"};
    const Metainfo Original = Metainfo::Parse(*CreateTorrent(Options));

    Metainfo Fetched = Metainfo::ParseInfoDictionary(Original.InfoBytes);  // As a magnet link gets it
    Fetched.TrackerTiers = {{"udp://a:1"}, {"udp://b:2"}};
    const Metainfo Back  = Metainfo::Parse(Fetched.ToTorrentFile());
    CHECK(Back.Hash == Original.Hash);
    CHECK(Back.TrackerTiers == Fetched.TrackerTiers);

    const Metainfo Again = Metainfo::Parse(Original.ToTorrentFile());
    CHECK(Again.Hash == Original.Hash);
    CHECK(Again.Comment == "hello");
    CHECK(Again.WebSeeds == Original.WebSeeds);
    CHECK(Again.TrackerTiers == Original.TrackerTiers);
}

TEST_CASE("Session restores torrents, trusting unchanged files and rechecking changed ones") {
    TempDir Dir;
    const auto State = Dir.Path / L"state";
    WriteFileBytes(Dir.Path / L"data/Share/a.bin", RandomBytes(200000, 1));
    WriteFileBytes(Dir.Path / L"data/Share/b.bin", RandomBytes(70000, 2));
    TorrentCreatorOptions Options;
    Options.Source      = Dir.Path / L"data/Share";
    Options.PieceLength = 16 * 1024;
    const Metainfo Meta = Metainfo::Parse(*CreateTorrent(Options));

    {
        Session First(StateSettings(State));
        First.AddTorrent(Meta, Dir.Path / L"data");
        REQUIRE(WaitUntil([&] { return Only(First).State == TorrentState::Seeding; }, 10s));
    }
    CHECK(std::filesystem::exists(State / L"resume" / (Meta.Hash.ToHex() + ".dat")));
    CHECK(std::filesystem::exists(State / L"torrents" / (Meta.Hash.ToHex() + ".torrent")));

    // Corrupt data the stamps can't see: a restored torrent trusts its saved pieces, so it stays complete
    CorruptByte(Dir.Path / L"data/Share/a.bin", true);
    {
        Session Second(StateSettings(State));
        REQUIRE(WaitUntil([&] { return Only(Second).State == TorrentState::Seeding; }, 10s));
        CHECK(Only(Second).Name == Meta.Name);
        CHECK(Only(Second).DownloadedSize == static_cast<double>(Meta.TotalSize));
    }

    // A change the stamps can see forces a full check, which finds the bad piece
    CorruptByte(Dir.Path / L"data/Share/a.bin", false, 1);  // Same piece as before
    {
        Session Third(StateSettings(State));
        REQUIRE(WaitUntil([&] { return Only(Third).State == TorrentState::Downloading; }, 10s));
        CHECK(Only(Third).DownloadedSize == static_cast<double>(Meta.TotalSize - Meta.PieceLength));
        Third.Pause(Only(Third).Id);
        REQUIRE(WaitUntil([&] { return Only(Third).State == TorrentState::Paused; }, 5s));
    }

    // Paused is remembered too, and removing forgets everything
    {
        Session Fourth(StateSettings(State));
        REQUIRE(WaitUntil([&] { return Only(Fourth).State == TorrentState::Paused; }, 10s));
        Fourth.RemoveTorrent(Only(Fourth).Id);
    }
    CHECK_FALSE(std::filesystem::exists(State / L"resume" / (Meta.Hash.ToHex() + ".dat")));
    CHECK_FALSE(std::filesystem::exists(State / L"torrents" / (Meta.Hash.ToHex() + ".torrent")));
    Session Fifth(StateSettings(State));
    CHECK(Fifth.Snapshot().empty());
}

TEST_CASE("Session restores a magnet link that hasn't fetched its metadata") {
    TempDir Dir;
    const auto State = Dir.Path / L"state";
    const MagnetUri Magnet =
      MagnetUri::Parse("magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Some+Name&tr=udp%3A%2F%2Ft%3A1");

    {
        Session First(StateSettings(State));
        First.AddMagnet(Magnet, Dir.Path / L"data");
        REQUIRE(WaitUntil([&] { return Only(First).State == TorrentState::FetchingMetadata; }, 5s));
    }
    Session Second(StateSettings(State));
    const TorrentStats Restored = Only(Second);
    CHECK(Restored.Hash == Magnet.Hash);
    CHECK(Restored.Name == "Some Name");
    CHECK(Restored.Destination == Dir.Path / L"data");
    CHECK(Second.Find(Magnet.Hash) == Restored.Id);
}
