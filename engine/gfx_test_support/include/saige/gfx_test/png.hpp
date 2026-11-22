// SAIGE gfx_test_support — a minimal PNG writer for tests: stored (uncompressed) deflate blocks, no filtering. Enough to
// produce valid PNGs of any size and layout without a dependency.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace saige::test {

inline uint32_t crc32(const uint8_t* data, std::size_t n, uint32_t crc = 0xFFFFFFFFu)
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

inline void put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

inline void chunk(std::vector<uint8_t>& png, const char type[5], const std::vector<uint8_t>& body)
{
    put32(png, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> tagged(type, type + 4);
    tagged.insert(tagged.end(), body.begin(), body.end());
    png.insert(png.end(), tagged.begin(), tagged.end());
    put32(png, crc32(tagged.data(), tagged.size()) ^ 0xFFFFFFFFu);
}

/// `pixels`: raw scanline bytes without the filter byte, `bytes_per_row` per row.
inline std::vector<uint8_t> make_png(uint32_t w, uint32_t h, uint8_t bit_depth, uint8_t color_type,
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


} // namespace saige::test
