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

#pragma once

#include "Core/InfoHash.hpp"
#include "Dht/Krpc.hpp"
#include "Net/Endpoint.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace Tori {
    struct DhtSettings {
        std::uint16_t Port    = 0;  // UDP port; conventionally the same number as the TCP listen port
        bool LoopbackOnly     = false;
        bool UsePublicRouters = true;          // router.bittorrent.com and friends
        std::vector<Endpoint> BootstrapNodes;  // Extra nodes to start from, e.g. saved from the last run
        std::optional<NodeId> Id;              // Kept between runs, as BEP 5 suggests; random when absent
    };

    /// @brief Receives peers found for an info hash. Called on the DHT thread, possibly several times per lookup.
    using DhtPeersCallback = std::function<void(const InfoHash& Hash, std::vector<Endpoint> Peers)>;

    /// @brief A Mainline DHT node (BEP 5, IPv4) running on its own thread.
    ///
    /// It answers other nodes' queries (ping, find_node, get_peers, announce_peer) and runs iterative lookups for
    /// the torrents that ask it to. All public methods are thread-safe.
    class DhtNode {
    public:
        DhtNode(DhtSettings Settings, DhtPeersCallback OnPeers);
        ~DhtNode();

        DhtNode(const DhtNode&)            = delete;
        DhtNode& operator=(const DhtNode&) = delete;

        /// @brief Finds peers for Hash. If AnnouncePort is non-zero, the closest nodes are then told we're a peer
        /// on that TCP port.
        void GetPeers(const InfoHash& Hash, std::uint16_t AnnouncePort);

        /// @brief A node to contact, such as a peer that sent a PORT message.
        void AddNode(const Endpoint& Address);

        [[nodiscard]] std::uint16_t Port() const;
        [[nodiscard]] const NodeId& Id() const;
        [[nodiscard]] std::size_t NodeCount() const;

        /// @brief Nodes from the routing table, worth saving to bootstrap from next time.
        [[nodiscard]] std::vector<Endpoint> KnownNodes() const;

    private:
        struct Shared;
        class Worker;

        std::shared_ptr<Shared> _Shared;
        std::jthread _Thread;
    };
}  // namespace Tori
