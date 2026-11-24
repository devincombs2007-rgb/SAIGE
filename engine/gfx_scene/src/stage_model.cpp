#include "saige/gfx_scene/stage_model.hpp"

#include "saige/gfx_scene/prepared_model.hpp"

#include <algorithm>

namespace saige::gfx {

rhi::SamplerDesc to_sampler_desc(const Sampler& s, float max_anisotropy)
{
    rhi::SamplerDesc d;
    d.mag_filter = s.mag_filter == 9728 ? rhi::Filter::Nearest : rhi::Filter::Linear;
    bool mips = true;
    switch (s.min_filter) {
    case 9728: d.min_filter = rhi::Filter::Nearest; mips = false; break;
    case 9729: d.min_filter = rhi::Filter::Linear; mips = false; break;
    case 9984: d.min_filter = rhi::Filter::Nearest; d.mipmap_mode = rhi::MipmapMode::Nearest; break;
    case 9985: d.min_filter = rhi::Filter::Linear; d.mipmap_mode = rhi::MipmapMode::Nearest; break;
    case 9986: d.min_filter = rhi::Filter::Nearest; d.mipmap_mode = rhi::MipmapMode::Linear; break;
    case 9987:
    default: d.min_filter = rhi::Filter::Linear; d.mipmap_mode = rhi::MipmapMode::Linear; break;
    }
    if (!mips) {
        d.max_lod = 0.0f;  // a min filter without mipmaps samples only the base level
    }
    const auto wrap = [](uint32_t mode) {
        switch (mode) {
        case 33071: return rhi::AddressMode::ClampToEdge;
        case 33648: return rhi::AddressMode::MirroredRepeat;
        default: return rhi::AddressMode::Repeat;  // 10497, and anything unknown
        }
    };
    d.address_u = wrap(s.wrap_s);
    d.address_v = wrap(s.wrap_t);
    if (mips && d.min_filter == rhi::Filter::Linear && d.mag_filter == rhi::Filter::Linear && max_anisotropy > 1.0f) {
        d.max_anisotropy = std::min(max_anisotropy, 16.0f);
    }
    return d;
}

render::GpuModel stage_model(render::SceneRenderer& renderer, const GltfAsset& asset, std::string_view name, const StageSettings& settings, StageReport* report_out)
{
    const PreparedModel prepared = prepare_model(asset, name, capabilities_of(renderer), settings);
    return upload_model(renderer, prepared, report_out);
}

} // namespace saige::gfx
