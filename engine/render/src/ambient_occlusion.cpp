#include "saige/render/ambient_occlusion.hpp"

#include "saige/shaders/scene_ao_apply.spv.hpp"
#include "saige/shaders/scene_ao_generate.spv.hpp"

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
    float width, height, near_plane, tan_x, tan_y, radius, intensity, bias, focal_px, pad[3];
};
static_assert(sizeof(Push) == 48);

constexpr float sky_z = 1.0e6f;
constexpr float bayer[16] = {0.0f, 8.0f, 2.0f, 10.0f, 12.0f, 4.0f, 14.0f, 6.0f, 3.0f, 11.0f, 1.0f, 9.0f, 15.0f, 7.0f, 13.0f, 5.0f};
constexpr float golden_angle = 2.3999632f;
constexpr float two_pi = 6.2831853f;

struct Ints {
    int x, y;
};

float depth_at(const AoDepthImage& img, Ints p)
{
    const int x = std::min(std::max(p.x, 0), static_cast<int>(img.width) - 1);
    const int y = std::min(std::max(p.y, 0), static_cast<int>(img.height) - 1);
    return img.at(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
}

float view_z(float d, float near_plane) { return d > 0.0f ? near_plane / d : sky_z; }

Float3 view_pos(Ints p, float z, const AoView& v, float tan_x)
{
    const float u = (static_cast<float>(p.x) + 0.5f) / static_cast<float>(v.width);
    const float w = (static_cast<float>(p.y) + 0.5f) / static_cast<float>(v.height);
    return {(2.0f * u - 1.0f) * tan_x * z, (1.0f - 2.0f * w) * v.tan_half_fov_y * z, -z};
}

float tan_x_of(const AoView& v) { return v.tan_half_fov_y * static_cast<float>(v.width) / static_cast<float>(v.height); }

void check(const AoDepthImage& depth, const AoView& view)
{
    if (view.width == 0 || view.height == 0 || depth.width != view.width || depth.height != view.height) {
        throw std::invalid_argument("ambient occlusion: the depth image must match the view's non-zero size");
    }
    if (!(view.near_plane > 0.0f) || !std::isfinite(view.near_plane) || !(view.tan_half_fov_y > 0.0f) || !std::isfinite(view.tan_half_fov_y)) {
        throw std::invalid_argument("ambient occlusion: the view's near plane and field of view must be finite and positive");
    }
}

Push make_push(const AoView& view, const AmbientOcclusionSettings& s)
{
    Push p{};
    p.width = static_cast<float>(view.width);
    p.height = static_cast<float>(view.height);
    p.near_plane = view.near_plane;
    p.tan_x = tan_x_of(view);
    p.tan_y = view.tan_half_fov_y;
    p.radius = s.radius;
    p.intensity = s.intensity;
    p.bias = s.bias * s.radius;
    p.focal_px = static_cast<float>(view.height) * 0.5f / view.tan_half_fov_y;
    return p;
}

}  // namespace

void validate_ambient_occlusion(const AmbientOcclusionSettings& s)
{
    const auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!in(s.radius, 0.01f, 16.0f)) throw std::invalid_argument("AmbientOcclusionSettings: the radius must be finite and in 0.01 to 16 meters");
    if (!in(s.intensity, 0.0f, 4.0f)) throw std::invalid_argument("AmbientOcclusionSettings: the intensity must be finite and in 0 to 4");
    if (!in(s.bias, 0.0f, 0.5f)) throw std::invalid_argument("AmbientOcclusionSettings: the bias must be finite and in 0 to 0.5");
}

