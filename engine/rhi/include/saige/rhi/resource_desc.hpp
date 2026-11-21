// SAIGE RHI — resource descriptions (buffers, textures, samplers) and their validation.
#pragma once

#include "saige/rhi/flags.hpp"
#include "saige/rhi/format.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace saige::rhi {

// ---------------------------------------------------------------------------------------------------------------------
// Common
// ---------------------------------------------------------------------------------------------------------------------

/// Result of validating a description. `error` is a static string, or nullptr when the description is valid.
struct ValidationResult {
    const char* error = nullptr;

    [[nodiscard]] constexpr bool ok() const noexcept { return error == nullptr; }
    constexpr explicit operator bool() const noexcept { return ok(); }
};

enum class QueueType : uint8_t { Graphics, Compute, Transfer };

/// Where a resource's memory lives and how the CPU may access it.
enum class MemoryLocation : uint8_t {
    GpuOnly,   ///< Device-local, no CPU access.
    CpuToGpu,  ///< Host-visible, write-combined. Upload/staging and per-frame dynamic data.
    GpuToCpu,  ///< Host-visible, cached. Readback.
};

struct Extent3D {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;

    friend constexpr bool operator==(const Extent3D&, const Extent3D&) noexcept = default;
};

// ---------------------------------------------------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------------------------------------------------

enum class BufferUsage : uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,  ///< Structured/byte-address buffer access (bindless).
    Indirect = 1u << 4,
    TransferSrc = 1u << 5,
    TransferDst = 1u << 6,
    DeviceAddress = 1u << 7,  ///< GPU virtual address queryable (buffer device address).
    AccelerationStructureInput = 1u << 8,
    AccelerationStructureStorage = 1u << 9,
    ShaderBindingTable = 1u << 10,
};
SAIGE_RHI_ENUM_FLAGS(BufferUsage)

struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryLocation memory = MemoryLocation::GpuOnly;
    uint32_t structure_stride = 0;  ///< Element stride for structured views (0 = raw/byte-address).
    const char* debug_name = nullptr;
};

[[nodiscard]] ValidationResult validate(const BufferDesc& desc) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------------------------------

enum class TextureDimension : uint8_t { Texture1D, Texture2D, Texture3D, TextureCube };

enum class TextureUsage : uint32_t {
    None = 0,
    Sampled = 1u << 0,
    Storage = 1u << 1,
    ColorAttachment = 1u << 2,
    DepthStencilAttachment = 1u << 3,
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
};
SAIGE_RHI_ENUM_FLAGS(TextureUsage)

/// Pass as `TextureDesc::mip_levels` to request a full mip chain down to 1x1(x1).
inline constexpr uint32_t full_mip_chain = 0;

struct TextureDesc {
    TextureDimension dimension = TextureDimension::Texture2D;
    Format format = Format::Unknown;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;        ///< Texture3D only.
    uint32_t array_layers = 1;  ///< For cubes: number of faces (a multiple of 6).
    uint32_t mip_levels = 1;    ///< `full_mip_chain` for a complete chain.
    uint32_t sample_count = 1;
    TextureUsage usage = TextureUsage::None;
    const char* debug_name = nullptr;

    [[nodiscard]] constexpr Extent3D extent() const noexcept { return {width, height, depth}; }
};

/// Number of mips in a full chain for the given base extent: floor(log2(max(w, h, d))) + 1.
[[nodiscard]] constexpr uint32_t max_mip_levels(uint32_t width, uint32_t height = 1, uint32_t depth = 1) noexcept
{
    uint32_t largest = width > height ? width : height;
    largest = largest > depth ? largest : depth;
    return largest == 0 ? 0u : static_cast<uint32_t>(std::bit_width(largest));
}

/// Extent of `level`, clamped to at least 1 in each dimension.
[[nodiscard]] constexpr Extent3D mip_extent(Extent3D base, uint32_t level) noexcept
{
    auto shrink = [level](uint32_t v) -> uint32_t {
        if (level >= 32u) {
            return 1u;
        }
        const uint32_t s = v >> level;
        return s > 0u ? s : 1u;
    };
    return {shrink(base.width), shrink(base.height), shrink(base.depth)};
}

/// Resolves `full_mip_chain` to a concrete count.
[[nodiscard]] constexpr uint32_t resolve_mip_levels(const TextureDesc& desc) noexcept
{
    const uint32_t depth = desc.dimension == TextureDimension::Texture3D ? desc.depth : 1u;
    return desc.mip_levels == full_mip_chain ? max_mip_levels(desc.width, desc.height, depth) : desc.mip_levels;
}

/// The rules every backend applies to `Device::upload_texture(texture, data)`: Sampled | TransferDst usage, a single
/// sample, 2D, a color (not depth/stencil) format, and `data_size` equal to `compute_texture_data_size`.
[[nodiscard]] ValidationResult validate_texture_upload(const TextureDesc& desc, uint64_t data_size) noexcept;

/// Total tightly packed size of every mip, layer and slice. Used for staging and memory budgeting
/// (GPU allocations add alignment and padding on top of this).
[[nodiscard]] uint64_t compute_texture_data_size(const TextureDesc& desc) noexcept;

/// Validates a texture description against backend-independent rules (dimension/extent consistency,
/// mip and sample counts, format/usage compatibility).
[[nodiscard]] ValidationResult validate(const TextureDesc& desc) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------------------------------------------------

enum class Filter : uint8_t { Nearest, Linear };
enum class MipmapMode : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder, MirrorClampToEdge };
enum class CompareOp : uint8_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class BorderColor : uint8_t { TransparentBlack, OpaqueBlack, OpaqueWhite };

/// Pass as `SamplerDesc::max_lod` to leave the LOD unclamped.
inline constexpr float lod_clamp_none = 1000.0f;

struct SamplerDesc {
    Filter min_filter = Filter::Linear;
    Filter mag_filter = Filter::Linear;
    MipmapMode mipmap_mode = MipmapMode::Linear;
    AddressMode address_u = AddressMode::Repeat;
    AddressMode address_v = AddressMode::Repeat;
    AddressMode address_w = AddressMode::Repeat;
    float mip_lod_bias = 0.0f;
    float max_anisotropy = 1.0f;  ///< 1 = anisotropic filtering off. Valid range [1, 16].
    bool compare_enable = false;  ///< Shadow-map comparison sampling.
    CompareOp compare_op = CompareOp::Never;
    float min_lod = 0.0f;
    float max_lod = lod_clamp_none;
    BorderColor border_color = BorderColor::TransparentBlack;

    friend bool operator==(const SamplerDesc&, const SamplerDesc&) noexcept = default;
};

[[nodiscard]] ValidationResult validate(const SamplerDesc& desc) noexcept;

/// Stable hash for sampler deduplication (the bindless sampler table is small and shared).
[[nodiscard]] std::size_t hash_sampler_desc(const SamplerDesc& desc) noexcept;

} // namespace saige::rhi
