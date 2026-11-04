#include "Core/Format.hpp"
#include "Core/TorrentTypes.hpp"

#include <doctest/doctest.h>

using namespace Tori;

TEST_CASE("FormatBytes picks binary units") {
    CHECK(FormatBytes(0) == "0 B");
    CHECK(FormatBytes(512) == "512 B");
    CHECK(FormatBytes(1536) == "1.50 KB");
    CHECK(FormatBytes(5.0 * 1024 * 1024) == "5.00 MB");
    CHECK(FormatBytes(2.5 * 1024 * 1024 * 1024) == "2.50 GB");
    CHECK(FormatBytes(3.0 * 1024 * 1024 * 1024 * 1024) == "3.00 TB");
}

TEST_CASE("FormatRate appends per-second suffix") {
    CHECK(FormatRate(2048) == "2.00 KB/s");
}

TEST_CASE("ComputeProgress handles unknown and overflowing sizes") {
    CHECK(ComputeProgress(100, 0) == 0.0f);
    CHECK(ComputeProgress(50, 100) == doctest::Approx(0.5f));
    CHECK(ComputeProgress(200, 100) == 1.0f);
}

TEST_CASE("ComputeRatio is zero before anything is downloaded") {
    CHECK(ComputeRatio(100, 0) == 0.0);
    CHECK(ComputeRatio(50, 100) == doctest::Approx(0.5));
    // Data already on disk counts as downloaded
    CHECK(ComputeRatio(50, 0, 100) == doctest::Approx(0.5));
    CHECK(ComputeRatio(50, 200, 100) == doctest::Approx(0.25));
    CHECK(ComputeRatio(0, 0, 0) == 0.0);
}
