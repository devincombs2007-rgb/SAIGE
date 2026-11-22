// SAIGE gfx_assets — bounded PNG/JPEG decoding to RGBA8, and mip chain generation.
//
// Decoding uses stb_image, pinned by commit and SHA-256 (docs/third-party/stb_image.md) and compiled for PNG and JPEG
// only. The decoder treats its input as hostile: the encoded size is bounded first, the header is parsed for the
// dimensions, and the dimensions and the decoded byte count are checked against limits *before* any pixel allocation.
// Errors are returned, never thrown.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace saige::gfx {

enum class ImageFormat : uint8_t { Png, Jpeg };

struct ImageLimits {
    std::size_t max_encoded_bytes = std::size_t{64} << 20;  ///< stored (compressed) size
    uint32_t max_dimension = 16384;                          ///< width and height, each
    std::size_t max_decoded_bytes = std::size_t{256} << 20;  ///< width * height * 4
};

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  ///< tightly packed rows, top row first, 4 bytes per pixel
    ImageFormat source = ImageFormat::Png;
    bool has_alpha = false;     ///< some pixel is not fully opaque
};

struct ImageDecodeResult {
    std::optional<DecodedImage> image;
    std::string error;  ///< set when image is empty
    [[nodiscard]] bool ok() const noexcept { return image.has_value(); }
};

/// The container format from the leading magic bytes, if it is one this decoder accepts.
[[nodiscard]] std::optional<ImageFormat> sniff_image_format(std::span<const uint8_t> bytes) noexcept;

/// Decodes a PNG or JPEG (any bit depth or channel layout) to 8-bit RGBA.
[[nodiscard]] ImageDecodeResult decode_image(std::span<const uint8_t> bytes, const ImageLimits& limits = {});

/// Whether the texels hold sRGB-encoded color (base color, emissive) or linear data (normals, metallic/roughness,
/// occlusion). Mip averaging happens in linear light for sRGB data; alpha is always linear.
enum class ColorSpace : uint8_t { Srgb, Linear };

/// A full mip chain, tightly packed with the largest level first. Level `i` has extent `max(1, size >> i)`, which is the
/// RHI's layout (`rhi::mip_extent`, `rhi::compute_texture_data_size` for one layer), so `data` can be handed straight to
/// `Device::upload_texture`.
struct MipChain {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    std::vector<uint8_t> data;
};

/// Builds the mip chain by averaging each destination texel's footprint in the previous level (a box filter whose
/// footprints overlap by one texel for odd sizes, so every source texel contributes). Color is averaged weighted by alpha
/// to avoid dark fringes at transparent edges. Normal maps are not renormalized.
[[nodiscard]] MipChain generate_mips(const DecodedImage& image, ColorSpace space);

} // namespace saige::gfx
