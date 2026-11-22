#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_assets/image.hpp"
#include "saige/rhi/resource_desc.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace saige::gfx;

namespace {

// ------------------------------------------------------------------------------------------------ PNG encoder
// Just enough of a PNG writer for the tests: stored (uncompressed) deflate blocks, no filtering.
uint32_t crc32(const uint8_t* data, std::size_t n, uint32_t crc = 0xFFFFFFFFu)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

void put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void chunk(std::vector<uint8_t>& png, const char type[5], const std::vector<uint8_t>& body)
{
    put32(png, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> tagged(type, type + 4);
    tagged.insert(tagged.end(), body.begin(), body.end());
    png.insert(png.end(), tagged.begin(), tagged.end());
    put32(png, crc32(tagged.data(), tagged.size()) ^ 0xFFFFFFFFu);
}

/// `pixels`: raw scanline bytes without the filter byte, `bytes_per_row` per row.
std::vector<uint8_t> make_png(uint32_t w, uint32_t h, uint8_t bit_depth, uint8_t color_type,
                              const std::vector<uint8_t>& pixels, std::size_t bytes_per_row)
{
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.push_back(bit_depth);
    ihdr.push_back(color_type);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(png, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), pixels.begin() + static_cast<std::ptrdiff_t>(y * bytes_per_row),
                   pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * bytes_per_row));
    }
    std::vector<uint8_t> z{0x78, 0x01};
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    put32(z, (b << 16) | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

std::vector<uint8_t> rgba_pixels(uint32_t w, uint32_t h, auto&& texel)
{
    std::vector<uint8_t> px;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const std::array<uint8_t, 4> t = texel(x, y);
            px.insert(px.end(), t.begin(), t.end());
        }
    }
    return px;
}

std::array<uint8_t, 4> pattern_texel(uint32_t x, uint32_t y)
{
    return {static_cast<uint8_t>(x * 17 + 3), static_cast<uint8_t>(y * 29 + 5), static_cast<uint8_t>(x * 3 + y * 7 + 11),
            static_cast<uint8_t>(255 - x)};
}

std::vector<uint8_t> pattern_png(uint32_t w, uint32_t h)
{
    return make_png(w, h, 8, 6, rgba_pixels(w, h, pattern_texel), std::size_t{w} * 4);
}

const uint8_t* at(const DecodedImage& img, uint32_t x, uint32_t y) { return &img.rgba[(std::size_t{y} * img.width + x) * 4]; }

const std::vector<uint8_t> quad_jpeg = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
    0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
    0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0B, 0x08, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x06, 0x08,
    0x0B, 0x0C, 0x0B, 0x0A, 0x0C, 0x09, 0x0A, 0x0A, 0x0A, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0A, 0x07, 0x06, 0x07, 0x0A, 0x0A, 0x0A, 0x0A,
    0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
    0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
    0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0xFF, 0xC0,
    0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
    0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23,
    0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5,
    0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1,
    0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xC4, 0x00, 0x1F, 0x01, 0x00, 0x03,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
    0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
    0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15,
    0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
    0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
    0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9,
    0xFA, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xF9,
    0x6E, 0xBF, 0x9F, 0xCF, 0xF5, 0xF0, 0xFA, 0xB2, 0xBF, 0x1F, 0x3F, 0xE5, 0x9C, 0xFC, 0xC7, 0xAF,
    0xFA, 0x5C, 0x3F, 0xB8, 0x0F, 0xE9, 0xB2, 0xBF, 0xE3, 0xCC, 0xFD, 0xF0, 0xFF, 0xD9,
};

const std::vector<uint8_t> gray_jpeg = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
    0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
    0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0B, 0x08, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x06, 0x08,
    0x0B, 0x0C, 0x0B, 0x0A, 0x0C, 0x09, 0x0A, 0x0A, 0x0A, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08,
    0x00, 0x08, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03,
    0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00,
    0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32,
    0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35,
    0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94,
    0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2,
    0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
    0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6,
    0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA,
    0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0xF3, 0xFF, 0x00, 0xF8, 0x36, 0xE3, 0xFE, 0x60,
    0x1F, 0xF6, 0xCB, 0xFA, 0x57, 0xFF, 0xD9,
};

} // namespace

