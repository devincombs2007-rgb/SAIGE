#include "saige/rhi/resource_desc.hpp"

#include <bit>
#include <cmath>
#include <cstring>

namespace saige::rhi {

namespace {

constexpr bool is_valid_sample_count(uint32_t n) noexcept
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
    if (desc.structure_stride % 4u != 0u) {
        return {"Buffer structure_stride must be a multiple of 4"};
    }
    if (desc.structure_stride != 0u && desc.size % desc.structure_stride != 0u) {
        return {"Buffer size must be a multiple of structure_stride"};
    }
    if (has_any(desc.usage, BufferUsage::ShaderBindingTable | BufferUsage::AccelerationStructureStorage |
                               BufferUsage::AccelerationStructureInput) &&
        !has_any(desc.usage, BufferUsage::DeviceAddress)) {
        return {"Ray tracing buffer usages require DeviceAddress"};
    }
    if (desc.memory == MemoryLocation::GpuToCpu &&
        has_any(desc.usage, BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Indirect)) {
        return {"Readback (GpuToCpu) buffers cannot be used as vertex, index or indirect buffers"};
    }
    return {};
}

uint64_t compute_texture_data_size(const TextureDesc& desc) noexcept
{
    const uint32_t mips = resolve_mip_levels(desc);
    const bool is_3d = desc.dimension == TextureDimension::Texture3D;
    const Extent3D base{desc.width, desc.height, is_3d ? desc.depth : 1u};
    const uint64_t layers = is_3d ? 1u : desc.array_layers;

    uint64_t total = 0;
    for (uint32_t level = 0; level < mips; ++level) {
        const Extent3D e = mip_extent(base, level);
        const SurfaceLayout s = compute_surface_layout(desc.format, e.width, e.height);
        total += s.slice_pitch * e.depth;
    }
    return total * layers * desc.sample_count;
}

ValidationResult validate(const TextureDesc& desc) noexcept
{
    const FormatInfo& fmt = get_format_info(desc.format);

    if (desc.format == Format::Unknown) {
        return {"Texture format must not be Unknown"};
    }
    if (desc.usage == TextureUsage::None) {
        return {"Texture usage must not be None"};
    }
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.array_layers == 0) {
        return {"Texture extent and array_layers must be at least 1"};
    }

    switch (desc.dimension) {
    case TextureDimension::Texture1D:
        if (desc.height != 1 || desc.depth != 1) {
            return {"Texture1D requires height == 1 and depth == 1"};
        }
        break;
    case TextureDimension::Texture2D:
        if (desc.depth != 1) {
            return {"Texture2D requires depth == 1 (use array_layers for arrays)"};
        }
        break;
    case TextureDimension::Texture3D:
        if (desc.array_layers != 1) {
            return {"Texture3D cannot be an array"};
        }
        if (fmt.is_depth_or_stencil()) {
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
        if (desc.array_layers % 6u != 0u) {
            return {"TextureCube array_layers must be a multiple of 6"};
        }
        break;
    }

    const uint32_t mips = resolve_mip_levels(desc);
    const uint32_t max_mips = max_mip_levels(desc.width, desc.height,
                                          desc.dimension == TextureDimension::Texture3D ? desc.depth : 1u);
    if (mips > max_mips) {
        return {"Texture mip_levels exceeds the full mip chain length for its extent"};
    }

    if (!is_valid_sample_count(desc.sample_count)) {
        return {"Texture sample_count must be 1, 2, 4, 8 or 16"};
    }
    if (desc.sample_count > 1) {
        if (desc.dimension != TextureDimension::Texture2D) {
            return {"Multisampled textures must be Texture2D"};
        }
        if (mips != 1) {
            return {"Multisampled textures must have exactly one mip level"};
        }
        if (fmt.compressed) {
            return {"Multisampled textures cannot use block-compressed formats"};
        }
        if (has_any(desc.usage, TextureUsage::Storage)) {
            return {"Multisampled storage textures are not supported"};
        }
    }

    if (fmt.compressed &&
        has_any(desc.usage, TextureUsage::ColorAttachment | TextureUsage::DepthStencilAttachment | TextureUsage::Storage)) {
        return {"Block-compressed textures can only be sampled or used for transfers"};
    }
    if (fmt.is_depth_or_stencil()) {
        if (has_any(desc.usage, TextureUsage::ColorAttachment | TextureUsage::Storage)) {
            return {"Depth/stencil formats cannot be color attachments or storage textures"};
        }
    } else if (has_any(desc.usage, TextureUsage::DepthStencilAttachment)) {
        return {"DepthStencilAttachment usage requires a depth/stencil format"};
    }
    if (fmt.is_srgb() && has_any(desc.usage, TextureUsage::Storage)) {
        return {"s_rgb formats cannot be storage textures (write a UNORM view instead)"};
    }
    return {};
}

ValidationResult validate(const SamplerDesc& desc) noexcept
{
    if (!(desc.max_anisotropy >= 1.0f && desc.max_anisotropy <= 16.0f)) {
        return {"Sampler max_anisotropy must be in [1, 16]"};
    }
    if (!(desc.min_lod <= desc.max_lod)) {
        return {"Sampler min_lod must be <= max_lod"};
    }
    if (!std::isfinite(desc.mip_lod_bias)) {
        return {"Sampler mip_lod_bias must be finite"};
    }
    return {};
}

std::size_t hash_sampler_desc(const SamplerDesc& desc) noexcept
{
    // FNV-1a over the fields (not raw struct bytes, so padding never affects the hash).
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    auto mix_float = [&mix](float f) {
        if (f == 0.0f) {
            f = 0.0f;  // canonicalize -0.0 so equal descs hash equally
        }
        mix(std::bit_cast<uint32_t>(f));
    };

    mix(static_cast<uint64_t>(desc.min_filter));
    mix(static_cast<uint64_t>(desc.mag_filter));
    mix(static_cast<uint64_t>(desc.mipmap_mode));
    mix(static_cast<uint64_t>(desc.address_u));
    mix(static_cast<uint64_t>(desc.address_v));
    mix(static_cast<uint64_t>(desc.address_w));
    mix_float(desc.mip_lod_bias);
    mix_float(desc.max_anisotropy);
    mix(desc.compare_enable ? 1u : 0u);
    mix(static_cast<uint64_t>(desc.compare_op));
    mix_float(desc.min_lod);
    mix_float(desc.max_lod);
    mix(static_cast<uint64_t>(desc.border_color));
    return static_cast<std::size_t>(h);
}

} // namespace saige::rhi
