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

#include "Core/Bencode.hpp"

#include "Core/Error.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <unordered_set>

namespace Tori::Bencode {
    namespace Detail {
        class Decoder {
        public:
            explicit Decoder(std::string_view Input) : _Input(Input) {}

            Value Parse(const int Depth) {
                if (Depth > kMaxDepth) { Fail("nesting too deep"); }

                const std::size_t Start = _Pos;
                const char Lead         = Peek();
                Value Result;

                if (Lead == 'i') {
                    Result = Value(ParseInteger());
                } else if (Lead == 'l') {
                    ++_Pos;
                    List Items;
                    while (Peek() != 'e') {
                        Items.push_back(Parse(Depth + 1));
                    }
                    ++_Pos;
                    Result = Value(std::move(Items));
                } else if (Lead == 'd') {
                    ++_Pos;
                    Dict Entries;
                    std::unordered_set<std::string_view> Seen;
                    while (Peek() != 'e') {
                        if (!IsDigit(Peek())) { Fail("dictionary key is not a string"); }
                        const std::string_view Key = ParseString();
                        if (!Seen.insert(Key).second) { Fail("duplicate dictionary key"); }
                        Value Item = Parse(Depth + 1);
                        Entries.emplace_back(std::string(Key), std::move(Item));
                    }
                    ++_Pos;
                    Result = Value(std::move(Entries));
                } else if (IsDigit(Lead)) {
                    Result = Value(ParseString());
                } else {
                    Fail("unexpected character");
                }

                Result._SourceOffset = Start;
                Result._SourceLength = _Pos - Start;
                return Result;
            }

            [[nodiscard]] std::size_t Position() const { return _Pos; }

        private:
            std::string_view _Input;
            std::size_t _Pos = 0;

            static bool IsDigit(const char C) { return C >= '0' && C <= '9'; }

            [[nodiscard]] char Peek() const {
                if (_Pos >= _Input.size()) { Fail("unexpected end of input"); }
                return _Input[_Pos];
            }

            Integer ParseInteger() {
                ++_Pos;  // 'i'
                const std::size_t End = _Input.find('e', _Pos);
                if (End == std::string_view::npos) { Fail("unterminated integer"); }

                const std::string_view Digits    = _Input.substr(_Pos, End - _Pos);
                const bool Negative              = !Digits.empty() && Digits.front() == '-';
                const std::string_view Magnitude = Negative ? Digits.substr(1) : Digits;
                if (Magnitude.empty() || !std::ranges::all_of(Magnitude, IsDigit)) { Fail("malformed integer"); }
                if (Magnitude.size() > 1 && Magnitude.front() == '0') { Fail("integer has leading zeros"); }
                if (Negative && Magnitude == "0") { Fail("negative zero"); }

                Integer Number       = 0;
                const auto [Ptr, Ec] = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Number);
                if (Ec != std::errc {}) { Fail("integer out of range"); }

                _Pos = End + 1;
                return Number;
            }

            std::string_view ParseString() {
                const std::size_t Colon = _Input.find(':', _Pos);
                if (Colon == std::string_view::npos) { Fail("unterminated string length"); }

                const std::string_view Digits = _Input.substr(_Pos, Colon - _Pos);
                if (Digits.empty() || !std::ranges::all_of(Digits, IsDigit)) { Fail("malformed string length"); }
                if (Digits.size() > 1 && Digits.front() == '0') { Fail("string length has leading zeros"); }

                std::uint64_t Length = 0;
                const auto [Ptr, Ec] = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Length);
                if (Ec != std::errc {}) { Fail("string length out of range"); }

                const std::size_t DataStart = Colon + 1;
                if (Length > _Input.size() - DataStart) { Fail("string extends past end of input"); }

                _Pos = DataStart + static_cast<std::size_t>(Length);
                return _Input.substr(DataStart, static_cast<std::size_t>(Length));
            }

