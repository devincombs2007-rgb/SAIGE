// The RHI pieces a shadow map is built from, on a real (validated) Vulkan device: a depth-only pipeline (no pixel shader) writes depth,
// the rasterizer's depth bias moves what it stores, and the depth texture can be read back through a comparison sampler. The probe
// shader shows a ladder of reference depths against the stored depth, so the row where the comparison flips *is* the stored depth.
// CI: lavapipe. Validation errors fail each test.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaders/test_depth_only.spv.hpp"
#include "saige/shaders/test_shadow_probe.spv.hpp"
#include "vulkan_test_util.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;

namespace {

constexpr uint32_t kMap = 256;

template <typename T, size_t N>
std::span<const uint32_t> words(const T (&a)[N])
{
    return std::span<const uint32_t>(a, N);
}

struct Vertex {
    float x, y, z;
};

/// A quad in clip space (depth written as given: the clip matrix is the identity), x and y in [-1, 1], z at each corner.
std::vector<Vertex> quad(float x0, float y0, float x1, float y1, float z_left, float z_right)
{
    return {{x0, y0, z_left}, {x1, y0, z_right}, {x1, y1, z_right}, {x0, y0, z_left}, {x1, y1, z_right}, {x0, y1, z_left}};
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    rhi::ShaderHandle depth_vs, probe_vs, probe_ps;
    rhi::BindGroupLayoutHandle layout;
    rhi::PipelineHandle probe_pipeline;
    rhi::TextureHandle depth, color;
    rhi::BufferHandle readback, uniform;
    rhi::SamplerHandle sampler;

    Rig(const char* name, rhi::CompareOp compare = rhi::CompareOp::GreaterOrEqual) : device(make_validated_device(name))
    {
        depth_vs = device->create_shader({rhi::ShaderStage::Vertex, words(shaders::test_depth_only_vs_main), "VSMain", "TestDepthOnly.VS"});
        probe_vs = device->create_shader({rhi::ShaderStage::Vertex, words(shaders::test_shadow_probe_vs_main), "VSMain", "TestShadowProbe.VS"});
        probe_ps = device->create_shader({rhi::ShaderStage::Pixel, words(shaders::test_shadow_probe_ps_main), "PSMain", "TestShadowProbe.PS"});
        rhi::BindGroupLayoutDesc ld;
        ld.entries = {{0, rhi::BindingType::SampledTexture}, {1, rhi::BindingType::Sampler}, {2, rhi::BindingType::UniformBuffer}};
        ld.debug_name = "ShadowProbeLayout";
        layout = device->create_bind_group_layout(ld);
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = probe_vs;
        pd.pixel_shader = probe_ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {rhi::Format::RGBA8_UNORM};
        pd.bind_group_layouts = {layout};
        pd.debug_name = "ShadowProbe";
        probe_pipeline = device->create_graphics_pipeline(pd);

        rhi::TextureDesc td;
        td.format = rhi::Format::D32_FLOAT;
        td.width = td.height = kMap;
        td.usage = rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled;
        td.debug_name = "ShadowMap";
        depth = device->create_texture(td);
        td.format = rhi::Format::RGBA8_UNORM;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "ProbeTarget";
        color = device->create_texture(td);
        rhi::BufferDesc bd;
        bd.size = uint64_t{kMap} * kMap * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "ProbeReadback";
        readback = device->create_buffer(bd);
        rhi::BufferDesc ub;
        ub.size = 256;
        ub.usage = rhi::BufferUsage::Uniform;
        ub.memory = rhi::MemoryLocation::CpuToGpu;
        ub.debug_name = "ProbeUniform";
        uniform = device->create_buffer(ub);
        rhi::SamplerDesc sd;
        sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
        sd.mipmap_mode = rhi::MipmapMode::Nearest;
        sd.address_u = sd.address_v = rhi::AddressMode::ClampToEdge;
        sd.compare_enable = true;
        sd.compare_op = compare;
        sampler = device->create_sampler(sd);
    }

    ~Rig()
    {
        device->wait_idle();
        device->destroy(sampler);
        device->destroy(uniform);
        device->destroy(readback);
        device->destroy(color);
        device->destroy(depth);
        device->destroy(probe_pipeline);
        device->destroy(layout);
        device->destroy(probe_ps);
        device->destroy(probe_vs);
        device->destroy(depth_vs);
    }

    rhi::PipelineHandle depth_pipeline(rhi::DepthBias bias, const char* name = "DepthOnly")
    {
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = depth_vs;  // no pixel shader: a depth-only pipeline
        pd.cull_mode = rhi::CullMode::None;
        pd.depth.test = true;
        pd.depth.write = true;
        pd.depth_format = rhi::Format::D32_FLOAT;
        pd.depth_bias = bias;
        pd.push_constant_size = 64;
        pd.vertex_layout.stride = sizeof(Vertex);
        pd.vertex_layout.attributes = {{0, rhi::Format::RGB32_FLOAT, 0}};
        pd.debug_name = name;
        return device->create_graphics_pipeline(pd);
    }

    /// Draws `vertices` depth-only into the shadow map, then reads it back through the comparison sampler with reference depths running
    /// from `base` to `base + range` down the picture. Returns the red channel (255 = the comparison passed).
    std::vector<uint8_t> probe(const std::vector<Vertex>& vertices, rhi::PipelineHandle pipeline, float base, float range)
    {
        rhi::BufferDesc vb;
        vb.size = vertices.size() * sizeof(Vertex);
        vb.usage = rhi::BufferUsage::Vertex;
        vb.memory = rhi::MemoryLocation::CpuToGpu;
        vb.debug_name = "ShadowCasterVertices";
        const rhi::BufferHandle vertex_buffer = device->create_buffer(vb);
        std::memcpy(device->map(vertex_buffer).data(), vertices.data(), vb.size);
        const float uniform_data[4] = {base, range, 0.0f, 0.0f};
        std::memcpy(device->map(uniform).data(), uniform_data, sizeof(uniform_data));
        rhi::BindGroupEntry entries[3];
        entries[0].binding = 0;
        entries[0].texture = depth;
        entries[1].binding = 1;
        entries[1].sampler = sampler;
        entries[2].binding = 2;
        entries[2].buffer = uniform;
        entries[2].size = 16;
        const rhi::BindGroupHandle group = device->create_bind_group(layout, entries);

        rhi::CommandList& cmd = device->begin_frame();
        const rhi::ResourceState depth_write{rhi::PipelineStage::EarlyFragmentTests | rhi::PipelineStage::LateFragmentTests,
                                             rhi::Access::DepthStencilWrite, rhi::TextureLayout::DepthStencilAttachment};
        // A depth map is sampled in the same state as any sampled texture: bind group descriptors are written for ShaderReadOnly, and
        // sampling from DepthStencilReadOnly instead is a validation error (found by this test; barrier.hpp now says so).
        const rhi::ResourceState depth_sample = rhi::sampled_texture_state;
        const rhi::TextureBarrier to_depth{depth, {rhi::PipelineStage::AllCommands, rhi::Access::DepthStencilWrite, rhi::TextureLayout::Undefined}, depth_write, {}};
        cmd.barrier(std::span(&to_depth, 1));
        {
            rhi::RenderingDesc rd;  // depth only: no color attachment
            rd.depth = rhi::DepthAttachment{depth, rhi::AttachmentLoadOp::Clear, rhi::AttachmentStoreOp::Store, 0.0f};
            rd.area = {0, 0, kMap, kMap};
            cmd.begin_rendering(rd);
            cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kMap), static_cast<float>(kMap), 0.0f, 1.0f});
            cmd.set_scissor({0, 0, kMap, kMap});
            cmd.bind_pipeline(pipeline);
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            cmd.push_constants(std::as_bytes(std::span(identity)));
            cmd.bind_vertex_buffer(vertex_buffer);
            cmd.draw(static_cast<uint32_t>(vertices.size()));
            cmd.end_rendering();
        }
        const rhi::TextureBarrier to_sampled{depth, depth_write, depth_sample, {}};
        const rhi::TextureBarrier to_color{color, {rhi::PipelineStage::AllCommands, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::Undefined},
                                           {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment}, {}};
        const rhi::TextureBarrier barriers[] = {to_sampled, to_color};
        cmd.barrier(barriers);
        {
            rhi::RenderingDesc rd;
            rhi::ColorAttachment ca;
            ca.texture = color;
            ca.clear = {0.0f, 0.0f, 0.0f, 1.0f};
            rd.colors.push_back(ca);
            rd.area = {0, 0, kMap, kMap};
            cmd.begin_rendering(rd);
            cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kMap), static_cast<float>(kMap), 0.0f, 1.0f});
            cmd.set_scissor({0, 0, kMap, kMap});
            cmd.bind_pipeline(probe_pipeline);
            cmd.bind_group(0, group);
            cmd.draw(3);
            cmd.end_rendering();
        }
        const rhi::TextureBarrier to_transfer{color, to_color.after, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(color, readback);
        (void)device->end_frame();
        device->wait_idle();
        const auto mapped = device->map(readback);
        std::vector<uint8_t> red(size_t{kMap} * kMap);
        for (size_t i = 0; i < red.size(); ++i) {
            red[i] = static_cast<uint8_t>(mapped[i * 4]);
        }
        device->destroy(group);
        device->destroy(vertex_buffer);
        return red;
    }
};

