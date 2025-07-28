#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <filesystem>
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/download_priority.hpp>

#ifdef _WIN32
    #define NOMINMAX
    #include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace {
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_RESET "\x1b[0m"

    void PrintProgress(const lt::torrent_status& ts) {
        const int BAR_WIDTH = 30;  // Width of the progress bar
        float progress      = ts.progress;

        // Calculate how many "=" characters to show based on progress
        int pos = BAR_WIDTH * progress;

        // Create the progress bar string with color
        std::stringstream bar;
        bar << "[";

        for (int i = 0; i < BAR_WIDTH; ++i) {
            if (i < pos) {
                bar << ANSI_COLOR_GREEN << "=" << ANSI_COLOR_RESET;
            } else if (i == pos) {
                bar << ANSI_COLOR_YELLOW << ">" << ANSI_COLOR_RESET;
            } else {
                bar << " ";
            }
        }
        bar << "]";

        // Clear the current line and move to the beginning
        std::cout << "\r\033[K";  // \033[K clears to the end of line

        // Print the progress bar and stats with color
        std::cout << bar.str() << " " << ANSI_COLOR_CYAN << std::fixed << std::setprecision(1) << (progress * 100)
                  << "%" << ANSI_COLOR_RESET << " | " << ANSI_COLOR_BLUE << std::setw(6)
                  << (ts.download_rate / 1000.f / 1000.f) << " MB/s down" << ANSI_COLOR_RESET << " | " << std::setw(6)
                  << (ts.upload_rate / 1000.f / 1000.f) << " MB/s up | "
                  << "peers: " << std::setw(2) << ts.num_peers << " | " << (ts.total_wanted / 1000 / 1000) << " MBs";

        std::cout.flush();
    }

    void PrintHelp() {
        std::cerr << "error: Missing magnet URI, nothing to download." << std::endl;
        std::cout << "usage: tori <magnet-uri> <location>" << std::endl;
    }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintHelp();
        return EXIT_FAILURE;
    }

    const std::string magnetUri = argv[1];
    std::string location        = fs::current_path().string();
    if (argc >= 3) { location = argv[2]; }

    lt::settings_pack pack;
    pack.set_int(lt::settings_pack::alert_mask,
                 lt::alert_category::status | lt::alert_category::error | lt::alert_category::storage |
                   lt::alert_category::torrent_log | lt::alert_category::file_progress |
                   lt::alert_category::block_progress | lt::alert_category::piece_progress);
    lt::session session(pack);

    lt::add_torrent_params params;
    lt::error_code ec;
    lt::parse_magnet_uri(magnetUri, params, ec);

    if (ec) {
        std::cerr << "error: " << ec.message() << std::endl;
        return 1;
    }

    params.save_path = location;
    params.flags |= lt::torrent_flags::auto_managed;
    params.flags |= lt::torrent_flags::need_save_resume;

    lt::torrent_handle handle = session.add_torrent(params);
    if (!handle.status().name.empty()) {
        std::cout << "\nTori started for: \n";
        std::cout << ANSI_COLOR_YELLOW << handle.status().name << ANSI_COLOR_RESET << std::endl;
    }

    bool finished {false};
    while (!finished) {
        std::vector<lt::alert*> alerts;
        session.pop_alerts(&alerts);

        for (lt::alert* a : alerts) {
            if (auto* ta = lt::alert_cast<lt::add_torrent_alert>(a)) { handle = ta->handle; }

            // Check if download is finished
            if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
                finished = true;
                break;
            }

            // Handle errors
            if (auto error = lt::alert_cast<lt::torrent_error_alert>(a)) {
                std::cerr << "\nError: " << error->message() << std::endl;
                return 1;
            }
        }

        // Display progress
        if (handle.is_valid()) {
            lt::torrent_status status = handle.status();
            PrintProgress(status);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "Waiting a moment to start seeding..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Save resume data and exit
    handle.save_resume_data(lt::torrent_handle::save_info_dict);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    session.remove_torrent(handle);

    std::cout << "Download completed" << std::endl;

    return 0;
}