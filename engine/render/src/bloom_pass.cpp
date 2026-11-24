#include "saige/render/bloom.hpp"

#include "saige/shaders/scene_bloom_down.spv.hpp"
#include "saige/shaders/scene_bloom_prefilter.spv.hpp"
#include "saige/shaders/scene_bloom_up.spv.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>

namespace saige::render {

namespace {

template <typename T, std::size_t N>
std::span<const uint32_t> words(const T (&a)[N])
{
    return {reinterpret_cast<const uint32_t*>(a), N * sizeof(T) / sizeof(uint32_t)};
}

struct DownPush {
    float texel[2];
    float threshold;
    float knee;
};
struct UpPush {
    float coarse_texel[2];
    float scatter;
    float pad;
};
static_assert(sizeof(DownPush) == 16 && sizeof(UpPush) == 16);

}  // namespace

BloomPass::BloomPass(rhi::Device& device, const BloomSettings& settings) : device_(device), settings_(settings)
{
    validate_bloom(settings_);
    try {
        rhi::BindGroupLayoutDesc ld;
        ld.entries = {{0, rhi::BindingType::SampledTexture}, {1, rhi::BindingType::Sampler}};
        ld.debug_name = "BloomDownLayout";
        down_layout_ = device_.create_bind_group_layout(ld);
        ld.entries = {{0, rhi::BindingType::SampledTexture}, {1, rhi::BindingType::SampledTexture}, {2, rhi::BindingType::Sampler}};
        ld.debug_name = "BloomUpLayout";
        up_layout_ = device_.create_bind_group_layout(ld);

        using namespace saige::shaders;
        vs_down_ = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_bloom_down_vs_main), "VSMain", "BloomDown.VS"});
        ps_down_ = device_.create_shader({rhi::ShaderStage::Pixel, words(scene_bloom_down_ps_main), "PSMain", "BloomDown.PS"});
        ps_prefilter_ = device_.create_shader({rhi::ShaderStage::Pixel, words(scene_bloom_prefilter_ps_main), "PSMain", "BloomPrefilter.PS"});
        vs_up_ = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_bloom_up_vs_main), "VSMain", "BloomUp.VS"});
        ps_up_ = device_.create_shader({rhi::ShaderStage::Pixel, words(scene_bloom_up_ps_main), "PSMain", "BloomUp.PS"});

        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs_down_;
        pd.pixel_shader = ps_down_;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {rhi::Format::RGBA16_FLOAT};
        pd.bind_group_layouts = {down_layout_};
        pd.push_constant_size = sizeof(DownPush);
        pd.debug_name = "BloomDown";
        down_pipeline_ = device_.create_graphics_pipeline(pd);
        pd.pixel_shader = ps_prefilter_;
        pd.debug_name = "BloomPrefilter";
        prefilter_pipeline_ = device_.create_graphics_pipeline(pd);
        pd.vertex_shader = vs_up_;
        pd.pixel_shader = ps_up_;
        pd.bind_group_layouts = {up_layout_};
        pd.push_constant_size = sizeof(UpPush);
        pd.debug_name = "BloomUp";
        up_pipeline_ = device_.create_graphics_pipeline(pd);

        rhi::SamplerDesc sd;  // bilinear, clamp to edge: the same sampling the CPU reference does
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

void BloomPass::release() noexcept
{
    const auto destroy_if = [this](auto& handle) {
        if (handle.is_valid()) {
            device_.destroy(handle);
            handle = {};
        }
    };
    destroy_if(sampler_);
    destroy_if(up_pipeline_);
    destroy_if(prefilter_pipeline_);
    destroy_if(down_pipeline_);
    destroy_if(ps_up_);
    destroy_if(vs_up_);
    destroy_if(ps_prefilter_);
    destroy_if(ps_down_);
    destroy_if(vs_down_);
    destroy_if(up_layout_);
    destroy_if(down_layout_);
}

BloomPass::~BloomPass()
{
    release();
}

rhi::TextureDesc BloomPass::level_desc(uint32_t source_width, uint32_t source_height, uint32_t level)
{
    rhi::TextureDesc d;
    d.format = rhi::Format::RGBA16_FLOAT;
    d.width = bloom_level_extent(source_width, level);
    d.height = bloom_level_extent(source_height, level);
    d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
    return d;
}

