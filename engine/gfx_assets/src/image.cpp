#include "saige/gfx_assets/image.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

// stb_image, compiled for PNG and JPEG only, from the pinned upstream revision (docs/third-party/stb_image.md).
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS (1 << 14)
#define STB_IMAGE_IMPLEMENTATION

// The third-party header is not held to this project's warning level.
#if defined(_MSC_VER) && !defined(__clang__)
#    pragma warning(push, 0)
#elif defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wall"
#    pragma GCC diagnostic ignored "-Wextra"
#    pragma GCC diagnostic ignored "-Wpedantic"
#    pragma GCC diagnostic ignored "-Wconversion"
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#    pragma GCC diagnostic ignored "-Wshadow"
#    pragma GCC diagnostic ignored "-Wold-style-cast"
#    pragma GCC diagnostic ignored "-Wcast-align"
#    pragma GCC diagnostic ignored "-Wdouble-promotion"
#    pragma GCC diagnostic ignored "-Wnull-dereference"
#    pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#    pragma GCC diagnostic ignored "-Wunused-function"
#    pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#    pragma GCC diagnostic ignored "-Wuseless-cast"
#    pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#include "stb_image.h"
#if defined(_MSC_VER) && !defined(__clang__)
#    pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic pop
#endif

namespace saige::gfx {
namespace {

ImageDecodeResult fail(std::string message)
{
    ImageDecodeResult r;
    r.error = std::move(message);
    return r;
}

/// Owns a buffer returned by stb_image until the output vector has been built, so an allocation failure (or any early
/// return) cannot leak it.
struct StbiPixels {
    void operator()(unsigned char* p) const noexcept { stbi_image_free(p); }
};
using StbiBuffer = std::unique_ptr<unsigned char, StbiPixels>;

} // namespace

std::optional<ImageFormat> sniff_image_format(std::span<const uint8_t> bytes) noexcept
{
    static constexpr std::array<uint8_t, 8> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() >= png.size() && std::equal(png.begin(), png.end(), bytes.begin())) {
        return ImageFormat::Png;
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return ImageFormat::Jpeg;
    }
    return std::nullopt;
}

