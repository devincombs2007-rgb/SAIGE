// SAIGE RHI — backend-agnostic pixel/vertex formats and format metadata.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace saige::rhi {

/// Texture and vertex formats. Names follow `<channels><bits>_<type>`. Channel order is memory order
/// (RGBA8 = R in byte 0). Backends map these to native formats (VkFormat, DXGI_FORMAT).
/// The order must match `kFormatTable` (checked at compile time).
enum class Format : uint8_t {
    Unknown,

    // 8-bit per channel
    R8_UNORM, R8_SNORM, R8_UINT, R8_SINT,
    RG8_UNORM, RG8_SNORM, RG8_UINT, RG8_SINT,
    RGBA8_UNORM, RGBA8_SNORM, RGBA8_UINT, RGBA8_SINT, RGBA8_SRGB,
    BGRA8_UNORM, BGRA8_SRGB,

    // 16-bit per channel
    R16_UNORM, R16_SNORM, R16_UINT, R16_SINT, R16_FLOAT,
    RG16_UNORM, RG16_SNORM, RG16_UINT, RG16_SINT, RG16_FLOAT,
    RGBA16_UNORM, RGBA16_SNORM, RGBA16_UINT, RGBA16_SINT, RGBA16_FLOAT,

    // 32-bit per channel
    R32_UINT, R32_SINT, R32_FLOAT,
    RG32_UINT, RG32_SINT, RG32_FLOAT,
    RGB32_UINT, RGB32_SINT, RGB32_FLOAT,
    RGBA32_UINT, RGBA32_SINT, RGBA32_FLOAT,

    // Packed
    RGB10A2_UNORM, RGB10A2_UINT, RG11B10_FLOAT, RGB9E5_UFLOAT,

    // Depth / stencil
    D16_UNORM, D24_UNORM_S8_UINT, D32_FLOAT, D32_FLOAT_S8_UINT,

    // Block-compressed (4x4 blocks)
    BC1_UNORM, BC1_SRGB,
    BC2_UNORM, BC2_SRGB,
    BC3_UNORM, BC3_SRGB,
    BC4_UNORM, BC4_SNORM,
    BC5_UNORM, BC5_SNORM,
    BC6H_UFLOAT, BC6H_SFLOAT,
    BC7_UNORM, BC7_SRGB,

    Count
};

inline constexpr std::size_t kFormatCount = static_cast<std::size_t>(Format::Count);

/// Numeric interpretation of a format's channels.
enum class FormatType : uint8_t {
    Unknown,
    Unorm,
    Snorm,
    Uint,
    Sint,
    Float,
    UnormSrgb,
    DepthStencil,
};

struct FormatInfo {
    Format format;
    std::string_view name;
    uint8_t bytesPerBlock;  ///< Bytes per texel for uncompressed formats, per block for compressed ones.
    uint8_t blockWidth;     ///< 1 for uncompressed formats.
    uint8_t blockHeight;    ///< 1 for uncompressed formats.
    uint8_t componentCount;
    FormatType type;
    bool hasDepth;
    bool hasStencil;
    bool compressed;

    [[nodiscard]] constexpr bool isSrgb() const noexcept { return type == FormatType::UnormSrgb; }
    [[nodiscard]] constexpr bool isDepthOrStencil() const noexcept { return hasDepth || hasStencil; }
};

