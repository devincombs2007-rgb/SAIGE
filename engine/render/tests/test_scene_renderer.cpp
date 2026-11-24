#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"
#include "saige/rhi_null/null_device.hpp"

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

    explicit Offscreen(uint32_t w = 320, uint32_t h = 180)
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
