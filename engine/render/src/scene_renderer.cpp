#include "saige/render/scene_renderer.hpp"

#include "saige/shaders/scene_cube.spv.hpp"
#include "saige/shaders/scene_ground.spv.hpp"
#include "saige/shaders/scene_sky.spv.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace saige::render {

namespace {

constexpr rhi::Format depth_format = rhi::Format::D32_FLOAT;

const RendererSettings& validated_bloom(const RendererSettings& s)
{
    if (s.bloom.enabled) {
        validate_bloom(s.bloom);
        if (!s.hdr) throw std::invalid_argument("RendererSettings: bloom needs hdr (the scene must be drawn in linear HDR to have anything above the threshold)");
    }
    return s;
}

const RendererSettings& validated_fxaa(const RendererSettings& s)
{
    if (s.fxaa.enabled) validate_fxaa(s.fxaa);
    return s;
}

const RendererSettings& validated_ao(const RendererSettings& s)
{
    if (s.ambient_occlusion.enabled) {
        validate_ambient_occlusion(s.ambient_occlusion);
        if (!s.hdr) throw std::invalid_argument("RendererSettings: ambient occlusion needs the HDR path (hdr = true)");
    }
    return s;
}

struct AoTargets {
    rhi::TextureHandle value, color;
    rhi::BindGroupHandle generate_input, apply_input;
};

void release_targets(rhi::Device& device, rhi::TextureHandle depth, rhi::TextureHandle hdr,
                     rhi::BindGroupHandle hdr_input, rhi::TextureHandle aa = {}, rhi::BindGroupHandle aa_input = {},
                     const AoTargets& ao = {})
{
    if (ao.apply_input.is_valid()) {
        device.destroy(ao.apply_input);
    }
    if (ao.generate_input.is_valid()) {
        device.destroy(ao.generate_input);
    }
    if (ao.color.is_valid()) {
        device.destroy(ao.color);
    }
    if (ao.value.is_valid()) {
        device.destroy(ao.value);
    }
    if (aa_input.is_valid()) {
        device.destroy(aa_input);
    }
    if (aa.is_valid()) {
        device.destroy(aa);
    }
    if (hdr_input.is_valid()) {
        device.destroy(hdr_input);
    }
    if (hdr.is_valid()) {
        device.destroy(hdr);
    }
    if (depth.is_valid()) {
        device.destroy(depth);
    }
}

const rhi::ResourceState depth_attachment_state{
    rhi::PipelineStage::EarlyFragmentTests | rhi::PipelineStage::LateFragmentTests, rhi::Access::DepthStencilWrite,
    rhi::TextureLayout::DepthStencilAttachment};

template <std::size_t N>
std::span<const uint32_t> words(const uint32_t (&array)[N])
{
    return {array, N};
}

void fill4(float (&out)[4], float x, float y, float z, float w)
{
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

} // namespace

CubeMesh make_unit_cube()
{
    struct Face {
        Float3 normal, u, v;  // u x v == normal, so (-u-v, +u-v, +u+v, -u+v) is counter-clockwise from outside
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
        {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    CubeMesh mesh{};
    for (int f = 0; f < 6; ++f) {
        const Face& face = faces[f];
        const Float3 center = face.normal * 0.5f;
        const float su[4] = {-0.5f, 0.5f, 0.5f, -0.5f};
        const float sv[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
        for (int c = 0; c < 4; ++c) {
            mesh.vertices[f * 4 + c] = {center + face.u * su[c] + face.v * sv[c], face.normal};
        }
        const auto base = static_cast<uint16_t>(f * 4);
        const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
            mesh.indices[f * 6 + i] = static_cast<uint16_t>(base + quad[i]);
        }
    }
    return mesh;
}

namespace {

/// Shadow settings are validated before the renderer acquires anything from the device (a settings error is an exception, not a leak).
void validate_shadows(const RendererSettings& settings)
{
    const SunShadowSettings& sh = settings.shadows;
    if (!sh.enabled) {
        return;
    }
    const auto finite = [](float v) { return std::isfinite(v); };
    if (sh.map_size < 16 || sh.map_size > 8192 || !finite(sh.distance) || !(sh.distance > settings.near_plane) || !finite(sh.caster_margin) ||
        sh.caster_margin < 0.0f || !finite(sh.depth_bias_constant) || !finite(sh.depth_bias_slope) || !finite(sh.normal_offset_texels) ||
        sh.normal_offset_texels < 0.0f || sh.filter_radius > 8 || sh.cascades < 1 || sh.cascades > kMaxShadowCascades || !finite(sh.split_lambda) ||
        sh.split_lambda < 0.0f || sh.split_lambda > 1.0f || !finite(sh.cascade_blend) || sh.cascade_blend < 0.0f || sh.cascade_blend > 0.5f) {
        throw std::invalid_argument("SceneRenderer: invalid sun shadow settings (map size 16..8192, 1 to 4 cascades, split lambda 0..1, blend 0..0.5, distance beyond the near plane, finite non-negative margin and offset, filter radius at most 8)");
    }
}

/// The environment is validated, and refused for a shading mode that would ignore it, before the renderer acquires anything from the device.
void validate_environment(const RendererSettings& settings)
{
    if (!settings.environment) {
        return;
    }
    validate_environment_lighting(*settings.environment);
    if (settings.model_shading != ModelShading::MetallicRoughness) {
        throw std::invalid_argument("SceneRenderer: an environment needs model_shading = MetallicRoughness (the other modes would ignore it)");
    }
}

} // namespace

namespace {

/// The atmosphere is validated before the renderer acquires anything from the device (a settings error is an exception, not a leak).
void validate_atmosphere_settings(const RendererSettings& settings)
{
    if (settings.atmosphere.enabled) {
        validate_atmosphere(settings.atmosphere);
    }
}

/// Push constants of the sky shader (shaders/scene/sky.hlsl), 112 bytes.
struct SkyConstants {
    float right[4];     ///< camera right * tan(fov / 2) * aspect
    float up[4];        ///< camera up * tan(fov / 2); w: exposure
    float forward[4];   ///< camera forward; w: sun intensity
    float to_sun[4];    ///< unit vector toward the sun; w: Mie anisotropy
    float geometry[4];  ///< planet radius, top radius, Rayleigh and Mie scale heights (km)
    float beta_r[4];    ///< Rayleigh coefficients per km; w: Mie coefficient
    float misc[4];      ///< x: 1 to apply 1 - exp(-x); y: observer height (km)
};
static_assert(sizeof(SkyConstants) == 112);

} // namespace

SceneRenderer::SceneRenderer(rhi::Device& device, rhi::Format color_format, RendererSettings settings)
    : device_(device),
      color_format_(color_format),
      scene_format_(settings.hdr ? rhi::Format::RGBA16_FLOAT : color_format),
      settings_((validate_lighting(settings.lighting), validate_shadows(settings), validate_environment(settings), validate_atmosphere_settings(settings), validated_ao(validated_bloom(validated_fxaa(settings))))),
      registry_(device)
{
    // The destructor does not run when a constructor throws, and the shader, pipeline and buffer handles below are raw: a creation that
    // fails part-way releases whatever exists by then (the registry, the tone mapper and the model renderer are RAII and release themselves).
    try {
        acquire();
    } catch (...) {
        release();
        throw;
    }
}

void SceneRenderer::acquire()
{
    using namespace saige::shaders;
    if (settings_.hdr) {
        tone_mapper_ = std::make_unique<ToneMapper>(device_, color_format_, settings_.bloom.enabled);
        if (settings_.bloom.enabled) {
            bloom_ = std::make_unique<BloomPass>(device_, settings_.bloom);
        }
    }
    if (settings_.fxaa.enabled) {
        fxaa_ = std::make_unique<FxaaPass>(device_, color_format_);
    }
    if (settings_.ambient_occlusion.enabled) {
        ao_ = std::make_unique<AmbientOcclusionPass>(device_);
        ao_->set_apply_format(rhi::Format::RGBA16_FLOAT);
    }
    cube_vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_cube_vs_main), "VSMain", "SceneCube.VS"});
    cube_ps_ = device_.create_shader({rhi::ShaderStage::Pixel, words(scene_cube_ps_main), "PSMain", "SceneCube.PS"});
    ground_vs_ =
        device_.create_shader({rhi::ShaderStage::Vertex, words(scene_ground_vs_main), "VSMain", "SceneGround.VS"});
    ground_ps_ =
        device_.create_shader({rhi::ShaderStage::Pixel, words(scene_ground_ps_main), "PSMain", "SceneGround.PS"});