AoImage ao_generate(const AoDepthImage& depth, const AoView& view, const AmbientOcclusionSettings& settings)
{
    validate_ambient_occlusion(settings);
    check(depth, view);
    const Push pc = make_push(view, settings);
    const float tan_x = pc.tan_x;
    AoImage out(view.width, view.height);
    for (uint32_t y = 0; y < view.height; ++y) {
        for (uint32_t x = 0; x < view.width; ++x) {
            const Ints p{static_cast<int>(x), static_cast<int>(y)};
            const float d = depth.at(x, y);
            if (d <= 0.0f) continue;  // sky: open
            const float zc = view_z(d, view.near_plane);
            const Float3 pc3 = view_pos(p, zc, view, tan_x);
            const float zr = view_z(depth_at(depth, {p.x + 1, p.y}), view.near_plane), zl = view_z(depth_at(depth, {p.x - 1, p.y}), view.near_plane);
            const float zd = view_z(depth_at(depth, {p.x, p.y + 1}), view.near_plane), zu = view_z(depth_at(depth, {p.x, p.y - 1}), view.near_plane);
            const Float3 dx = std::fabs(zr - zc) < std::fabs(zc - zl) ? view_pos({p.x + 1, p.y}, zr, view, tan_x) - pc3 : pc3 - view_pos({p.x - 1, p.y}, zl, view, tan_x);
            const Float3 dy = std::fabs(zd - zc) < std::fabs(zc - zu) ? view_pos({p.x, p.y + 1}, zd, view, tan_x) - pc3 : pc3 - view_pos({p.x, p.y - 1}, zu, view, tan_x);
            const Float3 nc = cross(dy, dx);
            const float nl = length(nc);
            const Float3 n = nl > 0.0f ? nc * (1.0f / nl) : Float3{0.0f, 0.0f, 1.0f};

            const float r_px = std::min(settings.radius * pc.focal_px / zc, ao_max_radius_pixels);
            const float phi = two_pi * bayer[(y & 3u) * 4u + (x & 3u)] / 16.0f;
            const float r2 = settings.radius * settings.radius;
            float occ = 0.0f;
            for (uint32_t i = 0; i < ao_sample_count; ++i) {
                const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(ao_sample_count);
                const float a = phi + static_cast<float>(i) * golden_angle;
                const Ints s{p.x + static_cast<int>(std::floor(std::cos(a) * t * r_px + 0.5f)), p.y + static_cast<int>(std::floor(std::sin(a) * t * r_px + 0.5f))};
                const float ds = depth_at(depth, s);
                if (ds <= 0.0f) continue;  // the sky occludes nothing
                const Ints sc{std::min(std::max(s.x, 0), static_cast<int>(view.width) - 1), std::min(std::max(s.y, 0), static_cast<int>(view.height) - 1)};
                const Float3 v = view_pos(sc, view_z(ds, view.near_plane), view, tan_x) - pc3;
                const float d2 = dot(v, v);
                const float f = std::max(0.0f, dot(v, n) - pc.bias);
                const float w = std::min(std::max(1.0f - d2 / r2, 0.0f), 1.0f);
                occ += f * w * settings.radius / (d2 + 0.01f * r2);
            }
            out.at(x, y) = std::min(std::max(1.0f - 2.0f * settings.intensity * occ / static_cast<float>(ao_sample_count), 0.0f), 1.0f);
        }
    }
    return out;
}

AoImage ao_blur(const AoImage& ao, const AoDepthImage& depth, const AoView& view, const AmbientOcclusionSettings& settings)
{
    validate_ambient_occlusion(settings);
    check(depth, view);
    if (ao.width != view.width || ao.height != view.height) throw std::invalid_argument("ao_blur: the occlusion image must match the view's size");
    AoImage out(view.width, view.height);
    for (uint32_t y = 0; y < view.height; ++y) {
        for (uint32_t x = 0; x < view.width; ++x) {
            const float d = depth.at(x, y);
            if (d <= 0.0f) continue;
            const float zc = view.near_plane / d;
            float sum = 0.0f, weight = 0.0f;
            for (int dy = -ao_blur_radius; dy <= ao_blur_radius; ++dy) {
                for (int dx = -ao_blur_radius; dx <= ao_blur_radius; ++dx) {
                    const int qx = std::min(std::max(static_cast<int>(x) + dx, 0), static_cast<int>(view.width) - 1);
                    const int qy = std::min(std::max(static_cast<int>(y) + dy, 0), static_cast<int>(view.height) - 1);
                    const float dq = depth.at(static_cast<uint32_t>(qx), static_cast<uint32_t>(qy));
                    if (dq <= 0.0f) continue;
                    const float zq = view.near_plane / dq;
                    const float spatial = 1.0f / (1.0f + static_cast<float>(dx * dx + dy * dy));
                    const float range = std::min(std::max(1.0f - std::fabs(zq - zc) / (0.5f * settings.radius), 0.0f), 1.0f);
                    const float w = spatial * range;
                    sum += w * ao.at(static_cast<uint32_t>(qx), static_cast<uint32_t>(qy));
                    weight += w;
                }
            }
            out.at(x, y) = weight > 0.0f ? sum / weight : 1.0f;
        }
    }
    return out;
}

