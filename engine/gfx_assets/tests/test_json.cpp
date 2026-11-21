#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/json.hpp"

#include <cmath>
#include <string>

using namespace saige::gfx;

namespace {

JsonValue parse_ok(const std::string& text)
{
    JsonParseResult r = parse_json(text);
    REQUIRE(r.ok());
    return std::move(*r.value);
}

bool fails_with(const std::string& text, const std::string& fragment, std::size_t depth = 64)
{
    const JsonParseResult r = parse_json(text, depth);
    return !r.ok() && r.error.find(fragment) != std::string::npos;
}

} // namespace

TEST_CASE("Json: scalars")
{
    CHECK(parse_ok("null").is_null());
    CHECK(parse_ok("true").as_bool() == true);
    CHECK(parse_ok("false").as_bool(true) == false);
    CHECK(parse_ok("  42 \n").as_number() == 42.0);
    CHECK(parse_ok("-0.5").as_number() == -0.5);
    CHECK(parse_ok("1e3").as_number() == 1000.0);
    CHECK(parse_ok("2.5E-2").as_number() == 0.025);
    CHECK(parse_ok("\"hi\"").as_string() == "hi");
    CHECK(parse_ok("0").as_number() == 0.0);
}

TEST_CASE("Json: structures and lookups never throw on the wrong type")
{
    const JsonValue v = parse_ok(R"({"a": [1, 2, {"b": "x"}], "n": null, "t": true, "e": {}, "ea": []})");
    REQUIRE(v.is_object());
    CHECK(v.size() == 5);
    CHECK(v["a"].size() == 3);
    CHECK(v["a"].at(2)["b"].as_string() == "x");
    CHECK(v["t"].as_bool());
    CHECK(v["e"].is_object());
    CHECK(v["e"].size() == 0);
    CHECK(v["ea"].is_array());
    // Chained lookups on missing members and wrong types are safe and yield null / fallbacks.
    CHECK(v["missing"]["deeper"]["still"].is_null());
    CHECK(v["a"]["not_an_object"].is_null());
    CHECK(v["a"].at(99).is_null());
    CHECK(v["t"].as_number(7.0) == 7.0);
    CHECK(v["n"].as_string().empty());
    CHECK(v.find("missing") == nullptr);
    CHECK(v.key_at(0) == "a");
}

TEST_CASE("Json: string escapes and unicode")
{
    CHECK(parse_ok(R"("a\"b\\c\/d\b\f\n\r\t")").as_string() == "a\"b\\c/d\b\f\n\r\t");
    CHECK(parse_ok(R"("\u0041\u00e9")").as_string() == "A\xC3\xA9");                // A, e-acute (2-byte UTF-8)
    CHECK(parse_ok(R"("\u20AC")").as_string() == "\xE2\x82\xAC");                    // euro sign (3 bytes)
    CHECK(parse_ok(R"("\uD83D\uDE00")").as_string() == "\xF0\x9F\x98\x80");          // surrogate pair -> U+1F600
    CHECK(parse_ok("\"\xC3\xA9\"").as_string() == "\xC3\xA9");                       // raw UTF-8 passes through
    CHECK(parse_ok(R"("\u0000")").as_string() == std::string(1, '\0'));
}

TEST_CASE("Json: a UTF-8 byte order mark is skipped, and duplicate keys keep the last value")
{
    CHECK(parse_ok("\xEF\xBB\xBF{\"a\":1}")["a"].as_number() == 1.0);
    const JsonValue dup = parse_ok(R"({"k": 1, "k": 2, "other": 3})");
    CHECK(dup.size() == 2);
    CHECK(dup["k"].as_number() == 2.0);
}

TEST_CASE("Json: as_index accepts only non-negative integral int32 values")
{
    CHECK(parse_ok("0").as_index() == 0);
    CHECK(parse_ok("17").as_index() == 17);
    CHECK(parse_ok("2147483647").as_index() == 2147483647);
    CHECK(parse_ok("2147483648").as_index() == -1);
    CHECK(parse_ok("-1").as_index() == -1);
    CHECK(parse_ok("1.5").as_index() == -1);
    CHECK(parse_ok("\"3\"").as_index() == -1);
    CHECK(parse_ok("null").as_index(5) == 5);
}