BloomChain BloomPass::create_chain(rhi::TextureHandle source, uint32_t width, uint32_t height, bool readable)
{
    BloomChain chain;
    chain.source_width = width;
    chain.source_height = height;
    const uint32_t levels = bloom_level_count(width, height, settings_.levels);
    if (levels == 0 || !source.is_valid()) {
        throw std::invalid_argument("BloomPass::create_chain: empty source");
    }
    chain.levels = levels;  // up_texture() reads it while the bind groups below are built
    try {
        for (uint32_t i = 0; i < levels; ++i) {
            rhi::TextureDesc d = level_desc(width, height, i);
            if (readable) d.usage = d.usage | rhi::TextureUsage::TransferSrc;
            d.debug_name = "BloomDown";
            chain.down.push_back(device_.create_texture(d));
        }
        for (uint32_t i = 0; i + 1 < levels; ++i) {
            rhi::TextureDesc d = level_desc(width, height, i);
            if (readable) d.usage = d.usage | rhi::TextureUsage::TransferSrc;
            d.debug_name = "BloomUp";
            chain.up.push_back(device_.create_texture(d));
        }
        for (uint32_t i = 0; i < levels; ++i) {
            rhi::BindGroupEntry e[2];
            e[0].binding = 0;
            e[0].texture = i == 0 ? source : chain.down[i - 1];
            e[1].binding = 1;
            e[1].sampler = sampler_;
            chain.down_groups.push_back(device_.create_bind_group(down_layout_, e));
        }
        for (uint32_t i = 0; i + 1 < levels; ++i) {
            rhi::BindGroupEntry e[3];
            e[0].binding = 0;
            e[0].texture = chain.down[i];
            e[1].binding = 1;
            e[1].texture = chain.up_texture(i + 1);
            e[2].binding = 2;
            e[2].sampler = sampler_;
            chain.up_groups.push_back(device_.create_bind_group(up_layout_, e));
        }
    } catch (...) {
        destroy_chain(chain);
        throw;
    }
    return chain;
}

void BloomPass::destroy_chain(BloomChain& chain) noexcept
{
    for (const rhi::BindGroupHandle g : chain.up_groups) device_.destroy(g);
    for (const rhi::BindGroupHandle g : chain.down_groups) device_.destroy(g);
    for (const rhi::TextureHandle t : chain.up) device_.destroy(t);
    for (const rhi::TextureHandle t : chain.down) device_.destroy(t);
    chain = {};
}

namespace {

void begin_level(rhi::CommandList& cmd, rhi::TextureHandle target, uint32_t w, uint32_t h, const char* label)
{
    rhi::RenderingDesc rd;
    rhi::ColorAttachment ca;
    ca.texture = target;
    ca.load = rhi::AttachmentLoadOp::DontCare;  // the full-screen triangle covers every pixel
    ca.store = rhi::AttachmentStoreOp::Store;
    rd.colors.push_back(ca);
    rd.area = {0, 0, w, h};
    cmd.begin_debug_label(label);
    cmd.begin_rendering(rd);
    cmd.set_viewport({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f});
    cmd.set_scissor({0, 0, w, h});
}

}  // namespace

void BloomPass::record_down(rhi::CommandList& cmd, const BloomChain& chain, uint32_t level) const
{
    const uint32_t w = bloom_level_extent(chain.source_width, level), h = bloom_level_extent(chain.source_height, level);
    const uint32_t sw = level == 0 ? chain.source_width : bloom_level_extent(chain.source_width, level - 1);
    const uint32_t sh = level == 0 ? chain.source_height : bloom_level_extent(chain.source_height, level - 1);
    begin_level(cmd, chain.down[level], w, h, level == 0 ? "BloomPrefilter" : "BloomDown");
    cmd.bind_pipeline(level == 0 ? prefilter_pipeline_ : down_pipeline_);
    cmd.bind_group(0, chain.down_groups[level]);
    DownPush push{{1.0f / static_cast<float>(sw), 1.0f / static_cast<float>(sh)}, settings_.threshold, settings_.threshold * settings_.knee};
    cmd.push_constants(std::as_bytes(std::span(&push, 1)), 0);
    cmd.draw(3);
    cmd.end_rendering();
    cmd.end_debug_label();
}

void BloomPass::record_up(rhi::CommandList& cmd, const BloomChain& chain, uint32_t level) const
{
    const uint32_t w = bloom_level_extent(chain.source_width, level), h = bloom_level_extent(chain.source_height, level);
    const uint32_t cw = bloom_level_extent(chain.source_width, level + 1), ch = bloom_level_extent(chain.source_height, level + 1);
    begin_level(cmd, chain.up[level], w, h, "BloomUp");
    cmd.bind_pipeline(up_pipeline_);
    cmd.bind_group(0, chain.up_groups[level]);
    UpPush push{{1.0f / static_cast<float>(cw), 1.0f / static_cast<float>(ch)}, settings_.scatter, 0.0f};
    cmd.push_constants(std::as_bytes(std::span(&push, 1)), 0);
    cmd.draw(3);
    cmd.end_rendering();
    cmd.end_debug_label();
}

}  // namespace saige::render