AmbientOcclusionPass::AmbientOcclusionPass(rhi::Device& device) : device_(device)
{
    try {
        rhi::BindGroupLayoutDesc gl;
        gl.entries = {{0, rhi::BindingType::SampledTexture}};
        gl.debug_name = "AoGenerateLayout";
        generate_layout_ = device_.create_bind_group_layout(gl);
        rhi::BindGroupLayoutDesc al;
        al.entries = {{0, rhi::BindingType::SampledTexture}, {1, rhi::BindingType::SampledTexture}, {2, rhi::BindingType::SampledTexture}};
        al.debug_name = "AoApplyLayout";
        apply_layout_ = device_.create_bind_group_layout(al);
        vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(shaders::scene_ao_generate_vs_main), "VSMain", "AoGenerate.VS"});
        generate_ps_ = device_.create_shader({rhi::ShaderStage::Pixel, words(shaders::scene_ao_generate_ps_main), "PSMain", "AoGenerate.PS"});
        apply_ps_ = device_.create_shader({rhi::ShaderStage::Pixel, words(shaders::scene_ao_apply_ps_main), "PSMain", "AoApply.PS"});
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs_;
        pd.pixel_shader = generate_ps_;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {ao_format};
        pd.bind_group_layouts = {generate_layout_};
        pd.push_constant_size = sizeof(Push);
        pd.debug_name = "AoGenerate";
        generate_pipeline_ = device_.create_graphics_pipeline(pd);
    } catch (...) {
        release();
        throw;
    }
}

void AmbientOcclusionPass::set_apply_format(rhi::Format format)
{
    if (apply_pipeline_.is_valid()) return;
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = vs_;
    pd.pixel_shader = apply_ps_;
    pd.cull_mode = rhi::CullMode::None;
    pd.color_formats = {format};
    pd.bind_group_layouts = {apply_layout_};
    pd.push_constant_size = sizeof(Push);
    pd.debug_name = "AoApply";
    apply_pipeline_ = device_.create_graphics_pipeline(pd);
}

void AmbientOcclusionPass::release() noexcept
{
    const auto destroy_if = [this](auto& handle) {
        if (handle.is_valid()) {
            device_.destroy(handle);
            handle = {};
        }
    };
    destroy_if(apply_pipeline_);
    destroy_if(generate_pipeline_);
    destroy_if(apply_ps_);
    destroy_if(generate_ps_);
    destroy_if(vs_);
    destroy_if(apply_layout_);
    destroy_if(generate_layout_);
}

AmbientOcclusionPass::~AmbientOcclusionPass()
{
    release();
}

rhi::BindGroupHandle AmbientOcclusionPass::create_generate_input(rhi::TextureHandle depth)
{
    rhi::BindGroupEntry e;
    e.binding = 0;
    e.texture = depth;
    return device_.create_bind_group(generate_layout_, std::span(&e, 1));
}

rhi::BindGroupHandle AmbientOcclusionPass::create_apply_input(rhi::TextureHandle color, rhi::TextureHandle ao, rhi::TextureHandle depth)
{
    rhi::BindGroupEntry e[3];
    e[0].binding = 0;
    e[0].texture = color;
    e[1].binding = 1;
    e[1].texture = ao;
    e[2].binding = 2;
    e[2].texture = depth;
    return device_.create_bind_group(apply_layout_, e);
}

void AmbientOcclusionPass::draw_generate(rhi::CommandList& cmd, rhi::BindGroupHandle input, const AoView& view, const AmbientOcclusionSettings& settings)
{
    cmd.bind_pipeline(generate_pipeline_);
    cmd.bind_group(0, input);
    const Push push = make_push(view, settings);
    cmd.push_constants(std::as_bytes(std::span(&push, 1)), 0);
    cmd.draw(3);
}

void AmbientOcclusionPass::draw_apply(rhi::CommandList& cmd, rhi::BindGroupHandle input, const AoView& view, const AmbientOcclusionSettings& settings)
{
    cmd.bind_pipeline(apply_pipeline_);
    cmd.bind_group(0, input);
    const Push push = make_push(view, settings);
    cmd.push_constants(std::as_bytes(std::span(&push, 1)), 0);
    cmd.draw(3);
}

}  // namespace saige::render
