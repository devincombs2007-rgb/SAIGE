#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/hdr_image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace saige::gfx;

namespace {

void rgbe(float r, float g, float b, uint8_t out[4])
{
    const float peak = std::max({r, g, b});
    if (peak < 1e-32f) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    int e;
    const float scale = std::frexp(peak, &e) * 256.0f / peak;
    out[0] = static_cast<uint8_t>(r * scale);
    out[1] = static_cast<uint8_t>(g * scale);
    out[2] = static_cast<uint8_t>(b * scale);
    out[3] = static_cast<uint8_t>(e + 128);
}

std::vector<uint8_t> header(uint32_t w, uint32_t h, const char* orientation = "-Y", const std::string& extra = "FORMAT=32-bit_rle_rgbe\n")
{
    const std::string text = "#?RADIANCE\n# test\n" + extra + "\n" + orientation + " " + std::to_string(h) + " +X " + std::to_string(w) + "\n";
    return {text.begin(), text.end()};
}

/// A flat (uncompressed) file: pixel (x, y) = f(x, y).
template <typename F>
std::vector<uint8_t> flat_file(uint32_t w, uint32_t h, F&& f, const char* orientation = "-Y")
{
    std::vector<uint8_t> out = header(w, h, orientation);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const std::array<float, 3> c = f(x, y);
            uint8_t p[4];
            rgbe(c[0], c[1], c[2], p);
            out.insert(out.end(), p, p + 4);
        }
    }
    return out;
}

/// The "new" run-length encoding of one scanline: each channel plane as literal runs of up to 128 and runs of repeats.
void rle_scanline(std::vector<uint8_t>& out, const std::vector<std::array<uint8_t, 4>>& pixels)
{
    const std::size_t w = pixels.size();
    out.insert(out.end(), {2, 2, static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w & 0xFF)});
    for (std::size_t c = 0; c < 4; ++c) {
        std::size_t x = 0;
        while (x < w) {
            std::size_t run = 1;
            while (x + run < w && run < 127 && pixels[x + run][c] == pixels[x][c]) ++run;
            if (run >= 3) {
                out.push_back(static_cast<uint8_t>(128 + run));
                out.push_back(pixels[x][c]);
                x += run;
            } else {
                std::size_t lit = 0;
                while (x + lit < w && lit < 128 && !(x + lit + 2 < w && pixels[x + lit][c] == pixels[x + lit + 1][c] && pixels[x + lit][c] == pixels[x + lit + 2][c])) ++lit;
                if (lit == 0) lit = 1;
                out.push_back(static_cast<uint8_t>(lit));
                for (std::size_t i = 0; i < lit; ++i) out.push_back(pixels[x + i][c]);
                x += lit;
            }
        }
    }
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

} // namespace

TEST_CASE("HDR: a flat file decodes to the radiance it encodes")
{
    const auto bytes = flat_file(4, 3, [](uint32_t x, uint32_t y) { return std::array<float, 3>{1.0f + static_cast<float>(x), 0.5f * static_cast<float>(y), 0.25f}; });
    const HdrDecodeResult r = decode_hdr(bytes);
    REQUIRE(r.ok());
    CHECK(r.image->width == 4 && r.image->height == 3 && r.image->rgb.size() == 36);
    for (uint32_t y = 0; y < 3; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            const float* p = r.image->rgb.data() + (y * 4 + x) * 3;
            CHECK(near(p[0], 1.0f + static_cast<float>(x), 0.02f * (1.0f + static_cast<float>(x))));  // RGBE keeps about 8 bits of mantissa
            CHECK(near(p[1], 0.5f * static_cast<float>(y), 0.01f + 0.01f * static_cast<float>(y)));
            CHECK(near(p[2], 0.25f, 0.003f));
        }
    }
    // The exact known value: (1, 0.5, 0.25) is mantissas 128, 64, 32 with exponent byte 129.
    const std::vector<uint8_t> one = [] {
        std::vector<uint8_t> v = header(1, 1);
        v.insert(v.end(), {128, 64, 32, 129});
        return v;
    }();
    const HdrDecodeResult k = decode_hdr(one);
    REQUIRE(k.ok());
    CHECK(k.image->rgb[0] == 1.0f && k.image->rgb[1] == 0.5f && k.image->rgb[2] == 0.25f);
}

