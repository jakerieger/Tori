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

#include "Dht/Krpc.hpp"

#include "Core/Error.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace Tori {
    namespace {
        constexpr std::size_t kCompactNodeSize = 26;

        std::string_view AsChars(const std::span<const std::uint8_t> Bytes) {
            return {reinterpret_cast<const char*>(Bytes.data()), Bytes.size()};
        }

        std::string IdString(const NodeId& Id) {
            return {reinterpret_cast<const char*>(Id.data()), Id.size()};
        }
    }  // namespace

    bool CloserTo(const NodeId& Target, const NodeId& A, const NodeId& B) {
        for (std::size_t I = 0; I < Target.size(); ++I) {
            const auto DistanceA = static_cast<std::uint8_t>(A[I] ^ Target[I]);
            const auto DistanceB = static_cast<std::uint8_t>(B[I] ^ Target[I]);
            if (DistanceA != DistanceB) { return DistanceA < DistanceB; }
        }
        return false;
    }

    int CommonPrefixBits(const NodeId& A, const NodeId& B) {
        for (std::size_t I = 0; I < A.size(); ++I) {
            const auto Difference = static_cast<std::uint8_t>(A[I] ^ B[I]);
            if (Difference != 0) { return static_cast<int>(I * 8) + std::countl_zero(Difference); }
        }
        return 160;
    }

    std::string EncodeCompactNodes(const std::span<const NodeContact> Nodes) {
        std::string Out;
        for (const NodeContact& Node : Nodes) {
            if (Node.Address.IsV6) { continue; }
            Out += IdString(Node.Id);
            Out.append(reinterpret_cast<const char*>(Node.Address.Address.data()), 4);
            Out += static_cast<char>(Node.Address.Port >> 8);
            Out += static_cast<char>(Node.Address.Port & 0xFF);
        }
        return Out;
    }

    std::vector<NodeContact> DecodeCompactNodes(const std::string_view Data) {
        std::vector<NodeContact> Nodes;
        for (std::size_t Offset = 0; Offset + kCompactNodeSize <= Data.size(); Offset += kCompactNodeSize) {
            NodeContact Node;
            std::memcpy(Node.Id.data(), Data.data() + Offset, 20);
            const std::vector<Endpoint> Address = ParseCompactPeers(Data.substr(Offset + 20, 6), false);
            if (Address.empty()) { continue; }
            Node.Address = Address.front();
            Nodes.push_back(Node);
        }
        return Nodes;
    }

    std::optional<KrpcMessage> DecodeKrpc(const std::span<const std::uint8_t> Packet) {
        Bencode::Value Root;
        try {
            Root = Bencode::Decode(AsChars(Packet));
        } catch (const ParseError&) { return std::nullopt; }

        const std::string* Transaction = Root.FindString("t");
        const std::string* Type        = Root.FindString("y");
        if (!Transaction || !Type || Type->size() != 1) { return std::nullopt; }

        KrpcMessage Message;
        Message.Transaction = *Transaction;

        switch ((*Type)[0]) {
            case 'q': {
                const std::string* Method  = Root.FindString("q");
                const Bencode::Value* Args = Root.FindDict("a");
                if (!Method || !Args) { return std::nullopt; }
                Message.Type   = KrpcMessage::Kind::Query;
                Message.Method = *Method;
                Message.Body   = *Args;
                break;
            }
            case 'r': {
                const Bencode::Value* Values = Root.FindDict("r");
                if (!Values) { return std::nullopt; }
                Message.Type = KrpcMessage::Kind::Response;
                Message.Body = *Values;
                break;
            }
            case 'e': {
                Message.Type = KrpcMessage::Kind::Error;
                if (const Bencode::List* Error = Root.FindList("e"); Error && Error->size() >= 2) {
                    if ((*Error)[0].IsInteger()) { Message.ErrorCode = static_cast<int>((*Error)[0].AsInteger()); }
                    if ((*Error)[1].IsString()) { Message.ErrorMessage = (*Error)[1].AsString(); }
                }
                return Message;
            }
            default:
                return std::nullopt;
        }

        if (const std::string* Id = Message.Body.FindString("id"); Id && Id->size() == 20) {
            NodeId Sender {};
            std::memcpy(Sender.data(), Id->data(), 20);
            Message.SenderId = Sender;
        }
        return Message;
    }

    std::string
    EncodeKrpcQuery(const std::string_view Transaction, const std::string_view Method, Bencode::Dict Arguments) {
        return Bencode::Encode(Bencode::Dict {
          {"t", Transaction},
          {"y", "q"},
          {"q", Method},
          {"a", std::move(Arguments)},
        });
    }

    std::string EncodeKrpcResponse(const std::string_view Transaction, Bencode::Dict Values) {
        return Bencode::Encode(Bencode::Dict {
          {"t", Transaction},
          {"y", "r"},
          {"r", std::move(Values)},
        });
    }

    std::string EncodeKrpcError(const std::string_view Transaction, const int Code, const std::string_view Message) {
        return Bencode::Encode(Bencode::Dict {
          {"t", Transaction},
          {"y", "e"},
          {"e", Bencode::List {Code, Message}},
        });
    }
}  // namespace Tori
