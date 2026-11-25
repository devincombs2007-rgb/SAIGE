#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/camera.hpp"
#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <limits>
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

TEST_CASE("SceneRenderer: a creation failure during construction releases everything already created")
{
    // The destructor does not run for a constructor that throws, and the shaders, pipelines and cube buffers are raw handles: the constructor
    // itself must release them. Each creation fails in turn, with and without the HDR path (which adds the tone mapper).
    for (const bool hdr : {false, true}) {
        Offscreen o;
        const uint32_t baseline = o.device.live_resource_count();
        uint32_t failures = 0;
        for (uint32_t n = 1; n <= 60; ++n) {
            bool threw = false;
            {
                RendererSettings settings;
                settings.hdr = hdr;
                o.device.fail_creation_after(n);
                try {
                    SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
                    o.device.fail_creation_after(0);
                    CHECK(o.device.live_resource_count() > baseline);
                } catch (const std::runtime_error&) {
                    threw = true;
                }
                o.device.fail_creation_after(0);
            }
            o.device.wait_idle();
            CHECK(o.device.live_resource_count() == baseline);  // whichever creation failed, nothing is left behind
            if (!threw) {
                break;  // n is beyond the number of creations: every step has been failed in turn
            }
            ++failures;
        }
        CHECK(failures >= (hdr ? 13u : 8u));
        // The same device builds a renderer afterwards.
        RendererSettings ok;
        ok.hdr = hdr;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, ok);
        CHECK(o.device.validation_error_count() == 0);
    }
}

TEST_CASE("ModelRenderer: a creation failure during construction releases everything already created")
{
    // Before this, 15 of the 16 failure points leaked: the shaders, layouts and pipelines are raw handles held until a destructor that
    // never runs for a constructor that throws.
    for (const ModelShading shading : {ModelShading::Legacy, ModelShading::MetallicRoughness}) {
        Offscreen o;
        RendererSettings settings;
        settings.model_shading = shading;
        SceneRenderer renderer(o.device, rhi::Format::RGBA8_SRGB, settings);
        const uint32_t baseline = o.device.live_resource_count();
        uint32_t failures = 0;
        for (uint32_t n = 1; n <= 60; ++n) {
            bool threw = false;
            o.device.fail_creation_after(n);
            try {
                (void)renderer.model_renderer();
            } catch (const std::runtime_error&) {
                threw = true;
            }
            o.device.fail_creation_after(0);
            if (!threw) {
                break;
            }
            ++failures;
            o.device.wait_idle();
            CHECK(o.device.live_resource_count() == baseline);
        }
        CHECK(failures >= 14);
        // The renderer is still usable: the model renderer now exists and the failed attempts left nothing behind.
        CHECK(renderer.model_renderer().shading() == shading);
        CHECK(o.device.validation_error_count() == 0);
    }
}

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

// ---- Bloom (RendererSettings::bloom) -------------------------------------------------------------------------------------------------------

namespace {

uint32_t scene_pass_count(const std::vector<rhi::RecordedCommand>& cmds)
{
    uint32_t n = 0;
    for (const auto& c : cmds) n += c.type == rhi::CommandType::BeginRendering ? 1u : 0u;
    return n;
}

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
