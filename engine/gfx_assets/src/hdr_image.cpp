#include "saige/gfx_assets/hdr_image.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string_view>

namespace saige::gfx {
namespace {

HdrDecodeResult fail(std::string message)
{
    HdrDecodeResult r;
    r.error = "HDR: " + std::move(message);
    return r;
}

float rgbe_channel(uint8_t mantissa, int exponent_byte) noexcept
{
    return exponent_byte == 0 ? 0.0f : std::ldexp(static_cast<float>(mantissa), exponent_byte - (128 + 8));
}

} // namespace

HdrDecodeResult decode_hdr(std::span<const uint8_t> bytes, const HdrLimits& limits)
{
    if (bytes.size() > limits.max_encoded_bytes) {
        return fail("the file is " + std::to_string(bytes.size()) + " bytes, over the limit of " + std::to_string(limits.max_encoded_bytes));
    }
    // ---- header: lines up to the first empty line, then the resolution line.
    std::size_t pos = 0;
    const auto read_line = [&](std::string_view& line) -> bool {
        const std::size_t start = pos;
        while (pos < bytes.size() && bytes[pos] != '\n') {
            if (pos - start > limits.max_header_bytes) return false;
            ++pos;
        }
        if (pos >= bytes.size()) return false;
        line = std::string_view(reinterpret_cast<const char*>(bytes.data()) + start, pos - start);
        ++pos;  // the newline
        return true;
    };
    std::string_view line;
    if (!read_line(line) || line.size() < 2 || line[0] != '#' || line[1] != '?') return fail("not a Radiance HDR file (the first line must start with '#?')");
    bool format_seen = false;
    for (;;) {
        if (pos > limits.max_header_bytes) return fail("the header is longer than " + std::to_string(limits.max_header_bytes) + " bytes");
        if (!read_line(line)) return fail("the header is truncated or too long");
        if (line.empty()) break;
        if (line.starts_with("FORMAT=")) {
            if (line != "FORMAT=32-bit_rle_rgbe") return fail("unsupported pixel format '" + std::string(line.substr(7)) + "' (only 32-bit_rle_rgbe)");
            format_seen = true;
        }
    }
    (void)format_seen;  // an absent FORMAT line means RGBE
    if (!read_line(line)) return fail("the resolution line is missing");
    // "-Y <height> +X <width>" (top row first) or "+Y <height> +X <width>" (bottom row first).
    bool flip = false;
    uint64_t height = 0, width = 0;
    {
        // "<sign>Y <height> +X <width>", parsed by hand: sscanf's behavior on an out-of-range number is undefined, and the dimensions are untrusted.
        const std::string_view text = line;
        std::size_t i = 0;
        const auto skip_space = [&] {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        };
        const auto bad = [&] { return fail("unsupported orientation or malformed resolution line '" + std::string(text) + "' (only '-Y h +X w' and '+Y h +X w')"); };
        const auto read_axis = [&](char axis, char& sign) {
            skip_space();
            if (i + 1 >= text.size() || (text[i] != '+' && text[i] != '-') || text[i + 1] != axis) return false;
            sign = text[i];
            i += 2;
            return true;
        };
        // Digits only, no sign, at most 20 of them, and the value must fit in 64 bits.
        bool overflow = false;
        const auto read_number = [&](uint64_t& out) {
            skip_space();
            const std::size_t start = i;
            uint64_t v = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
                if (v > (std::numeric_limits<uint64_t>::max() - digit) / 10) overflow = true;
                else v = v * 10 + digit;
                ++i;
            }
            out = v;
            return i > start;
        };
        char ysign = 0, xsign = 0;
        if (!read_axis('Y', ysign) || !read_number(height) || !read_axis('X', xsign) || !read_number(width) || xsign != '+') return bad();
        skip_space();
        while (i < text.size() && (text[i] == '\r')) ++i;
        if (i != text.size()) return fail("trailing characters on the resolution line");
        if (overflow) return fail("a dimension is too large to represent");
        flip = ysign == '+';
    }
    if (width == 0 || height == 0 || width > limits.max_dimension || height > limits.max_dimension) {
        return fail("the dimensions " + std::to_string(width) + "x" + std::to_string(height) + " are zero or over the limit of " + std::to_string(limits.max_dimension));
    }
    // Checked without multiplying: width and height are each at most 2^32 - 1 (max_dimension is 32-bit), but their product need not fit in 64 bits.
    const uint64_t byte_limit = std::min<uint64_t>(limits.max_decoded_bytes, std::numeric_limits<std::size_t>::max() / 4);
    if (height > byte_limit / 12 / width) return fail("the decoded size is over the limit of " + std::to_string(limits.max_decoded_bytes) + " bytes");

    HdrImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    try {
        image.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0.0f);
    } catch (const std::exception&) {
        return fail("not enough memory for a " + std::to_string(width) + "x" + std::to_string(height) + " image");
    }

    std::vector<uint8_t> scan(static_cast<std::size_t>(width) * 4);
    for (uint64_t row = 0; row < height; ++row) {
        const uint64_t y = flip ? height - 1 - row : row;
        const std::size_t remaining = bytes.size() - pos;
        const bool rle_width = width >= 8 && width <= 32767;
        if (rle_width && remaining >= 4 && bytes[pos] == 2 && bytes[pos + 1] == 2 && (bytes[pos + 2] & 0x80) == 0) {
            if ((static_cast<uint64_t>(bytes[pos + 2]) << 8 | bytes[pos + 3]) != width) return fail("a run-length scanline declares the wrong width");
            pos += 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                std::size_t x = 0;
                while (x < width) {
                    if (pos >= bytes.size()) return fail("the pixel data is truncated");
                    const unsigned count = bytes[pos++];
                    if (count > 128) {  // a run of one repeated byte
                        const unsigned run = count - 128;
                        if (pos >= bytes.size()) return fail("the pixel data is truncated");
                        if (x + run > width) return fail("a run goes past the end of its scanline");
                        const uint8_t value = bytes[pos++];
                        for (unsigned i = 0; i < run; ++i) scan[(x++) * 4 + channel] = value;
                    } else {                // literal bytes
                        if (count == 0) return fail("a literal run of length zero");
                        if (x + count > width) return fail("a literal run goes past the end of its scanline");
                        if (bytes.size() - pos < count) return fail("the pixel data is truncated");
                        for (unsigned i = 0; i < count; ++i) scan[(x++) * 4 + channel] = bytes[pos++];
                    }
                }
            }
        } else {
            if (remaining < static_cast<std::size_t>(width) * 4) return fail("the pixel data is truncated");
            // The old run-length encoding marks a repeat with r = g = b = 1; a flat scanline never has that in a valid file's first pixel either way.
            for (std::size_t x = 0; x < width; ++x) {
                if (bytes[pos + x * 4] == 1 && bytes[pos + x * 4 + 1] == 1 && bytes[pos + x * 4 + 2] == 1) return fail("the old run-length encoding is not supported");
            }
            std::memcpy(scan.data(), bytes.data() + pos, static_cast<std::size_t>(width) * 4);
            pos += static_cast<std::size_t>(width) * 4;
        }
        float* out = image.rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3;
        for (std::size_t x = 0; x < width; ++x) {
            const uint8_t* p = scan.data() + x * 4;
            out[x * 3] = rgbe_channel(p[0], p[3]);
            out[x * 3 + 1] = rgbe_channel(p[1], p[3]);
            out[x * 3 + 2] = rgbe_channel(p[2], p[3]);
        }
    }
    HdrDecodeResult ok;
    ok.image = std::move(image);
    return ok;
}

} // namespace saige::gfx
