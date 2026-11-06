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

#include "Dht/RoutingTable.hpp"

#include <algorithm>

namespace Tori {
    void RoutingTable::Seen(const NodeContact& Node, const Clock::time_point Now) {
        const int Prefix = CommonPrefixBits(_Self, Node.Id);
        if (Prefix >= 160 || Node.Address.Port == 0 || Node.Address.IsV6) { return; }  // ourselves, or unusable

        std::vector<Entry>& Bucket = _Buckets[static_cast<std::size_t>(Prefix)];
        const auto Existing = std::ranges::find_if(Bucket, [&](const Entry& E) { return E.Contact.Id == Node.Id; });
        if (Existing != Bucket.end()) {
            Existing->Contact.Address = Node.Address;
            Existing->LastSeen        = Now;
            Existing->Failures        = 0;
            return;
        }

        if (Bucket.size() < kBucketSize) {
            Bucket.push_back({Node, Now, 0});
            return;
        }

        const auto Worst = std::ranges::max_element(Bucket, {}, &Entry::Failures);
        if (Worst->Failures > 0) { *Worst = {Node, Now, 0}; }
    }

    void RoutingTable::Failed(const Endpoint& Address) {
        for (std::vector<Entry>& Bucket : _Buckets) {
            for (auto It = Bucket.begin(); It != Bucket.end(); ++It) {
                if (It->Contact.Address != Address) { continue; }
                if (++It->Failures >= kMaxFailures) { Bucket.erase(It); }
                return;
            }
        }
    }

    std::vector<NodeContact> RoutingTable::Closest(const NodeId& Target, const std::size_t Count) const {
        std::vector<NodeContact> Nodes = All();
        const std::size_t Keep         = std::min(Count, Nodes.size());
        std::ranges::partial_sort(
          Nodes,
          Nodes.begin() + static_cast<std::ptrdiff_t>(Keep),
          [&](const NodeContact& A, const NodeContact& B) { return CloserTo(Target, A.Id, B.Id); });
        Nodes.resize(Keep);
        return Nodes;
    }

    std::vector<NodeContact> RoutingTable::All() const {
        std::vector<NodeContact> Nodes;
        for (const std::vector<Entry>& Bucket : _Buckets) {
            for (const Entry& Node : Bucket) {
                Nodes.push_back(Node.Contact);
            }
        }
        return Nodes;
    }

    std::size_t RoutingTable::Size() const {
        std::size_t Total = 0;
        for (const std::vector<Entry>& Bucket : _Buckets) {
            Total += Bucket.size();
        }
        return Total;
    }
}  // namespace Tori
