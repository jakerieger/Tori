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

#include "Peer/Extensions.hpp"

#include "Core/Bencode.hpp"
#include "Core/Error.hpp"
#include "Core/Sha1.hpp"
#include "Core/Version.hpp"

#include <algorithm>
#include <cstring>

namespace Tori {
    namespace {
        constexpr auto kMetadataRequestTimeout = std::chrono::seconds(10);

        std::string_view AsChars(const std::span<const std::uint8_t> Bytes) {
            return {reinterpret_cast<const char*>(Bytes.data()), Bytes.size()};
        }

        std::vector<std::uint8_t> AsBytes(const std::string& Text) {
            return {Text.begin(), Text.end()};
        }

        std::vector<std::uint8_t>
        EncodeMetadataHeader(const MetadataMessageType Type, const int Piece, const std::int64_t TotalSize = -1) {
            Bencode::Dict Header = {{"msg_type", static_cast<int>(Type)}, {"piece", Piece}};
            if (TotalSize >= 0) { Header.emplace_back("total_size", TotalSize); }
            return AsBytes(Bencode::Encode(Header));
        }
    }  // namespace

    std::vector<std::uint8_t> EncodeExtensionHandshake(const std::optional<std::int64_t> MetadataSize,
                                                       const std::uint16_t ListenPort) {
        Bencode::Dict Handshake = {
          {"m", Bencode::Dict {{"ut_metadata", kLocalUtMetadataId}}},
          {"p", ListenPort},
          {"reqq", 250},
          {"v", kClientName},
        };
        if (MetadataSize) { Handshake.emplace_back("metadata_size", *MetadataSize); }
        return AsBytes(Bencode::Encode(Handshake));
    }

    ExtensionHandshake DecodeExtensionHandshake(const std::span<const std::uint8_t> Payload) {
        std::size_t Consumed      = 0;
        const Bencode::Value Root = Bencode::DecodePrefix(AsChars(Payload), Consumed);
        if (!Root.IsDict()) { throw ParseError("extension handshake is not a dictionary"); }

        ExtensionHandshake Result;
        if (const Bencode::Value* Messages = Root.FindDict("m")) {
            if (const Bencode::Integer* Id = Messages->FindInteger("ut_metadata"); Id && *Id > 0 && *Id < 256) {
                Result.UtMetadataId = static_cast<std::uint8_t>(*Id);
            }
        }
        if (const Bencode::Integer* Size = Root.FindInteger("metadata_size"); Size && *Size > 0) {
            Result.MetadataSize = *Size;
        }
        if (const Bencode::Integer* Port = Root.FindInteger("p"); Port && *Port > 0 && *Port < 65536) {
            Result.ListenPort = static_cast<std::uint16_t>(*Port);
        }
        if (const std::string* Client = Root.FindString("v")) { Result.Client = *Client; }
        return Result;
    }

    std::vector<std::uint8_t> EncodeMetadataRequest(const int Piece) {
        return EncodeMetadataHeader(MetadataMessageType::Request, Piece);
    }

    std::vector<std::uint8_t>
    EncodeMetadataData(const int Piece, const std::int64_t TotalSize, const std::span<const std::uint8_t> Data) {
        std::vector<std::uint8_t> Message = EncodeMetadataHeader(MetadataMessageType::Data, Piece, TotalSize);
        Message.insert(Message.end(), Data.begin(), Data.end());
        return Message;
    }

    std::vector<std::uint8_t> EncodeMetadataReject(const int Piece) {
        return EncodeMetadataHeader(MetadataMessageType::Reject, Piece);
    }

    MetadataMessage DecodeMetadataMessage(const std::span<const std::uint8_t> Payload) {
        std::size_t Consumed          = 0;
        const Bencode::Value Root     = Bencode::DecodePrefix(AsChars(Payload), Consumed);
        const Bencode::Integer* Type  = Root.FindInteger("msg_type");
        const Bencode::Integer* Piece = Root.FindInteger("piece");
        if (!Type || !Piece || *Type < 0 || *Type > 2 || *Piece < 0 || *Piece > INT32_MAX) {
            throw ParseError("malformed ut_metadata message");
        }

        MetadataMessage Message;
        Message.Type  = static_cast<MetadataMessageType>(*Type);
        Message.Piece = static_cast<int>(*Piece);
        if (Message.Type == MetadataMessageType::Data) {
            if (const Bencode::Integer* Total = Root.FindInteger("total_size")) { Message.TotalSize = *Total; }
            Message.Data.assign(Payload.begin() + static_cast<std::ptrdiff_t>(Consumed), Payload.end());
        }
        return Message;
    }

    int MetadataPieceCount(const std::int64_t Size) {
        return static_cast<int>((Size + kMetadataPieceSize - 1) / kMetadataPieceSize);
    }

    bool MetadataAssembler::OfferSize(const std::int64_t Size) {
        if (_Size > 0) { return Size == _Size; }
        if (Size <= 0 || Size > kMaxMetadataSize) { return false; }

        _Size = Size;
        _Buffer.assign(static_cast<std::size_t>(Size), '\0');
        _Have.assign(static_cast<std::size_t>(MetadataPieceCount(Size)), false);
        _RequestedAt.assign(_Have.size(), Clock::time_point {});
        return true;
    }

    std::optional<int> MetadataAssembler::NextRequest(const Clock::time_point Now) {
        for (std::size_t Piece = 0; Piece < _Have.size(); ++Piece) {
            const bool Pending =
              _RequestedAt[Piece] != Clock::time_point {} && Now - _RequestedAt[Piece] < kMetadataRequestTimeout;
            if (!_Have[Piece] && !Pending) {
                _RequestedAt[Piece] = Now;
                return static_cast<int>(Piece);
            }
        }
        return std::nullopt;
    }

    void MetadataAssembler::Received(const int Piece, const std::span<const std::uint8_t> Data) {
        if (Piece < 0 || static_cast<std::size_t>(Piece) >= _Have.size() || _Have[static_cast<std::size_t>(Piece)]) {
            return;
        }

        const std::int64_t Offset   = static_cast<std::int64_t>(Piece) * kMetadataPieceSize;
        const std::int64_t Expected = std::min<std::int64_t>(kMetadataPieceSize, _Size - Offset);
        if (static_cast<std::int64_t>(Data.size()) != Expected) { return; }

        std::memcpy(_Buffer.data() + Offset, Data.data(), Data.size());
        _Have[static_cast<std::size_t>(Piece)] = true;
    }

    void MetadataAssembler::Rejected(const int Piece) {
        if (Piece >= 0 && static_cast<std::size_t>(Piece) < _RequestedAt.size()) {
            _RequestedAt[static_cast<std::size_t>(Piece)] = {};
        }
    }

    std::optional<std::string> MetadataAssembler::TryComplete() {
        if (_Size <= 0 || !std::ranges::all_of(_Have, [](const bool Have) { return Have; })) { return std::nullopt; }
        if (Sha1::Hash(_Buffer) == _Hash.Bytes) { return _Buffer; }

        _Size = 0;
        _Buffer.clear();
        _Have.clear();
        _RequestedAt.clear();
        return std::nullopt;
    }
}  // namespace Tori