    rhi::GraphicsPipelineDesc cube;
    cube.vertex_shader = cube_vs_;
    cube.pixel_shader = cube_ps_;
    cube.vertex_layout.stride = sizeof(CubeVertex);
    cube.vertex_layout.attributes = {{0, rhi::Format::RGB32_FLOAT, offsetof(CubeVertex, position)},
                                     {1, rhi::Format::RGB32_FLOAT, offsetof(CubeVertex, normal)}};
    cube.cull_mode = rhi::CullMode::Back;
    cube.front_face = rhi::FrontFace::CounterClockwise;
    cube.depth = {true, true, rhi::CompareOp::Greater};
    cube.color_formats = {scene_format_};
    cube.depth_format = depth_format;
    cube.push_constant_size = sizeof(DrawConstants);
    cube.debug_name = "SceneCube";
    cube_pipeline_ = device_.create_graphics_pipeline(cube);

    rhi::GraphicsPipelineDesc ground = cube;
    ground.vertex_shader = ground_vs_;
    ground.pixel_shader = ground_ps_;
    ground.vertex_layout = {};
    ground.cull_mode = rhi::CullMode::None;
    ground.debug_name = "SceneGround";
    ground_pipeline_ = device_.create_graphics_pipeline(ground);

    const CubeMesh mesh = make_unit_cube();
    rhi::BufferDesc vb;
    vb.size = sizeof(mesh.vertices);
    vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    vb.debug_name = "CubeVertices";
    cube_vertices_ = device_.create_buffer(vb, std::as_bytes(std::span(mesh.vertices)));
    rhi::BufferDesc ib;
    ib.size = sizeof(mesh.indices);
    ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
    ib.debug_name = "CubeIndices";
    cube_indices_ = device_.create_buffer(ib, std::as_bytes(std::span(mesh.indices)));
}

SceneLighting time_of_day_lighting(const AtmosphereSettings& atmosphere, Float3 to_sun, const SceneLighting& day, float night_ambient)
{
    if (!std::isfinite(night_ambient) || night_ambient < 0.0f || night_ambient > 1.0f) {
        throw std::invalid_argument("time_of_day_lighting: the night ambient fraction must be in 0 to 1");
    }
    SceneLighting out = day;
    out.sun_direction = to_sun;
    validate_lighting(out);  // finite, non-zero direction (and the base colors) before anything is derived
    Float3 sun{};
    (void)unit_sun_direction(to_sun, sun);
    out.sun_direction = sun;
    const Float3 t = sun_transmittance(atmosphere, sun);
    out.sun_color = {day.sun_color.x * t.x, day.sun_color.y * t.y, day.sun_color.z * t.z};
    float s = (sun.z + 0.1f) / 0.45f;
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    s = s * s * (3.0f - 2.0f * s);
    const float k = night_ambient + (1.0f - night_ambient) * s;
    out.ambient_ground = {day.ambient_ground.x * k, day.ambient_ground.y * k, day.ambient_ground.z * k};
    return out;
}

Float3 SceneRenderer::atmosphere_fog(Float3 sun) const
{
    Float3 h = sky_horizon_color(settings_.atmosphere, sun);
    if (!settings_.hdr) {  // nothing downstream tone maps an LDR scene: the same curve the sky pass applies
        h = {1.0f - std::exp(-h.x), 1.0f - std::exp(-h.y), 1.0f - std::exp(-h.z)};
    }
    return h;
}