// ================================================================================================= decode
TEST_CASE("Image: a PNG decodes to exactly the encoded RGBA texels")
{
    const ImageDecodeResult r = decode_image(pattern_png(13, 9));
    REQUIRE(r.ok());
    CHECK(r.image->width == 13);
    CHECK(r.image->height == 9);
    CHECK(r.image->source == ImageFormat::Png);
    CHECK(r.image->has_alpha);
    bool exact = true;
    for (uint32_t y = 0; y < 9; ++y) {
        for (uint32_t x = 0; x < 13; ++x) {
            const auto want = pattern_texel(x, y);
            for (int c = 0; c < 4; ++c) exact &= at(*r.image, x, y)[c] == want[static_cast<std::size_t>(c)];
        }
    }
    CHECK(exact);  // a flipped row, swapped channel or wrong stride would fail
}

TEST_CASE("Image: other PNG layouts are expanded to RGBA")
{
    // 8-bit RGB: alpha becomes 255.
    std::vector<uint8_t> rgb;
    for (uint32_t i = 0; i < 4; ++i) { rgb.push_back(static_cast<uint8_t>(10 * i)); rgb.push_back(20); rgb.push_back(static_cast<uint8_t>(250 - i)); }
    ImageDecodeResult r = decode_image(make_png(4, 1, 8, 2, rgb, 12));
    REQUIRE(r.ok());
    CHECK_FALSE(r.image->has_alpha);
    CHECK(at(*r.image, 3, 0)[0] == 30);
    CHECK(at(*r.image, 3, 0)[2] == 247);
    CHECK(at(*r.image, 3, 0)[3] == 255);

    // 8-bit grayscale: replicated to RGB.
    r = decode_image(make_png(3, 1, 8, 0, {0, 128, 255}, 3));
    REQUIRE(r.ok());
    CHECK(at(*r.image, 1, 0)[0] == 128);
    CHECK(at(*r.image, 1, 0)[1] == 128);
    CHECK(at(*r.image, 1, 0)[2] == 128);

    // 16-bit RGBA: reduced to 8 bits (high byte).
    const std::vector<uint8_t> deep = {0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00, 0xC0, 0x00};
    r = decode_image(make_png(1, 1, 16, 6, deep, 8));
    REQUIRE(r.ok());
    CHECK(at(*r.image, 0, 0)[0] == 255);
    CHECK(std::abs(int{at(*r.image, 0, 0)[1]} - 128) <= 1);
    CHECK(at(*r.image, 0, 0)[2] == 0);
    CHECK(std::abs(int{at(*r.image, 0, 0)[3]} - 192) <= 1);
}

TEST_CASE("Image: a JPEG decodes with its quadrant colors")
{
    const ImageDecodeResult r = decode_image(quad_jpeg);
    REQUIRE(r.ok());
    CHECK(r.image->width == 16);
    CHECK(r.image->height == 16);
    CHECK(r.image->source == ImageFormat::Jpeg);
    CHECK_FALSE(r.image->has_alpha);
    auto near_color = [&](uint32_t x, uint32_t y, int cr, int cg, int cb) {
        const uint8_t* p = at(*r.image, x, y);
        return std::abs(p[0] - cr) <= 12 && std::abs(p[1] - cg) <= 12 && std::abs(p[2] - cb) <= 12 && p[3] == 255;
    };
    // Lossy, so a tolerance; the quadrants are far apart, so a flip or channel swap still fails.
    CHECK(near_color(3, 3, 220, 30, 30));      // top-left red
    CHECK(near_color(12, 3, 30, 200, 30));     // top-right green
    CHECK(near_color(3, 12, 30, 30, 220));     // bottom-left blue
    CHECK(near_color(12, 12, 230, 230, 30));   // bottom-right yellow

    const ImageDecodeResult g = decode_image(gray_jpeg);
    REQUIRE(g.ok());
    CHECK(g.image->width == 8);
    const uint8_t* dark = at(*g.image, 0, 4);
    const uint8_t* light = at(*g.image, 7, 4);
    CHECK(dark[0] == dark[1]);   // grayscale expands to equal channels
    CHECK(light[0] > dark[0] + 120);
}