ImageDecodeResult decode_image(std::span<const uint8_t> bytes, const ImageLimits& limits)
{
    if (bytes.empty()) {
        return fail("image: no data");
    }
    if (bytes.size() > limits.max_encoded_bytes) {
        return fail("image: " + std::to_string(bytes.size()) + " encoded bytes is over the limit of " +
                    std::to_string(limits.max_encoded_bytes));
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail("image: encoded data is too large for the decoder");
    }
    const std::optional<ImageFormat> format = sniff_image_format(bytes);
    if (!format) {
        return fail("image: not a PNG or JPEG (unsupported format)");
    }

    // Header only: dimensions and channel count, no pixel allocation.
    const int size = static_cast<int>(bytes.size());
    int w = 0;
    int h = 0;
    int channels = 0;
    if (stbi_info_from_memory(bytes.data(), size, &w, &h, &channels) == 0 || w <= 0 || h <= 0) {
        return fail(std::string("image: unreadable header (") + (stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown") + ")");
    }
    const uint32_t width = static_cast<uint32_t>(w);
    const uint32_t height = static_cast<uint32_t>(h);
    if (width > limits.max_dimension || height > limits.max_dimension) {
        return fail("image: dimensions " + std::to_string(width) + "x" + std::to_string(height) +
                    " are over the limit of " + std::to_string(limits.max_dimension));
    }
    const uint64_t decoded = uint64_t{width} * height * 4u;
    if (decoded > limits.max_decoded_bytes) {
        return fail("image: " + std::to_string(decoded) + " decoded bytes is over the limit of " +
                    std::to_string(limits.max_decoded_bytes));
    }

    int dw = 0;
    int dh = 0;
    int dc = 0;
    const StbiBuffer pixels(stbi_load_from_memory(bytes.data(), size, &dw, &dh, &dc, 4));
    if (pixels == nullptr) {
        return fail(std::string("image: decode failed (") + (stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown") + ")");
    }
    ImageDecodeResult result;
    if (dw != w || dh != h) {  // the header and the decoder disagree: refuse rather than trust either
        return fail("image: header and decoded dimensions differ");
    }
    DecodedImage image;
    image.width = width;
    image.height = height;
    image.source = *format;
    try {
        image.rgba.assign(pixels.get(), pixels.get() + decoded);
    } catch (const std::bad_alloc&) {
        return fail("image: out of memory for " + std::to_string(decoded) + " decoded bytes");
    }
    for (uint64_t i = 3; i < decoded; i += 4) {
        if (image.rgba[i] != 255) {
            image.has_alpha = true;
            break;
        }
    }
    result.image = std::move(image);
    return result;
}

namespace {

const std::array<float, 256>& srgb_to_linear_table()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

uint8_t linear_to_srgb8(float l)
{
    l = std::clamp(l, 0.0f, 1.0f);
    const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(std::lround(c * 255.0f), 0L, 255L));
}

uint32_t level_extent(uint32_t base, uint32_t level) noexcept
{
    return level >= 32u ? 1u : std::max(1u, base >> level);
}

} // namespace

MipChain generate_mips(const DecodedImage& image, ColorSpace space)
{
    MipChain chain;
    // DecodedImage is publicly constructible, so the metadata is not trusted: sizes are computed in 64 bits and must match
    // the pixel storage exactly before anything is sized from them (an empty chain means "rejected").
    if (image.width == 0 || image.height == 0 ||
        uint64_t{image.width} * image.height * 4u != uint64_t{image.rgba.size()}) {
        return chain;
    }
    // Level count from the bit width of the largest side: at most 32, with no shift by the type's width.
    const uint32_t levels = static_cast<uint32_t>(std::bit_width(std::max(image.width, image.height)));
    uint64_t total64 = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        total64 += uint64_t{level_extent(image.width, l)} * level_extent(image.height, l) * 4u;
        if (total64 > std::numeric_limits<std::size_t>::max()) {
            return chain;
        }
    }
    const std::size_t total = static_cast<std::size_t>(total64);
    chain.width = image.width;
    chain.height = image.height;
    chain.levels = levels;
    try {
        chain.data.resize(total);
    } catch (const std::bad_alloc&) {
        return MipChain{};
    }
    std::memcpy(chain.data.data(), image.rgba.data(), image.rgba.size());

    const auto& lut = srgb_to_linear_table();
    std::size_t src_offset = 0;
    std::size_t dst_offset = image.rgba.size();
    for (uint32_t l = 1; l < levels; ++l) {
        const uint32_t sw = level_extent(image.width, l - 1);
        const uint32_t sh = level_extent(image.height, l - 1);
        const uint32_t dw = level_extent(image.width, l);
        const uint32_t dh = level_extent(image.height, l);
        const uint8_t* src = chain.data.data() + src_offset;
        uint8_t* dst = chain.data.data() + dst_offset;
        for (uint32_t y = 0; y < dh; ++y) {
            // Footprint of this destination row in the source: [y*sh/dh, ceil((y+1)*sh/dh)), always at least one row.
            const uint32_t y0 = static_cast<uint32_t>((uint64_t{y} * sh) / dh);
            const uint32_t y1 = std::max(y0 + 1, static_cast<uint32_t>((uint64_t{y + 1} * sh + dh - 1) / dh));
            for (uint32_t x = 0; x < dw; ++x) {
                const uint32_t x0 = static_cast<uint32_t>((uint64_t{x} * sw) / dw);
                const uint32_t x1 = std::max(x0 + 1, static_cast<uint32_t>((uint64_t{x + 1} * sw + dw - 1) / dw));
                double r = 0, g = 0, b = 0, a = 0, weight = 0, ur = 0, ug = 0, ub = 0;
                uint32_t count = 0;
                for (uint32_t sy = y0; sy < y1 && sy < sh; ++sy) {
                    for (uint32_t sx = x0; sx < x1 && sx < sw; ++sx) {
                        const uint8_t* p = src + (std::size_t{sy} * sw + sx) * 4u;
                        const double pr = space == ColorSpace::Srgb ? static_cast<double>(lut[p[0]]) : p[0] / 255.0;
                        const double pg = space == ColorSpace::Srgb ? static_cast<double>(lut[p[1]]) : p[1] / 255.0;
                        const double pb = space == ColorSpace::Srgb ? static_cast<double>(lut[p[2]]) : p[2] / 255.0;
                        const double pa = p[3] / 255.0;
                        r += pr * pa; g += pg * pa; b += pb * pa; weight += pa;
                        ur += pr; ug += pg; ub += pb;
                        a += pa;
                        ++count;
                    }
                }
                // Alpha-weighted color; fully transparent footprints fall back to the plain average.
                if (weight > 0.0) {
                    r /= weight; g /= weight; b /= weight;
                } else {
                    r = ur / count; g = ug / count; b = ub / count;
                }
                uint8_t* out = dst + (std::size_t{y} * dw + x) * 4u;
                if (space == ColorSpace::Srgb) {
                    out[0] = linear_to_srgb8(static_cast<float>(r));
                    out[1] = linear_to_srgb8(static_cast<float>(g));
                    out[2] = linear_to_srgb8(static_cast<float>(b));
                } else {
                    out[0] = static_cast<uint8_t>(std::clamp(std::lround(r * 255.0), 0L, 255L));
                    out[1] = static_cast<uint8_t>(std::clamp(std::lround(g * 255.0), 0L, 255L));
                    out[2] = static_cast<uint8_t>(std::clamp(std::lround(b * 255.0), 0L, 255L));
                }
                out[3] = static_cast<uint8_t>(std::clamp(std::lround(a / count * 255.0), 0L, 255L));
            }
        }
        src_offset = dst_offset;
        dst_offset += std::size_t{dw} * dh * 4u;
    }
    return chain;
}

} // namespace saige::gfx