uint8_t at(const std::vector<uint8_t>& image, uint32_t x, uint32_t y) { return image[size_t{y} * kMap + x]; }

/// The reference depth the probe ladder shows at row `y` (the pixel center).
float ladder(uint32_t y, float base, float range) { return base + (static_cast<float>(y) + 0.5f) / static_cast<float>(kMap) * range; }

/// The first row of column x whose comparison passed, in the rows [from, to): the stored depth to the ladder's resolution.
std::optional<uint32_t> first_pass(const std::vector<uint8_t>& image, uint32_t x, uint32_t from, uint32_t to)
{
    for (uint32_t y = from; y < to; ++y) {
        if (at(image, x, y) > 127) return y;
    }
    return std::nullopt;
}

/// The stored depth at column x inside the quad (rows 64..191), measured from the ladder: the reference at the first row whose comparison
/// passed. (Above the quad the map is empty and everything passes, so the search must not start there.)
std::optional<float> stored_depth(const std::vector<uint8_t>& image, uint32_t x, float base, float range)
{
    const auto row = first_pass(image, x, 64, 192);
    if (!row) return std::nullopt;
    return ladder(*row, base, range);
}

} // namespace

TEST_CASE("Shadow map: a depth-only pass is read back through a comparison sampler")
{
    Rig rig("shadow_map_roundtrip");
    // A quad covering the middle quarter of the map, at depth 0.6 (reverse-Z: larger is closer to the light).
    const auto pipeline = rig.depth_pipeline({});
    const float base = 0.5f, range = 0.2f;  // the ladder runs from 0.5 at the top to 0.7 at the bottom
    const auto image = rig.probe(quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.6f, 0.6f), pipeline, base, range);
    CHECK(rig.device->validation_error_count() == 0);

    // Outside the quad nothing was drawn (stored depth 0, the far plane): every reference is in front of it, so everything is lit.
    for (const uint32_t y : {2u, 60u, 128u, 250u}) {
        CHECK(at(image, 4, y) == 255);
        CHECK(at(image, 251, y) == 255);
    }
    // Inside the quad, a reference deeper than the stored 0.6 is behind the occluder (shadowed) and one at least as near is lit. The ladder
    // crosses 0.6 halfway down the picture (row 128), and the quad covers rows 64..191.
    const uint32_t x = 128;
    for (uint32_t y = 70; y < 124; y += 6) CHECK(at(image, x, y) == 0);      // references 0.527 .. 0.597: behind the surface
    for (uint32_t y = 134; y < 188; y += 6) CHECK(at(image, x, y) == 255);   // references 0.603 .. 0.673: in front of it
    // Past the quad's edge in y the map is empty again, so even the deepest... (rows below 192 are outside the quad).
    CHECK(at(image, x, 200) == 255);
    const auto stored = stored_depth(image, x, base, range);
    REQUIRE(stored.has_value());
    // The ladder is read within its own resolution (range / rows = 0.00078); the first passing row can only be inside the quad here.
    CHECK(std::abs(*stored - 0.6f) < 0.002f || *stored < 0.51f);
    const auto inside = first_pass(image, x, 64, 192);
    REQUIRE(inside.has_value());
    CHECK(std::abs(ladder(*inside, base, range) - 0.6f) < 0.002f);

    // The comparison direction is the sampler's: with LessOrEqual the shadowed and lit halves swap inside the quad.
    Rig flipped("shadow_map_flipped", rhi::CompareOp::LessOrEqual);
    const auto flipped_pipeline = flipped.depth_pipeline({});
    const auto swapped = flipped.probe(quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.6f, 0.6f), flipped_pipeline, base, range);
    CHECK(at(swapped, x, 90) == 255);
    CHECK(at(swapped, x, 170) == 0);
    CHECK(flipped.device->validation_error_count() == 0);
    rig.device->destroy(pipeline);
    flipped.device->destroy(flipped_pipeline);
}

