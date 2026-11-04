#include "Dht/DhtNode.hpp"
#include "Dht/Krpc.hpp"
#include "Dht/RoutingTable.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

using namespace Tori;
using namespace std::chrono_literals;

namespace {
    NodeId IdWithPrefix(const std::uint8_t First, const std::uint8_t Last = 0) {
        NodeId Id {};
        Id[0]  = First;
        Id[19] = Last;
        return Id;
    }

    Endpoint Local(const std::uint16_t Port) {
        return *Endpoint::FromAddress("127.0.0.1", Port);
    }

    std::span<const std::uint8_t> Bytes(const std::string& Text) {
        return {reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size()};
    }
}  // namespace

TEST_CASE("XOR distance helpers") {
    const NodeId Target = IdWithPrefix(0x00);
    CHECK(CloserTo(Target, IdWithPrefix(0x01), IdWithPrefix(0x80)));
    CHECK_FALSE(CloserTo(Target, IdWithPrefix(0x80), IdWithPrefix(0x01)));
    CHECK_FALSE(CloserTo(Target, IdWithPrefix(0x01), IdWithPrefix(0x01)));

    CHECK(CommonPrefixBits(IdWithPrefix(0x00), IdWithPrefix(0x80)) == 0);
    CHECK(CommonPrefixBits(IdWithPrefix(0x00), IdWithPrefix(0x01)) == 7);
    CHECK(CommonPrefixBits(IdWithPrefix(0x00), IdWithPrefix(0x00, 1)) == 159);
    CHECK(CommonPrefixBits(Target, Target) == 160);
}

TEST_CASE("Compact node info round-trips") {
    const std::vector<NodeContact> Nodes = {{IdWithPrefix(1), Local(6881)}, {IdWithPrefix(2), Local(51413)}};
    const std::string Encoded            = EncodeCompactNodes(Nodes);
    CHECK(Encoded.size() == 52);

    const std::vector<NodeContact> Decoded = DecodeCompactNodes(Encoded + "xx");  // trailing junk ignored
    REQUIRE(Decoded.size() == 2);
    CHECK(Decoded[1].Id == Nodes[1].Id);
    CHECK(Decoded[1].Address == Nodes[1].Address);
}

TEST_CASE("KRPC messages decode") {
    const std::string Id(20, 'a');
    const auto Query = DecodeKrpc(Bytes(EncodeKrpcQuery("aa", "ping", {{"id", Id}})));
    REQUIRE(Query.has_value());
    CHECK(Query->Type == KrpcMessage::Kind::Query);
    CHECK(Query->Method == "ping");
    CHECK(Query->Transaction == "aa");
    REQUIRE(Query->SenderId.has_value());

    const auto Response = DecodeKrpc(Bytes(EncodeKrpcResponse("bb", {{"id", Id}, {"token", "t"}})));
    REQUIRE(Response.has_value());
    CHECK(Response->Type == KrpcMessage::Kind::Response);
    CHECK(*Response->Body.FindString("token") == "t");

    const auto Error = DecodeKrpc(Bytes(EncodeKrpcError("cc", 203, "bad token")));
    REQUIRE(Error.has_value());
    CHECK(Error->Type == KrpcMessage::Kind::Error);
    CHECK(Error->ErrorCode == 203);
    CHECK(Error->ErrorMessage == "bad token");

    CHECK_FALSE(DecodeKrpc(Bytes("garbage")).has_value());
    CHECK_FALSE(DecodeKrpc(Bytes("d1:t2:aa1:y1:qe")).has_value());  // query without method or arguments
}

TEST_CASE("RoutingTable keeps good nodes, replaces failing ones, and finds the closest") {
    const auto Now = RoutingTable::Clock::now();
    RoutingTable Table(IdWithPrefix(0x00));

    // All share no prefix bits with us, so they compete for one bucket
    for (std::uint8_t I = 0; I < 10; ++I) { Table.Seen({IdWithPrefix(0x80, I), Local(static_cast<std::uint16_t>(1000 + I))}, Now); }
    CHECK(Table.Size() == RoutingTable::kBucketSize);

    // A failing node makes room for a newcomer
    Table.Failed(Local(1000));
    Table.Seen({IdWithPrefix(0x80, 99), Local(2000)}, Now);
    CHECK(Table.Size() == RoutingTable::kBucketSize);
    const auto All = Table.All();
    CHECK(std::ranges::any_of(All, [](const NodeContact& Node) { return Node.Address.Port == 2000; }));
    CHECK(std::ranges::none_of(All, [](const NodeContact& Node) { return Node.Address.Port == 1000; }));

    // Nodes that keep failing are dropped
    for (int I = 0; I < RoutingTable::kMaxFailures; ++I) { Table.Failed(Local(1001)); }
    CHECK(Table.Size() == RoutingTable::kBucketSize - 1);

    Table.Seen({IdWithPrefix(0x01), Local(3000)}, Now);
    Table.Seen({IdWithPrefix(0x40), Local(3001)}, Now);
    const auto Closest = Table.Closest(IdWithPrefix(0x00), 2);
    REQUIRE(Closest.size() == 2);
    CHECK(Closest[0].Address.Port == 3000);
    CHECK(Closest[1].Address.Port == 3001);

    Table.Seen({IdWithPrefix(0x00), Local(4000)}, Now);  // our own ID is never stored
    CHECK(std::ranges::none_of(Table.All(), [](const NodeContact& Node) { return Node.Address.Port == 4000; }));
}

TEST_CASE("DHT nodes on loopback find each other's announced peers") {
    const auto LoopbackSettings = [](std::vector<Endpoint> Bootstrap) {
        DhtSettings Settings;
        Settings.LoopbackOnly     = true;
        Settings.UsePublicRouters = false;
        Settings.BootstrapNodes   = std::move(Bootstrap);
        return Settings;
    };

    InfoHash Hash;
    Hash.Bytes.fill(0x42);

    DhtNode Router(LoopbackSettings({}), {});
    DhtNode Announcer(LoopbackSettings({Local(Router.Port())}), {});

    std::mutex Mutex;
    std::vector<Endpoint> Found;
    DhtNode Seeker(LoopbackSettings({Local(Router.Port())}), [&](const InfoHash& Target, std::vector<Endpoint> Peers) {
        if (Target != Hash) { return; }
        std::scoped_lock Lock(Mutex);
        Found.insert(Found.end(), Peers.begin(), Peers.end());
    });

    // Each lookup ends by announcing to the closest nodes; retry until the seeker sees it
    bool Success = false;
    for (int Attempt = 0; Attempt < 40 && !Success; ++Attempt) {
        Announcer.GetPeers(Hash, 6881);
        std::this_thread::sleep_for(100ms);
        Seeker.GetPeers(Hash, 0);
        std::this_thread::sleep_for(150ms);
        std::scoped_lock Lock(Mutex);
        Success = std::ranges::any_of(Found, [](const Endpoint& Peer) { return Peer == Local(6881); });
    }
    CHECK(Success);
    CHECK(Router.NodeCount() >= 2);
    CHECK(Seeker.NodeCount() >= 1);
}