namespace detail {
// clang-format off
using FT = FormatType;
inline constexpr std::array<FormatInfo, kFormatCount> kFormatTable = {{
    //  format                       name                  B  bw bh  c  type          depth  stencil compressed
    { Format::Unknown,           "Unknown",            0, 0, 0, 0, FT::Unknown,   false, false, false },

    { Format::R8_UNORM,          "R8_UNORM",           1, 1, 1, 1, FT::Unorm,     false, false, false },
    { Format::R8_SNORM,          "R8_SNORM",           1, 1, 1, 1, FT::Snorm,     false, false, false },
    { Format::R8_UINT,           "R8_UINT",            1, 1, 1, 1, FT::Uint,      false, false, false },
    { Format::R8_SINT,           "R8_SINT",            1, 1, 1, 1, FT::Sint,      false, false, false },
    { Format::RG8_UNORM,         "RG8_UNORM",          2, 1, 1, 2, FT::Unorm,     false, false, false },
    { Format::RG8_SNORM,         "RG8_SNORM",          2, 1, 1, 2, FT::Snorm,     false, false, false },
    { Format::RG8_UINT,          "RG8_UINT",           2, 1, 1, 2, FT::Uint,      false, false, false },
    { Format::RG8_SINT,          "RG8_SINT",           2, 1, 1, 2, FT::Sint,      false, false, false },
    { Format::RGBA8_UNORM,       "RGBA8_UNORM",        4, 1, 1, 4, FT::Unorm,     false, false, false },
    { Format::RGBA8_SNORM,       "RGBA8_SNORM",        4, 1, 1, 4, FT::Snorm,     false, false, false },
    { Format::RGBA8_UINT,        "RGBA8_UINT",         4, 1, 1, 4, FT::Uint,      false, false, false },
    { Format::RGBA8_SINT,        "RGBA8_SINT",         4, 1, 1, 4, FT::Sint,      false, false, false },
    { Format::RGBA8_SRGB,        "RGBA8_SRGB",         4, 1, 1, 4, FT::UnormSrgb, false, false, false },
    { Format::BGRA8_UNORM,       "BGRA8_UNORM",        4, 1, 1, 4, FT::Unorm,     false, false, false },
    { Format::BGRA8_SRGB,        "BGRA8_SRGB",         4, 1, 1, 4, FT::UnormSrgb, false, false, false },

    { Format::R16_UNORM,         "R16_UNORM",          2, 1, 1, 1, FT::Unorm,     false, false, false },
    { Format::R16_SNORM,         "R16_SNORM",          2, 1, 1, 1, FT::Snorm,     false, false, false },
    { Format::R16_UINT,          "R16_UINT",           2, 1, 1, 1, FT::Uint,      false, false, false },
    { Format::R16_SINT,          "R16_SINT",           2, 1, 1, 1, FT::Sint,      false, false, false },
    { Format::R16_FLOAT,         "R16_FLOAT",          2, 1, 1, 1, FT::Float,     false, false, false },
    { Format::RG16_UNORM,        "RG16_UNORM",         4, 1, 1, 2, FT::Unorm,     false, false, false },
    { Format::RG16_SNORM,        "RG16_SNORM",         4, 1, 1, 2, FT::Snorm,     false, false, false },
    { Format::RG16_UINT,         "RG16_UINT",          4, 1, 1, 2, FT::Uint,      false, false, false },
    { Format::RG16_SINT,         "RG16_SINT",          4, 1, 1, 2, FT::Sint,      false, false, false },
    { Format::RG16_FLOAT,        "RG16_FLOAT",         4, 1, 1, 2, FT::Float,     false, false, false },
    { Format::RGBA16_UNORM,      "RGBA16_UNORM",       8, 1, 1, 4, FT::Unorm,     false, false, false },
    { Format::RGBA16_SNORM,      "RGBA16_SNORM",       8, 1, 1, 4, FT::Snorm,     false, false, false },
    { Format::RGBA16_UINT,       "RGBA16_UINT",        8, 1, 1, 4, FT::Uint,      false, false, false },
    { Format::RGBA16_SINT,       "RGBA16_SINT",        8, 1, 1, 4, FT::Sint,      false, false, false },
    { Format::RGBA16_FLOAT,      "RGBA16_FLOAT",       8, 1, 1, 4, FT::Float,     false, false, false },

    { Format::R32_UINT,          "R32_UINT",           4, 1, 1, 1, FT::Uint,      false, false, false },
    { Format::R32_SINT,          "R32_SINT",           4, 1, 1, 1, FT::Sint,      false, false, false },
    { Format::R32_FLOAT,         "R32_FLOAT",          4, 1, 1, 1, FT::Float,     false, false, false },
    { Format::RG32_UINT,         "RG32_UINT",          8, 1, 1, 2, FT::Uint,      false, false, false },
    { Format::RG32_SINT,         "RG32_SINT",          8, 1, 1, 2, FT::Sint,      false, false, false },
    { Format::RG32_FLOAT,        "RG32_FLOAT",         8, 1, 1, 2, FT::Float,     false, false, false },
    { Format::RGB32_UINT,        "RGB32_UINT",        12, 1, 1, 3, FT::Uint,      false, false, false },
    { Format::RGB32_SINT,        "RGB32_SINT",        12, 1, 1, 3, FT::Sint,      false, false, false },
    { Format::RGB32_FLOAT,       "RGB32_FLOAT",       12, 1, 1, 3, FT::Float,     false, false, false },
    { Format::RGBA32_UINT,       "RGBA32_UINT",       16, 1, 1, 4, FT::Uint,      false, false, false },
    { Format::RGBA32_SINT,       "RGBA32_SINT",       16, 1, 1, 4, FT::Sint,      false, false, false },
    { Format::RGBA32_FLOAT,      "RGBA32_FLOAT",      16, 1, 1, 4, FT::Float,     false, false, false },

    { Format::RGB10A2_UNORM,     "RGB10A2_UNORM",      4, 1, 1, 4, FT::Unorm,     false, false, false },
    { Format::RGB10A2_UINT,      "RGB10A2_UINT",       4, 1, 1, 4, FT::Uint,      false, false, false },
    { Format::RG11B10_FLOAT,     "RG11B10_FLOAT",      4, 1, 1, 3, FT::Float,     false, false, false },
    { Format::RGB9E5_UFLOAT,     "RGB9E5_UFLOAT",      4, 1, 1, 3, FT::Float,     false, false, false },

    { Format::D16_UNORM,         "D16_UNORM",          2, 1, 1, 1, FT::DepthStencil, true,  false, false },
    { Format::D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT",  4, 1, 1, 2, FT::DepthStencil, true,  true,  false },
    { Format::D32_FLOAT,         "D32_FLOAT",          4, 1, 1, 1, FT::DepthStencil, true,  false, false },
    { Format::D32_FLOAT_S8_UINT, "D32_FLOAT_S8_UINT",  8, 1, 1, 2, FT::DepthStencil, true,  true,  false },

    { Format::BC1_UNORM,         "BC1_UNORM",          8, 4, 4, 4, FT::Unorm,     false, false, true },
    { Format::BC1_SRGB,          "BC1_SRGB",           8, 4, 4, 4, FT::UnormSrgb, false, false, true },
    { Format::BC2_UNORM,         "BC2_UNORM",         16, 4, 4, 4, FT::Unorm,     false, false, true },
    { Format::BC2_SRGB,          "BC2_SRGB",          16, 4, 4, 4, FT::UnormSrgb, false, false, true },
    { Format::BC3_UNORM,         "BC3_UNORM",         16, 4, 4, 4, FT::Unorm,     false, false, true },
    { Format::BC3_SRGB,          "BC3_SRGB",          16, 4, 4, 4, FT::UnormSrgb, false, false, true },
    { Format::BC4_UNORM,         "BC4_UNORM",          8, 4, 4, 1, FT::Unorm,     false, false, true },
    { Format::BC4_SNORM,         "BC4_SNORM",          8, 4, 4, 1, FT::Snorm,     false, false, true },
    { Format::BC5_UNORM,         "BC5_UNORM",         16, 4, 4, 2, FT::Unorm,     false, false, true },
    { Format::BC5_SNORM,         "BC5_SNORM",         16, 4, 4, 2, FT::Snorm,     false, false, true },
    { Format::BC6H_UFLOAT,       "BC6H_UFLOAT",       16, 4, 4, 3, FT::Float,     false, false, true },
    { Format::BC6H_SFLOAT,       "BC6H_SFLOAT",       16, 4, 4, 3, FT::Float,     false, false, true },
    { Format::BC7_UNORM,         "BC7_UNORM",         16, 4, 4, 4, FT::Unorm,     false, false, true },
    { Format::BC7_SRGB,          "BC7_SRGB",          16, 4, 4, 4, FT::UnormSrgb, false, false, true },
}};
// clang-format on

[[nodiscard]] consteval bool formatTableMatchesEnum()
{
    for (std::size_t i = 0; i < kFormatCount; ++i) {
        if (static_cast<std::size_t>(kFormatTable[i].format) != i) {
            return false;
        }
    }
    return true;
}
static_assert(formatTableMatchesEnum(), "kFormatTable order must match enum Format");
} // namespace detail

[[nodiscard]] constexpr const FormatInfo& getFormatInfo(Format format) noexcept
{
    const auto i = static_cast<std::size_t>(format);
    return detail::kFormatTable[i < kFormatCount ? i : 0];
}

[[nodiscard]] constexpr std::string_view formatName(Format f) noexcept { return getFormatInfo(f).name; }
[[nodiscard]] constexpr bool isCompressed(Format f) noexcept { return getFormatInfo(f).compressed; }
[[nodiscard]] constexpr bool isSrgb(Format f) noexcept { return getFormatInfo(f).isSrgb(); }
[[nodiscard]] constexpr bool hasDepth(Format f) noexcept { return getFormatInfo(f).hasDepth; }
[[nodiscard]] constexpr bool hasStencil(Format f) noexcept { return getFormatInfo(f).hasStencil; }
[[nodiscard]] constexpr bool isDepthOrStencil(Format f) noexcept { return getFormatInfo(f).isDepthOrStencil(); }

/// Returns the sRGB variant of a UNORM color format, or `f` unchanged if there isn't one.
[[nodiscard]] constexpr Format toSrgb(Format f) noexcept
{
    switch (f) {
    case Format::RGBA8_UNORM: return Format::RGBA8_SRGB;
    case Format::BGRA8_UNORM: return Format::BGRA8_SRGB;
    case Format::BC1_UNORM: return Format::BC1_SRGB;
    case Format::BC2_UNORM: return Format::BC2_SRGB;
    case Format::BC3_UNORM: return Format::BC3_SRGB;
    case Format::BC7_UNORM: return Format::BC7_SRGB;
    default: return f;
    }
}

/// Returns the linear (UNORM) variant of an sRGB format, or `f` unchanged if it isn't sRGB.
[[nodiscard]] constexpr Format toLinear(Format f) noexcept
{
    switch (f) {
    case Format::RGBA8_SRGB: return Format::RGBA8_UNORM;
    case Format::BGRA8_SRGB: return Format::BGRA8_UNORM;
    case Format::BC1_SRGB: return Format::BC1_UNORM;
    case Format::BC2_SRGB: return Format::BC2_UNORM;
    case Format::BC3_SRGB: return Format::BC3_UNORM;
    case Format::BC7_SRGB: return Format::BC7_UNORM;
    default: return f;
    }
}

/// Memory layout of one 2D surface (a single mip level of a single array layer or depth slice).
struct SurfaceLayout {
    uint32_t blocksWide = 0;
    uint32_t blocksHigh = 0;
    uint64_t rowPitch = 0;    ///< Bytes per row of blocks, tightly packed.
    uint64_t slicePitch = 0;  ///< Bytes per 2D surface, tightly packed.
};

/// Tightly packed layout of a `width` x `height` surface. Block-compressed formats round partial blocks
/// up (a 1x1 BC7 mip still occupies one 16-byte block).
[[nodiscard]] constexpr SurfaceLayout computeSurfaceLayout(Format format, uint32_t width, uint32_t height) noexcept
{
    const FormatInfo& info = getFormatInfo(format);
    if (info.bytesPerBlock == 0 || width == 0 || height == 0) {
        return {};
    }
    SurfaceLayout layout;
    layout.blocksWide = (width + info.blockWidth - 1u) / info.blockWidth;
    layout.blocksHigh = (height + info.blockHeight - 1u) / info.blockHeight;
    layout.rowPitch = uint64_t{layout.blocksWide} * info.bytesPerBlock;
    layout.slicePitch = layout.rowPitch * layout.blocksHigh;
    return layout;
}

} // namespace saige::rhi