TEST_CASE("Image: format sniffing, empty and unsupported input")
{
    CHECK(sniff_image_format(pattern_png(2, 2)) == ImageFormat::Png);
    CHECK(sniff_image_format(quad_jpeg) == ImageFormat::Jpeg);
    CHECK_FALSE(sniff_image_format(std::vector<uint8_t>{'G', 'I', 'F', '8', '9', 'a'}).has_value());
    CHECK_FALSE(sniff_image_format({}).has_value());
    CHECK_FALSE(decode_image({}).ok());
    const std::vector<uint8_t> gif{'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0};
    const ImageDecodeResult r = decode_image(gif);
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("unsupported format") != std::string::npos);
    const std::vector<uint8_t> bmp{'B', 'M', 0, 0, 0, 0};
    CHECK_FALSE(decode_image(bmp).ok());
}

// ================================================================================================= limits
TEST_CASE("Image: sizes are bounded before any pixel allocation")
{
    const std::vector<uint8_t> png = pattern_png(64, 64);
    ImageLimits tiny;
    tiny.max_encoded_bytes = 100;
    ImageDecodeResult r = decode_image(png, tiny);
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("encoded bytes") != std::string::npos);

    tiny = {};
    tiny.max_dimension = 32;
    r = decode_image(png, tiny);
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("dimensions 64x64") != std::string::npos);

    tiny = {};
    tiny.max_decoded_bytes = 64 * 64 * 4 - 1;
    r = decode_image(png, tiny);
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("decoded bytes") != std::string::npos);
    tiny.max_decoded_bytes = 64 * 64 * 4;
    CHECK(decode_image(png, tiny).ok());  // exactly at the limit is accepted
}

TEST_CASE("Image: a header that declares enormous dimensions is rejected without decoding")
{
    // Rewrite the IHDR of a tiny valid PNG to claim 60000 x 60000 (14.4 GB decoded). The pixel data is not that large,
    // so a decoder that trusted the header would try to allocate it. The CRC is recomputed so the chunk stays valid.
    std::vector<uint8_t> png = pattern_png(4, 4);
    auto set32 = [&](std::size_t at_byte, uint32_t v) {
        png[at_byte] = static_cast<uint8_t>(v >> 24);
        png[at_byte + 1] = static_cast<uint8_t>(v >> 16);
        png[at_byte + 2] = static_cast<uint8_t>(v >> 8);
        png[at_byte + 3] = static_cast<uint8_t>(v);
    };
    set32(16, 60000);  // IHDR width at byte 16
    set32(20, 60000);  // height
    set32(29, crc32(&png[12], 17) ^ 0xFFFFFFFFu);  // IHDR crc covers type + 13 data bytes
    const ImageDecodeResult r = decode_image(png);
    CHECK_FALSE(r.ok());
    CHECK((r.error.find("over the limit") != std::string::npos || r.error.find("unreadable") != std::string::npos));

    // JPEG: patch the SOF0 height/width.
    std::vector<uint8_t> jpeg = quad_jpeg;
    bool patched = false;
    for (std::size_t i = 0; i + 9 < jpeg.size(); ++i) {
        if (jpeg[i] == 0xFF && (jpeg[i + 1] == 0xC0 || jpeg[i + 1] == 0xC2)) {
            jpeg[i + 5] = 0xEA; jpeg[i + 6] = 0x60;  // height 60000
            jpeg[i + 7] = 0xEA; jpeg[i + 8] = 0x60;  // width 60000
            patched = true;
            break;
        }
    }
    REQUIRE(patched);
    const ImageDecodeResult j = decode_image(jpeg);
    CHECK_FALSE(j.ok());
}