TEST_CASE("HDR: run-length scanlines (runs, literals, mixed) and the flipped orientation decode like flat ones")
{
    const uint32_t w = 300, h = 5;  // wider than a literal run, so both kinds of runs are exercised
    const auto pixel = [](uint32_t x, uint32_t y) { return std::array<float, 3>{static_cast<float>((x / 40) % 3 + 1), x % 7 == 0 ? 2.0f : 0.5f, static_cast<float>(y) + 0.5f}; };
    std::vector<uint8_t> rle = header(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        std::vector<std::array<uint8_t, 4>> row;
        for (uint32_t x = 0; x < w; ++x) {
            const auto c = pixel(x, y);
            std::array<uint8_t, 4> p{};
            rgbe(c[0], c[1], c[2], p.data());
            row.push_back(p);
        }
        rle_scanline(rle, row);
    }
    const HdrDecodeResult a = decode_hdr(rle);
    const HdrDecodeResult b = decode_hdr(flat_file(w, h, pixel));
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.image->rgb == b.image->rgb);
    // "+Y" lists the bottom row first: the same data read as flipped puts the first scanline last.
    const HdrDecodeResult flipped = decode_hdr(flat_file(w, h, pixel, "+Y"));
    REQUIRE(flipped.ok());
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; x += 37) {
            for (int c = 0; c < 3; ++c) CHECK(flipped.image->rgb[((h - 1 - y) * w + x) * 3 + static_cast<uint32_t>(c)] == b.image->rgb[(y * w + x) * 3 + static_cast<uint32_t>(c)]);
        }
    }
}

TEST_CASE("HDR: hostile and unsupported input is an error, never a crash")
{
    const auto error = [](const std::vector<uint8_t>& bytes, const HdrLimits& limits = {}) {
        const HdrDecodeResult r = decode_hdr(bytes, limits);
        return r.ok() ? std::string() : r.error;
    };
    const auto mentions = [&](const std::vector<uint8_t>& bytes, const char* what, const HdrLimits& limits = {}) {
        const std::string e = error(bytes, limits);
        if (e.find(what) == std::string::npos) std::printf("  expected '%s', got '%s'\n", what, e.c_str());
        return e.find(what) != std::string::npos;
    };
    CHECK(mentions({}, "not a Radiance"));
    CHECK(mentions({'P', '6', '\n'}, "not a Radiance"));
    const auto good = flat_file(4, 2, [](uint32_t, uint32_t) { return std::array<float, 3>{1, 1, 1}; });
    {
        auto bad = good;  // unsupported pixel format
        const std::string text = "#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 2 +X 4\n";
        bad.assign(text.begin(), text.end());
        bad.insert(bad.end(), 32, 0);
        CHECK(mentions(bad, "unsupported pixel format"));
    }
    CHECK(mentions([] { auto v = header(4, 2, "+Y"); std::string s(v.begin(), v.end()); s.replace(s.find("+X"), 2, "-X"); return std::vector<uint8_t>(s.begin(), s.end()); }(), "unsupported orientation"));
    CHECK(mentions(header(0, 4), "zero or over the limit"));
    CHECK(mentions(header(100000, 4), "zero or over the limit"));
    HdrLimits small;
    small.max_dimension = 8;
    CHECK(mentions(flat_file(16, 2, [](uint32_t, uint32_t) { return std::array<float, 3>{1, 1, 1}; }), "zero or over the limit", small));
    HdrLimits tiny_bytes;
    tiny_bytes.max_decoded_bytes = 50;  // the 4x2 image needs 96
    CHECK(mentions(good, "decoded size", tiny_bytes));
    HdrLimits tiny_file;
    tiny_file.max_encoded_bytes = 10;
    CHECK(mentions(good, "over the limit", tiny_file));
    // A header that never ends, with no newline within the limit.
    CHECK(!error(std::vector<uint8_t>(10000, 'a')).empty());
    std::vector<uint8_t> endless = {'#', '?', 'R', '\n'};
    endless.insert(endless.end(), 6000, 'x');
    CHECK(!error(endless).empty());
    // Truncation at every length, including inside run-length data, is an error or a smaller valid file, never a crash.
    std::vector<uint8_t> rle = header(16, 2);
    for (int y = 0; y < 2; ++y) {
        std::vector<std::array<uint8_t, 4>> row(16, std::array<uint8_t, 4>{100, 50, 25, 130});
        rle_scanline(rle, row);
    }
    CHECK(error(rle).empty());
    for (std::size_t n = 0; n < rle.size(); ++n) CHECK(!error(std::vector<uint8_t>(rle.begin(), rle.begin() + static_cast<std::ptrdiff_t>(n))).empty());
    {  // a run that overruns its scanline, a zero-length literal, a wrong declared width
        std::vector<uint8_t> v = header(16, 1);
        v.insert(v.end(), {2, 2, 0, 16, static_cast<uint8_t>(128 + 20), 7});
        CHECK(mentions(v, "past the end"));
        std::vector<uint8_t> z = header(16, 1);
        z.insert(z.end(), {2, 2, 0, 16, 0});
        CHECK(mentions(z, "length zero"));
        std::vector<uint8_t> wdt = header(16, 1);
        wdt.insert(wdt.end(), {2, 2, 0, 17});
        CHECK(mentions(wdt, "wrong width"));
    }
    {  // the old run-length encoding is refused by name
        std::vector<uint8_t> v = header(2, 1);
        v.insert(v.end(), {1, 1, 1, 130, 5, 5, 5, 130});
        CHECK(mentions(v, "old run-length"));
    }
}
