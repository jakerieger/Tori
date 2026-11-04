#include "Engine/Choker.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace Tori;
using namespace std::chrono_literals;

namespace {
    using Clock = Choker::Clock;
    const Clock::time_point T0 = Clock::time_point {} + 10h;

    ChokeCandidate Peer(const std::uint64_t Key, const bool Interested, const double Down, const double Up = 0.0) {
        return {
          .Key          = Key,
          .Interested   = Interested,
          .DownloadRate = Down,
          .UploadRate   = Up,
          .Connected    = T0 - 10min,
        };
    }

    std::vector<std::uint64_t> UnchokedKeys(const std::vector<ChokeCandidate>& Candidates, const std::vector<bool>& Unchoke) {
        std::vector<std::uint64_t> Keys;
        for (std::size_t I = 0; I < Candidates.size(); ++I) {
            if (Unchoke[I]) { Keys.push_back(Candidates[I].Key); }
        }
        return Keys;
    }

    bool Contains(const std::vector<std::uint64_t>& Keys, const std::uint64_t Key) {
        return std::ranges::find(Keys, Key) != Keys.end();
    }
}  // namespace

TEST_CASE("Choker gives regular slots to the fastest uploaders while downloading") {
    Choker Chooser({.Slots = 2}, 1);
    std::vector<ChokeCandidate> Candidates = {
      Peer(1, true, 100),
      Peer(2, true, 300),
      Peer(3, true, 200),
      Peer(4, false, 999),  // not interested: never unchoked
      Peer(5, true, 50),
    };
    Candidates[2].Snubbed = true;  // fast on paper, but stopped sending

    const auto Keys = UnchokedKeys(Candidates, Chooser.Decide(Candidates, false, T0));
    REQUIRE(Keys.size() == 3);  // two regular slots plus the optimistic one
    CHECK(Contains(Keys, 2));
    CHECK(Contains(Keys, 1));
    CHECK_FALSE(Contains(Keys, 4));

    const auto Optimistic = Chooser.Optimistic();
    REQUIRE(Optimistic.has_value());
    CHECK((*Optimistic == 3 || *Optimistic == 5));
}

TEST_CASE("Choker ranks by upload rate when seeding and rotates equally fast peers") {
    Choker Chooser({.Slots = 1}, 1);
    std::vector<ChokeCandidate> Candidates = {
      Peer(1, true, 0, 100),
      Peer(2, true, 0, 100),
      Peer(3, true, 0, 10),
    };
    Candidates[0].LastUnchoked = T0 - 1s;  // just had a turn
    Candidates[1].Snubbed      = true;     // irrelevant when seeding

    const auto Unchoke = Chooser.Decide(Candidates, true, T0);
    CHECK(Unchoke[1]);  // equally fast, but has waited longer
    CHECK(std::ranges::count(Unchoke, true) == 2);
}

TEST_CASE("Choker keeps its optimistic peer until the interval or the peer is gone") {
    Choker Chooser({.Slots = 1, .OptimisticInterval = 30s}, 7);
    std::vector<ChokeCandidate> Candidates = {Peer(1, true, 100), Peer(2, true, 0), Peer(3, true, 0), Peer(4, true, 0)};

    Chooser.Decide(Candidates, false, T0);
    const auto First = Chooser.Optimistic();
    REQUIRE(First.has_value());
    CHECK(*First != 1);

    Chooser.Decide(Candidates, false, T0 + 10s);
    CHECK(Chooser.Optimistic() == First);

    // The optimistic peer disconnects: a replacement is chosen straight away
    std::erase_if(Candidates, [&](const ChokeCandidate& Candidate) { return Candidate.Key == *First; });
    Chooser.Decide(Candidates, false, T0 + 20s);
    REQUIRE(Chooser.Optimistic().has_value());
    CHECK(*Chooser.Optimistic() != *First);
    CHECK(*Chooser.Optimistic() != 1);
}

TEST_CASE("Choker favours newly connected peers for the optimistic slot") {
    int NewPicked    = 0;
    constexpr int Runs = 2000;
    for (int Seed = 0; Seed < Runs; ++Seed) {
        Choker Chooser({.Slots = 1}, static_cast<std::uint32_t>(Seed));
        std::vector<ChokeCandidate> Candidates = {Peer(1, true, 100), Peer(2, true, 0), Peer(3, true, 0)};
        Candidates[2].Connected = T0 - 10s;  // new
        Chooser.Decide(Candidates, false, T0);
        if (Chooser.Optimistic() == 3u) { ++NewPicked; }
    }
    // Weighted 3:1, so about 75%
    const double Share = static_cast<double>(NewPicked) / Runs;
    CHECK(Share > 0.68);
    CHECK(Share < 0.82);
}

TEST_CASE("Choker unchokes nobody when nobody is interested") {
    Choker Chooser;
    const std::vector<ChokeCandidate> Candidates = {Peer(1, false, 100), Peer(2, false, 50)};
    CHECK(std::ranges::count(Chooser.Decide(Candidates, false, T0), true) == 0);
    CHECK_FALSE(Chooser.Optimistic().has_value());
}
