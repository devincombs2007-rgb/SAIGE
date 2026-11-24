#include "saige/render/scene_renderer.hpp"

#include "saige/shaders/scene_cube.spv.hpp"
#include "saige/shaders/scene_ground.spv.hpp"
#include "saige/shaders/scene_sky.spv.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace saige::render {

namespace {

constexpr rhi::Format depth_format = rhi::Format::D32_FLOAT;

void release_targets(rhi::Device& device, rhi::TextureHandle depth, rhi::TextureHandle hdr,
                     rhi::BindGroupHandle hdr_input)
{
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
      settings_((validate_lighting(settings.lighting), validate_atmosphere_settings(settings), settings)),
      registry_(device)
{
    using namespace saige::shaders;
    if (settings_.hdr) {
        tone_mapper_ = std::make_unique<ToneMapper>(device_, color_format_);
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

Float3 SceneRenderer::atmosphere_fog() const
{
    Float3 sun = settings_.lighting.sun_direction;
    sun = normalize(sun);
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
    if (sky_pipeline_.is_valid()) device_.destroy(sky_pipeline_);
    if (sky_ps_.is_valid()) device_.destroy(sky_ps_);
    if (sky_vs_.is_valid()) device_.destroy(sky_vs_);
    release_targets(device_, depth_, hdr_, hdr_input_);
    device_.destroy(cube_indices_);
    device_.destroy(cube_vertices_);
    device_.destroy(ground_pipeline_);
    device_.destroy(cube_pipeline_);
    device_.destroy(ground_ps_);
    device_.destroy(ground_vs_);
    device_.destroy(cube_ps_);
    device_.destroy(cube_vs_);
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
    depth_desc.debug_name = "SceneDepth";

    rhi::TextureHandle next_depth;
    rhi::TextureHandle next_hdr;
    rhi::BindGroupHandle next_input;
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
            next_input = tone_mapper_->create_input(next_hdr);
        }
    } catch (...) {
        release_targets(device_, next_depth, next_hdr, next_input);
        throw;
    }

    // Install the whole attachment set before retiring the old one on the device's completion timeline.
    std::swap(depth_, next_depth);
    std::swap(hdr_, next_hdr);
    std::swap(hdr_input_, next_input);
    target_width_ = width;
    target_height_ = height;
    depth_state_ = {};
    hdr_state_ = {};
    release_targets(device_, next_depth, next_hdr, next_input);
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
            const Float3 fog = atmosphere_fog();
            ms.sky = {fog.x, fog.y, fog.z, 1.0f};  // the models' fog and ambient sky color follow the horizon
        }
        ms.fog_density = settings_.fog_density;
        ms.shading = settings_.model_shading;
        ms.lighting = settings_.lighting;
        ms.alpha_blending = settings_.alpha_blending;
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
    if (settings_.atmosphere.enabled) {
        ensure_sky();  // first, with nothing else created yet: a device failure is a failed frame that leaves nothing behind
    }
    ensure_targets(target.width, target.height);

    const float aspect = static_cast<float>(target.width) / static_cast<float>(target.height);
    DrawConstants constants{};
    constants.view_proj = view_projection(frame, aspect);
    const Float3 eye = camera_eye(frame);
    fill4(constants.camera_fog, eye.x, eye.y, eye.z, settings_.fog_density);
    rhi::ClearColor clear = settings_.sky;
    if (settings_.atmosphere.enabled) {
        const Float3 fog = atmosphere_fog();
        clear = {fog.x, fog.y, fog.z, 1.0f};
    }
    fill4(constants.fog_color, clear.r, clear.g, clear.b, 1.0f);

    graph_.reset();
    const RGTexture color = graph_.import_texture("SceneColor", *color_desc, target.texture, target.initial_state,
                                                  target.final_state);
    // The texture the Scene pass draws into: the output itself, or the HDR target that the tone-map pass resolves to it.
    rhi::TextureHandle scene_texture = target.texture;
    RGTexture scene_color = color;
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
    const RGTexture depth = graph_.import_texture("SceneDepth", depth_desc, depth_, depth_state_);

    const Float3 ground_center = camera_target(frame);
    graph_.add_pass(
        "Scene", rhi::QueueType::Graphics,
        [&](PassBuilder& b) {
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
                const Float3 sun = normalize(settings_.lighting.sun_direction);
                SkyConstants sk{};
                fill4(sk.right, right.x, right.y, right.z, 0.0f);
                fill4(sk.up, up.x, up.y, up.z, at.exposure);
                fill4(sk.forward, fwd.x, fwd.y, fwd.z, at.sun_intensity);
                fill4(sk.to_sun, sun.x, sun.y, sun.z, at.mie_g);
                fill4(sk.geometry, at.planet_radius_km, at.planet_radius_km + at.atmosphere_height_km, at.rayleigh_scale_height_km, at.mie_scale_height_km);
                fill4(sk.beta_r, at.rayleigh_coefficient.x, at.rayleigh_coefficient.y, at.rayleigh_coefficient.z, at.mie_coefficient);
                fill4(sk.misc, settings_.hdr ? 0.0f : 1.0f, at.observer_height_km, 0.0f, 0.0f);
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
            }
            c.end_rendering();
            c.write_timestamp(timestamp_scene_end);
            c.end_debug_label();
        });

    if (settings_.hdr) {
        graph_.add_pass(
            "ToneMap", rhi::QueueType::Graphics,
            [&](PassBuilder& b) {
                b.sample(scene_color, rhi::PipelineStage::PixelShader);
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
                c.begin_debug_label("ToneMap");
                c.begin_rendering(rd);
                c.set_viewport({0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f});
                c.set_scissor({0, 0, target.width, target.height});
                tone_mapper_->draw(c, hdr_input_, settings_.tone_map);
                ++stats.draw_calls;
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
    depth_state_ = depth_attachment_state;
    if (settings_.hdr) {
        hdr_state_ = rhi::sampled_texture_state;  // the graph left it there (its final state)
    }
    return stats;
}

} // namespace saige::render
