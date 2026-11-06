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

#include "Dht/Krpc.hpp"

#include <array>
#include <chrono>
#include <vector>

namespace Tori {
    /// @brief Kademlia routing table (BEP 5): nodes grouped by how many leading bits their ID shares with ours,
    /// at most kBucketSize per group, so we know many nodes near us and progressively fewer far away.
    class RoutingTable {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr std::size_t kBucketSize = 8;
        static constexpr int kMaxFailures        = 3;

        struct Entry {
            NodeContact Contact;
            Clock::time_point LastSeen;
            int Failures = 0;
        };

        explicit RoutingTable(const NodeId& Self) : _Self(Self) {}

        /// @brief Records that a node answered or queried us. A full bucket only admits it in place of a node
        /// that has stopped responding, so long-lived good nodes are kept (they're the most likely to stay).
        void Seen(const NodeContact& Node, Clock::time_point Now);

        /// @brief A query to this address timed out. Nodes that keep failing are dropped.
        void Failed(const Endpoint& Address);

        [[nodiscard]] std::vector<NodeContact> Closest(const NodeId& Target, std::size_t Count) const;
        [[nodiscard]] std::vector<NodeContact> All() const;
        [[nodiscard]] std::size_t Size() const;

    private:
        NodeId _Self;
        std::array<std::vector<Entry>, 160> _Buckets;
    };
}  // namespace Tori