void SceneRenderer::ensure_sky()
{
    if (sky_pipeline_.is_valid()) {
        return;
    }
    using namespace saige::shaders;
    rhi::ShaderHandle vs, ps;
    try {
        vs = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_sky_vs_main), "VSMain", "SceneSky.VS"});
        ps = device_.create_shader({rhi::ShaderStage::Pixel, words(scene_sky_ps_main), "PSMain", "SceneSky.PS"});
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs;
        pd.pixel_shader = ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.depth = {false, false, rhi::CompareOp::Greater};  // behind everything: tests and writes no depth
        pd.color_formats = {scene_format_};
        pd.depth_format = depth_format;
        pd.push_constant_size = sizeof(SkyConstants);
        pd.debug_name = "SceneSky";
        sky_pipeline_ = device_.create_graphics_pipeline(pd);
    } catch (...) {
        if (ps.is_valid()) device_.destroy(ps);
        if (vs.is_valid()) device_.destroy(vs);
        throw;
    }
    sky_vs_ = vs;
    sky_ps_ = ps;
}

SceneRenderer::~SceneRenderer()
{
    release();
}

void SceneRenderer::release() noexcept
{
    if (sky_pipeline_.is_valid()) device_.destroy(sky_pipeline_);
    sky_pipeline_ = {};
    if (sky_ps_.is_valid()) device_.destroy(sky_ps_);
    sky_ps_ = {};
    if (sky_vs_.is_valid()) device_.destroy(sky_vs_);
    sky_vs_ = {};
    if (bloom_) {
        bloom_->destroy_chain(bloom_chain_);  // noexcept, and a no-op on a chain that was never built (a constructor that failed part-way)
    }
    release_targets(device_, depth_, hdr_, hdr_input_, aa_, aa_input_, {ao_value_, hdr_ao_, ao_generate_input_, ao_apply_input_});
    depth_ = {};
    hdr_ = {};
    hdr_input_ = {};
    aa_ = {};
    aa_input_ = {};
    ao_value_ = {};
    hdr_ao_ = {};
    ao_generate_input_ = {};
    ao_apply_input_ = {};
    const auto destroy_if = [this](auto& handle) {
        if (handle.is_valid()) {
            device_.destroy(handle);
            handle = {};
        }
    };
    destroy_if(cube_indices_);
    destroy_if(cube_vertices_);
    destroy_if(ground_pipeline_);
    destroy_if(cube_pipeline_);
    destroy_if(ground_ps_);
    destroy_if(ground_vs_);
    destroy_if(cube_ps_);
    destroy_if(cube_vs_);
}

void SceneRenderer::ensure_targets(uint32_t width, uint32_t height)
{
    if (depth_.is_valid() && target_width_ == width && target_height_ == height) {
        return;
    }

    rhi::TextureDesc depth_desc;
    depth_desc.format = depth_format;
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.usage = rhi::TextureUsage::DepthStencilAttachment;
    if (ao_) depth_desc.usage = depth_desc.usage | rhi::TextureUsage::Sampled;  // the occlusion passes read it
    depth_desc.debug_name = "SceneDepth";

    rhi::TextureHandle next_depth;
    rhi::TextureHandle next_hdr;
    rhi::BindGroupHandle next_input;
    BloomChain next_chain;
    rhi::TextureHandle next_aa;
    rhi::BindGroupHandle next_aa_input;
    AoTargets next_ao;
    try {
        next_depth = device_.create_texture(depth_desc);

        if (settings_.hdr) {
            rhi::TextureDesc hdr_desc;
            hdr_desc.format = rhi::Format::RGBA16_FLOAT;
            hdr_desc.width = width;
            hdr_desc.height = height;
            hdr_desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
            hdr_desc.debug_name = "SceneHdr";
            next_hdr = device_.create_texture(hdr_desc);
            if (ao_) {
                rhi::TextureDesc value_desc;
                value_desc.format = AmbientOcclusionPass::ao_format;
                value_desc.width = width;
                value_desc.height = height;
                value_desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
                value_desc.debug_name = "AoValue";
                next_ao.value = device_.create_texture(value_desc);
                hdr_desc.debug_name = "SceneHdrAo";
                next_ao.color = device_.create_texture(hdr_desc);
                next_ao.generate_input = ao_->create_generate_input(next_depth);
                next_ao.apply_input = ao_->create_apply_input(next_hdr, next_ao.value, next_depth);
            }
            const rhi::TextureHandle post_color = ao_ ? next_ao.color : next_hdr;  // bloom and the tone map read the occluded color when occlusion is on
            if (bloom_) {
                next_chain = bloom_->create_chain(post_color, width, height);
            }
            next_input = tone_mapper_->create_input(post_color, bloom_ ? next_chain.result() : rhi::TextureHandle{});
        }
        if (fxaa_) {
            rhi::TextureDesc aa_desc;
            aa_desc.format = color_format_;
            aa_desc.width = width;
            aa_desc.height = height;
            aa_desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
            aa_desc.debug_name = "FxaaInput";
            next_aa = device_.create_texture(aa_desc);
            next_aa_input = fxaa_->create_input(next_aa);
        }
    } catch (...) {
        if (bloom_) bloom_->destroy_chain(next_chain);
        release_targets(device_, next_depth, next_hdr, next_input, next_aa, next_aa_input, next_ao);
        throw;
    }

    // Install the whole attachment set before retiring the old one on the device's completion timeline.
    std::swap(depth_, next_depth);
    std::swap(hdr_, next_hdr);
    std::swap(hdr_input_, next_input);
    std::swap(bloom_chain_, next_chain);
    std::swap(aa_, next_aa);
    std::swap(aa_input_, next_aa_input);
    std::swap(ao_value_, next_ao.value);
    std::swap(hdr_ao_, next_ao.color);
    std::swap(ao_generate_input_, next_ao.generate_input);
    std::swap(ao_apply_input_, next_ao.apply_input);
    target_width_ = width;
    target_height_ = height;
    depth_state_ = {};
    hdr_state_ = {};
    if (bloom_) bloom_->destroy_chain(next_chain);
    aa_state_ = {};
    ao_value_state_ = {};
    hdr_ao_state_ = {};
    release_targets(device_, next_depth, next_hdr, next_input, next_aa, next_aa_input, next_ao);
}

