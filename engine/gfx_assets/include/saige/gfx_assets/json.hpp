// SAIGE gfx_assets — a small, strict JSON (RFC 8259) parser for untrusted input such as glTF files.
//
// No dependencies. Numbers are doubles; depth is limited (default 64) so hostile nesting cannot exhaust the stack;
// malformed input yields an error with line and column, never a crash. Duplicate object keys: the last one wins.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace saige::gfx {

class JsonValue {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    static JsonValue make_bool(bool v);
    static JsonValue make_number(double v);
    static JsonValue make_string(std::string v);
    static JsonValue make_array();
    static JsonValue make_object();

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

    /// Typed accessors with fallbacks: a value of the wrong type returns the fallback instead of throwing.
    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept { return is_bool() ? bool_ : fallback; }
    [[nodiscard]] double as_number(double fallback = 0.0) const noexcept { return is_number() ? number_ : fallback; }
    [[nodiscard]] const std::string& as_string() const noexcept;
    /// A non-negative integral number that fits in int32; otherwise `fallback` (default -1 = "not an index").
    [[nodiscard]] int64_t as_index(int64_t fallback = -1) const noexcept;

    /// Number of elements of an array, or members of an object (0 otherwise).
    [[nodiscard]] std::size_t size() const noexcept { return is_array() || is_object() ? items_.size() : 0; }
    /// Array element (or object member value) by position; a null value if out of range.
    [[nodiscard]] const JsonValue& at(std::size_t i) const noexcept;
    [[nodiscard]] const std::string& key_at(std::size_t i) const noexcept;
    /// Object member by key; nullptr if absent or not an object.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    /// Object member by key; a null value if absent (so chains like `v["a"]["b"]` never fail).
    [[nodiscard]] const JsonValue& operator[](std::string_view key) const noexcept;

    // Construction (used by the parser and tests).
    void push_back(JsonValue v);
    void set(std::string key, JsonValue v);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> items_;  // array elements, or object member values
    std::vector<std::string> keys_; // object member names, parallel to items_
};

struct JsonParseResult {
    std::optional<JsonValue> value;
    std::string error;  ///< "line L, column C: message" when value is empty
    [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

/// Parses a complete JSON document. A leading UTF-8 byte-order mark is skipped; trailing non-whitespace is an error.
[[nodiscard]] JsonParseResult parse_json(std::string_view text, std::size_t max_depth = 64);

} // namespace saige::gfx
