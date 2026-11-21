#include "saige/gfx_assets/json.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace saige::gfx {

namespace {
const JsonValue null_value;
const std::string empty_string;
} // namespace

JsonValue JsonValue::make_bool(bool v)
{
    JsonValue j;
    j.type_ = Type::Bool;
    j.bool_ = v;
    return j;
}
JsonValue JsonValue::make_number(double v)
{
    JsonValue j;
    j.type_ = Type::Number;
    j.number_ = v;
    return j;
}
JsonValue JsonValue::make_string(std::string v)
{
    JsonValue j;
    j.type_ = Type::String;
    j.string_ = std::move(v);
    return j;
}
JsonValue JsonValue::make_array()
{
    JsonValue j;
    j.type_ = Type::Array;
    return j;
}
JsonValue JsonValue::make_object()
{
    JsonValue j;
    j.type_ = Type::Object;
    return j;
}

const std::string& JsonValue::as_string() const noexcept
{
    return is_string() ? string_ : empty_string;
}

int64_t JsonValue::as_index(int64_t fallback) const noexcept
{
    if (!is_number() || !std::isfinite(number_) || number_ < 0.0 || number_ > 2147483647.0 ||
        number_ != std::floor(number_)) {
        return fallback;
    }
    return static_cast<int64_t>(number_);
}

const JsonValue& JsonValue::at(std::size_t i) const noexcept
{
    return (is_array() || is_object()) && i < items_.size() ? items_[i] : null_value;
}

const std::string& JsonValue::key_at(std::size_t i) const noexcept
{
    return is_object() && i < keys_.size() ? keys_[i] : empty_string;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept
{
    if (!is_object()) {
        return nullptr;
    }
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            return &items_[i];
        }
    }
    return nullptr;
}

const JsonValue& JsonValue::operator[](std::string_view key) const noexcept
{
    const JsonValue* v = find(key);
    return v != nullptr ? *v : null_value;
}

void JsonValue::push_back(JsonValue v)
{
    items_.push_back(std::move(v));
}

void JsonValue::set(std::string key, JsonValue v)
{
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(v);  // duplicate key: the last one wins
            return;
        }
    }
    keys_.push_back(std::move(key));
    items_.push_back(std::move(v));
}

namespace {

class Parser {
public:
    Parser(std::string_view text, std::size_t max_depth) : text_(text), max_depth_(max_depth) {}

    JsonParseResult run()
    {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
        skip_ws();
        JsonValue value;
        if (!parse_value(value, 0)) {
            return {std::nullopt, error_};
        }
        skip_ws();
        if (pos_ != text_.size()) {
            fail("unexpected content after the JSON document");
            return {std::nullopt, error_};
        }
        return {std::move(value), {}};
    }

private:
    bool fail(const std::string& message)
    {
        if (error_.empty()) {
            std::size_t line = 1;
            std::size_t column = 1;
            for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
                if (text_[i] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            error_ = "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
        }
        return false;
    }

    void skip_ws()
    {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool consume_literal(std::string_view literal)
    {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return fail("invalid literal (expected '" + std::string(literal) + "')");
    }

    bool parse_value(JsonValue& out, std::size_t depth)
    {
        if (pos_ >= text_.size()) {
            return fail("unexpected end of input");
        }
        if (depth > max_depth_) {
            return fail("nesting is deeper than the limit of " + std::to_string(max_depth_));
        }
        const char c = text_[pos_];
        switch (c) {
        case '{': return parse_object(out, depth);
        case '[': return parse_array(out, depth);
        case '"': {
            std::string s;
            if (!parse_string(s)) {
                return false;
            }
            out = JsonValue::make_string(std::move(s));
            return true;
        }
        case 't':
            if (!consume_literal("true")) {
                return false;
            }
            out = JsonValue::make_bool(true);
            return true;
        case 'f':
            if (!consume_literal("false")) {
                return false;
            }
            out = JsonValue::make_bool(false);
            return true;
        case 'n':
            if (!consume_literal("null")) {
                return false;
            }
            out = JsonValue{};
            return true;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parse_number(out);
            }
            return fail(std::string("unexpected character '") + c + "'");
        }
    }

    bool parse_number(JsonValue& out)
    {
        const std::size_t start = pos_;
        if (text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size()) {
            return fail("a number needs digits");
        }
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                return fail("numbers must not have leading zeros");
            }
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        } else {
            return fail("a number needs digits");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                return fail("a fraction needs digits after the '.'");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                return fail("an exponent needs digits");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        double value = 0.0;
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec == std::errc::result_out_of_range) {
            // Overflow (a positive exponent) is an error; underflow (a negative exponent) is harmlessly zero.
            const std::string_view token(first, static_cast<std::size_t>(last - first));
            const std::size_t e = token.find_first_of("eE");
            const bool underflow = e != std::string_view::npos && e + 1 < token.size() && token[e + 1] == '-';
            value = underflow ? 0.0 : std::numeric_limits<double>::infinity();
        } else if (ec != std::errc{} || ptr != last) {
            return fail("invalid number");
        }
        if (!std::isfinite(value)) {
            return fail("number is out of range");
        }
        out = JsonValue::make_number(value);
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(uint32_t& out)
    {
        if (pos_ + 4 > text_.size()) {
            return fail("incomplete \\u escape");
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_ + static_cast<std::size_t>(i)];
            v <<= 4;
            if (h >= '0' && h <= '9') {
                v |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                v |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                v |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                return fail("invalid hex digit in \\u escape");
            }
        }
        pos_ += 4;
        out = v;
        return true;
    }

    bool parse_string(std::string& out)
    {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= text_.size()) {
                return fail("unterminated string");
            }
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control characters must be escaped inside strings");
            }
            if (c != '\\') {
                out.push_back(c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) {
                return fail("unterminated escape sequence");
            }
            const char e = text_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: must pair with a low surrogate
                    if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                        return fail("a high surrogate must be followed by a low surrogate");
                    }
                    pos_ += 2;
                    uint32_t low = 0;
                    if (!parse_hex4(low)) {
                        return false;
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return fail("a high surrogate must be followed by a low surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired low surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default: return fail(std::string("invalid escape '\\") + e + "'");
            }
        }
    }

    bool parse_array(JsonValue& out, std::size_t depth)
    {
        ++pos_;
        out = JsonValue::make_array();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            JsonValue element;
            if (!parse_value(element, depth + 1)) {
                return false;
            }
            out.push_back(std::move(element));
            skip_ws();
            if (pos_ >= text_.size()) {
                return fail("unterminated array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']' in an array");
        }
    }

    bool parse_object(JsonValue& out, std::size_t depth)
    {
        ++pos_;
        out = JsonValue::make_object();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("expected a string key in an object");
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("expected ':' after an object key");
            }
            ++pos_;
            skip_ws();
            JsonValue value;
            if (!parse_value(value, depth + 1)) {
                return false;
            }
            out.set(std::move(key), std::move(value));
            skip_ws();
            if (pos_ >= text_.size()) {
                return fail("unterminated object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}' in an object");
        }
    }

    std::string_view text_;
    std::size_t max_depth_;
    std::size_t pos_ = 0;
    std::string error_;
};

} // namespace

JsonParseResult parse_json(std::string_view text, std::size_t max_depth)
{
    return Parser(text, max_depth).run();
}

} // namespace saige::gfx