Float3 SceneRenderer::camera_target(const SceneFrame& frame) const
{
    if (frame.camera != nullptr) {
        return frame.camera->position;  // the ground plane is centered under the camera
    }
    if (settings_.camera_mode == CameraMode::Fixed) {
        return settings_.fixed_target;
    }
    const Float3 base = frame.instances.empty() ? Float3{} : frame.instances.front().position;
    return base + Float3{0.0f, 0.0f, settings_.cube_edge * 0.5f};
}

Float3 SceneRenderer::camera_eye(const SceneFrame& frame) const
{
    if (frame.camera != nullptr) {
        return frame.camera->position;
    }
    if (settings_.camera_mode == CameraMode::Fixed) {
        return settings_.fixed_eye;
    }
    return camera_target(frame) + settings_.follow_offset;
}

Mat4 SceneRenderer::view_projection(const SceneFrame& frame, float aspect) const
{
    if (frame.camera != nullptr) {
        return view_projection_matrix(*frame.camera, aspect);
    }
    const Mat4 view = look_at(camera_eye(frame), camera_target(frame));
    const Mat4 proj = perspective_reverse_z(settings_.fov_y_radians, aspect, settings_.near_plane);
    return proj * view;
}

Camera SceneRenderer::sky_camera(const SceneFrame& frame) const
{
    if (frame.camera != nullptr) {
        return *frame.camera;
    }
    return look_at_camera(camera_eye(frame), camera_target(frame), {0.0f, 0.0f, 1.0f}, settings_.fov_y_radians, settings_.near_plane);
}

float SceneRenderer::instance_yaw(const SceneFrame& frame, const SceneInstance& instance) const
{
    const double seconds = (static_cast<double>(frame.tick) + frame.alpha) * settings_.seconds_per_tick;
    const double yaw = std::fmod(seconds * static_cast<double>(settings_.spin_radians_per_second), 2.0 * 3.14159265358979323846);
    return static_cast<float>(yaw) + instance.yaw_offset;
}

ModelRenderer& SceneRenderer::model_renderer()
{
    if (!models_) {
        ModelRenderSettings ms;
        ms.sky = settings_.sky;
        if (settings_.atmosphere.enabled) {
            Float3 sun{};
            (void)unit_sun_direction(settings_.lighting.sun_direction, sun);
            const Float3 fog = atmosphere_fog(sun);
            ms.sky = {fog.x, fog.y, fog.z, 1.0f};  // the models' fog and ambient sky color follow the horizon
        }
        ms.fog_density = settings_.fog_density;
        ms.shading = settings_.model_shading;
        ms.lighting = settings_.lighting;
        ms.dynamic_lighting = settings_.dynamic_lighting;
        ms.alpha_blending = settings_.alpha_blending;
        ms.environment = settings_.environment;
        ms.skinning = settings_.skinning;
        ms.frustum_culling = settings_.frustum_culling;
        ms.shadows.enabled = settings_.shadows.enabled;
        ms.shadows.map_size = settings_.shadows.map_size;
        ms.shadows.cascades = settings_.shadows.cascades;
        ms.shadows.depth_bias_constant = settings_.shadows.depth_bias_constant;
        ms.shadows.depth_bias_slope = settings_.shadows.depth_bias_slope;
        models_ = std::make_unique<ModelRenderer>(device_, registry_, scene_format_, depth_format, ms);
    }
    return *models_;
}