TEST_CASE("Json: malformed documents are rejected with a position")
{
    CHECK(fails_with("", "unexpected end"));
    CHECK(fails_with("   ", "unexpected end"));
    CHECK(fails_with("{", "expected a string key"));
    CHECK(fails_with("[1, 2", "unterminated array"));
    CHECK(fails_with("[1 2]", "expected ',' or ']'"));
    CHECK(fails_with("{\"a\" 1}", "expected ':'"));
    CHECK(fails_with("{\"a\": 1,}", "expected a string key"));
    CHECK(fails_with("[1,]", "unexpected character ']'"));
    CHECK(fails_with("{a: 1}", "expected a string key"));
    CHECK(fails_with("'single'", "unexpected character"));
    CHECK(fails_with("tru", "invalid literal"));
    CHECK(fails_with("nul", "invalid literal"));
    CHECK(fails_with("{} extra", "unexpected content after"));
    CHECK(fails_with("[] []", "unexpected content after"));
    CHECK(fails_with("\"abc", "unterminated string"));
    CHECK(fails_with("\"a\nb\"", "control characters"));
    CHECK(fails_with("\"\\x\"", "invalid escape"));
    CHECK(fails_with("\"\\u12\"", "incomplete \\u escape"));
    CHECK(fails_with("\"\\u12G4\"", "invalid hex digit"));
    CHECK(fails_with("\"\\uD83D\"", "high surrogate"));
    CHECK(fails_with("\"\\uD83D\\u0041\"", "high surrogate"));
    CHECK(fails_with("\"\\uDE00\"", "unpaired low surrogate"));
    // Error messages carry line and column.
    const JsonParseResult r = parse_json("{\n  \"a\": 1,\n  \"b\": ?\n}");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error.find("line 3") != std::string::npos);
}

TEST_CASE("Json: number grammar is strict")
{
    CHECK(fails_with("01", "leading zeros"));
    CHECK(fails_with("-", "a number needs digits"));
    CHECK(fails_with("+1", "unexpected character '+'"));
    CHECK(fails_with("1.", "fraction needs digits"));
    CHECK(fails_with(".5", "unexpected character"));
    CHECK(fails_with("1e", "exponent needs digits"));
    CHECK(fails_with("1e+", "exponent needs digits"));
    CHECK(fails_with("NaN", "unexpected character"));
    CHECK(fails_with("Infinity", "unexpected character"));
    CHECK(fails_with("1e999", "out of range"));
    CHECK(fails_with("-1e999", "out of range"));
    CHECK(parse_ok("1e-999").as_number() == 0.0);  // underflow to zero is harmless
}

TEST_CASE("Json: nesting depth is limited so hostile input cannot exhaust the stack")
{
    std::string deep_array(10000, '[');
    CHECK(fails_with(deep_array, "deeper than the limit"));
    std::string deep_object;
    for (int i = 0; i < 5000; ++i) {
        deep_object += "{\"a\":";
    }
    CHECK(fails_with(deep_object, "deeper than the limit"));

    // Exactly at the limit is fine; one past it is not.
    std::string at_limit;
    for (int i = 0; i < 5; ++i) at_limit += "[";
    at_limit += "1";
    for (int i = 0; i < 5; ++i) at_limit += "]";
    CHECK(parse_json(at_limit, 5).ok());
    CHECK_FALSE(parse_json(at_limit, 4).ok());
}

TEST_CASE("Json: every truncation of a valid document is rejected cleanly (no crash, no hang)")
{
    const std::string doc = R"({"asset": {"version": "2.0"}, "nodes": [{"name": "n\u00e9", "children": [1, 2.5e1, true, null]}], "s": "x"})";
    REQUIRE(parse_json(doc).ok());
    for (std::size_t n = 0; n + 1 < doc.size(); ++n) {
        const JsonParseResult r = parse_json(std::string_view(doc.data(), n));
        CHECK_FALSE(r.ok());  // a proper prefix of this document is never valid JSON
    }
}

TEST_CASE("Json: large flat documents parse")
{
    std::string text = "[";
    for (int i = 0; i < 20000; ++i) {
        text += (i != 0 ? "," : "");
        text += std::to_string(i);
    }
    text += "]";
    const JsonValue v = parse_ok(text);
    CHECK(v.size() == 20000);
    CHECK(v.at(19999).as_number() == 19999.0);
}
