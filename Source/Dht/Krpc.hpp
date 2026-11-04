#pragma once

#include "Core/Bencode.hpp"
#include "Core/Sha1.hpp"
#include "Net/Endpoint.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Tori {
    /// @brief DHT node IDs share the info hash's 160-bit space.
    using NodeId = Sha1Digest;

    /// @brief True when A is closer to Target than B by XOR distance.
    bool CloserTo(const NodeId& Target, const NodeId& A, const NodeId& B);

    /// @brief Leading bits A and B have in common, 0..160.
    int CommonPrefixBits(const NodeId& A, const NodeId& B);

    struct NodeContact {
        NodeId Id {};
        Endpoint Address;
    };

    /// @brief BEP 5 compact node info: 26 bytes per IPv4 node.
    std::string EncodeCompactNodes(std::span<const NodeContact> Nodes);
    std::vector<NodeContact> DecodeCompactNodes(std::string_view Data);

    /// @brief One KRPC message (BEP 5): a query, a response, or an error.
    struct KrpcMessage {
        enum class Kind {
            Query,
            Response,
            Error,
        };

        Kind Type = Kind::Query;
        std::string Transaction;
        std::string Method;     // Queries only
        Bencode::Value Body;    // "a" for queries, "r" for responses
        std::optional<NodeId> SenderId;
        int ErrorCode = 0;
        std::string ErrorMessage;
    };

    /// @brief Returns nullopt for anything that isn't a well-formed KRPC message.
    std::optional<KrpcMessage> DecodeKrpc(std::span<const std::uint8_t> Packet);

    std::string EncodeKrpcQuery(std::string_view Transaction, std::string_view Method, Bencode::Dict Arguments);
    std::string EncodeKrpcResponse(std::string_view Transaction, Bencode::Dict Values);
    std::string EncodeKrpcError(std::string_view Transaction, int Code, std::string_view Message);
}  // namespace Tori
