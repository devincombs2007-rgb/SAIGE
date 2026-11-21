// SAIGE RHI — resource descriptions (buffers, textures, samplers) and their validation.
#pragma once

#include "saige/rhi/flags.h"
#include "saige/rhi/format.h"

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
    uint32_t structureStride = 0;  ///< Element stride for structured views (0 = raw/byte-address).
    const char* debugName = nullptr;
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

/// Pass as `TextureDesc::mipLevels` to request a full mip chain down to 1x1(x1).
inline constexpr uint32_t kFullMipChain = 0;

struct TextureDesc {
    TextureDimension dimension = TextureDimension::Texture2D;
    Format format = Format::Unknown;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;        ///< Texture3D only.
    uint32_t arrayLayers = 1;  ///< For cubes: number of faces (a multiple of 6).
    uint32_t mipLevels = 1;    ///< `kFullMipChain` for a complete chain.
    uint32_t sampleCount = 1;
    TextureUsage usage = TextureUsage::None;
    const char* debugName = nullptr;

    [[nodiscard]] constexpr Extent3D extent() const noexcept { return {width, height, depth}; }
};

/// Number of mips in a full chain for the given base extent: floor(log2(max(w, h, d))) + 1.
[[nodiscard]] constexpr uint32_t maxMipLevels(uint32_t width, uint32_t height = 1, uint32_t depth = 1) noexcept
{
    uint32_t largest = width > height ? width : height;
    largest = largest > depth ? largest : depth;
    return largest == 0 ? 0u : static_cast<uint32_t>(std::bit_width(largest));
}

/// Extent of `level`, clamped to at least 1 in each dimension.
[[nodiscard]] constexpr Extent3D mipExtent(Extent3D base, uint32_t level) noexcept
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

/// Resolves `kFullMipChain` to a concrete count.
[[nodiscard]] constexpr uint32_t resolveMipLevels(const TextureDesc& desc) noexcept
{
    const uint32_t depth = desc.dimension == TextureDimension::Texture3D ? desc.depth : 1u;
    return desc.mipLevels == kFullMipChain ? maxMipLevels(desc.width, desc.height, depth) : desc.mipLevels;
}

/// Total tightly packed size of every mip, layer and slice. Used for staging and memory budgeting
/// (GPU allocations add alignment and padding on top of this).
[[nodiscard]] uint64_t computeTextureDataSize(const TextureDesc& desc) noexcept;

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

/// Pass as `SamplerDesc::maxLod` to leave the LOD unclamped.
inline constexpr float kLodClampNone = 1000.0f;

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    MipmapMode mipmapMode = MipmapMode::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    float mipLodBias = 0.0f;
    float maxAnisotropy = 1.0f;  ///< 1 = anisotropic filtering off. Valid range [1, 16].
    bool compareEnable = false;  ///< Shadow-map comparison sampling.
    CompareOp compareOp = CompareOp::Never;
    float minLod = 0.0f;
    float maxLod = kLodClampNone;
    BorderColor borderColor = BorderColor::TransparentBlack;

    friend bool operator==(const SamplerDesc&, const SamplerDesc&) noexcept = default;
};

[[nodiscard]] ValidationResult validate(const SamplerDesc& desc) noexcept;

/// Stable hash for sampler deduplication (the bindless sampler table is small and shared).
[[nodiscard]] std::size_t hashSamplerDesc(const SamplerDesc& desc) noexcept;

} // namespace saige::rhi
