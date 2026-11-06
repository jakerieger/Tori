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

#include "Peer/Messages.hpp"

#include <algorithm>
#include <format>
#include <string_view>

namespace Tori {
    namespace {
        constexpr std::string_view kProtocol = "BitTorrent protocol";

        void Expect(const bool Condition, const MessageId Id) {
            if (!Condition) {
                throw ParseError(std::format("peer sent a malformed message (id {})", static_cast<int>(Id)));
            }
        }

        void EncodeBlockMessage(ByteWriter& Out, const MessageId Id, const BlockRequest& Block) {
            Out.WriteU32(13);
            Out.WriteU8(static_cast<std::uint8_t>(Id));
            Out.WriteU32(static_cast<std::uint32_t>(Block.Piece));
            Out.WriteU32(static_cast<std::uint32_t>(Block.Offset));
            Out.WriteU32(static_cast<std::uint32_t>(Block.Length));
        }
    }  // namespace

    std::vector<std::uint8_t> EncodeHandshake(const Handshake& Value) {
        ByteWriter Out;
        Out.WriteU8(static_cast<std::uint8_t>(kProtocol.size()));
        Out.WriteBytes(kProtocol);
        Out.WriteBytes(Value.Reserved);
        Out.WriteBytes(Value.Hash.Bytes);
        Out.WriteBytes(Value.Id);
        return Out.Take();
    }

    std::optional<Handshake> DecodeHandshake(const std::span<const std::uint8_t> Data) {
        if (Data.size() != kHandshakeSize || Data[0] != kProtocol.size()) { return std::nullopt; }
        if (!std::ranges::equal(Data.subspan(1, kProtocol.size()), kProtocol, {}, {}, [](const char C) {
                return static_cast<std::uint8_t>(C);
            })) {
            return std::nullopt;
        }

        Handshake Value;
        ByteReader Reader(Data.subspan(1 + kProtocol.size()));
        std::ranges::copy(Reader.ReadBytes(8), Value.Reserved.begin());
        std::ranges::copy(Reader.ReadBytes(20), Value.Hash.Bytes.begin());
        std::ranges::copy(Reader.ReadBytes(20), Value.Id.begin());
        return Value;
    }

    void EncodeKeepAlive(ByteWriter& Out) {
        Out.WriteU32(0);
    }

    void EncodeSimple(ByteWriter& Out, const MessageId Id) {
        Out.WriteU32(1);
        Out.WriteU8(static_cast<std::uint8_t>(Id));
    }

    void EncodeHave(ByteWriter& Out, const std::uint32_t Piece) {
        Out.WriteU32(5);
        Out.WriteU8(static_cast<std::uint8_t>(MessageId::Have));
        Out.WriteU32(Piece);
    }

    void EncodeBitfield(ByteWriter& Out, const std::span<const std::uint8_t> Bits) {
        Out.WriteU32(static_cast<std::uint32_t>(1 + Bits.size()));
        Out.WriteU8(static_cast<std::uint8_t>(MessageId::Bitfield));
        Out.WriteBytes(Bits);
    }

    void EncodeRequest(ByteWriter& Out, const BlockRequest& Block) {
        EncodeBlockMessage(Out, MessageId::Request, Block);
    }

    void EncodeCancel(ByteWriter& Out, const BlockRequest& Block) {
        EncodeBlockMessage(Out, MessageId::Cancel, Block);
    }

    void EncodePiece(ByteWriter& Out,
                     const std::uint32_t Piece,
                     const std::uint32_t Begin,
                     const std::span<const std::uint8_t> Data) {
        Out.WriteU32(static_cast<std::uint32_t>(9 + Data.size()));
        Out.WriteU8(static_cast<std::uint8_t>(MessageId::Piece));
        Out.WriteU32(Piece);
        Out.WriteU32(Begin);
        Out.WriteBytes(Data);
    }

    void EncodeExtended(ByteWriter& Out, const std::uint8_t ExtendedId, const std::span<const std::uint8_t> Payload) {
        Out.WriteU32(static_cast<std::uint32_t>(2 + Payload.size()));
        Out.WriteU8(static_cast<std::uint8_t>(MessageId::Extended));
        Out.WriteU8(ExtendedId);
        Out.WriteBytes(Payload);
    }

    void EncodePort(ByteWriter& Out, const std::uint16_t DhtPort) {
        Out.WriteU32(3);
        Out.WriteU8(static_cast<std::uint8_t>(MessageId::Port));
        Out.WriteU16(DhtPort);
    }

    void MessageParser::Feed(const std::span<const std::uint8_t> Data) {
        if (_Start > 0 && _Start * 2 >= _Buffer.size()) {
            _Buffer.erase(_Buffer.begin(), _Buffer.begin() + static_cast<std::ptrdiff_t>(_Start));
            _Start = 0;
        }
        _Buffer.insert(_Buffer.end(), Data.begin(), Data.end());
    }

    std::optional<PeerMessage> MessageParser::Next() {
        while (true) {
            if (Buffered() < 4) { return std::nullopt; }

            const std::uint32_t Length = ByteReader(std::span(_Buffer).subspan(_Start, 4)).ReadU32();
            if (Length > kMaxMessageSize) { throw ParseError("peer sent an oversized message"); }
            if (Buffered() < 4 + static_cast<std::size_t>(Length)) { return std::nullopt; }

            const std::span<const std::uint8_t> Body = std::span(_Buffer).subspan(_Start + 4, Length);
            _Start += 4 + Length;

            PeerMessage Message;
            if (Length == 0) {
                Message.IsKeepAlive = true;
                return Message;
            }

            Message.Id = static_cast<MessageId>(Body[0]);
            ByteReader Reader(Body.subspan(1));

            switch (Message.Id) {
                case MessageId::Choke:
                case MessageId::Unchoke:
                case MessageId::Interested:
                case MessageId::NotInterested:
                    Expect(Length == 1, Message.Id);
                    return Message;
                case MessageId::Have:
                    Expect(Length == 5, Message.Id);
                    Message.Index = Reader.ReadU32();
                    return Message;
                case MessageId::Bitfield: {
                    const auto Bits = Reader.ReadRest();
                    Message.Payload.assign(Bits.begin(), Bits.end());
                    return Message;
                }
                case MessageId::Request:
                case MessageId::Cancel:
                    Expect(Length == 13, Message.Id);
                    Message.Index  = Reader.ReadU32();
                    Message.Begin  = Reader.ReadU32();
                    Message.Length = Reader.ReadU32();
                    return Message;
                case MessageId::Piece: {
                    Expect(Length >= 9, Message.Id);
                    Message.Index   = Reader.ReadU32();
                    Message.Begin   = Reader.ReadU32();
                    const auto Data = Reader.ReadRest();
                    Message.Payload.assign(Data.begin(), Data.end());
                    return Message;
                }
                case MessageId::Port:
                    Expect(Length == 3, Message.Id);
                    Message.Port = Reader.ReadU16();
                    return Message;
                case MessageId::Extended: {
                    Expect(Length >= 2, Message.Id);
                    Message.ExtendedId = Reader.ReadU8();
                    const auto Data    = Reader.ReadRest();
                    Message.Payload.assign(Data.begin(), Data.end());
                    return Message;
                }
            }
            // Unknown extension messages (e.g. BEP 6 fast extension) are ignored, since we never advertise them
        }
    }
}  // namespace Tori
