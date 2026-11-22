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

ValidationResult validate_texture_upload(const TextureDesc& desc, uint64_t data_size) noexcept
{
    if (desc.dimension != TextureDimension::Texture2D || desc.sample_count != 1) {
        return {"upload_texture: only single-sample 2D textures can be uploaded"};
    }
    if (is_depth_or_stencil(desc.format)) {
        return {"upload_texture: depth/stencil textures cannot be uploaded (color formats only)"};
    }
    if (!has_all(desc.usage, TextureUsage::Sampled | TextureUsage::TransferDst)) {
        return {"upload_texture: the texture needs Sampled | TransferDst usage"};
    }
    if (data_size != compute_texture_data_size(desc)) {
        return {"upload_texture: data size does not match the texture"};
    }
    return {};
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

ValidationResult validate(const BindGroupLayoutDesc& desc) noexcept
{
    if (desc.entries.empty() || desc.entries.size() > max_bindings_per_group) {
        return {"A bind group layout needs between 1 and 16 entries"};
    }
    for (std::size_t i = 0; i < desc.entries.size(); ++i) {
        for (std::size_t j = i + 1; j < desc.entries.size(); ++j) {
            if (desc.entries[i].binding == desc.entries[j].binding) {
                return {"A bind group layout must not repeat a binding number"};
            }
        }
    }
    return {};
}

ValidationResult validate_pipeline_bindings(std::span<const BindGroupLayoutDesc* const> layouts) noexcept
{
    uint32_t textures = 0;
    uint32_t samplers = 0;
    uint32_t uniforms = 0;
    uint32_t storages = 0;
    for (const BindGroupLayoutDesc* layout : layouts) {
        for (const BindGroupLayoutEntry& e : layout->entries) {
            switch (e.type) {
            case BindingType::SampledTexture: ++textures; break;
            case BindingType::Sampler: ++samplers; break;
            case BindingType::UniformBuffer: ++uniforms; break;
            case BindingType::StorageBuffer: ++storages; break;
            }
        }
    }
    if (textures > max_pipeline_sampled_textures) {
        return {"A pipeline's bind group layouts exceed the portable total of 16 sampled textures"};
    }
    if (samplers > max_pipeline_samplers) {
        return {"A pipeline's bind group layouts exceed the portable total of 16 samplers"};
    }
    if (uniforms > max_pipeline_uniform_buffers) {
        return {"A pipeline's bind group layouts exceed the portable total of 12 uniform buffers"};
    }
    if (storages > max_pipeline_storage_buffers) {
        return {"A pipeline's bind group layouts exceed the portable total of 4 storage buffers"};
    }
    return {};
}

ValidationResult validate(const BindGroupLayoutDesc& layout, std::span<const BindGroupEntry> entries,
                          const BindGroupResolver& resolver)
{
    if (entries.size() != layout.entries.size()) {
        return {"A bind group needs exactly one entry for every layout binding"};
    }
    for (const BindGroupLayoutEntry& expected : layout.entries) {
        const BindGroupEntry* found = nullptr;
        for (const BindGroupEntry& e : entries) {
            if (e.binding == expected.binding) {
                if (found != nullptr) {
                    return {"A bind group entry binding is repeated"};
                }
                found = &e;
            }
        }
        if (found == nullptr) {
            return {"A bind group has no entry for one of its layout bindings"};
        }
        if (expected.type == BindingType::SampledTexture) {
            const std::optional<TextureDesc> t = resolver.texture(found->texture);
            if (!t) {
                return {"A sampled-texture binding refers to an invalid or destroyed texture"};
            }
            if (!has_any(t->usage, TextureUsage::Sampled)) {
                return {"A sampled-texture binding needs a texture with Sampled usage"};
            }
            if (t->sample_count != 1) {
                return {"A sampled-texture binding does not accept multisampled textures"};
            }
        } else if (expected.type == BindingType::Sampler) {
            if (!resolver.sampler_exists(found->sampler)) {
                return {"A sampler binding refers to an invalid or destroyed sampler"};
            }
        } else {
            const std::optional<BufferDesc> b = resolver.buffer(found->buffer);
            if (!b) {
                return {"A buffer binding refers to an invalid or destroyed buffer"};
            }
            const BufferUsage need = expected.type == BindingType::UniformBuffer ? BufferUsage::Uniform : BufferUsage::Storage;
            if (!has_any(b->usage, need)) {
                return {"A buffer binding needs a buffer with Uniform or Storage usage matching the binding type"};
            }
            if (found->offset % bind_group_buffer_alignment != 0) {
                return {"A buffer binding offset must be a multiple of 256 bytes"};
            }
            if (found->offset >= b->size) {
                return {"A buffer binding offset is outside the buffer"};
            }
            const uint64_t available = b->size - found->offset;
            if (found->size == 0 || (found->size != ~uint64_t{0} && found->size > available)) {
                return {"A buffer binding range is empty or extends past the end of the buffer"};
            }
            const uint64_t range = found->size == ~uint64_t{0} ? available : found->size;
            const uint64_t limit = expected.type == BindingType::UniformBuffer ? max_uniform_binding_range : max_storage_binding_range;
            if (range > limit) {
                return {"A buffer binding range exceeds the portable per-binding limit (16 KiB uniform, 128 MiB storage)"};
            }
        }
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
