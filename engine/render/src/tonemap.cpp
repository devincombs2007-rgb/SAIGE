#include "saige/render/tonemap.hpp"

#include "saige/shaders/scene_tonemap.spv.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>

namespace saige::render {

namespace {

constexpr float max_half = 65504.0f;

/// Mirrors `clean` in the shader.
float clean(float x) noexcept
{
    if (std::isnan(x)) {
        return 0.0f;
    }
    return std::min(std::max(x, 0.0f), max_half);
}

float saturate(float x) noexcept { return std::min(std::max(x, 0.0f), 1.0f); }

float curve(float c, const ToneMapSettings& s) noexcept
{
    switch (s.op) {
    case ToneMapOperator::Reinhard: {
        const float w2 = s.white_point * s.white_point;
        return saturate(c * (1.0f + c / w2) / (1.0f + c));
    }
    case ToneMapOperator::AcesFitted:
        return saturate((c * (2.51f * c + 0.03f)) / (c * (2.43f * c + 0.59f) + 0.14f));
    case ToneMapOperator::Linear:
        break;
    }
    return saturate(c);
}

struct Push {
    float exposure;
    uint32_t op;
    float white_point;
    float pad;
};
static_assert(sizeof(Push) == 16, "the shader's push block is 16 bytes");

template <typename T, std::size_t N>
std::span<const uint32_t> words(const T (&a)[N])
{
    return std::span<const uint32_t>(a, N);
}

} // namespace

float exposure_scale(float exposure_ev) noexcept
{
    return std::exp2(exposure_ev);
}

Float3 tone_map(Float3 c, const ToneMapSettings& settings) noexcept
{
    const float scale = exposure_scale(settings.exposure_ev);
    return {curve(clean(c.x * scale), settings), curve(clean(c.y * scale), settings), curve(clean(c.z * scale), settings)};
}

ToneMapper::ToneMapper(rhi::Device& device, rhi::Format target_format) : device_(device)
{
    rhi::BindGroupLayoutDesc ld;
    ld.entries = {{0, rhi::BindingType::SampledTexture}, {1, rhi::BindingType::Sampler}};
    ld.debug_name = "ToneMapLayout";
    layout_ = device_.create_bind_group_layout(ld);
    vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(shaders::scene_tone_map_vs_main), "VSMain", "ToneMap.VS"});
    ps_ = device_.create_shader({rhi::ShaderStage::Pixel, words(shaders::scene_tone_map_ps_main), "PSMain", "ToneMap.PS"});
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = vs_;
    pd.pixel_shader = ps_;
    pd.cull_mode = rhi::CullMode::None;
    pd.color_formats = {target_format};
    pd.bind_group_layouts = {layout_};
    pd.push_constant_size = sizeof(Push);
    pd.debug_name = "ToneMap";
    pipeline_ = device_.create_graphics_pipeline(pd);
    rhi::SamplerDesc sd;  // nearest, clamp: the pass maps pixels 1:1, so no filtering of HDR values is wanted
    sd.mag_filter = rhi::Filter::Nearest;
    sd.min_filter = rhi::Filter::Nearest;
    sd.mipmap_mode = rhi::MipmapMode::Nearest;
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    sampler_ = device_.create_sampler(sd);
}

ToneMapper::~ToneMapper()
{
    device_.destroy(sampler_);
    device_.destroy(pipeline_);
    device_.destroy(ps_);
    device_.destroy(vs_);
    device_.destroy(layout_);
}

rhi::BindGroupHandle ToneMapper::create_input(rhi::TextureHandle hdr)
{
    rhi::BindGroupEntry entries[2];
    entries[0].binding = 0;
    entries[0].texture = hdr;
    entries[1].binding = 1;
    entries[1].sampler = sampler_;
    return device_.create_bind_group(layout_, entries);
}

void ToneMapper::draw(rhi::CommandList& cmd, rhi::BindGroupHandle input, const ToneMapSettings& settings)
{
    cmd.bind_pipeline(pipeline_);
    cmd.bind_group(0, input);
    Push push{exposure_scale(settings.exposure_ev), static_cast<uint32_t>(settings.op), settings.white_point, 0.0f};
    cmd.push_constants(std::as_bytes(std::span(&push, 1)), 0);
    cmd.draw(3);
}

} // namespace saige::render
