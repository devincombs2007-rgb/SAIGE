#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <memory>
#include <stdexcept>
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
