#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/scene_renderer.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <cmath>
#include <cstring>
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