// ================================================================================================= hostile input
TEST_CASE("Image: every truncation and byte corruption of a PNG and a JPEG fails cleanly or decodes (sanitizers watch)")
{
    const std::vector<uint8_t> png = pattern_png(24, 20);
    for (const std::vector<uint8_t>* source : {&png, &quad_jpeg}) {
        std::size_t decoded_ok = 0;
        for (std::size_t n = 0; n < source->size(); ++n) {
            const ImageDecodeResult r = decode_image(std::span<const uint8_t>(source->data(), n));
            decoded_ok += r.ok() ? 1u : 0u;
            CHECK((r.ok() || !r.error.empty()));
            if (r.ok()) {
                // Whatever decodes must be internally consistent.
                CHECK(r.image->rgba.size() == std::size_t{r.image->width} * r.image->height * 4);
            }
        }
        std::size_t corrupt_ok = 0;
        for (std::size_t i = 0; i < source->size(); ++i) {
            for (const unsigned flip : {0x01u, 0x80u, 0xFFu}) {
                std::vector<uint8_t> bad = *source;
                bad[i] = static_cast<uint8_t>(bad[i] ^ static_cast<uint8_t>(flip));
                const ImageDecodeResult r = decode_image(bad);
                CHECK((r.ok() || !r.error.empty()));
                if (r.ok()) {
                    CHECK(r.image->rgba.size() == std::size_t{r.image->width} * r.image->height * 4);
                    corrupt_ok += 1u;
                }
            }
        }
        std::printf("  %zu truncations (%zu still decoded) and %zu corruptions (%zu still decoded) processed\n", source->size(),
                    decoded_ok, source->size() * 3, corrupt_ok);
    }
}

TEST_CASE("Image: random garbage never crashes")
{
    uint32_t state = 12345;
    for (int round = 0; round < 200; ++round) {
        std::vector<uint8_t> junk(static_cast<std::size_t>(round) * 7 + 1);
        for (uint8_t& b : junk) {
            state = state * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(state >> 24);
        }
        if (round % 2 == 0 && junk.size() > 8) {  // make half of them look like PNGs so they reach the decoder
            const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            std::copy(sig, sig + 8, junk.begin());
        } else if (junk.size() > 3) {
            junk[0] = 0xFF; junk[1] = 0xD8; junk[2] = 0xFF;
        }
        const ImageDecodeResult r = decode_image(junk);
        CHECK((r.ok() || !r.error.empty()));
    }
}

// ================================================================================================= fixture
TEST_CASE("Image: the textured scene fixture's embedded PNG decodes to its exact checker pattern")
{
    const GltfLoadResult scene =
        load_gltf_file(std::filesystem::path(SAIGE_TEXTURED_SCENE_DIR) / "checker-box.gltf");
    REQUIRE(scene.ok());
    REQUIRE(scene.asset->images.size() == 1);
    const ImageDecodeResult r = decode_image(scene.asset->images[0].bytes);
    REQUIRE(r.ok());
    // The fixture is 8x8 with 2x2-pixel cells: orange (e86c25) and blue (2282ba), orange in the top-left cell.
    REQUIRE(r.image->width == 8);
    REQUIRE(r.image->height == 8);
    CHECK_FALSE(r.image->has_alpha);
    auto is = [&](uint32_t x, uint32_t y, uint8_t cr, uint8_t cg, uint8_t cb) {
        const uint8_t* p = at(*r.image, x, y);
        return p[0] == cr && p[1] == cg && p[2] == cb && p[3] == 255;
    };
    bool exact = true;
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            const bool orange = (((x / 2) + (y / 2)) % 2) == 0;
            exact &= orange ? is(x, y, 0xE8, 0x6C, 0x25) : is(x, y, 0x22, 0x82, 0xBA);
        }
    }
    CHECK(exact);  // every texel: the right color in the right cell, so orientation and channel order are pinned
}

// ================================================================================================= mips
namespace {

DecodedImage solid_image(uint32_t w, uint32_t h, std::array<uint8_t, 4> color)
{
    DecodedImage img;
    img.width = w;
    img.height = h;
    for (uint32_t i = 0; i < w * h; ++i) img.rgba.insert(img.rgba.end(), color.begin(), color.end());
    return img;
}

std::size_t level_offset(uint32_t w, uint32_t h, uint32_t level)
{
    std::size_t off = 0;
    for (uint32_t l = 0; l < level; ++l) {
        off += std::size_t{std::max(1u, w >> l)} * std::max(1u, h >> l) * 4;
    }
    return off;
}

} // namespace

