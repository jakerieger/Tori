#pragma once

#include <stdexcept>

namespace Tori {
    /// @brief Thrown when untrusted input (bencode, .torrent files, magnet links, peer messages) is malformed.
    class ParseError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };
}  // namespace Tori
