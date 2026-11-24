// SAIGE gfx_assets — bounded Radiance HDR (.hdr, RGBE) decoding to linear float RGB.
//
// Radiance RGBE is the format HDR environment maps are most commonly distributed in. The decoder treats its input as hostile like the other
// decoders: the encoded size is bounded, the header is parsed with a length limit, the dimensions and the decoded size are checked against
// limits before any pixel allocation, every read is bounds-checked, and failure is a returned error, never an exception or a crash.
//
// Supported: `#?RADIANCE` / `#?RGBE` header, `FORMAT=32-bit_rle_rgbe` (or no FORMAT), the standard orientation `-Y h +X w` and the vertically
// flipped `+Y h +X w`, flat scanlines and the "new" run-length encoding (widths 8 to 32767). Not supported (a clear error): XYZE, other
// orientations, the old run-length encoding. EXPOSURE and COLORCORR lines are ignored.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace saige::gfx {

struct HdrLimits {
    std::size_t max_encoded_bytes = std::size_t{256} << 20;
    uint32_t max_dimension = 16384;                          ///< width and height, each
    std::size_t max_decoded_bytes = std::size_t{1} << 30;    ///< width * height * 12 (three floats per pixel)
    std::size_t max_header_bytes = 4096;
};

struct HdrImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgb;  ///< linear radiance, three floats per pixel, tightly packed rows, top row first
};

struct HdrDecodeResult {
    std::optional<HdrImage> image;
    std::string error;  ///< set when image is empty
    [[nodiscard]] bool ok() const noexcept { return image.has_value(); }
};

[[nodiscard]] HdrDecodeResult decode_hdr(std::span<const uint8_t> bytes, const HdrLimits& limits = {});

} // namespace saige::gfx