TEST_CASE("Shadow map: the rasterizer's depth bias moves the stored depth, constant and slope factors independently")
{
    Rig rig("shadow_map_bias");
    // The ladder spans 0.59..0.61 (about 0.00008 per row), around the quad's depth of 0.6.
    const float base = 0.59f, range = 0.02f;
    const auto flat = quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.6f, 0.6f);
    const auto measure = [&](const std::vector<Vertex>& vertices, rhi::DepthBias bias) {
        const auto pipeline = rig.depth_pipeline(bias, "DepthOnlyBiased");
        const auto image = rig.probe(vertices, pipeline, base, range);
        rig.device->destroy(pipeline);
        const auto stored = stored_depth(image, 128, base, range);
        REQUIRE(stored.has_value());
        return *stored;
    };
    const float plain = measure(flat, {});
    CHECK(std::abs(plain - 0.6f) < 0.0002f);

    // A constant factor of N moves the depth by about N * r, r = 2^-24 for depths in [0.5, 1) in a float depth format (Vulkan's rule).
    const float r = std::ldexp(1.0f, -24);
    const float pushed_back = measure(flat, {-16384.0f, 0.0f});
    const float pulled_forward = measure(flat, {16384.0f, 0.0f});
    CHECK(pushed_back < plain - 0.5f * 16384.0f * r);        // reverse-Z: a negative bias moves the surface away from the light
    CHECK(pushed_back > plain - 2.0f * 16384.0f * r - 0.0002f);
    CHECK(pulled_forward > plain + 0.5f * 16384.0f * r);
    CHECK(pulled_forward < plain + 2.0f * 16384.0f * r + 0.0002f);

    // A slope factor scales with how steeply depth changes across the pixel, so a flat quad does not move and a tilted one does. The tilted
    // quad's depth changes by 0.1 over 128 pixels (0.00078 per pixel); a slope factor of -1 moves it by about that much.
    const float flat_slope = measure(flat, {0.0f, -2.0f});
    CHECK(std::abs(flat_slope - plain) < 0.0002f);
    const auto tilted = quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.55f, 0.65f);
    const float tilted_plain = measure(tilted, {});
    const float tilted_biased = measure(tilted, {0.0f, -1.0f});
    CHECK(std::abs(tilted_plain - 0.6f) < 0.001f);
    CHECK(tilted_biased < tilted_plain - 0.0003f);
    CHECK(tilted_biased > tilted_plain - 0.0016f);
    const float tilted_double = measure(tilted, {0.0f, -2.0f});
    CHECK(tilted_double < tilted_biased - 0.0003f);   // a larger factor moves it further
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Shadow map: depth-only pipelines and depth bias are validated before anything is created")
{
    Rig rig("shadow_map_validation");
    const auto rejected = [&](auto&& configure) {
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = rig.depth_vs;
        pd.cull_mode = rhi::CullMode::None;
        pd.depth.test = true;
        pd.depth.write = true;
        pd.depth_format = rhi::Format::D32_FLOAT;
        pd.push_constant_size = 64;
        pd.vertex_layout.stride = sizeof(Vertex);
        pd.vertex_layout.attributes = {{0, rhi::Format::RGB32_FLOAT, 0}};
        configure(pd);
        try {
            const rhi::PipelineHandle h = rig.device->create_graphics_pipeline(pd);
            rig.device->destroy(h);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(!rejected([](rhi::GraphicsPipelineDesc&) {}));  // the valid depth-only description is accepted
    CHECK(rejected([](rhi::GraphicsPipelineDesc& pd) { pd.color_formats = {rhi::Format::RGBA8_UNORM}; }));  // no pixel shader but a color target
    CHECK(rejected([](rhi::GraphicsPipelineDesc& pd) { pd.depth_format = rhi::Format::Unknown; pd.depth.test = pd.depth.write = false; }));  // nothing to write at all
    CHECK(rejected([](rhi::GraphicsPipelineDesc& pd) { pd.depth_bias.constant_factor = std::numeric_limits<float>::quiet_NaN(); }));
    CHECK(rejected([](rhi::GraphicsPipelineDesc& pd) { pd.depth_bias.slope_factor = std::numeric_limits<float>::infinity(); }));
    CHECK(rejected([&](rhi::GraphicsPipelineDesc& pd) { pd.pixel_shader = rig.depth_vs; }));  // a vertex shader is not a pixel shader
    CHECK(rig.device->validation_error_count() == 0);
}
