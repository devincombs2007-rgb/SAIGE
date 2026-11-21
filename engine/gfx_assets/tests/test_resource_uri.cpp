#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/resource_uri.hpp"

#include <string>

using namespace saige::gfx;

namespace {

std::string b64(const std::string& raw)
{
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        const uint32_t b0 = static_cast<unsigned char>(raw[i]);
        const uint32_t b1 = i + 1 < raw.size() ? static_cast<unsigned char>(raw[i + 1]) : 0;
        const uint32_t b2 = i + 2 < raw.size() ? static_cast<unsigned char>(raw[i + 2]) : 0;
        const uint32_t t = (b0 << 16) | (b1 << 8) | b2;
        out += a[(t >> 18) & 63];
        out += a[(t >> 12) & 63];
        out += i + 1 < raw.size() ? a[(t >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? a[t & 63] : '=';
    }
    return out;
}

std::string str(const std::vector<uint8_t>& v)
{
    return std::string(v.begin(), v.end());
}

} // namespace

TEST_CASE("Base64: round trips every length and every byte value")
{
    for (std::size_t len = 0; len < 40; ++len) {
        std::string raw;
        for (std::size_t i = 0; i < len; ++i) {
            raw.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
        }
        const auto decoded = decode_base64(b64(raw));
        REQUIRE(decoded.has_value());
        CHECK(str(*decoded) == raw);
    }
    std::string all;
    for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
    CHECK(str(*decode_base64(b64(all))) == all);
    CHECK(str(*decode_base64("TWFu")) == "Man");
    CHECK(str(*decode_base64("TWE=")) == "Ma");
    CHECK(str(*decode_base64("TQ==")) == "M");
    CHECK(decode_base64("")->empty());
}

TEST_CASE("Base64: malformed input is rejected")
{
    CHECK_FALSE(decode_base64("TWF").has_value());        // length not a multiple of 4
    CHECK_FALSE(decode_base64("TWFu\n").has_value());      // whitespace
    CHECK_FALSE(decode_base64("TW Fu").has_value());
    CHECK_FALSE(decode_base64("TWF*").has_value());        // outside the alphabet
    CHECK_FALSE(decode_base64("TWFu-_==").has_value());    // URL-safe alphabet is not accepted
    CHECK_FALSE(decode_base64("=WFu").has_value());        // padding in the first positions
    CHECK_FALSE(decode_base64("T=Fu").has_value());
    CHECK_FALSE(decode_base64("TW=u").has_value());        // data after padding
    CHECK_FALSE(decode_base64("TWFu====").has_value());    // padding outside the last quad
    CHECK_FALSE(decode_base64("TQ==TWFu").has_value());    // padding in a non-final quad
}

TEST_CASE("Data URIs: base64 payloads only, with their MIME type")
{
    const auto png = decode_data_uri("data:image/png;base64,TWFu");
    REQUIRE(png.has_value());
    CHECK(png->mime_type == "image/png");
    CHECK(str(png->bytes) == "Man");
    const auto bin = decode_data_uri("data:application/octet-stream;base64,TQ==");
    REQUIRE(bin.has_value());
    CHECK(bin->mime_type == "application/octet-stream");
    const auto no_mime = decode_data_uri("data:;base64,TWFu");
    REQUIRE(no_mime.has_value());
    CHECK(no_mime->mime_type.empty());
    CHECK(decode_data_uri("DATA:text/plain;base64,TWFu").has_value());  // case-insensitive scheme

    CHECK_FALSE(decode_data_uri("data:text/plain,hello").has_value());       // not base64
    CHECK_FALSE(decode_data_uri("data:text/plain;base64").has_value());      // no payload separator
    CHECK_FALSE(decode_data_uri("data:text/plain;base64,TW*u").has_value()); // bad payload
    CHECK_FALSE(decode_data_uri("image.png").has_value());
    CHECK(is_data_uri("data:x"));
    CHECK_FALSE(is_data_uri("dat:x"));
    CHECK_FALSE(is_data_uri("textures/data:x"));
}

TEST_CASE("Resource URIs: plain relative paths are accepted and normalized")
{
    CHECK(resolve_relative_uri("mesh.bin").path == "mesh.bin");
    CHECK(resolve_relative_uri("textures/albedo.png").path == "textures/albedo.png");
    CHECK(resolve_relative_uri("./textures/./a.png").path == "textures/a.png");
    CHECK(resolve_relative_uri("my%20file.bin").path == "my file.bin");           // percent-decoded once
    CHECK(resolve_relative_uri("a%2Fb.bin").path == "a/b.bin");                    // %2F is a separator after decoding
    CHECK(resolve_relative_uri("caf%C3%A9.png").ok);                                // UTF-8 bytes pass
    CHECK(resolve_relative_uri("a.b.c/d-e_f.bin").ok);
}

TEST_CASE("Resource URIs: everything that could escape the asset directory is rejected")
{
    const char* bad[] = {
        "",                          // empty
        "/etc/passwd",               // absolute
        "//server/share/x.bin",      // network path
        "C:/Windows/x.bin",          // drive letter
        "c:x.bin",                   // drive-relative
        "file:///etc/passwd",        // scheme
        "http://example.com/x.bin",
        "..",                        // parent
        "../secret.bin",
        "a/../../secret.bin",
        "a/..",
        "textures/../../x.png",
        "..\\secret.bin",            // backslash
        "a\\b.bin",
        "a//b.bin",                  // empty segment
        "a/",                        // trailing slash: names no file
        "./",
        ".",
        "%2e%2e/secret.bin",         // encoded '..'
        "%2E%2E%2Fsecret.bin",       // encoded '../'
        "a%2F..%2F..%2Fb.bin",
        "%00evil.bin",               // NUL
        "a%0Ab.bin",                 // newline
        "a%5Cb.bin",                 // encoded backslash
        "%3A",                       // encoded colon
        "%2Fetc%2Fpasswd",           // encoded absolute path
        "bad%zz.bin",                // invalid escape
        "trunc%2",                   // truncated escape
        "trunc%",
    };
    for (const char* uri : bad) {
        const ResolvedUri r = resolve_relative_uri(uri);
        if (r.ok) {
            std::printf("  wrongly accepted: '%s' -> '%s'\n", uri, r.path.c_str());
        }
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    CHECK_FALSE(resolve_relative_uri(std::string(1025, 'a')).ok);   // over-long
    CHECK(resolve_relative_uri(std::string(1024, 'a')).ok);
}
