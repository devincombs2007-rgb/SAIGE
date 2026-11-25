#include "saige/render/fxaa.hpp"

#include "saige/shaders/scene_fxaa.spv.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>

namespace saige::render {

namespace {

template <typename T, std::size_t N>
std::span<const uint32_t> words(const T (&a)[N])
{
    return {reinterpret_cast<const uint32_t*>(a), N * sizeof(T) / sizeof(uint32_t)};
}

struct Push {
    float texel[2];
    float edge_threshold;
    float edge_threshold_min;
    float span_max;
    float luma_exponent;
    float pad[2];
};
static_assert(sizeof(Push) == 32);

Float3 sample(const FxaaImage& image, float u, float v)
{
    const float fw = static_cast<float>(image.width), fh = static_cast<float>(image.height);
    const float x = u * fw - 0.5f, y = v * fh - 0.5f;
    const float x0f = std::floor(x), y0f = std::floor(y);
    const float tx = x - x0f, ty = y - y0f;
    const auto texel = [&](float i, float n) { return static_cast<uint32_t>(std::min(std::max(i, 0.0f), n - 1.0f)); };
    const uint32_t xa = texel(x0f, fw), xb = texel(x0f + 1.0f, fw), ya = texel(y0f, fh), yb = texel(y0f + 1.0f, fh);
    const Float3 top = image.at(xa, ya) * (1.0f - tx) + image.at(xb, ya) * tx;
    const Float3 bottom = image.at(xa, yb) * (1.0f - tx) + image.at(xb, yb) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

}  // namespace

void validate_fxaa(const FxaaSettings& s)
{
    const auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!in(s.edge_threshold, 0.0f, 1.0f)) throw std::invalid_argument("FxaaSettings: the edge threshold must be finite and in 0 to 1");
    if (!in(s.edge_threshold_min, 0.0f, 1.0f)) throw std::invalid_argument("FxaaSettings: the minimum edge threshold must be finite and in 0 to 1");
    if (!in(s.span_max, 1.0f, 32.0f)) throw std::invalid_argument("FxaaSettings: the span maximum must be finite and in 1 to 32");
}

float fxaa_luma(Float3 c, float luma_exponent) noexcept
{
    const float r = std::pow(std::max(c.x, 0.0f), luma_exponent), g = std::pow(std::max(c.y, 0.0f), luma_exponent), b = std::pow(std::max(c.z, 0.0f), luma_exponent);
    return r * 0.299f + g * 0.587f + b * 0.114f;
}

FxaaImage fxaa_apply(const FxaaImage& source, const FxaaSettings& settings, float luma_exponent)
{
    validate_fxaa(settings);
    if (source.width == 0 || source.height == 0) throw std::invalid_argument("fxaa_apply: empty image");
    FxaaImage out(source.width, source.height);
    const float tx = 1.0f / static_cast<float>(source.width), ty = 1.0f / static_cast<float>(source.height);
    for (uint32_t y = 0; y < source.height; ++y) {
        for (uint32_t x = 0; x < source.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(source.width), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(source.height);
            const auto fetch = [&](float ox, float oy) { return sample(source, u + ox * tx, v + oy * ty); };
            const Float3 m = fetch(0, 0), nw = fetch(-1, -1), ne = fetch(1, -1), sw = fetch(-1, 1), se = fetch(1, 1);
            const float lm = fxaa_luma(m, luma_exponent), lnw = fxaa_luma(nw, luma_exponent), lne = fxaa_luma(ne, luma_exponent), lsw = fxaa_luma(sw, luma_exponent),
                        lse = fxaa_luma(se, luma_exponent);
            const float lmin = std::min(lm, std::min(std::min(lnw, lne), std::min(lsw, lse)));
            const float lmax = std::max(lm, std::max(std::max(lnw, lne), std::max(lsw, lse)));
            if (lmax - lmin < std::max(settings.edge_threshold_min, lmax * settings.edge_threshold)) {
                out.at(x, y) = m;
                continue;
            }
            float dx = -((lnw + lne) - (lsw + lse));
            float dy = (lnw + lsw) - (lne + lse);
            const float reduce = std::max((lnw + lne + lsw + lse) * (0.25f * (1.0f / 8.0f)), 1.0f / 128.0f);
            const float rcp = 1.0f / (std::min(std::fabs(dx), std::fabs(dy)) + reduce);
            dx = std::min(std::max(dx * rcp, -settings.span_max), settings.span_max) * tx;
            dy = std::min(std::max(dy * rcp, -settings.span_max), settings.span_max) * ty;
            const auto along = [&](float t) { return sample(source, u + dx * t, v + dy * t); };
            const Float3 a = (along(1.0f / 3.0f - 0.5f) + along(2.0f / 3.0f - 0.5f)) * 0.5f;
            const Float3 b = a * 0.5f + (along(-0.5f) + along(0.5f)) * 0.25f;
            const float lb = fxaa_luma(b, luma_exponent);
            out.at(x, y) = (lb < lmin || lb > lmax) ? a : b;
        }
    }
    return out;
}

FxaaPass::FxaaPass(rhi::Device& device, rhi::Format target_format) : device_(device), luma_exponent_(rhi::is_srgb(target_format) ? 0.5f : 1.0f)
{
    try {
        rhi::BindGroupLayoutDesc ld;
        ld.entries = {{0, rhi::BindingType::SampledTexture}, {1, rhi::BindingType::Sampler}};
        ld.debug_name = "FxaaLayout";
        layout_ = device_.create_bind_group_layout(ld);
        vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(shaders::scene_fxaa_vs_main), "VSMain", "Fxaa.VS"});
        ps_ = device_.create_shader({rhi::ShaderStage::Pixel, words(shaders::scene_fxaa_ps_main), "PSMain", "Fxaa.PS"});
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs_;
        pd.pixel_shader = ps_;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {target_format};
        pd.bind_group_layouts = {layout_};
        pd.push_constant_size = sizeof(Push);
        pd.debug_name = "Fxaa";
        pipeline_ = device_.create_graphics_pipeline(pd);
        rhi::SamplerDesc sd;  // bilinear, clamp to edge: the sampling the reference does
        sd.mag_filter = rhi::Filter::Linear;
        sd.min_filter = rhi::Filter::Linear;
        sd.mipmap_mode = rhi::MipmapMode::Nearest;
        sd.address_u = rhi::AddressMode::ClampToEdge;
        sd.address_v = rhi::AddressMode::ClampToEdge;
        sampler_ = device_.create_sampler(sd);
    } catch (...) {
        release();
        throw;
    }
}