TEST_CASE("Mips: the chain matches the RHI's layout for any size")
{
    for (const auto& [w, h] : {std::pair{1u, 1u}, {2u, 2u}, {7u, 3u}, {16u, 16u}, {13u, 1u}, {1u, 9u}, {100u, 37u}}) {
        const MipChain chain = generate_mips(solid_image(w, h, {10, 20, 30, 255}), ColorSpace::Srgb);
        saige::rhi::TextureDesc td;
        td.format = saige::rhi::Format::RGBA8_UNORM;
        td.width = w;
        td.height = h;
        td.mip_levels = saige::rhi::full_mip_chain;
        td.usage = saige::rhi::TextureUsage::Sampled;
        CHECK(chain.levels == saige::rhi::max_mip_levels(w, h));
        CHECK(chain.data.size() == saige::rhi::compute_texture_data_size(td));  // upload_texture needs exactly this
        // A solid color stays solid at every level (sRGB round trip is exact for a constant).
        bool solid = true;
        for (std::size_t i = 0; i < chain.data.size(); i += 4) {
            solid &= chain.data[i] == 10 && chain.data[i + 1] == 20 && chain.data[i + 2] == 30 && chain.data[i + 3] == 255;
        }
        CHECK(solid);
    }
    CHECK(generate_mips(DecodedImage{}, ColorSpace::Srgb).levels == 0);  // empty input yields an empty chain
}

TEST_CASE("Mips: sRGB color is averaged in linear light, linear data is averaged as stored")
{
    // 2x2 checker of black and white.
    DecodedImage img;
    img.width = img.height = 2;
    img.rgba = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    const MipChain srgb = generate_mips(img, ColorSpace::Srgb);
    const MipChain linear = generate_mips(img, ColorSpace::Linear);
    REQUIRE(srgb.levels == 2);
    const std::size_t top = level_offset(2, 2, 1);
    // Half the light of white is sRGB ~188, not 128.
    CHECK(std::abs(int{srgb.data[top]} - 188) <= 1);
    CHECK(std::abs(int{linear.data[top]} - 128) <= 1);
}

TEST_CASE("Mips: color is weighted by alpha so transparent texels do not darken the edge")
{
    DecodedImage img;
    img.width = 2;
    img.height = 1;
    img.rgba = {255, 0, 0, 255, 0, 0, 255, 0};  // opaque red, fully transparent blue
    const MipChain chain = generate_mips(img, ColorSpace::Linear);
    REQUIRE(chain.levels == 2);
    const uint8_t* top = &chain.data[level_offset(2, 1, 1)];
    CHECK(top[0] == 255);  // still red, not purple
    CHECK(top[2] == 0);
    CHECK(std::abs(int{top[3]} - 128) <= 1);  // alpha averages normally
    // Fully transparent footprint falls back to the plain color average.
    DecodedImage clear = solid_image(2, 2, {40, 80, 120, 0});
    const MipChain c = generate_mips(clear, ColorSpace::Linear);
    const uint8_t* ctop = &c.data[level_offset(2, 2, 1)];
    CHECK(ctop[0] == 40);
    CHECK(ctop[3] == 0);
}

TEST_CASE("Mips: odd sizes include every source texel")
{
    // 3x1: [0, 255, 0] in red. The 1x1 level must see the 255 (a naive 2x2 box over the first two texels, or one that
    // drops the last odd column, would not give the right mean).
    DecodedImage img;
    img.width = 3;
    img.height = 1;
    img.rgba = {0, 0, 0, 255, 255, 0, 0, 255, 0, 0, 0, 255};
    const MipChain chain = generate_mips(img, ColorSpace::Linear);
    REQUIRE(chain.levels == 2);
    CHECK(chain.data.size() == (3 + 1) * 4);
    // 3 -> 1: the single destination texel's footprint is all three source texels: mean of 0, 255, 0 = 85.
    CHECK(std::abs(int{chain.data[level_offset(3, 1, 1)]} - 85) <= 1);

    // 5x1 -> 2x1: footprints [0,3) and [2,5) overlap on texel 2; texel 4 reaches the second destination texel.
    DecodedImage five;
    five.width = 5;
    five.height = 1;
    for (int i = 0; i < 5; ++i) {
        const uint8_t v = i == 4 ? 255 : 0;
        five.rgba.insert(five.rgba.end(), {v, 0, 0, 255});
    }
    const MipChain c5 = generate_mips(five, ColorSpace::Linear);
    const std::size_t l1 = level_offset(5, 1, 1);
    CHECK(c5.data[l1] == 0);                                  // first destination: texels 0..2, all black
    CHECK(std::abs(int{c5.data[l1 + 4]} - 85) <= 1);          // second destination: texels 2..4 = (0+0+255)/3
}
