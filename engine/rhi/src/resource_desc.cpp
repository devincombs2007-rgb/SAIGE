#include "saige/rhi/resource_desc.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace saige::rhi {

namespace {

constexpr bool isValidSampleCount(uint32_t n) noexcept
{
    return n >= 1u && n <= 16u && std::has_single_bit(n);
}

} // namespace

ValidationResult validate(const BufferDesc& desc) noexcept
{
    if (desc.size == 0) {
        return {"Buffer size must be greater than zero"};
    }
    if (desc.usage == BufferUsage::None) {
        return {"Buffer usage must not be None"};
    }
    if (desc.structureStride % 4u != 0u) {
        return {"Buffer structureStride must be a multiple of 4"};
    }
    if (desc.structureStride != 0u && desc.size % desc.structureStride != 0u) {
        return {"Buffer size must be a multiple of structureStride"};
    }
    if (hasAny(desc.usage, BufferUsage::ShaderBindingTable | BufferUsage::AccelerationStructureStorage |
                               BufferUsage::AccelerationStructureInput) &&
        !hasAny(desc.usage, BufferUsage::DeviceAddress)) {
        return {"Ray tracing buffer usages require DeviceAddress"};
    }
    if (desc.memory == MemoryLocation::GpuToCpu &&
        hasAny(desc.usage, BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Indirect)) {
        return {"Readback (GpuToCpu) buffers cannot be used as vertex, index or indirect buffers"};
    }
    return {};
}

uint64_t computeTextureDataSize(const TextureDesc& desc) noexcept
{
    const uint32_t mips = resolveMipLevels(desc);
    const bool is3D = desc.dimension == TextureDimension::Texture3D;
    const Extent3D base{desc.width, desc.height, is3D ? desc.depth : 1u};
    const uint64_t layers = is3D ? 1u : desc.arrayLayers;

    uint64_t total = 0;
    for (uint32_t level = 0; level < mips; ++level) {
        const Extent3D e = mipExtent(base, level);
        const SurfaceLayout s = computeSurfaceLayout(desc.format, e.width, e.height);
        total += s.slicePitch * e.depth;
    }
    return total * layers * desc.sampleCount;
}

ValidationResult validate(const TextureDesc& desc) noexcept
{
    const FormatInfo& fmt = getFormatInfo(desc.format);

    if (desc.format == Format::Unknown) {
        return {"Texture format must not be Unknown"};
    }
    if (desc.usage == TextureUsage::None) {
        return {"Texture usage must not be None"};
    }
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.arrayLayers == 0) {
        return {"Texture extent and arrayLayers must be at least 1"};
    }

    switch (desc.dimension) {
    case TextureDimension::Texture1D:
        if (desc.height != 1 || desc.depth != 1) {
            return {"Texture1D requires height == 1 and depth == 1"};
        }
        break;
    case TextureDimension::Texture2D:
        if (desc.depth != 1) {
            return {"Texture2D requires depth == 1 (use arrayLayers for arrays)"};
        }
        break;
    case TextureDimension::Texture3D:
        if (desc.arrayLayers != 1) {
            return {"Texture3D cannot be an array"};
        }
        if (fmt.isDepthOrStencil()) {
            return {"Texture3D cannot use a depth/stencil format"};
        }
        break;
    case TextureDimension::TextureCube:
        if (desc.width != desc.height) {
            return {"TextureCube faces must be square"};
        }
        if (desc.depth != 1) {
            return {"TextureCube requires depth == 1"};
        }
        if (desc.arrayLayers % 6u != 0u) {
            return {"TextureCube arrayLayers must be a multiple of 6"};
        }
        break;
    }

    const uint32_t mips = resolveMipLevels(desc);
    const uint32_t maxMips = maxMipLevels(desc.width, desc.height,
                                          desc.dimension == TextureDimension::Texture3D ? desc.depth : 1u);
    if (mips > maxMips) {
        return {"Texture mipLevels exceeds the full mip chain length for its extent"};
    }

    if (!isValidSampleCount(desc.sampleCount)) {
        return {"Texture sampleCount must be 1, 2, 4, 8 or 16"};
    }
    if (desc.sampleCount > 1) {
        if (desc.dimension != TextureDimension::Texture2D) {
            return {"Multisampled textures must be Texture2D"};
        }
        if (mips != 1) {
            return {"Multisampled textures must have exactly one mip level"};
        }
        if (fmt.compressed) {
            return {"Multisampled textures cannot use block-compressed formats"};
        }
        if (hasAny(desc.usage, TextureUsage::Storage)) {
            return {"Multisampled storage textures are not supported"};
        }
    }

    if (fmt.compressed &&
        hasAny(desc.usage, TextureUsage::ColorAttachment | TextureUsage::DepthStencilAttachment | TextureUsage::Storage)) {
        return {"Block-compressed textures can only be sampled or used for transfers"};
    }
    if (fmt.isDepthOrStencil()) {
        if (hasAny(desc.usage, TextureUsage::ColorAttachment | TextureUsage::Storage)) {
            return {"Depth/stencil formats cannot be color attachments or storage textures"};
        }
    } else if (hasAny(desc.usage, TextureUsage::DepthStencilAttachment)) {
        return {"DepthStencilAttachment usage requires a depth/stencil format"};
    }
    if (fmt.isSrgb() && hasAny(desc.usage, TextureUsage::Storage)) {
        return {"sRGB formats cannot be storage textures (write a UNORM view instead)"};
    }
    return {};
}

ValidationResult validate(const SamplerDesc& desc) noexcept
{
    if (!(desc.maxAnisotropy >= 1.0f && desc.maxAnisotropy <= 16.0f)) {
        return {"Sampler maxAnisotropy must be in [1, 16]"};
    }
    if (!(desc.minLod <= desc.maxLod)) {
        return {"Sampler minLod must be <= maxLod"};
    }
    if (!std::isfinite(desc.mipLodBias)) {
        return {"Sampler mipLodBias must be finite"};
    }
    return {};
}

std::size_t hashSamplerDesc(const SamplerDesc& desc) noexcept
{
    // FNV-1a over the fields (not raw struct bytes, so padding never affects the hash).
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    auto mixFloat = [&mix](float f) {
        if (f == 0.0f) {
            f = 0.0f;  // canonicalize -0.0 so equal descs hash equally
        }
        mix(std::bit_cast<uint32_t>(f));
    };

    mix(static_cast<uint64_t>(desc.minFilter));
    mix(static_cast<uint64_t>(desc.magFilter));
    mix(static_cast<uint64_t>(desc.mipmapMode));
    mix(static_cast<uint64_t>(desc.addressU));
    mix(static_cast<uint64_t>(desc.addressV));
    mix(static_cast<uint64_t>(desc.addressW));
    mixFloat(desc.mipLodBias);
    mixFloat(desc.maxAnisotropy);
    mix(desc.compareEnable ? 1u : 0u);
    mix(static_cast<uint64_t>(desc.compareOp));
    mixFloat(desc.minLod);
    mixFloat(desc.maxLod);
    mix(static_cast<uint64_t>(desc.borderColor));
    return static_cast<std::size_t>(h);
}

} // namespace saige::rhi
