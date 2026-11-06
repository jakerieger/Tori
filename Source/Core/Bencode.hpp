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

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Tori::Bencode {
    class Value;

    using Integer = std::int64_t;
    using List    = std::vector<Value>;
    /// @brief Dictionary entries in the order they were decoded or added. Encode() sorts keys as BEP 3 requires.
    using Dict = std::vector<std::pair<std::string, Value>>;

    namespace Detail {
        class Decoder;
    }

    /// @brief A decoded bencode value: integer, byte string, list or dictionary.
    class Value {
    public:
        Value() : _Data(Integer {0}) {}

        template<std::integral T>
        Value(T Number) : _Data(static_cast<Integer>(Number)) {}

        Value(std::string Bytes) : _Data(std::move(Bytes)) {}
        Value(std::string_view Bytes) : _Data(std::string(Bytes)) {}
        Value(const char* Bytes) : _Data(std::string(Bytes)) {}
        Value(List Items) : _Data(std::move(Items)) {}
        Value(Dict Entries) : _Data(std::move(Entries)) {}

        [[nodiscard]] bool IsInteger() const { return std::holds_alternative<Integer>(_Data); }
        [[nodiscard]] bool IsString() const { return std::holds_alternative<std::string>(_Data); }
        [[nodiscard]] bool IsList() const { return std::holds_alternative<List>(_Data); }
        [[nodiscard]] bool IsDict() const { return std::holds_alternative<Dict>(_Data); }

        /// @brief Typed access. Throws ParseError when the value holds a different type.
        [[nodiscard]] Integer AsInteger() const;
        [[nodiscard]] const std::string& AsString() const;
        [[nodiscard]] const List& AsList() const;
        [[nodiscard]] const Dict& AsDict() const;

        /// @brief Dictionary lookup. Returns nullptr when this isn't a dictionary or the key is missing.
        [[nodiscard]] const Value* Find(std::string_view Key) const;

        /// @brief Dictionary lookups that also return nullptr when the entry holds a different type.
        [[nodiscard]] const Integer* FindInteger(std::string_view Key) const;
        [[nodiscard]] const std::string* FindString(std::string_view Key) const;
        [[nodiscard]] const List* FindList(std::string_view Key) const;
        [[nodiscard]] const Value* FindDict(std::string_view Key) const;

        /// @brief Where this value sat in the input it was decoded from. Both are 0 for values built in code.
        /// Used to hash the exact bytes of a torrent's info dictionary.
        [[nodiscard]] std::size_t SourceOffset() const { return _SourceOffset; }
        [[nodiscard]] std::size_t SourceLength() const { return _SourceLength; }

    private:
        friend class Detail::Decoder;

        std::variant<Integer, std::string, List, Dict> _Data;
        std::size_t _SourceOffset = 0;
        std::size_t _SourceLength = 0;
    };

    /// @brief Maximum nesting of lists and dictionaries the decoder accepts. Guards against stack exhaustion
    /// from hostile input.
    inline constexpr int kMaxDepth = 64;

    /// @brief Decodes exactly one value spanning all of Input. Throws ParseError on malformed or trailing data.
    Value Decode(std::string_view Input);

    /// @brief Decodes one value from the start of Input, allowing trailing bytes. Consumed receives the number of
    /// bytes the value occupied (ut_metadata messages append raw data after a dictionary).
    Value DecodePrefix(std::string_view Input, std::size_t& Consumed);

    /// @brief Encodes a value, sorting dictionary keys as raw byte strings.
    std::string Encode(const Value& Root);
}  // namespace Tori::Bencode