            [[noreturn]] void Fail(const std::string_view Reason) const {
                throw ParseError(std::format("bencode: {} at offset {}", Reason, _Pos));
            }
        };
    }  // namespace Detail

    namespace {
        void EncodeTo(const Value& Item, std::string& Out) {
            if (Item.IsInteger()) {
                Out += std::format("i{}e", Item.AsInteger());
            } else if (Item.IsString()) {
                const std::string& Bytes = Item.AsString();
                Out += std::format("{}:", Bytes.size());
                Out += Bytes;
            } else if (Item.IsList()) {
                Out += 'l';
                for (const Value& Child : Item.AsList()) {
                    EncodeTo(Child, Out);
                }
                Out += 'e';
            } else {
                std::vector<const Dict::value_type*> Sorted;
                for (const auto& Entry : Item.AsDict()) {
                    Sorted.push_back(&Entry);
                }
                // std::string compares as unsigned bytes, which is the ordering BEP 3 specifies
                std::ranges::sort(Sorted, {}, [](const Dict::value_type* Entry) -> const std::string& {
                    return Entry->first;
                });

                Out += 'd';
                for (const Dict::value_type* Entry : Sorted) {
                    Out += std::format("{}:", Entry->first.size());
                    Out += Entry->first;
                    EncodeTo(Entry->second, Out);
                }
                Out += 'e';
            }
        }
    }  // namespace

    Integer Value::AsInteger() const {
        if (const auto* Number = std::get_if<Integer>(&_Data)) { return *Number; }
        throw ParseError("bencode: expected an integer");
    }

    const std::string& Value::AsString() const {
        if (const auto* Bytes = std::get_if<std::string>(&_Data)) { return *Bytes; }
        throw ParseError("bencode: expected a string");
    }

    const List& Value::AsList() const {
        if (const auto* Items = std::get_if<List>(&_Data)) { return *Items; }
        throw ParseError("bencode: expected a list");
    }

    const Dict& Value::AsDict() const {
        if (const auto* Entries = std::get_if<Dict>(&_Data)) { return *Entries; }
        throw ParseError("bencode: expected a dictionary");
    }

    const Value* Value::Find(const std::string_view Key) const {
        const auto* Entries = std::get_if<Dict>(&_Data);
        if (!Entries) { return nullptr; }
        for (const auto& [EntryKey, EntryValue] : *Entries) {
            if (EntryKey == Key) { return &EntryValue; }
        }
        return nullptr;
    }

    const Integer* Value::FindInteger(const std::string_view Key) const {
        const Value* Item = Find(Key);
        return Item ? std::get_if<Integer>(&Item->_Data) : nullptr;
    }

    const std::string* Value::FindString(const std::string_view Key) const {
        const Value* Item = Find(Key);
        return Item ? std::get_if<std::string>(&Item->_Data) : nullptr;
    }

    const List* Value::FindList(const std::string_view Key) const {
        const Value* Item = Find(Key);
        return Item ? std::get_if<List>(&Item->_Data) : nullptr;
    }

    const Value* Value::FindDict(const std::string_view Key) const {
        const Value* Item = Find(Key);
        return Item && Item->IsDict() ? Item : nullptr;
    }

    Value Decode(const std::string_view Input) {
        Detail::Decoder Parser(Input);
        Value Result = Parser.Parse(0);
        if (Parser.Position() != Input.size()) {
            throw ParseError(std::format("bencode: trailing data at offset {}", Parser.Position()));
        }
        return Result;
    }

    Value DecodePrefix(const std::string_view Input, std::size_t& Consumed) {
        Detail::Decoder Parser(Input);
        Value Result = Parser.Parse(0);
        Consumed     = Parser.Position();
        return Result;
    }

    std::string Encode(const Value& Root) {
        std::string Out;
        EncodeTo(Root, Out);
        return Out;
    }
}  // namespace Tori::Bencode