FrameStats SceneRenderer::render(rhi::CommandList& cmd, const SceneFrame& frame, const RenderTarget& target)
{
    FrameStats stats;
    if (target.width == 0 || target.height == 0 || !target.texture.is_valid()) {
        return stats;
    }
    const std::optional<rhi::TextureDesc> color_desc = device_.texture_desc(target.texture);
    if (!color_desc) {
        throw std::invalid_argument("SceneRenderer::render: invalid target texture");
    }
    if (models_ && !frame.models.empty()) {
        models_->check_palettes_unfrozen(frame.models);  // a palette changed after its first use this frame: refused before anything is created or recorded
    }
    // This frame's sun. Checked before anything is created, recorded or written, so a bad value or a renderer without dynamic lighting changes nothing.
    SceneLighting frame_lighting = settings_.lighting;
    if (frame.lighting != nullptr) {
        if (!settings_.dynamic_lighting) {
            throw std::logic_error("SceneRenderer::render: SceneFrame::lighting needs RendererSettings::dynamic_lighting");
        }
        validate_lighting(*frame.lighting);
        frame_lighting = *frame.lighting;
    }
    Float3 frame_sun{};
    (void)unit_sun_direction(frame_lighting.sun_direction, frame_sun);  // validated non-zero: the one direction the sky, fog, lighting and shadow fit all read
    const LightingSnapshot snapshot{frame_sun, frame_lighting.sun_color, frame_lighting.ambient_ground};
    const uint64_t device_frame = device_.frame_number();
    if (settings_.dynamic_lighting && lighting_snapshot_frame_ == device_frame) {
        // A second render of this device frame (a second view, or a sky-only view and a model view): one effective lighting per frame, enforced here as well as in the
        // model renderer, because a view without models publishes nothing to it.
        const auto same = [](Float3 a, Float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        if (!same(snapshot.sun, lighting_snapshot_.sun) || !same(snapshot.sun_color, lighting_snapshot_.sun_color) ||
            !same(snapshot.ambient_ground, lighting_snapshot_.ambient_ground)) {
            throw std::logic_error("SceneRenderer::render: this device frame already has a different lighting (one effective lighting per frame: a second view must supply the same one)");
        }
    }
    if (settings_.atmosphere.enabled) {
        ensure_sky();  // first, with nothing else created yet: a device failure is a failed frame that leaves nothing behind
    }
    ensure_targets(target.width, target.height);
    if (settings_.dynamic_lighting) {
        lighting_snapshot_ = snapshot;  // after the last throw that leaves nothing behind: this frame's lighting is now fixed
        lighting_snapshot_frame_ = device_frame;
    }

    const float aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
    DrawConstants constants{};
    constants.view_proj = view_projection(frame, aspect);
    const Float3 eye = camera_eye(frame);
    fill4(constants.camera_fog, eye.x, eye.y, eye.z, settings_.fog_density);
    rhi::ClearColor clear = settings_.sky;
    if (settings_.atmosphere.enabled) {
        const Float3 fog = atmosphere_fog(frame_sun);
        clear = {fog.x, fog.y, fog.z, 1.0f};
    }
    fill4(constants.fog_color, clear.r, clear.g, clear.b, 1.0f);

    // Sun shadows (imported models only, and only when asked for). The model renderer is created up front when there are models, because
    // the shadow map's synchronization is part of the graph built below. With shadows off none of this runs: no fit, no pass, no import.
    ModelRenderer* shadow_models = (settings_.shadows.enabled && !frame.models.empty()) ? &model_renderer() : nullptr;
    if (settings_.dynamic_lighting && !frame.models.empty() && lighting_published_frame_ != device_frame) {
        // One immutable payload per renderer per frame, published before the first draw of the frame reads the slot (once: a second view of the frame has the same lighting).
        ModelRenderer& mr = model_renderer();
        ModelLightingFrame lf;
        lf.lighting = frame_lighting;
        lf.lighting.sun_direction = frame_sun;
        lf.sky_color = {clear.r, clear.g, clear.b};
        mr.set_lighting_frame(&lf);
        lighting_published_frame_ = device_frame;
    }
    std::optional<ShadowCascades> sun_cascades;
    if (shadow_models != nullptr) {
        CascadeCamera cam;
        cam.position = eye;
        if (frame.camera != nullptr) {
            cam.forward = camera_forward(*frame.camera);
            cam.fov_y_radians = frame.camera->fov_y_radians;
        } else {
            cam.forward = normalize(camera_target(frame) - eye);
            cam.fov_y_radians = settings_.fov_y_radians;
        }
        cam.aspect = aspect;
        CascadeSettings cs;
        cs.count = settings_.shadows.cascades;
        cs.split_lambda = settings_.shadows.split_lambda;
        cs.near_plane = frame.camera != nullptr ? frame.camera->near_plane : settings_.near_plane;
        cs.shadow_distance = settings_.shadows.distance;
        cs.resolution = settings_.shadows.map_size;
        cs.caster_margin = settings_.shadows.caster_margin;
        // The same normalized direction the lighting constants were built from (SceneLighting::sun_direction), not a copy of it.
        const bool sun_up = !settings_.dynamic_lighting || frame_sun.z > 0.0f;  // a sun below the horizon casts no shadow (and a fit toward it is meaningless)
        if (sun_up) {
            const ShadowCascades fitted = fit_shadow_cascades(cam, frame_sun, cs);
            if (fitted.count == settings_.shadows.cascades) {
                sun_cascades = fitted;
            }
        }
        if (sun_cascades) {
            ModelShadowFrame sf;
            sf.light_view_proj = sun_cascades->cascades[0].view_proj;
            sf.texel_world_size = sun_cascades->cascades[0].texel_world_size;
            sf.normal_offset_texels = settings_.shadows.normal_offset_texels;
            sf.filter_radius = settings_.shadows.filter_radius;
            sf.cascade_count = sun_cascades->count;
            for (uint32_t i = 1; i < sun_cascades->count; ++i) {
                sf.more_view_proj[i - 1] = sun_cascades->cascades[i].view_proj;
                sf.more_texel_world_size[i - 1] = sun_cascades->cascades[i].texel_world_size;
            }
            for (uint32_t i = 0; i < sun_cascades->count; ++i) sf.split_far[i] = sun_cascades->cascades[i].split_far;
            sf.camera_position = cam.position;
            sf.camera_forward = cam.forward;
            sf.blend = settings_.shadows.cascade_blend;
            shadow_models->set_shadow_frame(&sf);
        } else {
            shadow_models->set_shadow_frame(nullptr);  // an invalid fit this frame: the sun is not occluded
        }
    }

    graph_.reset();
    const RGTexture color = graph_.import_texture("SceneColor", *color_desc, target.texture, target.initial_state,
                                                  target.final_state);
    // The texture the Scene pass draws into: the output itself, or the HDR target that the tone-map pass resolves to it.
    // With FXAA the finished image first goes to an intermediate target of the output's format, and the FXAA pass writes the output.
    rhi::TextureHandle final_texture = target.texture;
    RGTexture final_color = color;
    if (fxaa_) {
        rhi::TextureDesc aa_desc;
        aa_desc.format = color_format_;
        aa_desc.width = target.width;
        aa_desc.height = target.height;
        aa_desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        final_texture = aa_;
        final_color = graph_.import_texture("FxaaInput", aa_desc, aa_, aa_state_, rhi::sampled_texture_state);
    }
    rhi::TextureHandle scene_texture = final_texture;
    RGTexture scene_color = final_color;
    if (settings_.hdr) {
        rhi::TextureDesc hdr_desc;
        hdr_desc.format = rhi::Format::RGBA16_FLOAT;
        hdr_desc.width = target.width;
        hdr_desc.height = target.height;
        hdr_desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        scene_texture = hdr_;
        scene_color = graph_.import_texture("SceneHdr", hdr_desc, hdr_, hdr_state_, rhi::sampled_texture_state);
    }
    rhi::TextureDesc depth_desc;
    depth_desc.format = depth_format;
    depth_desc.width = target.width;
    depth_desc.height = target.height;
    depth_desc.usage = rhi::TextureUsage::DepthStencilAttachment;
    if (ao_) depth_desc.usage = depth_desc.usage | rhi::TextureUsage::Sampled;
    // With ambient occlusion the passes sample the depth, so it ends the frame in the sampled state; the next frame's Scene pass transitions it back.
    const RGTexture depth = ao_ ? graph_.import_texture("SceneDepth", depth_desc, depth_, depth_state_, rhi::sampled_texture_state)
                                : graph_.import_texture("SceneDepth", depth_desc, depth_, depth_state_);
    RGTexture tonemap_input = scene_color;

    // When shadows are on, the map is always bound to the model shader, so it must be in the sampled state when the scene pass draws
    // models. It is written by the shadow pass when there is a fit; the first frame with models also runs a pass that only clears it, which
    // takes it from undefined to a valid depth image (a map that was never written must still be a legal descriptor).
    RGTexture shadow_map_rg{};
    const bool shadow_pass = shadow_models != nullptr && (sun_cascades.has_value() || !shadow_ready_);
    if (shadow_models != nullptr) {
        const std::optional<rhi::TextureDesc> shadow_desc = device_.texture_desc(shadow_models->shadow_map());
        if (!shadow_desc) {
            throw std::logic_error("SceneRenderer: the shadow map texture is invalid");
        }
        shadow_map_rg = graph_.import_texture("SunShadowMap", *shadow_desc, shadow_models->shadow_map(), shadow_state_, rhi::sampled_texture_state);
    }
    if (shadow_pass) {
        graph_.add_pass(
            "SunShadow", rhi::QueueType::Graphics, [&](PassBuilder& b) { b.write_depth(shadow_map_rg, LoadOp::Clear); },
            [&](PassContext& ctx) {
                rhi::CommandList& c = *ctx.command_list();
                const uint32_t tile = shadow_models->shadow_map_size();
                const uint32_t columns = shadow_models->shadow_atlas_columns();
                const uint32_t atlas_w = shadow_models->shadow_atlas_width(), atlas_h = shadow_models->shadow_atlas_height();
                rhi::RenderingDesc rd;
                rhi::DepthAttachment da;
                da.texture = shadow_models->shadow_map();
                da.load = rhi::AttachmentLoadOp::Clear;
                da.store = rhi::AttachmentStoreOp::Store;
                da.clear_depth = 0.0f;  // reverse-Z far plane: nothing occludes
                rd.depth = da;
                rd.area = {0, 0, atlas_w, atlas_h};
                c.begin_debug_label("SunShadow");
                c.begin_rendering(rd);
                if (sun_cascades) {
                    for (uint32_t i = 0; i < sun_cascades->count; ++i) {  // one tile of the atlas per cascade
                        const int32_t x = static_cast<int32_t>((i % columns) * tile), y = static_cast<int32_t>((i / columns) * tile);
                        c.set_viewport({static_cast<float>(x), static_cast<float>(y), static_cast<float>(tile), static_cast<float>(tile), 0.0f, 1.0f});
                        c.set_scissor({x, y, tile, tile});
                        ModelFrameStats shadow_stats;
                        shadow_models->draw_shadow(c, sun_cascades->cascades[i].view_proj, frame.models, shadow_stats);
                        stats.draw_calls += shadow_stats.draw_calls;
                        stats.shadow_draw_calls += shadow_stats.draw_calls;
                        stats.model_skinned_draws += shadow_stats.skinned_draws;
                        stats.model_skinned_skipped += shadow_stats.skinned_skipped;
                    }
                } else {
                    c.set_viewport({0.0f, 0.0f, static_cast<float>(atlas_w), static_cast<float>(atlas_h), 0.0f, 1.0f});
                    c.set_scissor({0, 0, atlas_w, atlas_h});
                }
                c.end_rendering();
                c.end_debug_label();
            });
    }

    const Float3 ground_center = camera_target(frame);
    graph_.add_pass(
        "Scene", rhi::QueueType::Graphics,
        [&](PassBuilder& b) {
            if (shadow_models != nullptr) {
                b.sample(shadow_map_rg, rhi::PipelineStage::PixelShader);
            }
            b.write_color(scene_color, LoadOp::Clear);
            b.write_depth(depth, LoadOp::Clear);
        },
        [&](PassContext& ctx) {
            rhi::CommandList& c = *ctx.command_list();
            c.write_timestamp(timestamp_scene_begin);
            rhi::RenderingDesc rd;
            rhi::ColorAttachment ca;
            ca.texture = scene_texture;
            ca.load = rhi::AttachmentLoadOp::Clear;
            ca.store = rhi::AttachmentStoreOp::Store;
            ca.clear = clear;
            rd.colors.push_back(ca);
            rhi::DepthAttachment da;
            da.texture = depth_;
            rd.depth = da;
            rd.area = {0, 0, target.width, target.height};
            c.begin_debug_label("Scene");
            c.begin_rendering(rd);
            c.set_viewport({0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f,
                            1.0f});
            c.set_scissor({0, 0, target.width, target.height});

            if (settings_.atmosphere.enabled) {
                const AtmosphereSettings& at = settings_.atmosphere;
                const Camera cam = sky_camera(frame);
                const float tan_half = std::tan(cam.fov_y_radians * 0.5f);
                const Float3 right = camera_right(cam) * (tan_half * aspect), up = camera_up(cam) * tan_half, fwd = camera_forward(cam);
                const Float3 sun = frame_sun;
                SkyConstants sk{};
                fill4(sk.right, right.x, right.y, right.z, at.sun_disc_intensity);
                fill4(sk.up, up.x, up.y, up.z, at.exposure);
                fill4(sk.forward, fwd.x, fwd.y, fwd.z, at.sun_intensity);
                fill4(sk.to_sun, sun.x, sun.y, sun.z, at.mie_g);
                fill4(sk.geometry, at.planet_radius_km, at.planet_radius_km + at.atmosphere_height_km, at.rayleigh_scale_height_km, at.mie_scale_height_km);
                fill4(sk.beta_r, at.rayleigh_coefficient.x, at.rayleigh_coefficient.y, at.rayleigh_coefficient.z, at.mie_coefficient);
                fill4(sk.misc, settings_.hdr ? 0.0f : 1.0f, at.observer_height_km, at.moon_intensity, at.star_intensity);
                c.bind_pipeline(sky_pipeline_);
                c.push_constant_struct(sk);
                c.draw(3);
                ++stats.draw_calls;
            }

            DrawConstants dc = constants;
            c.bind_pipeline(ground_pipeline_);
            fill4(dc.position_yaw, ground_center.x, ground_center.y, 0.0f, settings_.ground_half_size);
            c.push_constant_struct(dc);
            c.draw(6);
            ++stats.draw_calls;

            if (!frame.instances.empty()) {
                c.bind_pipeline(cube_pipeline_);
                c.bind_vertex_buffer(cube_vertices_);
                c.bind_index_buffer(cube_indices_, rhi::IndexType::Uint16);
                for (const SceneInstance& inst : frame.instances) {
                    fill4(dc.position_yaw, inst.position.x, inst.position.y, inst.position.z, instance_yaw(frame, inst));
                    fill4(dc.color_scale, inst.color.x, inst.color.y, inst.color.z, settings_.cube_edge * inst.scale);
                    c.push_constant_struct(dc);
                    c.draw_indexed(36);
                    ++stats.draw_calls;
                }
            }
            if (!frame.models.empty()) {
                ModelFrameStats model_stats;
                model_renderer().draw(c, constants.view_proj, eye, frame.models, model_stats);
                stats.draw_calls += model_stats.draw_calls;
                stats.model_instances += model_stats.instances_drawn;
                stats.model_instances_skipped += model_stats.instances_skipped;
                stats.model_blended_primitives += model_stats.blended_primitives;
                stats.model_primitives_culled += model_stats.primitives_culled;
                stats.model_skinned_draws += model_stats.skinned_draws;
                stats.model_skinned_skipped += model_stats.skinned_skipped;
            }
            c.end_rendering();
            c.write_timestamp(timestamp_scene_end);
            c.end_debug_label();
        });

    if (ao_) {
        rhi::TextureDesc value_desc;
        value_desc.format = AmbientOcclusionPass::ao_format;
        value_desc.width = target.width;
        value_desc.height = target.height;
        value_desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        const RGTexture ao_value = graph_.import_texture("AoValue", value_desc, ao_value_, ao_value_state_, rhi::sampled_texture_state);
        rhi::TextureDesc occluded_desc;
        occluded_desc.format = rhi::Format::RGBA16_FLOAT;
        occluded_desc.width = target.width;
        occluded_desc.height = target.height;
        occluded_desc.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        const RGTexture occluded = graph_.import_texture("SceneHdrAo", occluded_desc, hdr_ao_, hdr_ao_state_, rhi::sampled_texture_state);
        const AoView ao_view{target.width, target.height, frame.camera != nullptr ? frame.camera->near_plane : settings_.near_plane,
                             std::tan(0.5f * (frame.camera != nullptr ? frame.camera->fov_y_radians : settings_.fov_y_radians))};
        graph_.add_pass(
            "AoGenerate", rhi::QueueType::Graphics,
            [&](PassBuilder& b) {
                b.sample(depth, rhi::PipelineStage::PixelShader);
                b.write_color(ao_value, LoadOp::DontCare);  // the full-screen triangle covers every pixel
            },
            [&, ao_view](PassContext& ctx) {
                rhi::CommandList& c = *ctx.command_list();
                rhi::RenderingDesc rd;
                rhi::ColorAttachment ca;
                ca.texture = ao_value_;
                ca.load = rhi::AttachmentLoadOp::DontCare;
                ca.store = rhi::AttachmentStoreOp::Store;
                rd.colors.push_back(ca);
                rd.area = {0, 0, target.width, target.height};
                c.begin_debug_label("AoGenerate");
                c.begin_rendering(rd);
                c.set_viewport({0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f});
                c.set_scissor({0, 0, target.width, target.height});
                ao_->draw_generate(c, ao_generate_input_, ao_view, settings_.ambient_occlusion);
                ++stats.draw_calls;
                ++stats.ao_draw_calls;
                c.end_rendering();
                c.end_debug_label();
            });
        graph_.add_pass(
            "AoApply", rhi::QueueType::Graphics,
            [&](PassBuilder& b) {
                b.sample(scene_color, rhi::PipelineStage::PixelShader);
                b.sample(ao_value, rhi::PipelineStage::PixelShader);
                b.sample(depth, rhi::PipelineStage::PixelShader);
                b.write_color(occluded, LoadOp::DontCare);
            },
            [&, ao_view](PassContext& ctx) {
                rhi::CommandList& c = *ctx.command_list();
                rhi::RenderingDesc rd;
                rhi::ColorAttachment ca;
                ca.texture = hdr_ao_;
                ca.load = rhi::AttachmentLoadOp::DontCare;
                ca.store = rhi::AttachmentStoreOp::Store;
                rd.colors.push_back(ca);
                rd.area = {0, 0, target.width, target.height};
                c.begin_debug_label("AoApply");
                c.begin_rendering(rd);
                c.set_viewport({0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f});
                c.set_scissor({0, 0, target.width, target.height});
                ao_->draw_apply(c, ao_apply_input_, ao_view, settings_.ambient_occlusion);
                ++stats.draw_calls;
                ++stats.ao_draw_calls;
                c.end_rendering();
                c.end_debug_label();
            });
        tonemap_input = occluded;
    }

    // Bloom (after occlusion, so it blooms what the tone map will show): D_0 .. D_{n-1} (the first thresholds the scene), then U_{n-2} .. U_0; every texture is the chain's own (imported, and left sampled). Each is fully
    // overwritten every frame, so each starts the frame in the undefined state (its previous contents are never read).
    RGTexture bloom_result;
    if (bloom_) {
        const BloomChain& chain = bloom_chain_;
        const uint32_t n = chain.levels;
        std::vector<RGTexture> down(n), up(n > 0 ? n - 1 : 0);
        for (uint32_t i = 0; i < n; ++i) {
            down[i] = graph_.import_texture("BloomDown", BloomPass::level_desc(target.width, target.height, i), chain.down[i], rhi::ResourceState{}, rhi::sampled_texture_state);
        }
        for (uint32_t i = 0; i + 1 < n; ++i) {
            up[i] = graph_.import_texture("BloomUp", BloomPass::level_desc(target.width, target.height, i), chain.up[i], rhi::ResourceState{}, rhi::sampled_texture_state);
        }
        for (uint32_t i = 0; i < n; ++i) {
            graph_.add_pass(
                "BloomDown", rhi::QueueType::Graphics,
                [&, i](PassBuilder& b) {
                    b.sample(i == 0 ? tonemap_input : down[i - 1], rhi::PipelineStage::PixelShader);
                    b.write_color(down[i], LoadOp::DontCare);
                },
                [&, i](PassContext& ctx) {
                    bloom_->record_down(*ctx.command_list(), chain, i);
                    ++stats.draw_calls;
                    ++stats.bloom_draw_calls;
                });
        }
        for (uint32_t i = n - 1; i-- > 0;) {
            graph_.add_pass(
                "BloomUp", rhi::QueueType::Graphics,
                [&, i](PassBuilder& b) {
                    b.sample(down[i], rhi::PipelineStage::PixelShader);
                    b.sample(i + 2 < n ? up[i + 1] : down[n - 1], rhi::PipelineStage::PixelShader);
                    b.write_color(up[i], LoadOp::DontCare);
                },
                [&, i](PassContext& ctx) {
                    bloom_->record_up(*ctx.command_list(), chain, i);
                    ++stats.draw_calls;
                    ++stats.bloom_draw_calls;
                });
        }
        bloom_result = n > 1 ? up[0] : down[0];
    }

    if (settings_.hdr) {
        graph_.add_pass(
            "ToneMap", rhi::QueueType::Graphics,
            [&](PassBuilder& b) {
                b.sample(tonemap_input, rhi::PipelineStage::PixelShader);
                if (bloom_) b.sample(bloom_result, rhi::PipelineStage::PixelShader);
                b.write_color(final_color, LoadOp::DontCare);  // the full-screen triangle covers every pixel
            },
            [&](PassContext& ctx) {
                rhi::CommandList& c = *ctx.command_list();
                rhi::RenderingDesc rd;
                rhi::ColorAttachment ca;
                ca.texture = final_texture;
                ca.load = rhi::AttachmentLoadOp::DontCare;
                ca.store = rhi::AttachmentStoreOp::Store;
                rd.colors.push_back(ca);
                rd.area = {0, 0, target.width, target.height};
                c.begin_debug_label("ToneMap");
                c.begin_rendering(rd);
                c.set_viewport({0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f});
                c.set_scissor({0, 0, target.width, target.height});
                tone_mapper_->draw(c, hdr_input_, settings_.tone_map, settings_.bloom.intensity);
                ++stats.draw_calls;
                c.end_rendering();
                c.end_debug_label();
            });
    }

    if (fxaa_) {
        graph_.add_pass(
            "Fxaa", rhi::QueueType::Graphics,
            [&](PassBuilder& b) {
                b.sample(final_color, rhi::PipelineStage::PixelShader);
                b.write_color(color, LoadOp::DontCare);  // the full-screen triangle covers every pixel
            },
            [&](PassContext& ctx) {
                rhi::CommandList& c = *ctx.command_list();
                rhi::RenderingDesc rd;
                rhi::ColorAttachment ca;
                ca.texture = target.texture;
                ca.load = rhi::AttachmentLoadOp::DontCare;
                ca.store = rhi::AttachmentStoreOp::Store;
                rd.colors.push_back(ca);
                rd.area = {0, 0, target.width, target.height};
                c.begin_debug_label("Fxaa");
                c.begin_rendering(rd);
                c.set_viewport({0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f});
                c.set_scissor({0, 0, target.width, target.height});
                fxaa_->draw(c, aa_input_, settings_.fxaa, target.width, target.height);
                ++stats.draw_calls;
                ++stats.fxaa_draw_calls;
                c.end_rendering();
                c.end_debug_label();
            });
    }

    if (target.readback_buffer.is_valid()) {
        graph_.add_pass(
            "Readback", rhi::QueueType::Graphics,
            [&](PassBuilder& b) {
                b.copy_src(color);
                b.set_side_effects();
            },
            [&](PassContext& ctx) {
                // The buffer is shared by consecutive frames, and an earlier frame's copy into it may still be running when
                // this one is submitted: order this copy's write after that one (write-after-write).
                const rhi::ResourceState copy_write{rhi::PipelineStage::Transfer, rhi::Access::TransferWrite, rhi::TextureLayout::Undefined};
                const rhi::BufferBarrier waw{target.readback_buffer, copy_write, copy_write, 0, ~uint64_t{0}};
                ctx.command_list()->barrier({}, std::span(&waw, 1));
                ctx.command_list()->copy_texture_to_buffer(target.texture, target.readback_buffer);
            });
    }

    const CompileResult& compiled = graph_.compile();
    if (!compiled.ok) {
        throw std::logic_error("SceneRenderer: render graph failed to compile: " + compiled.errors.front());
    }
    ExecuteContext ec;
    ec.command_list = &cmd;
    ec.submit_barriers = [&](std::span<const rhi::TextureBarrier> t, std::span<const rhi::BufferBarrier> b) {
        cmd.barrier(t, b);
        stats.barriers += static_cast<uint32_t>(t.size() + b.size());
    };
    graph_.execute(ec);
    depth_state_ = ao_ ? rhi::sampled_texture_state : depth_attachment_state;
    if (shadow_models != nullptr) {
        shadow_state_ = rhi::sampled_texture_state;  // the graph left it there (its final state)
        shadow_ready_ = true;
    }
    if (settings_.hdr) {
        hdr_state_ = rhi::sampled_texture_state;  // the graph left it there (its final state)
    }
    if (fxaa_) {
        aa_state_ = rhi::sampled_texture_state;
    }
    if (ao_) {
        ao_value_state_ = rhi::sampled_texture_state;
        hdr_ao_state_ = rhi::sampled_texture_state;
    }
    return stats;
}

} // namespace saige::render

namespace saige::render {

SunShadowSettings shadow_quality_preset(ShadowQuality quality, float distance)
{
    SunShadowSettings s;
    s.enabled = true;
    s.distance = distance;
    switch (quality) {
    case ShadowQuality::Low:
        s.cascades = 1;
        s.map_size = 1024;
        s.filter_radius = 0;
        break;
    case ShadowQuality::Medium:
        s.cascades = 2;
        s.map_size = 1024;
        s.filter_radius = 1;
        break;
    case ShadowQuality::High:
        s.cascades = 3;
        s.map_size = 2048;
        s.filter_radius = 1;
        break;
    case ShadowQuality::Ultra:
        s.cascades = 4;
        s.map_size = 2048;
        s.filter_radius = 2;
        break;
    }
    return s;
}

} // namespace saige::render
