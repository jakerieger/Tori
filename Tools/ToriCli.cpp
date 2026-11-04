// Headless driver for the engine: downloads one torrent and prints progress. Used to verify real-world transfers
// without the GUI.
//
//   ToriCli <file.torrent | magnet link> <destination> [--loopback-listen] [--timeout-minutes N] [--seed-minutes N]
//           [--state FOLDER]
//
// --loopback-listen keeps the TCP listener on 127.0.0.1 (no inbound peers, no firewall prompt for it). The DHT
// binds UDP on all interfaces either way, since it has to reach the internet. --state saves progress to FOLDER and
// resumes from it, as the app does.

#include "Core/Format.hpp"
#include "Core/Utf8.hpp"
#include "Engine/Session.hpp"
#include "Meta/MagnetUri.hpp"
#include "Meta/Metainfo.hpp"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace Tori;
using namespace std::chrono_literals;

int main() {
    int Count         = 0;
    LPWSTR* Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    std::vector<std::wstring> Args(Arguments + 1, Arguments + Count);
    LocalFree(Arguments);

    if (Args.size() < 2) {
        std::fprintf(stderr,
                     "usage: ToriCli <file.torrent> <destination> [--loopback-listen] [--timeout-minutes N] "
                     "[--seed-minutes N] [--state FOLDER]\n");
        return 2;
    }

    SessionSettings Settings;
    int TimeoutMinutes = 60;
    int SeedMinutes    = 0;  // Keep seeding this long after the download completes
    for (std::size_t I = 2; I < Args.size(); ++I) {
        if (Args[I] == L"--loopback-listen") {
            Settings.LoopbackOnly = true;  // No inbound peers, but no firewall prompt either
        } else if (Args[I] == L"--timeout-minutes" && I + 1 < Args.size()) {
            TimeoutMinutes = std::stoi(Args[++I]);
        } else if (Args[I] == L"--seed-minutes" && I + 1 < Args.size()) {
            SeedMinutes = std::stoi(Args[++I]);
        } else if (Args[I] == L"--state" && I + 1 < Args.size()) {
            Settings.StateFolder = Args[++I];
        }
    }

    try {
        Session Engine(Settings);
        TorrentId Id = kInvalidTorrentId;

        if (Args[0].starts_with(L"magnet:")) {
            const MagnetUri Magnet = MagnetUri::Parse(WideToUtf8(Args[0]));
            std::printf("magnet %s: %zu tracker(s), %zu peer(s)\n",
                        Magnet.Hash.ToHex().c_str(),
                        Magnet.Trackers.size(),
                        Magnet.Peers.size());
            Id = Engine.Find(Magnet.Hash).value_or(kInvalidTorrentId);
            if (Id == kInvalidTorrentId) { Id = Engine.AddMagnet(Magnet, Args[1]); }
        } else {
            Metainfo Meta = Metainfo::Load(Args[0]);
            std::printf("%s: %s in %d pieces of %s, %zu tracker tier(s)\n",
                        Meta.Name.c_str(),
                        FormatBytes(static_cast<double>(Meta.TotalSize)).c_str(),
                        Meta.PieceCount(),
                        FormatBytes(static_cast<double>(Meta.PieceLength)).c_str(),
                        Meta.TrackerTiers.size());
            Id = Engine.Find(Meta.Hash).value_or(kInvalidTorrentId);
            if (Id == kInvalidTorrentId) { Id = Engine.AddTorrent(std::move(Meta), Args[1]); }
        }

        const auto StatsOf = [&] {
            for (TorrentStats& Stats : Engine.Snapshot()) {
                if (Stats.Id == Id) { return Stats; }
            }
            return TorrentStats {};
        };
        if (const TorrentStats Restored = StatsOf(); Restored.UploadedTotal > 0 || Restored.DownloadedTotal > 0) {
            std::printf("restored: downloaded %s, uploaded %s so far\n",
                        FormatBytes(Restored.DownloadedTotal).c_str(),
                        FormatBytes(Restored.UploadedTotal).c_str());
        }

        const auto Started    = std::chrono::steady_clock::now();
        auto Deadline         = Started + std::chrono::minutes(TimeoutMinutes);
        bool Seeding          = false;

        while (std::chrono::steady_clock::now() < Deadline) {
            std::this_thread::sleep_for(5s);
            const TorrentStats Stats = StatsOf();
            const auto Elapsed =
              std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - Started).count();
            std::printf("[%4llds] %-17s %5.1f%%  %s of %s  down %s  up %s  peers %d  dht nodes %zu %s\n",
                        static_cast<long long>(Elapsed),
                        TorrentStateString(Stats.State).c_str(),
                        ComputeProgress(Stats.DownloadedSize, Stats.TotalSize) * 100.0f,
                        FormatBytes(Stats.DownloadedSize).c_str(),
                        FormatBytes(Stats.TotalSize).c_str(),
                        FormatRate(Stats.DownloadSpeed).c_str(),
                        FormatRate(Stats.UploadSpeed).c_str(),
                        Stats.Peers,
                        Engine.DhtNodeCount(),
                        Stats.ErrorMessage.c_str());
            std::fflush(stdout);

            if (Stats.State == TorrentState::Seeding && !Seeding) {
                Seeding = true;
                std::printf("Complete in %llds\n", static_cast<long long>(Elapsed));
                if (SeedMinutes <= 0) { return 0; }
                Deadline = std::chrono::steady_clock::now() + std::chrono::minutes(SeedMinutes);
                std::printf("Seeding for %d minute(s)\n", SeedMinutes);
            }
            if (Stats.State == TorrentState::Error) { return 1; }
        }

        if (Seeding) {
            const TorrentStats Final = StatsOf();
            std::printf("Seeding finished: uploaded %s, ratio %.3f\n",
                        FormatBytes(Final.UploadedTotal).c_str(),
                        ComputeRatio(Final.UploadedTotal, Final.DownloadedTotal, Final.DownloadedSize));
            return 0;
        }
        std::printf("Timed out\n");
        return 1;
    } catch (const std::exception& Ex) {
        std::fprintf(stderr, "error: %s\n", Ex.what());
        return 1;
    }
}
