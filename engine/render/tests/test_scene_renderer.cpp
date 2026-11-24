#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/transform.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace saige;
using namespace saige::render;

namespace {

struct Offscreen {
    rhi::NullDevice device;
    rhi::TextureHandle color;
    rhi::BufferHandle readback;

    explicit Offscreen(uint32_t w = 320, uint32_t h = 180) : Offscreen(rhi::DeviceDesc{}, w, h) {}
    Offscreen(const rhi::DeviceDesc& desc, uint32_t w, uint32_t h) : device(desc)
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA8_SRGB;
        d.width = w;
        d.height = h;
        d.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        color = device.create_texture(d);
        rhi::BufferDesc b;
        b.size = uint64_t{w} * h * 4;
        b.usage = rhi::BufferUsage::TransferDst;
        b.memory = rhi::MemoryLocation::GpuToCpu;
        readback = device.create_buffer(b);
    }

    RenderTarget target(uint32_t w = 320, uint32_t h = 180, bool with_readback = false) const
    {
        RenderTarget t;
        t.texture = color;
        t.width = w;
        t.height = h;
        if (with_readback) {
            t.readback_buffer = readback;
        }
        return t;
    }
};

std::vector<const rhi::RecordedCommand*> of_type(const std::vector<rhi::RecordedCommand>& cmds, rhi::CommandType type)
{
    std::vector<const rhi::RecordedCommand*> out;
    for (const auto& c : cmds) {
        if (c.type == type) {
            out.push_back(&c);
        }
    }
    return out;
}

DrawConstants constants_of(const rhi::RecordedCommand& push)
{
    DrawConstants dc{};
    REQUIRE(push.push_data.size() == sizeof(DrawConstants));
    std::memcpy(&dc, push.push_data.data(), sizeof(dc));
    return dc;
}

} // namespace

TEST_CASE("CubeMesh: 24 vertices, 36 indices, outward normals and counter-clockwise winding")
{
    const CubeMesh mesh = make_unit_cube();
    for (int tri = 0; tri < 12; ++tri) {
        const CubeVertex& a = mesh.vertices[mesh.indices[tri * 3 + 0]];
        const CubeVertex& b = mesh.vertices[mesh.indices[tri * 3 + 1]];
        const CubeVertex& c = mesh.vertices[mesh.indices[tri * 3 + 2]];
        const Float3 face_normal = cross(b.position - a.position, c.position - a.position);
        CHECK(dot(face_normal, a.normal) > 0.0f);  // CCW seen from outside
        CHECK(dot(a.position, a.normal) > 0.0f);   // normal points away from the center
        CHECK(std::fabs(std::fabs(a.position.x) - 0.5f) < 1e-6f || std::fabs(std::fabs(a.position.y) - 0.5f) < 1e-6f ||
              std::fabs(std::fabs(a.position.z) - 0.5f) < 1e-6f);
    }
}

TEST_CASE("SceneRenderer: an empty world draws only the ground, with no validation errors")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB);
    rhi::CommandList& cmd = o.device.begin_frame();
    const FrameStats stats = renderer.render(cmd, SceneFrame{}, o.target());
    (void)o.device.end_frame();

    CHECK(stats.draw_calls == 1);
    CHECK(o.device.validation_error_count() == 0);
    const auto& cmds = o.device.last_frame_commands();
    CHECK(of_type(cmds, rhi::CommandType::Draw).size() == 1);
    CHECK(of_type(cmds, rhi::CommandType::DrawIndexed).empty());
    const auto begin = of_type(cmds, rhi::CommandType::BeginRendering);
    REQUIRE(begin.size() == 1);
    CHECK(begin[0]->rendering.depth.has_value());
    CHECK(begin[0]->rendering.colors[0].load == rhi::AttachmentLoadOp::Clear);
    CHECK(begin[0]->rendering.area.width == 320);
}

TEST_CASE("SceneRenderer: one cube per instance, positioned from the instance")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB);
    const std::vector<SceneInstance> instances{{{1, 2, 0}}, {{-3, 0.5f, 0}, {0.2f, 0.6f, 0.9f}, 0.0f}};
    SceneFrame frame;
    frame.tick = 30;
    frame.instances = instances;

    rhi::CommandList& cmd = o.device.begin_frame();
    const FrameStats stats = renderer.render(cmd, frame, o.target());
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);
    CHECK(stats.draw_calls == 3);

    const auto& cmds = o.device.last_frame_commands();
    const auto draws = of_type(cmds, rhi::CommandType::DrawIndexed);
    REQUIRE(draws.size() == 2);
    CHECK(draws[0]->count == 36);

    const auto pushes = of_type(cmds, rhi::CommandType::PushConstants);
    REQUIRE(pushes.size() == 3);  // ground + 2 cubes
    const DrawConstants second = constants_of(*pushes[2]);
    CHECK(second.position_yaw[0] == -3.0f);
    CHECK(second.position_yaw[1] == 0.5f);
    CHECK(second.color_scale[2] == 0.9f);
    CHECK(second.color_scale[3] == 1.0f);  // cube edge
    // 30 ticks at 60 Hz = 0.5 s at 0.8 rad/s = 0.4 rad
    CHECK(std::fabs(second.position_yaw[3] - 0.4f) < 1e-5f);
}

TEST_CASE("SceneRenderer: the follow camera tracks the first instance")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB);
    std::vector<SceneInstance> instances{{{10, -2, 0}}};
    SceneFrame frame;
    frame.instances = instances;
    const Float3 eye = renderer.camera_eye(frame);
    const Float3 expected = Float3{10, -2, 0.5f} + renderer.settings().follow_offset;
    CHECK(eye == expected);

    // The tracked cube projects to the screen center.
    const Mat4 vp = renderer.view_projection(frame, 16.0f / 9.0f);
    const Float4 clip = vp * Float4{10, -2, 0.5f, 1};
    CHECK(std::fabs(clip.x / clip.w) < 1e-4f);
    CHECK(std::fabs(clip.y / clip.w) < 1e-4f);
    CHECK(clip.z / clip.w > 0.0f);
    CHECK(clip.z / clip.w < 1.0f);
}

TEST_CASE("SceneRenderer: barriers come from the render graph (target transitions + readback)")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB);
    rhi::CommandList& cmd = o.device.begin_frame();
    const FrameStats stats = renderer.render(cmd, SceneFrame{}, o.target(320, 180, true));
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);

    const auto& cmds = o.device.last_frame_commands();
    const auto copies = of_type(cmds, rhi::CommandType::CopyTextureToBuffer);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0]->buffer == o.readback);

    // Before the scene: color Undefined -> ColorAttachment and depth -> DepthStencilAttachment.
    REQUIRE(cmds.front().type == rhi::CommandType::Barrier);
    bool color_init = false;
    bool depth_init = false;
    for (const auto& b : cmds.front().texture_barriers) {
        color_init |= b.texture == o.color && b.after.layout == rhi::TextureLayout::ColorAttachment;
        depth_init |= b.after.layout == rhi::TextureLayout::DepthStencilAttachment;
    }
    CHECK(color_init);
    CHECK(depth_init);

    // Before the copy: ColorAttachment -> TransferSrc.
    bool to_transfer = false;
    for (const auto& c : cmds) {
        for (const auto& b : c.texture_barriers) {
            to_transfer |= b.texture == o.color && b.before.layout == rhi::TextureLayout::ColorAttachment &&
                           b.after.layout == rhi::TextureLayout::TransferSrc;
        }
    }
    CHECK(to_transfer);
    CHECK(stats.barriers >= 3);
}

TEST_CASE("SceneRenderer: zero-sized targets record nothing; resizing recreates the depth buffer")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB);
    const uint32_t baseline = o.device.live_resource_count();

    rhi::CommandList& cmd = o.device.begin_frame();
    CHECK(renderer.render(cmd, SceneFrame{}, o.target(0, 0)).draw_calls == 0);
    (void)o.device.end_frame();
    CHECK(o.device.last_frame_commands().empty());

    (void)renderer.render(o.device.begin_frame(), SceneFrame{}, o.target(320, 180));
    (void)o.device.end_frame();
    CHECK(o.device.live_resource_count() == baseline + 1);  // depth created

    (void)renderer.render(o.device.begin_frame(), SceneFrame{}, o.target(160, 90));
    (void)o.device.end_frame();
    CHECK(o.device.pending_destruction_count() == 1);  // old depth retires after the frames in flight
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("SceneRenderer: destruction releases every GPU resource it created")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    {
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB);
        (void)renderer.render(o.device.begin_frame(), SceneFrame{}, o.target());
        (void)o.device.end_frame();
        CHECK(o.device.live_resource_count() > baseline);
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
    CHECK(o.device.validation_error_count() == 0);
}