void FxaaPass::release() noexcept
{
    const auto destroy_if = [this](auto& handle) {
        if (handle.is_valid()) {
            device_.destroy(handle);
            handle = {};
        }
    };
    destroy_if(sampler_);
    destroy_if(pipeline_);
    destroy_if(ps_);
    destroy_if(vs_);
    destroy_if(layout_);
}

FxaaPass::~FxaaPass()
{
    release();
}

rhi::BindGroupHandle FxaaPass::create_input(rhi::TextureHandle color)
{
    rhi::BindGroupEntry entries[2];
    entries[0].binding = 0;
    entries[0].texture = color;
    entries[1].binding = 1;
    entries[1].sampler = sampler_;
    return device_.create_bind_group(layout_, entries);
}

void FxaaPass::draw(rhi::CommandList& cmd, rhi::BindGroupHandle input, const FxaaSettings& settings, uint32_t width, uint32_t height)
{
    cmd.bind_pipeline(pipeline_);
    cmd.bind_group(0, input);
    Push push{{1.0f / static_cast<float>(std::max(width, 1u)), 1.0f / static_cast<float>(std::max(height, 1u))}, settings.edge_threshold, settings.edge_threshold_min, settings.span_max,
              luma_exponent_, {0.0f, 0.0f}};
    cmd.push_constants(std::as_bytes(std::span(&push, 1)), 0);
    cmd.draw(3);
}

} // namespace saige::render