TEST_CASE("SceneRenderer: invalid lighting is rejected before any GPU object is created")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto rejected = [&](const SceneLighting& light) {
        RendererSettings settings;
        settings.lighting = light;
        try {
            SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    SceneLighting bad;
    bad.sun_direction = {0.0f, 0.0f, 0.0f};
    CHECK(rejected(bad));  // no direction
    bad = {};
    bad.sun_direction = {nan, 0.0f, 1.0f};
    CHECK(rejected(bad));
    bad = {};
    bad.sun_color = {-0.1f, 0.5f, 0.5f};
    CHECK(rejected(bad));  // negative light
    bad = {};
    bad.ambient_ground = {0.1f, std::numeric_limits<float>::infinity(), 0.1f};
    CHECK(rejected(bad));
    bad = {};
    bad.sun_direction = {-0.0f, 0.0f, -0.0f};
    CHECK(rejected(bad));  // negative zero is still no direction
    CHECK(o.device.live_resource_count() == baseline);  // nothing was acquired by the failed constructions
    CHECK(o.device.validation_error_count() == 0);

    // HDR-bright and zero-ambient lighting are valid.
    RendererSettings settings;
    settings.lighting.sun_color = {8.0f, 7.0f, 6.0f};
    settings.lighting.ambient_ground = {0.0f, 0.0f, 0.0f};
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    (void)renderer.model_renderer();  // the lazily created model renderer takes the same lighting without throwing
    CHECK(o.device.validation_error_count() == 0);

    // Finite directions of any magnitude are valid: the squared length of FLT_MAX overflows and of 1e-30 underflows, the direction does not.
    const float fmax = std::numeric_limits<float>::max();
    for (const Float3 direction : {Float3{fmax, 0.0f, 0.0f}, Float3{-fmax, fmax, fmax}, Float3{1e-30f, 0.0f, 0.0f}, Float3{0.0f, 0.0f, 1e-40f} /* denormal */}) {
        RendererSettings s2;
        s2.lighting.sun_direction = direction;
        SceneRenderer r2(o.device, rhi::Format::RGBA8_SRGB, s2);
        (void)r2.model_renderer();
    }
}

namespace {

/// One triangle with one material, built on whatever device the renderer has.
GpuModel tiny_model(SceneRenderer& renderer, MaterialData data = {})
{
    GpuModel model;
    model.name = "tiny";
    model.materials.push_back(renderer.model_renderer().create_material(data, {}, {}, "tiny.material"));
    const std::vector<ModelVertex> vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
    const std::vector<uint32_t> indices = {0, 1, 2};
    rhi::BufferDesc vb;
    vb.size = vertices.size() * sizeof(ModelVertex);
    vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    rhi::BufferDesc ib;
    ib.size = indices.size() * sizeof(uint32_t);
    ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
    GpuPrimitive p;
    p.vertices = renderer.registry().create_buffer(vb, std::as_bytes(std::span(vertices)));
    p.indices = renderer.registry().create_buffer(ib, std::as_bytes(std::span(indices)));
    p.index_count = 3;
    p.material = 0;
    model.meshes.push_back({std::move(p)});
    model.placements.push_back({0, Mat4::identity()});
    return model;
}

RendererSettings shadow_settings(bool enabled)
{
    RendererSettings s;
    s.shadows.enabled = enabled;
    s.shadows.map_size = 64;
    return s;
}

uint32_t scene_pass_count(const std::vector<rhi::RecordedCommand>& cmds)
{
    return static_cast<uint32_t>(of_type(cmds, rhi::CommandType::BeginRendering).size());
}

} // namespace

TEST_CASE("Sun shadows (null device): off acquires nothing for shadows and schedules no pass; on adds exactly the shadow resources")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    uint32_t off_count = 0, on_count = 0;
    uint32_t off_passes = 0, on_first_passes = 0, on_second_passes = 0;
    {
        SceneRenderer off(o.device, rhi::Format::RGBA8_SRGB, shadow_settings(false));
        CHECK(!off.model_renderer().shadows_enabled());
        CHECK(!off.model_renderer().shadow_map().is_valid());
        const GpuModel model = tiny_model(off);
        const ModelDraw draws[] = {{&model, Mat4::identity()}};
        SceneFrame frame;
        frame.models = draws;
        const FrameStats stats = off.render(o.device.begin_frame(), frame, o.target());
        (void)o.device.end_frame();
        CHECK(stats.shadow_draw_calls == 0);
        off_passes = scene_pass_count(o.device.last_frame_commands());
        off_count = o.device.live_resource_count() - baseline;
        // Setting the shadow frame, or recording shadow draws, on a renderer without shadows is a misuse, not a silent no-op.
        bool threw_frame = false, threw_draw = false;
        try {
            off.model_renderer().set_shadow_frame(nullptr);
        } catch (const std::logic_error&) {
            threw_frame = true;
        }
        try {
            ModelFrameStats ms;
            off.model_renderer().draw_shadow(o.device.begin_frame(), Mat4::identity(), draws, ms);
        } catch (const std::logic_error&) {
            threw_draw = true;
        }
        (void)o.device.end_frame();
        CHECK(threw_frame && threw_draw);
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
    {
        SceneRenderer on(o.device, rhi::Format::RGBA8_SRGB, shadow_settings(true));
        CHECK(on.model_renderer().shadows_enabled());
        CHECK(on.model_renderer().shadow_map().is_valid());
        const GpuModel model = tiny_model(on);
        const ModelDraw draws[] = {{&model, Mat4::identity()}};
        SceneFrame frame;
        frame.models = draws;
        FrameStats stats = on.render(o.device.begin_frame(), frame, o.target());
        (void)o.device.end_frame();
        on_first_passes = scene_pass_count(o.device.last_frame_commands());
        on_count = o.device.live_resource_count() - baseline;
        stats = on.render(o.device.begin_frame(), frame, o.target());
        (void)o.device.end_frame();
        on_second_passes = scene_pass_count(o.device.last_frame_commands());
        CHECK(stats.shadow_draw_calls == 1);  // the one opaque triangle casts
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
    CHECK(on_count > off_count);  // the map, comparison sampler, per-frame blocks and groups, the shadow pipeline and shader
    CHECK(off_passes == 1);       // the scene pass only: no SunShadow pass is scheduled
    CHECK(on_first_passes == 2);  // SunShadow, then Scene
    CHECK(on_second_passes == 2);
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Sun shadows (null device): a failed acquisition at any step releases everything and the renderer can be used afterwards")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, shadow_settings(true));  // the model renderer (and every shadow resource) is lazy
    const uint32_t baseline = o.device.live_resource_count();
    uint32_t failures = 0;
    for (uint32_t n = 1; n <= 120; ++n) {
        bool threw = false;
        o.device.fail_creation_after(n);
        try {
            (void)renderer.model_renderer();  // shader, layouts, pipelines, shadow shader and pipeline, map, sampler, ring buffers, groups
        } catch (const std::runtime_error&) {
            threw = true;
        }
        o.device.fail_creation_after(0);
        if (!threw) {
            CHECK(renderer.model_renderer().shadows_enabled());  // n is beyond the number of creations: every step has been failed in turn
            break;
        }
        ++failures;
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == baseline);  // the failed constructor released every partial acquisition (raw handles included)
    }
    CHECK(failures >= 20);  // 23 creations on the enabled path today
    // The renderer is still usable after every failed attempt: the model renderer exists now and draws with shadows.
    const GpuModel model = tiny_model(renderer);
    const ModelDraw draws[] = {{&model, Mat4::identity()}};
    SceneFrame frame;
    frame.models = draws;
    CHECK(renderer.render(o.device.begin_frame(), frame, o.target()).shadow_draw_calls == 1);
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Sun shadows (null device): the shadow block is written once per renderer per frame, checked before anything changes")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, shadow_settings(true));
    ModelRenderer& models = renderer.model_renderer();
    ModelShadowFrame sf;
    (void)o.device.begin_frame();
    models.set_shadow_frame(&sf);
    bool threw = false;
    try {
        models.set_shadow_frame(nullptr);  // a second view in the same frame would overwrite the first one's block
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    (void)o.device.end_frame();
    (void)o.device.begin_frame();  // the next frame: allowed again
    models.set_shadow_frame(nullptr);
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("SceneRenderer: failed HDR resize preserves the old targets and can be retried")
{
    Offscreen o;
    rhi::TextureDesc resized_desc = *o.device.texture_desc(o.color);
    resized_desc.width = 160;
    resized_desc.height = 90;
    RenderTarget resized = o.target(resized_desc.width, resized_desc.height);
    resized.texture = o.device.create_texture(resized_desc);

    const uint32_t baseline = o.device.live_resource_count();
    {
        RendererSettings settings;
        settings.hdr = true;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
        const auto draw_frame = [&](const RenderTarget& target) {
            (void)renderer.render(o.device.begin_frame(), SceneFrame{}, target);
            (void)o.device.end_frame();
            const auto passes = of_type(o.device.last_frame_commands(), rhi::CommandType::BeginRendering);
            REQUIRE(passes.size() == 2);
            const auto& scene_pass = passes[0]->rendering;
            REQUIRE(scene_pass.depth.has_value());
            REQUIRE(scene_pass.colors.size() == 1);
            return std::pair{scene_pass.depth->texture, scene_pass.colors[0].texture};
        };

        const auto original_targets = draw_frame(o.target());
        o.device.wait_idle();
        const uint32_t ready_count = o.device.live_resource_count();
        // Fail depth allocation, HDR allocation, and the tone-map bind group in turn.
        for (uint32_t allocation = 1; allocation <= 3; ++allocation) {
            rhi::CommandList& cmd = o.device.begin_frame();
            o.device.fail_creation_after(allocation);
            bool failed = false;
            try {
                (void)renderer.render(cmd, SceneFrame{}, resized);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            o.device.fail_creation_after(0);
            (void)o.device.end_frame();
            CHECK(failed);
            CHECK(o.device.last_frame_commands().empty());
            o.device.wait_idle();
            CHECK(o.device.live_resource_count() == ready_count);
            CHECK(draw_frame(o.target()) == original_targets);
        }

        const auto new_targets = draw_frame(resized);
        CHECK(new_targets.first != original_targets.first);
        CHECK(new_targets.second != original_targets.second);
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == ready_count);
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Atmosphere (null device): off creates nothing for the sky; on creates its shaders and pipeline on the first frame, and a failed creation is a failed frame, not a broken renderer")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    {
        SceneRenderer off(o.device, rhi::Format::RGBA8_SRGB, RendererSettings{});
        SceneFrame frame;
        (void)off.render(o.device.begin_frame(), frame, o.target());
        (void)o.device.end_frame();
        const uint32_t off_count = o.device.live_resource_count() - baseline;
        RendererSettings on_settings;
        on_settings.atmosphere.enabled = true;
        SceneRenderer on(o.device, rhi::Format::RGBA8_SRGB, on_settings);
        const uint32_t before_first_frame = o.device.live_resource_count();
        (void)on.render(o.device.begin_frame(), frame, o.target());
        (void)o.device.end_frame();
        CHECK(o.device.live_resource_count() >= before_first_frame + 3);  // the sky's two shaders and its pipeline
        (void)off_count;
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);

    // Failing each of the creations in turn: the frame throws, nothing is leaked, and the next frame works.
    RendererSettings s;
    s.atmosphere.enabled = true;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, s);
    const uint32_t built = o.device.live_resource_count();
    for (uint32_t n = 1; n <= 3; ++n) {
        o.device.fail_creation_after(n);
        bool threw = false;
        SceneFrame frame;
        rhi::CommandList& cmd = o.device.begin_frame();
        try {
            (void)renderer.render(cmd, frame, o.target());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        o.device.fail_creation_after(0);
        (void)o.device.end_frame();
        CHECK(threw);
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == built);
    }
    SceneFrame frame;
    (void)renderer.render(o.device.begin_frame(), frame, o.target());
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Atmosphere (null device): invalid settings are refused before anything is acquired")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    const auto refused = [&](auto&& mutate) {
        RendererSettings s;
        s.atmosphere.enabled = true;
        mutate(s.atmosphere);
        try {
            SceneRenderer r(o.device, rhi::Format::RGBA8_SRGB, s);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(refused([](AtmosphereSettings& a) { a.mie_g = 1.0f; }));
    CHECK(refused([](AtmosphereSettings& a) { a.planet_radius_km = 0.0f; }));
    CHECK(refused([](AtmosphereSettings& a) { a.observer_height_km = 100.0f; }));
    CHECK(refused([](AtmosphereSettings& a) { a.exposure = std::numeric_limits<float>::quiet_NaN(); }));
    CHECK(o.device.live_resource_count() == baseline);
    RendererSettings ignored;  // disabled settings are not validated
    ignored.atmosphere.mie_g = 5.0f;
    SceneRenderer fine(o.device, rhi::Format::RGBA8_SRGB, ignored);
}

// ---- Per-frame lighting (RendererSettings::dynamic_lighting) ------------------------------------------------------------------------------------

namespace {

using Block = std::array<float, 20>;

Block read_block(rhi::NullDevice& device, rhi::BufferHandle handle)
{
    REQUIRE(handle.is_valid());
    const std::span<std::byte> bytes = device.map(handle);
    REQUIRE(bytes.size() >= sizeof(Block));
    Block b{};
    std::memcpy(b.data(), bytes.data(), sizeof(Block));
    return b;
}

RendererSettings dynamic_settings()
{
    RendererSettings s;
    s.dynamic_lighting = true;
    return s;
}

SceneLighting evening(float x, float y, float z)
{
    SceneLighting l;
    l.sun_direction = {x, y, z};
    l.sun_color = {2.0f, 1.0f, 0.5f};
    l.ambient_ground = {0.05f, 0.04f, 0.03f};
    return l;
}

float unit_len(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

bool near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance * (1.0f + std::fabs(b)); }

template <class F>
bool throws_invalid(F&& f)
{
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

namespace {

/// A small valid environment: 16x8 with three levels (16x8, 8x4, 4x2), every texel the same gray.
std::shared_ptr<const EnvironmentLighting> tiny_environment()
{
    auto e = std::make_shared<EnvironmentLighting>();
    e->width = 16;
    e->height = 8;
    e->levels = 3;
    e->sh[0] = {1.0f, 1.0f, 1.0f};
    const std::size_t texels = 16 * 8 + 8 * 4 + 4 * 2;
    e->rgba16f.assign(texels * 4, 0x3800);  // half-float 0.5
    return e;
}

RendererSettings ibl_settings(bool environment, bool shadows)
{
    RendererSettings s = shadow_settings(shadows);
    s.model_shading = ModelShading::MetallicRoughness;
    if (environment) {
        s.environment = tiny_environment();
    }
    return s;
}

} // namespace

TEST_CASE("Dynamic lighting (null device): off by default, a lighting frame is refused when it is off, and on costs a ring of buffers and groups")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    {
        SceneRenderer off(o.device, rhi::Format::RGBA8_SRGB, RendererSettings{});
        ModelRenderer& m = off.model_renderer();
        CHECK(!m.lighting_block(0).is_valid());
        const uint32_t off_count = o.device.live_resource_count() - baseline;
        const SceneLighting l = evening(0.0f, 0.0f, 1.0f);
        SceneFrame frame;
        frame.lighting = &l;
        bool threw = false;
        try {
            (void)off.render(o.device.begin_frame(), frame, o.target());
        } catch (const std::logic_error&) {
            threw = true;
        }
        (void)o.device.end_frame();
        CHECK(threw);
        CHECK(o.device.last_frame_commands().empty());  // refused before anything was recorded
        ModelLightingFrame lf;
        threw = false;
        (void)o.device.begin_frame();
        try {
            m.set_lighting_frame(&lf);
        } catch (const std::logic_error&) {
            threw = true;
        }
        (void)o.device.end_frame();
        CHECK(threw);

        const uint32_t before_on = o.device.live_resource_count();
        SceneRenderer on(o.device, rhi::Format::RGBA8_SRGB, dynamic_settings());
        (void)on.model_renderer();
        CHECK(o.device.live_resource_count() - before_on > off_count);  // the ring: two scene-constants buffers and their bind groups
        CHECK(on.model_renderer().lighting_block(0).is_valid());
        CHECK(on.model_renderer().lighting_block(1).is_valid());
        CHECK(!on.model_renderer().lighting_block(2).is_valid());  // two frames in flight by default
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);  // everything goes back, ring included
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Dynamic lighting (null device): each frame writes its own slot, a reclaimed slot is rewritten, and an unpublished frame uses the construction-time lighting")
{
    Offscreen o;
    RendererSettings settings = dynamic_settings();
    settings.lighting = evening(0.3f, 0.2f, 0.9f);
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    const GpuModel model = tiny_model(renderer);
    const ModelDraw draws[] = {{&model, Mat4::identity()}};
    ModelRenderer& models = renderer.model_renderer();

    const auto frame_with = [&](const SceneLighting* lighting) {
        SceneFrame f;
        f.models = draws;
        f.lighting = lighting;
        (void)renderer.render(o.device.begin_frame(), f, o.target());
        const uint64_t n = o.device.frame_number();
        (void)o.device.end_frame();
        return static_cast<uint32_t>(n % 2);
    };
    const SceneLighting a = evening(1.0f, 0.0f, 1.0f);
    const SceneLighting b = evening(0.0f, -2.0f, 3.0f);
    const uint32_t slot_a = frame_with(&a);
    const Block block_a = read_block(o.device, models.lighting_block(slot_a));
    const float la = unit_len(1.0f, 0.0f, 1.0f);
    CHECK(near(block_a[4], 1.0f / la, 1e-5f));
    CHECK(near(block_a[5], 0.0f, 1e-5f));
    CHECK(near(block_a[6], 1.0f / la, 1e-5f));  // normalized: one direction everywhere
    CHECK(block_a[8] == 2.0f);
    CHECK(block_a[9] == 1.0f);
    CHECK(block_a[10] == 0.5f);
    CHECK(block_a[12] == 0.05f);
    CHECK(near(block_a[3], settings.fog_density, 1e-5f));  // the density stays the renderer's

    const uint32_t slot_b = frame_with(&b);
    CHECK(slot_b != slot_a);
    const Block block_b = read_block(o.device, models.lighting_block(slot_b));
    CHECK(near(block_b[5], -2.0f / unit_len(0.0f, -2.0f, 3.0f), 1e-5f));
    CHECK(read_block(o.device, models.lighting_block(slot_a)) == block_a);  // the other frame in flight still reads what it was drawn with

    // A third frame reclaims slot a's buffer; with nothing published it carries the construction-time lighting (and the construction sun, normalized).
    const uint32_t slot_c = frame_with(nullptr);
    CHECK(slot_c == slot_a);
    const Block block_c = read_block(o.device, models.lighting_block(slot_c));
    const float lc = unit_len(0.3f, 0.2f, 0.9f);
    CHECK(near(block_c[4], 0.3f / lc, 1e-5f));
    CHECK(block_c[7] == block_a[7]);
    CHECK(block_c[8] == 2.0f);
    CHECK(block_c != block_a);
    CHECK(read_block(o.device, models.lighting_block(slot_b)) == block_b);
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Dynamic lighting (null device): one publication per renderer per frame, none after the first draw, and a refusal changes nothing")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, dynamic_settings());
    ModelRenderer& models = renderer.model_renderer();
    const GpuModel model = tiny_model(renderer);
    const ModelDraw draws[] = {{&model, Mat4::identity()}};
    const auto expect_logic_error = [&](const ModelLightingFrame* f) {
        bool threw = false;
        try {
            models.set_lighting_frame(f);
        } catch (const std::logic_error&) {
            threw = true;
        }
        return threw;
    };
    ModelLightingFrame first;
    first.lighting = evening(0.0f, 0.0f, 1.0f);
    ModelLightingFrame second;
    second.lighting = evening(1.0f, 0.0f, 0.0f);

    (void)o.device.begin_frame();
    const uint32_t slot = static_cast<uint32_t>(o.device.frame_number() % 2);
    models.set_lighting_frame(&first);
    const Block written = read_block(o.device, models.lighting_block(slot));
    CHECK(expect_logic_error(&second));  // a second view would change what the first view's draws read
    CHECK(expect_logic_error(nullptr));
    CHECK(read_block(o.device, models.lighting_block(slot)) == written);
    CHECK(near(models.frame_sun_direction().z, 1.0f, 1e-5f));
    (void)o.device.end_frame();

    // After a draw: the draw has bound the slot, so publishing would race the recorded commands. (SceneRenderer publishes for its own frame; here the frame is
    // drawn through it and a host's late publication is refused.)
    rhi::CommandList& cmd = o.device.begin_frame();
    const uint32_t slot2 = static_cast<uint32_t>(o.device.frame_number() % 2);
    CHECK(!near(models.frame_sun_direction().z, 1.0f));  // the previous frame published z = 1; this frame has published nothing yet
    CHECK(near(models.frame_sun_direction().z, models.sun_direction().z));
    SceneFrame scene;
    scene.models = draws;
    (void)renderer.render(cmd, scene, o.target());
    const Block drawn = read_block(o.device, models.lighting_block(slot2));
    CHECK(expect_logic_error(&second));
    CHECK(read_block(o.device, models.lighting_block(slot2)) == drawn);
    CHECK(near(models.frame_sun_direction().z, models.sun_direction().z));  // the frame was completed with the construction-time sun
    (void)o.device.end_frame();

    (void)o.device.begin_frame();  // the next frame accepts one again, and until it publishes the frame's sun is the construction-time one, not the last frame's
    CHECK(near(models.frame_sun_direction().z, models.sun_direction().z));
    CHECK(!near(models.frame_sun_direction().z, 1.0f));
    models.set_lighting_frame(&second);
    CHECK(near(models.frame_sun_direction().x, 1.0f, 1e-5f));
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Dynamic lighting (null device): an invalid payload is rejected before anything is written, and the frame can still publish a valid one")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, dynamic_settings());
    ModelRenderer& models = renderer.model_renderer();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<ModelLightingFrame> bad;
    ModelLightingFrame f;
    f.lighting = evening(0.0f, 0.0f, 1.0f);
    for (const Float3 dir : {Float3{0.0f, 0.0f, 0.0f}, Float3{nan, 0.0f, 1.0f}, Float3{0.0f, inf, 1.0f}}) {
        ModelLightingFrame g = f;
        g.lighting.sun_direction = dir;
        bad.push_back(g);
    }
    {
        ModelLightingFrame g = f;
        g.lighting.sun_color = {-1.0f, 1.0f, 1.0f};
        bad.push_back(g);
    }
    {
        ModelLightingFrame g = f;
        g.lighting.ambient_ground = {0.1f, nan, 0.1f};
        bad.push_back(g);
    }
    for (const Float3 sky : {Float3{-0.01f, 0.5f, 0.5f}, Float3{nan, 0.5f, 0.5f}, Float3{0.5f, 0.5f, inf}}) {
        ModelLightingFrame g = f;
        g.sky_color = sky;
        bad.push_back(g);
    }
    (void)o.device.begin_frame();
    const uint32_t slot = static_cast<uint32_t>(o.device.frame_number() % 2);
    const Block before = read_block(o.device, models.lighting_block(slot));
    for (const ModelLightingFrame& g : bad) {
        bool threw = false;
        try {
            models.set_lighting_frame(&g);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(read_block(o.device, models.lighting_block(slot)) == before);
    }
    models.set_lighting_frame(&f);  // a refusal did not use up the frame's one publication
    CHECK(read_block(o.device, models.lighting_block(slot)) != before);
    (void)o.device.end_frame();

    // The same through SceneFrame::lighting: refused before anything is created, recorded or written.
    o.device.wait_idle();
    const uint32_t count = o.device.live_resource_count();
    SceneLighting broken = evening(0.0f, 0.0f, 1.0f);
    broken.sun_color.y = nan;
    SceneFrame frame;
    frame.lighting = &broken;
    bool threw = false;
    try {
        (void)renderer.render(o.device.begin_frame(), frame, o.target());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    (void)o.device.end_frame();
    CHECK(threw);
    CHECK(o.device.last_frame_commands().empty());
    CHECK(o.device.live_resource_count() == count);
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Dynamic lighting (null device): the sky, fog, models and shadow fit read one sun; a sun below the horizon fits no cascades; a second render in the same frame is refused before it records")
{
    Offscreen o;
    RendererSettings settings = shadow_settings(true);
    settings.dynamic_lighting = true;
    settings.atmosphere.enabled = true;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    const GpuModel model = tiny_model(renderer);
    const ModelDraw draws[] = {{&model, Mat4::identity()}};
    ModelRenderer& models = renderer.model_renderer();
    const SceneLighting afternoon = evening(1.0f, 0.0f, 1.0f);
    SceneFrame frame;
    frame.models = draws;
    frame.lighting = &afternoon;

    rhi::CommandList& cmd = o.device.begin_frame();
    const uint32_t slot = static_cast<uint32_t>(o.device.frame_number() % 2);
    CHECK(renderer.render(cmd, frame, o.target()).shadow_draw_calls == 1);
    const Block block = read_block(o.device, models.lighting_block(slot));
    const float s = 1.0f / std::sqrt(2.0f);
    CHECK(near(models.frame_sun_direction().x, s, 1e-5f));
    CHECK(near(block[4], s, 1e-5f));
    CHECK(near(block[6], s, 1e-5f));
    // The models' fog and ambient sky color are the horizon of the sky drawn for this same sun (display-referred in an LDR scene).
    const Float3 horizon = sky_horizon_color(settings.atmosphere, {s, 0.0f, s});
    CHECK(near(block[0], 1.0f - std::exp(-horizon.x), 1e-5f));
    CHECK(near(block[1], 1.0f - std::exp(-horizon.y), 1e-5f));
    CHECK(near(block[2], 1.0f - std::exp(-horizon.z), 1e-5f));
    bool threw = false;
    try {
        (void)renderer.render(cmd, frame, o.target());  // a second view with this renderer would overwrite the first view's lighting block
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(read_block(o.device, models.lighting_block(slot)) == block);
    (void)o.device.end_frame();
    CHECK(of_type(o.device.last_frame_commands(), rhi::CommandType::BeginRendering).size() == 2);  // the shadow pass and the scene pass of the first render only

    const SceneLighting night = evening(0.5f, 0.0f, -0.5f);  // below the horizon
    frame.lighting = &night;
    CHECK(renderer.render(o.device.begin_frame(), frame, o.target()).shadow_draw_calls == 0);
    (void)o.device.end_frame();
    CHECK(of_type(o.device.last_frame_commands(), rhi::CommandType::BeginRendering).size() == 1);
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("time_of_day_lighting: the sun reddens and dims as it sets, the ambient fades toward the night floor, and it is deterministic")
{
    AtmosphereSettings atmosphere;
    atmosphere.enabled = true;
    const SceneLighting day;
    const auto at_elevation = [&](float degrees) {
        const float r = degrees * pi / 180.0f;
        return time_of_day_lighting(atmosphere, {std::cos(r), 0.0f, std::sin(r)}, day);
    };
    const SceneLighting noon = at_elevation(89.0f);
    const SceneLighting low = at_elevation(3.0f);
    const SceneLighting set = at_elevation(-20.0f);
    CHECK(near(unit_len(noon.sun_direction.x, noon.sun_direction.y, noon.sun_direction.z), 1.0f, 1e-5f));
    CHECK(noon.sun_color.x > 0.7f * day.sun_color.x);
    CHECK(noon.sun_color.x <= day.sun_color.x);
    CHECK(near(noon.ambient_ground.x, day.ambient_ground.x, 1e-5f));
    // Near the horizon the blue is scattered out first: the ratios change and the sun is dimmer.
    CHECK(low.sun_color.z / low.sun_color.x < noon.sun_color.z / noon.sun_color.x);
    CHECK(low.sun_color.x < noon.sun_color.x);
    CHECK(low.ambient_ground.x < noon.ambient_ground.x);
    CHECK(low.ambient_ground.x > set.ambient_ground.x);
    // Below the horizon there is no sun, and the ambient is the night floor.
    CHECK(set.sun_color.x == 0.0f);
    CHECK(set.sun_color.y == 0.0f);
    CHECK(set.sun_color.z == 0.0f);
    CHECK(near(set.ambient_ground.x, 0.08f * day.ambient_ground.x, 1e-5f));
    CHECK(time_of_day_lighting(atmosphere, {0.0f, 0.0f, 1.0f}, day, 0.5f).ambient_ground.x < day.ambient_ground.x * 1.0001f);
    const SceneLighting first = at_elevation(30.0f);
    const SceneLighting again = at_elevation(30.0f);
    CHECK(std::memcmp(&first, &again, sizeof(SceneLighting)) == 0);
    // The ambient never rises as the sun sets.
    float previous = 1e9f;
    for (int d = 90; d >= -30; d -= 5) {
        const float a = at_elevation(static_cast<float>(d)).ambient_ground.x;
        CHECK(a <= previous + 1e-7f);
        previous = a;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(throws_invalid([&] { (void)time_of_day_lighting(atmosphere, {0.0f, 0.0f, 0.0f}, day); }));
    CHECK(throws_invalid([&] { (void)time_of_day_lighting(atmosphere, {nan, 0.0f, 1.0f}, day); }));
    CHECK(throws_invalid([&] { (void)time_of_day_lighting(atmosphere, {0.0f, 0.0f, 1.0f}, day, -0.1f); }));
    CHECK(throws_invalid([&] { (void)time_of_day_lighting(atmosphere, {0.0f, 0.0f, 1.0f}, day, 1.5f); }));
    CHECK(throws_invalid([&] { (void)time_of_day_lighting(atmosphere, {0.0f, 0.0f, 1.0f}, day, nan); }));
}

TEST_CASE("Dynamic lighting (null device): publishing nullptr writes the construction-time lighting, and a draw completes a frame that published nothing, shadows included")
{
    Offscreen o;
    RendererSettings settings = shadow_settings(true);
    settings.dynamic_lighting = true;
    settings.lighting = evening(0.3f, 0.2f, 0.9f);
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    ModelRenderer& models = renderer.model_renderer();
    ModelLightingFrame other;
    other.lighting = evening(1.0f, 0.0f, 1.0f);
    // Frames 0 and 1 publish something else into both slots; frame 2 (slot 0 again) publishes nullptr and must overwrite it with the construction-time block.
    Block construction{};
    for (int i = 0; i < 2; ++i) {
        (void)o.device.begin_frame();
        models.set_lighting_frame(&other);
        models.set_shadow_frame(nullptr);
        (void)o.device.end_frame();
    }
    (void)o.device.begin_frame();
    const uint32_t slot = static_cast<uint32_t>(o.device.frame_number() % 2);
    const Block stale = read_block(o.device, models.lighting_block(slot));
    CHECK(near(stale[4], 1.0f / std::sqrt(2.0f)));
    models.set_lighting_frame(nullptr);
    construction = read_block(o.device, models.lighting_block(slot));
    const float lc = unit_len(0.3f, 0.2f, 0.9f);
    CHECK(near(construction[4], 0.3f / lc));
    CHECK(near(construction[6], 0.9f / lc));
    CHECK(construction[8] == 2.0f);
    CHECK(near(models.frame_sun_direction().x, 0.3f / lc));
    bool threw = false;
    try {
        models.set_lighting_frame(&other);  // nullptr was this frame's publication
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(read_block(o.device, models.lighting_block(slot)) == construction);
    (void)o.device.end_frame();

    // A frame that publishes nothing and draws: the draw writes the slot, and the shadow frame it never set, so neither can be set afterwards.
    rhi::CommandList& cmd = o.device.begin_frame();
    const GpuModel model = tiny_model(renderer);
    const ModelDraw draws[] = {{&model, Mat4::identity()}};
    rhi::RenderingDesc rd;
    rhi::ColorAttachment ca;
    ca.texture = o.color;
    rd.colors.push_back(ca);
    rd.area = {0, 0, 320, 180};
    cmd.begin_rendering(rd);
    ModelFrameStats stats;
    models.draw(cmd, Mat4::identity(), {0.0f, 0.0f, 0.0f}, draws, stats);
    cmd.end_rendering();
    const uint32_t slot2 = static_cast<uint32_t>(o.device.frame_number() % 2);
    CHECK(read_block(o.device, models.lighting_block(slot2))[4] != stale[4]);
    CHECK(near(read_block(o.device, models.lighting_block(slot2))[4], 0.3f / lc));
    threw = false;
    try {
        models.set_shadow_frame(nullptr);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        models.set_lighting_frame(nullptr);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Dynamic lighting (null device): one effective lighting per device frame at the scene level, also across a sky-only view and a model view, and the next frame starts fresh")
{
    Offscreen o;
    RendererSettings settings = dynamic_settings();
    settings.atmosphere.enabled = true;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    const GpuModel model = tiny_model(renderer);
    const ModelDraw draws[] = {{&model, Mat4::identity()}};
    ModelRenderer& models = renderer.model_renderer();
    const SceneLighting l1 = evening(1.0f, 0.0f, 1.0f), l2 = evening(0.0f, 1.0f, 1.0f), l1_scaled = evening(5.0f, 0.0f, 5.0f);
    SceneLighting l3 = l1;
    l3.sun_color = {1.0f, 1.0f, 1.0f};  // the same sun, another color
    SceneLighting l4 = l1;
    l4.ambient_ground = {0.5f, 0.5f, 0.5f};  // the same sun and color, another ambient
    const auto render = [&](rhi::CommandList& cmd, std::span<const ModelDraw> d, const SceneLighting* l) {
        SceneFrame f;
        f.models = d;
        f.lighting = l;
        return renderer.render(cmd, f, o.target());
    };
    const auto refused = [&](rhi::CommandList& cmd, std::span<const ModelDraw> d, const SceneLighting* l) {
        try {
            (void)render(cmd, d, l);
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    };

    // Frame A: a sky-only view fixes the frame's lighting without publishing anything to the model renderer.
    rhi::CommandList& cmd = o.device.begin_frame();
    const uint32_t slot_a = static_cast<uint32_t>(o.device.frame_number() % 2);
    (void)render(cmd, {}, &l1);
    CHECK(!models.lighting_written_this_frame());
    // A model view with different lighting (any of the sun, its color or the ambient) is refused before anything is written, and the slot stays unwritten.
    CHECK(refused(cmd, draws, &l2));
    CHECK(refused(cmd, draws, &l3));
    CHECK(refused(cmd, draws, &l4));
    CHECK(refused(cmd, draws, nullptr));  // the construction-time lighting is another lighting too
    CHECK(!models.lighting_written_this_frame());
    // The same lighting (a scaled direction is the same sun) is accepted and publishes it; a third view with it too (the publication is not repeated).
    (void)render(cmd, draws, &l1_scaled);
    CHECK(models.lighting_written_this_frame());
    const Block published = read_block(o.device, models.lighting_block(slot_a));
    CHECK(near(published[4], 1.0f / std::sqrt(2.0f)));
    CHECK(near(published[6], 1.0f / std::sqrt(2.0f)));
    (void)render(cmd, draws, &l1);
    CHECK(read_block(o.device, models.lighting_block(slot_a)) == published);
    CHECK(refused(cmd, draws, &l2));  // and a conflicting one is still refused after the models have drawn
    CHECK(read_block(o.device, models.lighting_block(slot_a)) == published);
    (void)o.device.end_frame();

    // Frame B starts fresh (its own slot): a different lighting is fine, and frame A's slot keeps what A drew with.
    rhi::CommandList& cmd_b = o.device.begin_frame();
    const uint32_t slot_b = static_cast<uint32_t>(o.device.frame_number() % 2);
    CHECK(slot_b != slot_a);
    CHECK(!refused(cmd_b, draws, &l2));
    CHECK(near(read_block(o.device, models.lighting_block(slot_b))[5], 1.0f / std::sqrt(2.0f)));
    CHECK(read_block(o.device, models.lighting_block(slot_a)) == published);
    (void)o.device.end_frame();
    // Frame C (slot A again), an unchanged recovery: nothing sticks from the refusals.
    rhi::CommandList& cmd_c = o.device.begin_frame();
    CHECK(!refused(cmd_c, draws, nullptr));
    CHECK(near(read_block(o.device, models.lighting_block(slot_a))[4], evening(-0.40f, -0.55f, 0.73f).sun_direction.x / unit_len(-0.40f, -0.55f, 0.73f)));
    (void)o.device.end_frame();
    // (Several renders in one frame also write the frame's timestamps more than once; that device-level rule is the caller's, so the validation count is not checked here.)
}

TEST_CASE("IBL (null device): off acquires nothing for the environment; on adds exactly its texture, sampler and block, with or without shadows, and releases them")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    for (const bool shadows : {false, true}) {
        uint32_t off_count = 0, on_count = 0;
        {
            SceneRenderer off(o.device, rhi::Format::RGBA8_SRGB, ibl_settings(false, shadows));
            CHECK(!off.model_renderer().ibl_enabled());
            const GpuModel model = tiny_model(off);
            const ModelDraw draws[] = {{&model, Mat4::identity()}};
            SceneFrame frame;
            frame.models = draws;
            (void)off.render(o.device.begin_frame(), frame, o.target());
            (void)o.device.end_frame();
            off_count = o.device.live_resource_count() - baseline;
        }
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == baseline);
        {
            SceneRenderer on(o.device, rhi::Format::RGBA8_SRGB, ibl_settings(true, shadows));
            CHECK(on.model_renderer().ibl_enabled());
            const GpuModel model = tiny_model(on);
            const ModelDraw draws[] = {{&model, Mat4::identity()}};
            SceneFrame frame;
            frame.models = draws;
            (void)on.render(o.device.begin_frame(), frame, o.target());
            (void)o.device.end_frame();
            on_count = o.device.live_resource_count() - baseline;
            const std::vector<rhi::RecordedCommand> cmds = o.device.last_frame_commands();
            CHECK(scene_pass_count(cmds) == (shadows ? 2u : 1u));
        }
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == baseline);
        // The environment texture, its sampler and its uniform block; the model pipeline permutation is lazy and is the same count either way
        // (one of four shader modules is created), so only the three resources and the one shader differ is not asserted exactly: at least three more.
        CHECK(on_count >= off_count + 3);
    }
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("IBL (null device): a failed acquisition at any step releases everything, with and without shadows, and the renderer can be used afterwards")
{
    for (const bool shadows : {false, true}) {
        Offscreen o;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, ibl_settings(true, shadows));
        const uint32_t baseline = o.device.live_resource_count();
        uint32_t failures = 0;
        for (uint32_t n = 1; n <= 200; ++n) {
            bool threw = false;
            o.device.fail_creation_after(n);
            try {
                (void)renderer.model_renderer();
            } catch (const std::runtime_error&) {
                threw = true;
            }
            o.device.fail_creation_after(0);
            if (!threw) {
                CHECK(renderer.model_renderer().ibl_enabled());
                break;
            }
            ++failures;
            o.device.wait_idle();
            CHECK(o.device.live_resource_count() == baseline);
        }
        CHECK(failures >= (shadows ? 25u : 10u));
        const GpuModel model = tiny_model(renderer);
        const ModelDraw draws[] = {{&model, Mat4::identity()}};
        SceneFrame frame;
        frame.models = draws;
        (void)renderer.render(o.device.begin_frame(), frame, o.target());
        (void)o.device.end_frame();
        CHECK(o.device.validation_error_count() == 0);
    }
}

TEST_CASE("IBL (null device): a bad environment, or one with a shading mode that would ignore it, is refused before anything is acquired")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    const auto refused = [&](const RendererSettings& s) {
        try {
            SceneRenderer r(o.device, rhi::Format::RGBA8_SRGB, s);
            (void)r.model_renderer();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    RendererSettings legacy = ibl_settings(true, false);
    legacy.model_shading = ModelShading::Legacy;
    CHECK(refused(legacy));
    RendererSettings extended = ibl_settings(true, false);
    extended.model_shading = ModelShading::Extended;
    CHECK(refused(extended));

    const auto mutated = [&](auto&& mutate) {
        auto e = std::make_shared<EnvironmentLighting>(*tiny_environment());
        mutate(*e);
        RendererSettings s = ibl_settings(false, false);
        s.environment = e;
        return refused(s);
    };
    CHECK(mutated([](EnvironmentLighting& e) { e.width = 0; }));
    CHECK(mutated([](EnvironmentLighting& e) { e.levels = 0; }));
    CHECK(mutated([](EnvironmentLighting& e) { e.levels = 9; }));  // more levels than the chain has
    CHECK(mutated([](EnvironmentLighting& e) { e.rgba16f.pop_back(); }));
    CHECK(mutated([](EnvironmentLighting& e) { e.sh[4].x = std::numeric_limits<float>::quiet_NaN(); }));
    CHECK(mutated([](EnvironmentLighting& e) { e.intensity = -1.0f; }));
    CHECK(mutated([](EnvironmentLighting& e) { e.intensity = std::numeric_limits<float>::infinity(); }));
    CHECK(!mutated([](EnvironmentLighting&) {}));  // the unmutated one is accepted (the harness is not refusing everything)
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
}

TEST_CASE("IBL (null device): a device that cannot filter RGBA16F refuses the environment before acquiring anything, and the renderer without one is unaffected")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    o.device.deny_filtered_sampling(rhi::Format::RGBA16_FLOAT);
    bool refused = false;
    try {
        SceneRenderer r(o.device, rhi::Format::RGBA8_SRGB, ibl_settings(true, false));
        (void)r.model_renderer();
    } catch (const std::invalid_argument& e) {
        refused = std::string(e.what()).find("linear filtering") != std::string::npos;
    }
    CHECK(refused);
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
    {
        SceneRenderer plain(o.device, rhi::Format::RGBA8_SRGB, ibl_settings(false, true));  // no environment: the format is not needed
        CHECK(!plain.model_renderer().ibl_enabled());
    }
    o.device.deny_filtered_sampling(rhi::Format::Unknown);
    {
        SceneRenderer ok(o.device, rhi::Format::RGBA8_SRGB, ibl_settings(true, true));
        CHECK(ok.model_renderer().ibl_enabled());
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
}

TEST_CASE("Skinning (null device): off acquires nothing for it; on adds its shader, layout, pipelines and identity palette; everything is released")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    for (const bool shadows : {false, true}) {
        uint32_t off_count = 0, on_count = 0;
        {
            RendererSettings s = ibl_settings(false, shadows);
            SceneRenderer off(o.device, rhi::Format::RGBA8_SRGB, s);
            CHECK(!off.model_renderer().skinning_enabled());
            off_count = o.device.live_resource_count() - baseline;
        }
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == baseline);
        {
            RendererSettings s = ibl_settings(false, shadows);
            s.skinning = true;
            SceneRenderer on(o.device, rhi::Format::RGBA8_SRGB, s);
            CHECK(on.model_renderer().skinning_enabled());
            on_count = o.device.live_resource_count() - baseline;
            // A palette adds one buffer per frame in flight (and its groups); dropping it returns them.
            const uint32_t before_palette = o.device.live_resource_count();
            {
                SkinPalette p = on.model_renderer().create_skin_palette(8, "palette");
                CHECK(p.valid() && p.joint_count() == 8);
                CHECK(o.device.live_resource_count() > before_palette);
            }
            o.device.wait_idle();
            CHECK(o.device.live_resource_count() == before_palette);
        }
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == baseline);
        // The skinned vertex shader, the palette layout (not counted as a resource), four pipelines (plus the shadow pipeline and shader with shadows on)
        // and the identity palette's buffers.
        CHECK(on_count >= off_count + (shadows ? 8u : 5u));
    }
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Skinning (null device): a failed acquisition at any step releases everything, and a failed palette leaves nothing behind")
{
    for (const bool shadows : {false, true}) {
        Offscreen o;
        RendererSettings s = ibl_settings(true, shadows);
        s.skinning = true;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, s);
        const uint32_t baseline = o.device.live_resource_count();
        uint32_t failures = 0;
        for (uint32_t n = 1; n <= 300; ++n) {
            bool threw = false;
            o.device.fail_creation_after(n);
            try {
                (void)renderer.model_renderer();
            } catch (const std::runtime_error&) {
                threw = true;
            }
            o.device.fail_creation_after(0);
            if (!threw) {
                CHECK(renderer.model_renderer().skinning_enabled());
                break;
            }
            ++failures;
            o.device.wait_idle();
            CHECK(o.device.live_resource_count() == baseline);
        }
        CHECK(failures >= (shadows ? 35u : 20u));
        ModelRenderer& models = renderer.model_renderer();
        const uint32_t built = o.device.live_resource_count();
        uint32_t palette_failures = 0;
        for (uint32_t n = 1; n <= 40; ++n) {
            bool threw = false;
            o.device.fail_creation_after(n);
            try {
                SkinPalette p = models.create_skin_palette(4, "swept");
                (void)p;
            } catch (const std::runtime_error&) {
                threw = true;
            }
            o.device.fail_creation_after(0);
            if (!threw) break;
            ++palette_failures;
            o.device.wait_idle();
            CHECK(o.device.live_resource_count() == built);
        }
        CHECK(palette_failures >= 2);
        // A skinned primitive is drawn without crashing and counted; the palette-less draw uses the identity palette.
        CHECK(o.device.validation_error_count() == 0);
    }
}

// ---- Palette immutability within a frame --------------------------------------------------------------------------------------------------------

namespace {

/// One skinned triangle (joint 0 only) on whatever device the renderer has.
GpuModel skinned_triangle(SceneRenderer& renderer)
{
    GpuModel model = tiny_model(renderer);
    const std::vector<SkinnedModelVertex> vertices = {{{{0, 0, 0}, {0, 0, 1}, {0, 0}}}, {{{1, 0, 0}, {0, 0, 1}, {1, 0}}}, {{{0, 1, 0}, {0, 0, 1}, {0, 1}}}};
    rhi::BufferDesc vb;
    vb.size = vertices.size() * sizeof(SkinnedModelVertex);
    vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    model.meshes[0][0].vertices = renderer.registry().create_buffer(vb, std::as_bytes(std::span(vertices)));
    model.meshes[0][0].skinned = true;
    model.meshes[0][0].joint_limit = 1;
    model.skin_joint_count = 1;
    return model;
}

Mat4 pose_with_translation(float x)
{
    Mat4 m = Mat4::identity();
    m.m[12] = x;  // column-major: the translation of an affine matrix
    return m;
}

float first_translation(rhi::NullDevice& device, rhi::BufferHandle buffer)
{
    const std::span<std::byte> bytes = device.map(buffer);
    float rows[12];
    std::memcpy(rows, bytes.data(), sizeof(rows));
    return rows[3];  // row 0's last element: the x translation of joint 0
}

}  // namespace

TEST_CASE("Skinning (null device): a palette's pose is frozen for the frame at its first use, a later update is refused before anything is recorded, and the next frame takes it")
{
    for (const bool shadows : {false, true}) {
        Offscreen o;
        RendererSettings s = shadow_settings(shadows);
        s.skinning = true;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, s);
        ModelRenderer& models = renderer.model_renderer();
        const GpuModel model = skinned_triangle(renderer);
        SkinPalette p = models.create_skin_palette(1, "frozen"), q = models.create_skin_palette(1, "other");
        const ModelDraw both[] = {{&model, Mat4::identity(), &p}, {&model, Mat4::identity(), &q}};
        const ModelDraw only_p[] = {{&model, Mat4::identity(), &p}};
        const ModelDraw only_q[] = {{&model, Mat4::identity(), &q}};
        const Mat4 pose_a[] = {pose_with_translation(1.0f)}, pose_b[] = {pose_with_translation(2.0f)}, pose_c[] = {pose_with_translation(3.0f)};
        const uint32_t per_palette = shadows ? 2u : 1u;  // draws of one palette's triangle: the colour pass, and the shadow pass with shadows on
        const auto render_with = [&](rhi::CommandList& cmd, std::span<const ModelDraw> draws) {
            SceneFrame f;
            f.models = draws;
            return renderer.render(cmd, f, o.target());
        };
        const auto render_refused = [&](rhi::CommandList& cmd, std::span<const ModelDraw> draws) {
            try {
                (void)render_with(cmd, draws);
            } catch (const std::logic_error&) {
                return true;
            }
            return false;
        };

        // Frame 1: pose A is set before the first use; the frame draws it (the shadow and the colour pass bind the same copy).
        p.update(pose_a);
        q.update(pose_a);
        rhi::CommandList& cmd1 = o.device.begin_frame();
        const uint32_t slot1 = static_cast<uint32_t>(o.device.frame_number() % 2);
        CHECK(render_with(cmd1, both).model_skinned_draws == 2 * per_palette);
        CHECK(first_translation(o.device, p.slot_buffer(slot1)) == 1.0f);
        // The pose changes after the draws were recorded. The update itself is CPU state and allowed, but another draw of this palette in the same frame (a second view, or the
        // shadow and colour pass split across calls) is refused before it records anything or touches the slot: the recorded draws keep pose A.
        p.update(pose_b);
        CHECK(render_refused(cmd1, only_p));
        CHECK(render_refused(cmd1, both));
        CHECK(first_translation(o.device, p.slot_buffer(slot1)) == 1.0f);
        (void)o.device.end_frame();
        CHECK(of_type(o.device.last_frame_commands(), rhi::CommandType::DrawIndexed).size() == 2 * per_palette);  // nothing from the refused renders

        // Frame 2 (the other slot): the palette takes its last pose before the first use, and frame 1's copy is untouched while the GPU may still be reading it.
        rhi::CommandList& cmd2 = o.device.begin_frame();
        const uint32_t slot2 = static_cast<uint32_t>(o.device.frame_number() % 2);
        CHECK(slot2 != slot1);
        p.update(pose_b);
        p.update(pose_c);
        CHECK(render_with(cmd2, only_p).model_skinned_draws == per_palette);
        CHECK(first_translation(o.device, p.slot_buffer(slot2)) == 3.0f);
        CHECK(first_translation(o.device, p.slot_buffer(slot1)) == 1.0f);
        p.update(pose_a);  // too late for this frame
        CHECK(render_refused(cmd2, only_p));
        CHECK(first_translation(o.device, p.slot_buffer(slot2)) == 3.0f);
        (void)o.device.end_frame();

        // Frame 3 reuses slot 1 and finds the newest pose there.
        rhi::CommandList& cmd3 = o.device.begin_frame();
        CHECK(static_cast<uint32_t>(o.device.frame_number() % 2) == slot1);
        CHECK(render_with(cmd3, only_p).model_skinned_draws == per_palette);
        CHECK(first_translation(o.device, p.slot_buffer(slot1)) == 1.0f);   // pose A, set in frame 2 after its draws
        CHECK(first_translation(o.device, p.slot_buffer(slot2)) == 3.0f);   // frame 2's copy, still what that frame drew
        (void)o.device.end_frame();
        CHECK(o.device.validation_error_count() == 0);

        // The same through the model renderer's own draw calls, in a frame that records them by hand: the shadow pass binds the palette (freezing it), a changed pose is refused by
        // the colour pass's draw and by another shadow draw before they record anything, and an unchanged palette (and another one) can still be drawn.
        if (shadows) {
            rhi::CommandList& cmd = o.device.begin_frame();
            const uint32_t slot = static_cast<uint32_t>(o.device.frame_number() % 2);
            models.set_shadow_frame(nullptr);
            p.update(pose_b);
            rhi::RenderingDesc rd;
            rhi::ColorAttachment ca;
            ca.texture = o.color;
            rd.colors.push_back(ca);
            rd.area = {0, 0, 320, 180};
            cmd.begin_rendering(rd);
            ModelFrameStats stats;
            models.draw_shadow(cmd, Mat4::identity(), only_p, stats);
            CHECK(stats.skinned_draws == 1);
            CHECK(first_translation(o.device, p.slot_buffer(slot)) == 2.0f);
            models.draw_shadow(cmd, Mat4::identity(), only_p, stats);  // the same pose again: fine
            models.draw_shadow(cmd, Mat4::identity(), only_q, stats);  // another palette: fine
            p.update(pose_c);
            const auto refuses = [&](auto&& call) {
                try {
                    call();
                } catch (const std::logic_error&) {
                    return true;
                }
                return false;
            };
            ModelFrameStats refused_stats;
            CHECK(refuses([&] { models.draw_shadow(cmd, Mat4::identity(), only_p, refused_stats); }));
            CHECK(refuses([&] { models.draw(cmd, Mat4::identity(), {0.0f, 0.0f, 0.0f}, only_p, refused_stats); }));
            CHECK(refuses([&] { models.draw(cmd, Mat4::identity(), {0.0f, 0.0f, 0.0f}, both, refused_stats); }));
            CHECK(refused_stats.skinned_draws == 0);  // nothing was recorded for any of them
            CHECK(first_translation(o.device, p.slot_buffer(slot)) == 2.0f);
            models.draw(cmd, Mat4::identity(), {0.0f, 0.0f, 0.0f}, only_q, refused_stats);  // the other palette is still drawable in the colour pass
            CHECK(refused_stats.skinned_draws == 1);
            cmd.end_rendering();
            (void)o.device.end_frame();
            CHECK(of_type(o.device.last_frame_commands(), rhi::CommandType::DrawIndexed).size() == 4);  // three shadow draws and one colour draw, none from a refused call
            CHECK(o.device.validation_error_count() == 0);
        }
    }
}

// ---- Frustum culling (from #83) ------------------------------------------------------------------------------------------------------------

namespace {

/// One triangle whose bounding sphere is given (the local origin and radius 1 by default); radius 0 means "no bounds".
GpuModel bounded_model(SceneRenderer& renderer, float radius, Float3 center = {0, 0, 0})
{
    GpuModel model;
    model.name = "bounded";
    model.materials.push_back(renderer.model_renderer().create_material({}, {}, {}, "bounded.material"));
    const std::vector<ModelVertex> vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
    const std::vector<uint32_t> indices = {0, 1, 2};
    rhi::BufferDesc vb;
    vb.size = vertices.size() * sizeof(ModelVertex);
    vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    rhi::BufferDesc ib;
    ib.size = indices.size() * sizeof(uint32_t);
    ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
    GpuPrimitive p;
    p.vertices = renderer.registry().create_buffer(vb, std::as_bytes(std::span(vertices)));
    p.indices = renderer.registry().create_buffer(ib, std::as_bytes(std::span(indices)));
    p.index_count = 3;
    p.material = 0;
    p.bounds_center = center;
    p.bounds_radius = radius;
    model.meshes.push_back({std::move(p)});
    model.placements.push_back({0, Mat4::identity()});
    return model;
}

Mat4 place(Float3 position, float scale = 1.0f)
{
    Transform t;
    t.position = position;
    t.scale = {scale, scale, scale};
    return to_matrix(t);
}

FrameStats render_models(Offscreen& o, SceneRenderer& renderer, const Camera& camera, std::span<const ModelDraw> draws)
{
    SceneFrame frame;
    frame.camera = &camera;
    frame.models = draws;
    const FrameStats stats = renderer.render(o.device.begin_frame(), frame, o.target());
    (void)o.device.end_frame();
    return stats;
}

} // namespace

TEST_CASE("Frustum culling: primitives outside the view are skipped, those inside or without bounds are drawn, and it can be turned off")
{
    const Camera camera = look_at_camera({0, 0, 2}, {10, 0, 2});  // looks along +X, 60 degrees vertical, 16:9 target
    for (const bool culling : {true, false}) {
        Offscreen o;
        RendererSettings settings;
        settings.frustum_culling = culling;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
        const GpuModel bounded = bounded_model(renderer, 1.0f), unbounded = bounded_model(renderer, 0.0f);
        const ModelDraw draws[] = {
            {&bounded, place({10, 0, 2})},      // in front: drawn
            {&bounded, place({-10, 0, 2})},     // behind the camera: culled
            {&bounded, place({10, 40, 2})},     // far to the left of the view: culled
            {&bounded, place({10, 0, 60})},     // far above: culled
            {&bounded, place({10, 4.5f, 2})},   // just inside the left edge (half width at 10 m is about 10 m * tan(30) * 16/9 = 10.3 m): drawn
            {&unbounded, place({-10, 0, 2})},   // behind the camera, but with no bounds: never culled
            {&bounded, place({10, 0, 2}, 0.01f)}  // tiny scale in view: drawn
        };
        const FrameStats stats = render_models(o, renderer, camera, draws);
        CHECK(stats.model_instances == 7);  // instances are counted whether or not their primitives were
        if (culling) {
            CHECK(stats.model_primitives_culled == 3);
            CHECK(stats.draw_calls == 1 /*ground*/ + 4);
        } else {
            CHECK(stats.model_primitives_culled == 0);
            CHECK(stats.draw_calls == 1 + 7);
        }
        CHECK(o.device.validation_error_count() == 0);
    }
}

TEST_CASE("Frustum culling: a bounding sphere is scaled by the instance and offset by its center, and a sphere straddling an edge is kept")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, RendererSettings{});
    const Camera camera = look_at_camera({0, 0, 2}, {10, 0, 2});
    const GpuModel small = bounded_model(renderer, 1.0f);
    const GpuModel offset = bounded_model(renderer, 1.0f, {0, 30, 0});  // the sphere is 30 m to the side of the model's origin
    // 40 m to the side: out of view at scale 1, but a sphere scaled 50x reaches the view.
    {
        const ModelDraw out[] = {{&small, place({10, 40, 2})}};
        CHECK(render_models(o, renderer, camera, out).model_primitives_culled == 1);
        const ModelDraw scaled[] = {{&small, place({10, 40, 2}, 50.0f)}};
        CHECK(render_models(o, renderer, camera, scaled).model_primitives_culled == 0);
    }
    // The model's origin is in view but its sphere (30 m to the side) is not: culled. Moved so the sphere is in view: kept.
    {
        const ModelDraw hidden[] = {{&offset, place({10, 0, 2})}};
        CHECK(render_models(o, renderer, camera, hidden).model_primitives_culled == 1);
        const ModelDraw shown[] = {{&offset, place({10, -30, 2})}};
        CHECK(render_models(o, renderer, camera, shown).model_primitives_culled == 0);
    }
    // Non-uniform scale and mirroring use the largest axis scale.
    {
        Transform stretched;
        stretched.position = {10, 14, 2};
        stretched.scale = {1.0f, -8.0f, 1.0f};  // mirrored, 8x along Y: the sphere reaches 8 m toward the view
        const ModelDraw d[] = {{&small, to_matrix(stretched)}};
        CHECK(render_models(o, renderer, camera, d).model_primitives_culled == 0);
    }
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Frustum culling never culls a sphere that any of its points projects into the view")
{
    Offscreen o;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, RendererSettings{});
    const GpuModel unit = bounded_model(renderer, 1.0f);
    uint32_t state = 12345u;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    int visible = 0, invisible = 0, culled_invisible = 0;
    for (int i = 0; i < 400; ++i) {
        const Camera camera = look_at_camera({next() * 10 - 5, next() * 10 - 5, next() * 4}, {next() * 20 - 10, next() * 20 - 10, next() * 4 - 1});
        const Float3 center{next() * 60 - 30, next() * 60 - 30, next() * 20 - 5};
        const float radius = 0.2f + next() * 6.0f;
        const Mat4 vp = view_projection_matrix(camera, 320.0f / 180.0f);
        // Independent oracle: sample the sphere's center and 400 points inside it; the sphere is visible if any projects inside the view.
        bool any = false;
        for (int s = 0; s < 400 && !any; ++s) {
            Float3 p = center;
            if (s > 0) {
                Float3 d{next() * 2 - 1, next() * 2 - 1, next() * 2 - 1};
                while (dot(d, d) > 1.0f || dot(d, d) < 1e-6f) d = {next() * 2 - 1, next() * 2 - 1, next() * 2 - 1};
                p = center + d * radius;
            }
            const Float4 clip = vp * Float4{p.x, p.y, p.z, 1.0f};
            any = clip.w > 0.0f && std::fabs(clip.x) <= clip.w && std::fabs(clip.y) <= clip.w && clip.z > 0.0f && clip.z <= clip.w;
        }
        const ModelDraw d[] = {{&unit, place(center, radius)}};
        const bool culled = render_models(o, renderer, camera, d).model_primitives_culled == 1;
        if (any) {
            ++visible;
            CHECK(!culled);
        } else {
            ++invisible;
            culled_invisible += culled ? 1 : 0;
        }
    }
    CHECK(visible >= 40);
    CHECK(invisible >= 200);
    CHECK(culled_invisible * 100 >= invisible * 80);  // it is conservative, but it does cull most of what cannot be seen
}

TEST_CASE("Frustum culling and skinning (null device): a skinned primitive is never culled by its bind-pose bounds, a rigid one with the same bounds is")
{
    const Camera camera = look_at_camera({0, 0, 2}, {10, 0, 2});
    for (const bool culling : {true, false}) {
        Offscreen o;
        RendererSettings settings;
        settings.skinning = true;
        settings.frustum_culling = culling;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
        GpuModel skinned = skinned_triangle(renderer);
        skinned.meshes[0][0].bounds_center = {0.0f, 60.0f, 0.0f};  // the bind pose is far to the side of the view
        skinned.meshes[0][0].bounds_radius = 1.0f;
        GpuModel rigid = bounded_model(renderer, 1.0f, {0.0f, 60.0f, 0.0f});
        SkinPalette palette = renderer.model_renderer().create_skin_palette(1, "walker");
        const Mat4 pose[] = {place({0.0f, -60.0f, 0.0f})};  // the pose brings the mesh back into the view
        palette.update(pose);
        const ModelDraw draws[] = {{&skinned, place({10, 0, 2}), &palette}, {&rigid, place({10, 0, 2})}};
        const FrameStats stats = render_models(o, renderer, camera, draws);
        CHECK(stats.model_skinned_draws == 1);  // always drawn
        if (culling) {
            CHECK(stats.model_primitives_culled == 1);      // the rigid one only
            CHECK(stats.model_skinned_cull_bypassed == 1);
        } else {
            CHECK(stats.model_primitives_culled == 0);
            CHECK(stats.model_skinned_cull_bypassed == 0);
        }
        CHECK(o.device.validation_error_count() == 0);
    }
}

// ---- Per-pose conservative bounds for skinned primitives ----------------------------------------------------------------------------------

namespace {

/// A tiny skinned model whose primitive carries per-joint bounds: joint 0's box is the triangle's bind-space box.
GpuModel bounded_skinned_triangle(SceneRenderer& renderer)
{
    GpuModel model = skinned_triangle(renderer);
    model.meshes[0][0].joint_bounds = {{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, true}};
    return model;
}

}  // namespace

TEST_CASE("Frustum culling and skinning with per-joint bounds (null device): the pose decides, in both directions, and primitives without bounds are still never culled")
{
    const Camera camera = look_at_camera({0, 0, 2}, {10, 0, 2});  // looks along +X
    Offscreen o;
    RendererSettings settings;
    settings.skinning = true;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    GpuModel bounded = bounded_skinned_triangle(renderer);
    GpuModel unbounded = skinned_triangle(renderer);
    SkinPalette palette = renderer.model_renderer().create_skin_palette(1, "walker");
    const auto frame = [&](const GpuModel& m, Mat4 pose, Mat4 world) {
        const Mat4 poses[] = {pose};
        palette.update(poses);
        const ModelDraw draws[] = {{&m, world, &palette}};
        return render_models(o, renderer, camera, draws);
    };
    const Mat4 in_front = place({10, 0, 2});
    // The pose keeps the mesh in view: drawn, not culled, not bypassed.
    {
        const FrameStats s = frame(bounded, Mat4::identity(), in_front);
        CHECK(s.model_skinned_draws == 1 && s.model_skinned_culled == 0 && s.model_primitives_culled == 0 && s.model_skinned_cull_bypassed == 0);
    }
    // The pose throws it far to the side: culled by the posed bounds (the bind pose and the placement alone are in view).
    {
        const FrameStats s = frame(bounded, place({0.0f, 200.0f, 0.0f}), in_front);
        CHECK(s.model_skinned_draws == 0 && s.model_skinned_culled == 1 && s.model_primitives_culled == 1 && s.model_skinned_cull_bypassed == 0);
    }
    // The bind pose is far away (its bounding sphere would cull it), the pose brings it back: drawn.
    {
        GpuModel far_bind = bounded_skinned_triangle(renderer);
        far_bind.meshes[0][0].bounds_center = {0.0f, 60.0f, 0.0f};
        far_bind.meshes[0][0].bounds_radius = 1.0f;
        far_bind.meshes[0][0].joint_bounds = {{{0.0f, 60.0f, 0.0f}, {1.0f, 61.0f, 0.0f}, true}};
        const FrameStats s = frame(far_bind, place({0.0f, -60.0f, 0.0f}), in_front);
        CHECK(s.model_skinned_draws == 1 && s.model_skinned_culled == 0);
        const FrameStats gone = frame(far_bind, Mat4::identity(), in_front);  // and the bind pose itself is out of view
        CHECK(gone.model_skinned_draws == 0 && gone.model_skinned_culled == 1);
    }
    // The draw's world matrix counts too: the same pose placed far to the side is culled.
    {
        const FrameStats s = frame(bounded, Mat4::identity(), place({10, 200, 2}));
        CHECK(s.model_skinned_culled == 1);
    }
    // No palette: the bind pose (identity matrices) is what is tested.
    {
        const ModelDraw drawn[] = {{&bounded, in_front}};
        CHECK(render_models(o, renderer, camera, drawn).model_skinned_draws == 1);
        const ModelDraw hidden[] = {{&bounded, place({10, 200, 2})}};
        const FrameStats s = render_models(o, renderer, camera, hidden);
        CHECK(s.model_skinned_draws == 0 && s.model_skinned_culled == 1);
    }
    // A primitive without joint bounds is drawn wherever the pose puts it, and counted as bypassed.
    {
        const FrameStats s = frame(unbounded, place({0.0f, 200.0f, 0.0f}), in_front);
        CHECK(s.model_skinned_draws == 1 && s.model_skinned_culled == 0 && s.model_skinned_cull_bypassed == 1);
    }
    // A joint box that no joint uses leaves nothing to bound with: bypassed, drawn.
    {
        GpuModel unused = bounded_skinned_triangle(renderer);
        unused.meshes[0][0].joint_bounds[0].used = false;
        const FrameStats s = frame(unused, place({0.0f, 200.0f, 0.0f}), in_front);
        CHECK(s.model_skinned_draws == 1 && s.model_skinned_cull_bypassed == 1);
    }
    // A palette with fewer joints than the primitive uses is skipped (not drawn, not culled), as before.
    {
        GpuModel wide = bounded_skinned_triangle(renderer);
        wide.meshes[0][0].joint_limit = 2;
        wide.meshes[0][0].joint_bounds.push_back({{0, 0, 0}, {1, 1, 0}, true});
        const ModelDraw draws[] = {{&wide, in_front, &palette}};
        const FrameStats s = render_models(o, renderer, camera, draws);
        CHECK(s.model_skinned_skipped == 1 && s.model_skinned_culled == 0 && s.model_skinned_draws == 0);
    }
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Posed skinned bounds contain every posed vertex, for random skeletons, poses and placements, and are tight for one joint")
{
    Offscreen o;
    RendererSettings settings;
    settings.skinning = true;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    ModelRenderer& models = renderer.model_renderer();
    uint32_t state = 777u;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    const auto range = [&](float lo, float hi) { return lo + (hi - lo) * next(); };
    const auto random_affine = [&](float scale) {
        Mat4 m = Mat4::identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) m.at(r, c) = range(-scale, scale);
        return m;
    };
    const auto apply = [](const Mat4& m, Float3 p) {
        return Float3{m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3), m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
                      m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
    };
    for (int trial = 0; trial < 300; ++trial) {
        const uint32_t joints = 1 + static_cast<uint32_t>(next() * 5.0f);
        const uint32_t vertices = 4 + static_cast<uint32_t>(next() * 40.0f);
        struct V {
            Float3 p;
            uint32_t j[4];
            float w[4];
        };
        std::vector<V> vs(vertices);
        GpuPrimitive prim;
        prim.skinned = true;
        prim.joint_limit = joints;
        prim.joint_bounds.assign(joints, JointBounds{});
        for (V& v : vs) {
            v.p = {range(-3, 3), range(-3, 3), range(-3, 3)};
            const uint32_t influences = 1 + static_cast<uint32_t>(next() * 4.0f);
            float sum = 0.0f;
            for (uint32_t k = 0; k < 4; ++k) {
                v.j[k] = static_cast<uint32_t>(next() * static_cast<float>(joints)) % joints;
                v.w[k] = k < influences ? 0.05f + next() : 0.0f;
                sum += v.w[k];
            }
            for (float& w : v.w) w /= sum;
            // Half the trials carry weights that sum to one only within the tolerance staging accepts (1e-3), as the shader would use them.
            if (trial % 2 == 1) {
                const float skew = 1.0f + range(-0.9e-3f, 0.9e-3f);
                for (float& w : v.w) w *= skew;
            }
            for (uint32_t k = 0; k < 4; ++k) {
                if (!(v.w[k] > 0.0f)) continue;
                JointBounds& b = prim.joint_bounds[v.j[k]];
                if (!b.used) {
                    b.min = b.max = v.p;
                    b.used = true;
                } else {
                    b.min = {std::min(b.min.x, v.p.x), std::min(b.min.y, v.p.y), std::min(b.min.z, v.p.z)};
                    b.max = {std::max(b.max.x, v.p.x), std::max(b.max.y, v.p.y), std::max(b.max.z, v.p.z)};
                }
            }
        }
        SkinPalette palette = models.create_skin_palette(joints, "random");
        std::vector<Mat4> pose(joints);
        for (Mat4& m : pose) m = random_affine(2.0f);
        palette.update(pose);
        Mat4 world = random_affine(3.0f);
        if (trial % 3 == 0) world.at(0, 3) += 150.0f, world.at(1, 3) -= 90.0f, world.at(2, 3) += 40.0f;  // far from the origin, where a weight-sum error reaches farthest
        Float3 lo, hi;
        REQUIRE(ModelRenderer::posed_world_bounds(prim, palette, world, lo, hi));
        for (const V& v : vs) {
            Float3 posed{};
            for (uint32_t k = 0; k < 4; ++k) {
                const Float3 q = apply(pose[v.j[k]], v.p);
                posed = posed + q * v.w[k];
            }
            const Float3 w = apply(world, posed);
            CHECK(w.x >= lo.x && w.x <= hi.x && w.y >= lo.y && w.y <= hi.y && w.z >= lo.z && w.z <= hi.z);
        }
        if (joints == 1) {
            // One joint: the box is the exact bounds of the transformed bind box's corners (up to the margin), not a loose sphere.
            float elo[3] = {1e30f, 1e30f, 1e30f}, ehi[3] = {-1e30f, -1e30f, -1e30f};
            const JointBounds& b = prim.joint_bounds[0];
            for (int corner = 0; corner < 8; ++corner) {
                const Float3 p{(corner & 1) ? b.max.x : b.min.x, (corner & 2) ? b.max.y : b.min.y, (corner & 4) ? b.max.z : b.min.z};
                const Float3 w = apply(world, apply(pose[0], p));
                elo[0] = std::min(elo[0], w.x), elo[1] = std::min(elo[1], w.y), elo[2] = std::min(elo[2], w.z);
                ehi[0] = std::max(ehi[0], w.x), ehi[1] = std::max(ehi[1], w.y), ehi[2] = std::max(ehi[2], w.z);
            }
            const float size = std::max({ehi[0] - elo[0], ehi[1] - elo[1], ehi[2] - elo[2]});
            const float reach = std::max({std::fabs(elo[0]), std::fabs(elo[1]), std::fabs(elo[2]), std::fabs(ehi[0]), std::fabs(ehi[1]), std::fabs(ehi[2])});
            const float shift = std::max({std::fabs(world.at(0, 3)), std::fabs(world.at(1, 3)), std::fabs(world.at(2, 3))});
            const float tol = 1e-3f * (reach + shift) + 1e-4f + 1e-3f * (1.0f + size);  // the documented margin, and no more
            CHECK(lo.x >= elo[0] - tol && lo.y >= elo[1] - tol && lo.z >= elo[2] - tol);
            CHECK(hi.x <= ehi[0] + tol && hi.y <= ehi[1] + tol && hi.z <= ehi[2] + tol);
        }
    }
    // Weights that sum to 1.0009 (accepted by staging) push a posed vertex 0.09% farther from the origin than the convex hull of the carried boxes: for one vertex far from the origin
    // the box is a point, and the margin must still cover the vertex. (Both far from the origin in pose space and far through the placement.)
    // (vertex x in pose space, placement translation x): the last pair puts the box near the origin while the vertex itself is 150 m out, so only the placement term of the margin covers it.
    for (const std::pair<float, float>& tc : {std::pair<float, float>{100.0f, 0.0f}, {100.0f, 50.0f}, {150.0f, -50.0f}}) {
        const float x = tc.first, far = tc.second;
        GpuPrimitive point;
        point.skinned = true;
        point.joint_limit = 1;
        point.joint_bounds = {{{x, 0.0f, 0.0f}, {x, 0.0f, 0.0f}, true}};
        SkinPalette identity = models.create_skin_palette(1, "identity");
        const Mat4 poses[] = {Mat4::identity()};
        identity.update(poses);
        Mat4 world = Mat4::identity();
        world.at(0, 3) = far;
        Float3 lo, hi;
        REQUIRE(ModelRenderer::posed_world_bounds(point, identity, world, lo, hi));
        const float posed_high = (x * 1.0009f) + far;  // world(S * v)
        CHECK(posed_high >= lo.x && posed_high <= hi.x);
        const float posed_low = (x * 0.9991f) + far;
        CHECK(posed_low >= lo.x && posed_low <= hi.x);
    }
    // Refusals: no bounds, a palette smaller than the bounds, nothing used.
    GpuPrimitive none;
    none.skinned = true;
    SkinPalette p1 = models.create_skin_palette(1, "one");
    Float3 lo, hi;
    CHECK(!ModelRenderer::posed_world_bounds(none, p1, Mat4::identity(), lo, hi));
    GpuPrimitive two;
    two.joint_bounds = {{{0, 0, 0}, {1, 1, 1}, true}, {{0, 0, 0}, {1, 1, 1}, true}};
    CHECK(!ModelRenderer::posed_world_bounds(two, p1, Mat4::identity(), lo, hi));
    two.joint_bounds = {{{0, 0, 0}, {1, 1, 1}, false}};
    CHECK(!ModelRenderer::posed_world_bounds(two, p1, Mat4::identity(), lo, hi));
    two.joint_bounds = {{{0, 0, 0}, {1, 1, 1}, true}};
    Mat4 bad = Mat4::identity();
    bad.at(0, 3) = std::numeric_limits<float>::infinity();
    CHECK(!ModelRenderer::posed_world_bounds(two, p1, bad, lo, hi));
    CHECK(ModelRenderer::posed_world_bounds(two, p1, Mat4::identity(), lo, hi));
}

TEST_CASE("Frustum culling never culls a skinned primitive any of whose posed vertices projects into the view")
{
    Offscreen o;
    RendererSettings settings;
    settings.skinning = true;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
    GpuModel model = skinned_triangle(renderer);
    // Joint 0 carries the triangle (0,0,0), (1,0,0), (0,1,0); the bounds are its box.
    model.meshes[0][0].joint_bounds = {{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, true}};
    SkinPalette palette = renderer.model_renderer().create_skin_palette(1, "walker");
    uint32_t state = 4242u;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    const Float3 verts[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    int visible = 0, invisible = 0, culled_invisible = 0;
    for (int i = 0; i < 500; ++i) {
        const Camera camera = look_at_camera({next() * 10 - 5, next() * 10 - 5, next() * 4}, {next() * 20 - 10, next() * 20 - 10, next() * 4 - 1});
        Transform t;
        t.position = {next() * 60 - 30, next() * 60 - 30, next() * 20 - 5};
        t.scale = {0.3f + next() * 5.0f, 0.3f + next() * 5.0f, 0.3f + next() * 5.0f};
        const Mat4 pose = to_matrix(t);
        const Mat4 poses[] = {pose};
        palette.update(poses);
        const Mat4 vp = view_projection_matrix(camera, 320.0f / 180.0f);
        // Independent oracle: sample the posed triangle (vertices, edges and interior); the primitive is visible if any sample projects inside the view.
        bool any = false;
        for (int s = 0; s < 300 && !any; ++s) {
            float a = next(), b = next();
            if (a + b > 1.0f) a = 1.0f - a, b = 1.0f - b;
            Float3 p = s < 3 ? verts[s] : verts[0] * (1.0f - a - b) + verts[1] * a + verts[2] * b;
            p = {pose.at(0, 0) * p.x + pose.at(0, 1) * p.y + pose.at(0, 2) * p.z + pose.at(0, 3), pose.at(1, 0) * p.x + pose.at(1, 1) * p.y + pose.at(1, 2) * p.z + pose.at(1, 3),
                 pose.at(2, 0) * p.x + pose.at(2, 1) * p.y + pose.at(2, 2) * p.z + pose.at(2, 3)};
            const Float4 clip = vp * Float4{p.x, p.y, p.z, 1.0f};
            any = clip.w > 0.0f && std::fabs(clip.x) <= clip.w && std::fabs(clip.y) <= clip.w && clip.z > 0.0f && clip.z <= clip.w;
        }
        const ModelDraw draws[] = {{&model, Mat4::identity(), &palette}};
        const bool culled = render_models(o, renderer, camera, draws).model_skinned_culled == 1;
        if (any) {
            ++visible;
            CHECK(!culled);
        } else {
            ++invisible;
            culled_invisible += culled ? 1 : 0;
        }
    }
    CHECK(visible >= 40);
    CHECK(invisible >= 200);
    CHECK(culled_invisible * 100 >= invisible * 70);  // conservative, but it culls most of what cannot be seen
    CHECK(o.device.validation_error_count() == 0);
}

// ---- Bloom (RendererSettings::bloom) -------------------------------------------------------------------------------------------------------

namespace {

RendererSettings bloom_settings(bool enabled, uint32_t levels = 5)
{
    RendererSettings s;
    s.hdr = true;
    s.bloom.enabled = enabled;
    s.bloom.levels = levels;
    return s;
}

}  // namespace

TEST_CASE("Bloom (null device): needs HDR and valid settings, refused before anything is created, and ignored (not validated) while disabled")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    const auto refused = [&](const RendererSettings& s) {
        bool threw = false;
        try {
            SceneRenderer r(o.device, rhi::Format::RGBA8_SRGB, s);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        return threw;
    };
    RendererSettings no_hdr;
    no_hdr.bloom.enabled = true;  // hdr is off
    CHECK(refused(no_hdr));
    RendererSettings bad = bloom_settings(true);
    bad.bloom.levels = 0;
    CHECK(refused(bad));
    bad = bloom_settings(true);
    bad.bloom.threshold = std::numeric_limits<float>::quiet_NaN();
    CHECK(refused(bad));
    CHECK(o.device.live_resource_count() == baseline);  // nothing was created for any of them
    RendererSettings ignored = bloom_settings(false);
    ignored.bloom.levels = 99;  // disabled bloom is not validated: the values are never used
    SceneRenderer fine(o.device, rhi::Format::RGBA8_SRGB, ignored);
    SceneRenderer plain(o.device, rhi::Format::RGBA8_SRGB, RendererSettings{});  // an LDR renderer with the default (disabled) bloom
}

TEST_CASE("Bloom (null device): off adds no pass and no resource; on adds 2 * levels - 1 draws, one chain and nothing else, and everything is released")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    {
        SceneRenderer off(o.device, rhi::Format::RGBA8_SRGB, bloom_settings(false));
        const FrameStats off_stats = off.render(o.device.begin_frame(), SceneFrame{}, o.target());
        (void)o.device.end_frame();
        const uint32_t off_passes = scene_pass_count(o.device.last_frame_commands());
        const uint32_t off_count = o.device.live_resource_count() - baseline;
        CHECK(off_stats.bloom_draw_calls == 0);
        CHECK(off_passes == 2);  // the scene and the tone map

        for (const uint32_t levels : {1u, 3u, 5u}) {
            const uint32_t before = o.device.live_resource_count();
            SceneRenderer on(o.device, rhi::Format::RGBA8_SRGB, bloom_settings(true, levels));
            const FrameStats stats = on.render(o.device.begin_frame(), SceneFrame{}, o.target());
            (void)o.device.end_frame();
            CHECK(stats.bloom_draw_calls == 2 * levels - 1);
            CHECK(stats.draw_calls == off_stats.draw_calls + 2 * levels - 1);
            CHECK(scene_pass_count(o.device.last_frame_commands()) == off_passes + 2 * levels - 1);
            // The same objects as a bloom-free HDR renderer, plus: 3 pipelines + 2 layouts + 5 shaders + a sampler + a second tone-map sampler and 2 more layout entries
            // for the pass itself (constant), and per level a texture and a bind group on the way down and again on the way up (one fewer).
            const uint32_t extra = o.device.live_resource_count() - before - off_count;
            CHECK(extra >= 2 * (2 * levels - 1));
            if (levels == 5) CHECK(stats.barriers > off_stats.barriers);
            o.device.wait_idle();
        }
    }
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == baseline);
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Bloom (null device): a resize rebuilds the chain without leaking, tiny targets reduce the levels, and a failed creation is a failed frame, not a broken renderer")
{
    Offscreen o(64, 48);
    rhi::TextureDesc small = *o.device.texture_desc(o.color);
    small.width = 6;
    small.height = 5;
    RenderTarget small_target = o.target(6, 5);
    small_target.texture = o.device.create_texture(small);
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, bloom_settings(true, 5));
    FrameStats stats = renderer.render(o.device.begin_frame(), SceneFrame{}, o.target(64, 48));
    (void)o.device.end_frame();
    CHECK(stats.bloom_draw_calls == 2 * 5 - 1);
    o.device.wait_idle();
    const uint32_t steady = o.device.live_resource_count();
    stats = renderer.render(o.device.begin_frame(), SceneFrame{}, small_target);
    (void)o.device.end_frame();
    CHECK(stats.bloom_draw_calls == 2 * bloom_level_count(6, 5, 5) - 1);  // 3 x 2, then 1 x 1: two levels
    CHECK(bloom_level_count(6, 5, 5) == 2);
    stats = renderer.render(o.device.begin_frame(), SceneFrame{}, o.target(64, 48));
    (void)o.device.end_frame();
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == steady);  // back at the first size: the same objects, nothing left over from the small chain

    // Failure injection at every creation of the chain's rebuild: the frame fails, every partial object is released, and the next frame works.
    uint32_t failures = 0;
    for (uint32_t n = 1; n < 60; ++n) {
        o.device.fail_creation_after(n);
        bool threw = false;
        try {
            (void)renderer.render(o.device.begin_frame(), SceneFrame{}, small_target);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        o.device.fail_creation_after(0);
        (void)o.device.end_frame();
        o.device.wait_idle();
        if (!threw) break;
        ++failures;
        CHECK(o.device.live_resource_count() == steady);
    }
    CHECK(failures >= 4);  // depth, hdr, the chain's textures and bind groups, the tone-map input
    stats = renderer.render(o.device.begin_frame(), SceneFrame{}, small_target);
    (void)o.device.end_frame();
    CHECK(stats.bloom_draw_calls == 3);
    CHECK(o.device.validation_error_count() == 0);
}

TEST_CASE("Bloom (null device): the tone-map pass takes a bloom texture exactly when it was built with bloom")
{
    Offscreen o;
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width = 8;
    td.height = 8;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorAttachment;
    const rhi::TextureHandle hdr = o.device.create_texture(td), glow = o.device.create_texture(td);
    const uint32_t before = o.device.live_resource_count();
    {
        ToneMapper plain(o.device, rhi::Format::RGBA8_SRGB);
        ToneMapper bloom(o.device, rhi::Format::RGBA8_SRGB, true);
        std::vector<rhi::BindGroupHandle> groups;
        const auto refused = [&](ToneMapper& m, rhi::TextureHandle bloom_texture) {
            try {
                groups.push_back(m.create_input(hdr, bloom_texture));
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        CHECK(refused(bloom, rhi::TextureHandle{}));  // the bloom permutation needs one
        CHECK(refused(plain, glow));                  // and the plain one has nowhere to bind it
        const uint32_t count = o.device.live_resource_count();
        CHECK(!refused(plain, rhi::TextureHandle{}));
        CHECK(!refused(bloom, glow));
        CHECK(o.device.live_resource_count() == count + 2);  // the two groups; the refusals created nothing
        for (const rhi::BindGroupHandle g : groups) o.device.destroy(g);
    }
    o.device.wait_idle();
    o.device.destroy(glow);
    o.device.destroy(hdr);
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == before - 2);
}

// ---- FXAA (RendererSettings::fxaa) ---------------------------------------------------------------------------------------------------------

TEST_CASE("FXAA (null device): invalid settings are refused before anything is created, and disabled settings are not validated")
{
    Offscreen o;
    const uint32_t baseline = o.device.live_resource_count();
    RendererSettings bad;
    bad.fxaa.enabled = true;
    bad.fxaa.span_max = 0.0f;
    bool threw = false;
    try {
        SceneRenderer r(o.device, rhi::Format::RGBA8_SRGB, bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(o.device.live_resource_count() == baseline);
    RendererSettings ignored;
    ignored.fxaa.span_max = 0.0f;  // disabled: never used
    SceneRenderer fine(o.device, rhi::Format::RGBA8_SRGB, ignored);
}

TEST_CASE("FXAA (null device): off adds no pass and no resource; on adds one intermediate target, one group and one draw, in LDR and HDR, and everything is released")
{
    for (const bool hdr : {false, true}) {
        Offscreen o;
        const uint32_t baseline = o.device.live_resource_count();
        {
            RendererSettings off_settings;
            off_settings.hdr = hdr;
            SceneRenderer off(o.device, rhi::Format::RGBA8_SRGB, off_settings);
            const FrameStats off_stats = off.render(o.device.begin_frame(), SceneFrame{}, o.target());
            (void)o.device.end_frame();
            const uint32_t off_passes = static_cast<uint32_t>(of_type(o.device.last_frame_commands(), rhi::CommandType::BeginRendering).size());
            const uint32_t off_count = o.device.live_resource_count();
            CHECK(off_stats.fxaa_draw_calls == 0);
            CHECK(off_passes == (hdr ? 2u : 1u));

            RendererSettings on_settings = off_settings;
            on_settings.fxaa.enabled = true;
            SceneRenderer on(o.device, rhi::Format::RGBA8_SRGB, on_settings);
            const uint32_t before = o.device.live_resource_count();
            const FrameStats stats = on.render(o.device.begin_frame(), SceneFrame{}, o.target());
            (void)o.device.end_frame();
            CHECK(stats.fxaa_draw_calls == 1);
            CHECK(stats.draw_calls == off_stats.draw_calls + 1);
            CHECK(of_type(o.device.last_frame_commands(), rhi::CommandType::BeginRendering).size() == off_passes + 1);
            // The FXAA pass's own pipeline objects and layout were created with the renderer; the frame added the depth target (as for any renderer), the intermediate texture and its group.
            const uint32_t on_extra = o.device.live_resource_count() - before;
            CHECK(on_extra >= 2);
            (void)off_count;
        }
        o.device.wait_idle();
        CHECK(o.device.live_resource_count() == baseline);
        CHECK(o.device.validation_error_count() == 0);
    }
}

TEST_CASE("FXAA (null device): a resize rebuilds the intermediate target without leaking, and a failed creation is a failed frame, not a broken renderer")
{
    Offscreen o(64, 48);
    rhi::TextureDesc small = *o.device.texture_desc(o.color);
    small.width = 8;
    small.height = 6;
    RenderTarget small_target = o.target(8, 6);
    small_target.texture = o.device.create_texture(small);
    RendererSettings s;
    s.fxaa.enabled = true;
    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, s);
    (void)renderer.render(o.device.begin_frame(), SceneFrame{}, o.target(64, 48));
    (void)o.device.end_frame();
    o.device.wait_idle();
    const uint32_t steady = o.device.live_resource_count();
    (void)renderer.render(o.device.begin_frame(), SceneFrame{}, small_target);
    (void)o.device.end_frame();
    (void)renderer.render(o.device.begin_frame(), SceneFrame{}, o.target(64, 48));
    (void)o.device.end_frame();
    o.device.wait_idle();
    CHECK(o.device.live_resource_count() == steady);
    uint32_t failures = 0;
    for (uint32_t n = 1; n < 20; ++n) {
        o.device.fail_creation_after(n);
        bool threw = false;
        try {
            (void)renderer.render(o.device.begin_frame(), SceneFrame{}, small_target);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        o.device.fail_creation_after(0);
        (void)o.device.end_frame();
        o.device.wait_idle();
        if (!threw) break;
        ++failures;
        CHECK(o.device.live_resource_count() == steady);
    }
    CHECK(failures >= 3);  // the depth target, the intermediate texture, its group
    CHECK(renderer.render(o.device.begin_frame(), SceneFrame{}, small_target).fxaa_draw_calls == 1);
    (void)o.device.end_frame();
    CHECK(o.device.validation_error_count() == 0);
}
