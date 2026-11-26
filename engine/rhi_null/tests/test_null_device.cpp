#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi_null/null_device.hpp"

#include <array>
#include <limits>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>
#include <stdexcept>

using namespace saige::rhi;

namespace {

const std::array<uint32_t, 5> fake_spirv{0x07230203u, 0x00010600u, 0u, 1u, 0u};

TextureDesc color_target(uint32_t w = 64, uint32_t h = 32)
{
    TextureDesc d;
    d.format = Format::RGBA8_SRGB;
    d.width = w;
    d.height = h;
    d.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    return d;
}

struct PipelineFixture {
    NullDevice device;
    ShaderHandle vs, ps;
    PipelineHandle pipeline;

    PipelineFixture()
    {
        vs = device.create_shader({ShaderStage::Vertex, fake_spirv, "VSMain", "vs"});
        ps = device.create_shader({ShaderStage::Pixel, fake_spirv, "PSMain", "ps"});
        GraphicsPipelineDesc pd;
        pd.vertex_shader = vs;
        pd.pixel_shader = ps;
        pd.color_formats = {Format::RGBA8_SRGB};
        pd.push_constant_size = 16;
        pipeline = device.create_graphics_pipeline(pd);
    }
};

template <typename Fn>
bool throws_invalid_argument(Fn&& fn)
{
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

TEST_CASE("NullDevice: reports a software device with validation")
{
    NullDevice device;
    CHECK(device.info().backend == Backend::Null);
    CHECK(device.info().software_device);
    CHECK(device.validation_error_count() == 0);
}

TEST_CASE("NullDevice: resource creation validates descriptions")
{
    NullDevice device;
    CHECK(throws_invalid_argument([&] { (void)device.create_texture(TextureDesc{}); }));
    CHECK(throws_invalid_argument([&] { (void)device.create_buffer(BufferDesc{}); }));
    const std::array<uint32_t, 2> not_spirv{1u, 2u};
    CHECK(throws_invalid_argument([&] { (void)device.create_shader({ShaderStage::Vertex, not_spirv, "main", nullptr}); }));

    BufferDesc bd;
    bd.size = 4;
    bd.usage = BufferUsage::Vertex;
    const std::array<std::byte, 8> too_much{};
    CHECK(throws_invalid_argument([&] { (void)device.create_buffer(bd, too_much); }));
}

TEST_CASE("NullDevice: pipeline creation checks shaders, formats and push constants")
{
    PipelineFixture f;
    REQUIRE(f.pipeline.is_valid());

    GraphicsPipelineDesc bad;
    bad.vertex_shader = f.ps;  // wrong stage
    bad.pixel_shader = f.ps;
    bad.color_formats = {Format::RGBA8_SRGB};
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));

    bad.vertex_shader = f.vs;
    bad.push_constant_size = 132;
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));

    bad.push_constant_size = 0;
    bad.depth.test = true;  // no depth format
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));
}

TEST_CASE("NullDevice: depth-only pipelines (no pixel shader) and depth bias are validated like the Vulkan backend")
{
    PipelineFixture f;
    GraphicsPipelineDesc pd;
    pd.vertex_shader = f.vs;  // a shadow pass: no pixel shader, no color target, a depth target
    pd.depth.test = true;
    pd.depth.write = true;
    pd.depth_format = Format::D32_FLOAT;
    pd.depth_bias = {-4.0f, -1.5f};
    CHECK(f.device.create_graphics_pipeline(pd).is_valid());
    CHECK(pd.depth_bias.enabled());
    CHECK(!DepthBias{}.enabled());

    GraphicsPipelineDesc bad = pd;
    bad.color_formats = {Format::RGBA8_SRGB};  // color output needs a pixel shader
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));
    bad = pd;
    bad.depth_format = Format::Unknown;  // nothing would be written
    bad.depth.test = bad.depth.write = false;
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));
    bad = pd;
    bad.depth_bias.constant_factor = std::numeric_limits<float>::quiet_NaN();
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));
    bad = pd;
    bad.depth_bias.slope_factor = -std::numeric_limits<float>::infinity();
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));
    bad = pd;
    bad.pixel_shader = f.vs;  // a vertex shader in the pixel slot is still an error
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(bad); }));
    CHECK(f.device.validation_error_count() == 0);
}

TEST_CASE("NullDevice: blend state is optional, validated, and kept with the pipeline")
{
    PipelineFixture f;  // its pipeline has no blend states: blending off, as before blend state existed
    GraphicsPipelineDesc pd;
    pd.vertex_shader = f.vs;
    pd.pixel_shader = f.ps;
    pd.color_formats = {Format::RGBA8_SRGB};
    pd.blend = {BlendState::source_over()};
    CHECK(f.device.create_graphics_pipeline(pd).is_valid());

    pd.color_formats = {Format::RGBA16_FLOAT};
    CHECK(f.device.create_graphics_pipeline(pd).is_valid());

    pd.color_formats = {Format::RGBA8_UINT};  // integer formats do not blend
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(pd); }));
    pd.blend = {BlendState{}};  // a disabled state needs no blendable format
    CHECK(f.device.create_graphics_pipeline(pd).is_valid());

    pd.color_formats = {Format::RGBA8_SRGB, Format::RGBA8_SRGB};
    pd.blend = {BlendState::source_over()};  // one state for two attachments
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(pd); }));
    pd.blend = {BlendState::source_over(), BlendState::additive()};  // independent blending is not exposed
    CHECK(throws_invalid_argument([&] { (void)f.device.create_graphics_pipeline(pd); }));
    CHECK(f.device.validation_error_count() == 0);  // rejected descriptions are exceptions, not validation errors
}

TEST_CASE("NullDevice: host-visible buffers are mapped with their initial data")
{
    NullDevice device;
    BufferDesc bd;
    bd.size = 16;
    bd.usage = BufferUsage::TransferDst;
    bd.memory = MemoryLocation::GpuToCpu;
    const std::array<std::byte, 4> init{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const BufferHandle b = device.create_buffer(bd, init);
    const auto mapped = device.map(b);
    REQUIRE(mapped.size() == 16);
    CHECK(mapped[2] == std::byte{3});
    CHECK(mapped[10] == std::byte{0});

    bd.memory = MemoryLocation::GpuOnly;
    bd.usage = BufferUsage::Vertex;
    CHECK(device.map(device.create_buffer(bd)).empty());
}

TEST_CASE("NullDevice: records a valid frame without validation errors")
{
    PipelineFixture f;
    const TextureHandle rt = f.device.create_texture(color_target());

    CommandList& cmd = f.device.begin_frame();
    cmd.begin_debug_label("Frame");
    cmd.begin_rendering({{{rt, AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0.1f, 0.2f, 0.3f, 1.0f}}},
                         std::nullopt,
                         {0, 0, 64, 32}});
    cmd.set_viewport({0, 0, 64, 32, 0, 1});
    cmd.set_scissor({0, 0, 64, 32});
    cmd.bind_pipeline(f.pipeline);
    const std::array<float, 4> pc{1, 2, 3, 4};
    cmd.push_constant_struct(pc);
    cmd.draw(3);
    cmd.end_rendering();
    cmd.end_debug_label();
    const FrameResult r = f.device.end_frame();

    CHECK_FALSE(r.presented);
    CHECK(f.device.validation_error_count() == 0);
    const auto& cmds = f.device.last_frame_commands();
    REQUIRE(cmds.size() == 9);  // label, begin, viewport, scissor, bind, push, draw, end, end label
    CHECK(cmds[1].type == CommandType::BeginRendering);
    CHECK(cmds[1].rendering.colors[0].clear.g == 0.2f);
    CHECK(cmds[5].type == CommandType::PushConstants);
    CHECK(cmds[5].push_data.size() == 16);
    CHECK(cmds[6].type == CommandType::Draw);
    CHECK(cmds[6].count == 3);
    CHECK(cmds[6].pipeline == f.pipeline);
    CHECK(f.device.frame_number() == 1);
}

TEST_CASE("NullDevice: API misuse is reported as validation errors")
{
    PipelineFixture f;
    CommandList& cmd = f.device.begin_frame();
    cmd.draw(3);  // outside rendering, no pipeline
    const uint32_t after_draw = f.device.validation_error_count();
    CHECK(after_draw >= 2);

    const TextureHandle rt = f.device.create_texture(color_target());
    ColorAttachment color;
    color.texture = rt;
    cmd.begin_rendering({{color}, std::nullopt, {0, 0, 64, 32}});
    cmd.bind_pipeline(f.pipeline);
    const std::array<float, 8> too_big{};
    cmd.push_constant_struct(too_big);  // 32 bytes > 16-byte range
    CHECK(f.device.validation_error_count() == after_draw + 1);
    (void)f.device.end_frame();  // rendering still open
    CHECK(f.device.validation_error_count() == after_draw + 2);

    (void)f.device.end_frame();  // without begin
    CHECK(f.device.validation_error_count() == after_draw + 3);
    CHECK_FALSE(f.device.validation_messages().empty());
}

TEST_CASE("NullDevice: destruction is deferred until the frame retires")
{
    DeviceDesc dd;
    dd.frames_in_flight = 2;
    NullDevice device(dd);
    const TextureHandle t = device.create_texture(color_target());
    CHECK(device.live_resource_count() == 1);
    device.destroy(t);
    CHECK(device.pending_destruction_count() == 1);
    CHECK(device.texture_desc(t).has_value());  // still alive for in-flight frames

    (void)device.begin_frame();
    (void)device.end_frame();
    CHECK(device.live_resource_count() == 1);
    (void)device.begin_frame();
    (void)device.end_frame();
    CHECK(device.live_resource_count() == 0);
    CHECK_FALSE(device.texture_desc(t).has_value());

    device.destroy(t);  // double destroy
    CHECK(device.validation_error_count() == 1);
}

TEST_CASE("NullDevice: wait_idle releases everything pending")
{
    NullDevice device;
    BufferDesc bd;
    bd.size = 64;
    bd.usage = BufferUsage::Vertex;
    device.destroy(device.create_buffer(bd));
    device.wait_idle();
    CHECK(device.pending_destruction_count() == 0);
    CHECK(device.live_resource_count() == 0);
}

TEST_CASE("NullDevice: swapchain acquire, present, resize and out-of-date")
{
    NullDevice device;
    SwapchainDesc sd;
    sd.surface = {NativeSurface::System::Win32, nullptr, 1};
    sd.width = 800;
    sd.height = 600;
    const SwapchainHandle sc = device.create_swapchain(sd);
    CHECK(device.swapchain_format(sc) == Format::BGRA8_SRGB);

    (void)device.begin_frame();
    const AcquireResult a = device.acquire_next_image(sc);
    REQUIRE(a.status == AcquireStatus::Success);
    REQUIRE(device.texture_desc(a.texture).has_value());
    CHECK(device.texture_desc(a.texture)->width == 800);
    const FrameResult r = device.end_frame();
    CHECK(r.presented);
    CHECK(device.present_count() == 1);

    device.resize_swapchain(sc, 0, 0);  // minimized
    (void)device.begin_frame();
    CHECK(device.acquire_next_image(sc).status == AcquireStatus::NotReady);
    (void)device.end_frame();

    device.resize_swapchain(sc, 1024, 768);
    CHECK(device.swapchain_extent(sc).width == 1024);
    CHECK(device.swapchain_recreate_count(sc) == 2);

    device.force_out_of_date(sc);
    (void)device.begin_frame();
    CHECK(device.acquire_next_image(sc).status == AcquireStatus::OutOfDate);
    (void)device.end_frame();
    CHECK(device.validation_error_count() == 0);
    device.destroy(sc);
    CHECK(device.live_resource_count() == 0);
}

TEST_CASE("NullDevice: headless surfaces cannot create swapchains, swapchain images cannot be destroyed")
{
    NullDevice device;
    CHECK(throws_invalid_argument([&] { (void)device.create_swapchain(SwapchainDesc{}); }));

    SwapchainDesc sd;
    sd.surface = {NativeSurface::System::Win32, nullptr, 1};
    sd.width = 4;
    sd.height = 4;
    const SwapchainHandle sc = device.create_swapchain(sd);
    (void)device.begin_frame();
    const AcquireResult a = device.acquire_next_image(sc);
    device.destroy(a.texture);
    (void)device.end_frame();
    CHECK(device.validation_error_count() == 1);
}

TEST_CASE("NullDevice: readback copy validates usage and size")
{
    NullDevice device;
    const TextureHandle rt = device.create_texture(color_target(8, 8));
    BufferDesc bd;
    bd.size = 8 * 8 * 4;
    bd.usage = BufferUsage::TransferDst;
    bd.memory = MemoryLocation::GpuToCpu;
    const BufferHandle ok = device.create_buffer(bd);
    bd.size = 16;
    const BufferHandle small = device.create_buffer(bd);

    CommandList& cmd = device.begin_frame();
    cmd.copy_texture_to_buffer(rt, ok);
    CHECK(device.validation_error_count() == 0);
    cmd.copy_texture_to_buffer(rt, small);
    CHECK(device.validation_error_count() == 1);
    (void)device.end_frame();
}

namespace {

TextureDesc sampled_texture(uint32_t w, uint32_t h, uint32_t mips = 1, uint32_t layers = 1)
{
    TextureDesc d;
    d.format = Format::RGBA8_UNORM;
    d.width = w;
    d.height = h;
    d.mip_levels = mips;
    d.array_layers = layers;
    d.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    return d;
}

template <typename Fn>
bool throws_invalid(Fn&& fn)
{
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

} // namespace

TEST_CASE("NullDevice: upload_texture checks the data size against the full mip chain and layers")
{
    NullDevice device;
    const TextureDesc desc = sampled_texture(13, 7, full_mip_chain, 3);
    const TextureHandle t = device.create_texture(desc);
    const uint64_t expected = compute_texture_data_size(desc);
    CHECK(expected > 13 * 7 * 4 * 3);  // more than level 0 alone
    std::vector<std::byte> data(expected);
    CHECK(device.uploaded_bytes(t) == 0);
    device.upload_texture(t, data);
    CHECK(device.uploaded_bytes(t) == expected);

    CHECK(throws_invalid([&] { device.upload_texture(t, std::span<const std::byte>(data).first(expected - 1)); }));
    data.push_back(std::byte{0});
    CHECK(throws_invalid([&] { device.upload_texture(t, data); }));
    CHECK(throws_invalid([&] { device.upload_texture(TextureHandle{}, std::span<const std::byte>{}); }));
    CHECK(device.validation_error_count() == 0);
}

TEST_CASE("NullDevice: upload_texture requires Sampled | TransferDst, single sample, and no open frame")
{
    NullDevice device;
    TextureDesc no_dst = sampled_texture(4, 4);
    no_dst.usage = TextureUsage::Sampled;
    TextureDesc no_sampled = sampled_texture(4, 4);
    no_sampled.usage = TextureUsage::TransferDst;
    const std::vector<std::byte> data(4 * 4 * 4);
    CHECK(throws_invalid([&] { device.upload_texture(device.create_texture(no_dst), data); }));
    CHECK(throws_invalid([&] { device.upload_texture(device.create_texture(no_sampled), data); }));

    const TextureHandle ok = device.create_texture(sampled_texture(4, 4));
    (void)device.begin_frame();
    bool threw_logic = false;
    try {
        device.upload_texture(ok, data);
    } catch (const std::logic_error&) {
        threw_logic = true;
    }
    CHECK(threw_logic);
    (void)device.end_frame();
    device.upload_texture(ok, data);
    CHECK(device.uploaded_bytes(ok) == data.size());
}

TEST_CASE("NullDevice: samplers are validated, counted as live, and retire after their frames")
{
    NullDevice device;
    SamplerDesc bad;
    bad.max_anisotropy = 32.0f;
    CHECK(throws_invalid([&] { (void)device.create_sampler(bad); }));

    const SamplerHandle s = device.create_sampler({});
    CHECK(s.is_valid());
    CHECK(device.live_resource_count() == 1);
    device.destroy(s);
    CHECK(device.pending_destruction_count() == 1);
    CHECK(device.live_resource_count() == 1);  // still alive for in-flight frames
    device.wait_idle();
    CHECK(device.pending_destruction_count() == 0);
    CHECK(device.live_resource_count() == 0);

    device.destroy(s);  // a stale handle is a reported validation error, not a crash
    CHECK(device.validation_error_count() == 1);
}

namespace {

struct BindFixture {
    NullDevice device;
    BindGroupLayoutHandle layout;
    ShaderHandle vs, ps;
    PipelineHandle pipeline;
    TextureHandle texture;
    SamplerHandle sampler;
    BufferHandle uniform;
    TextureHandle target;

    BindFixture()
    {
        BindGroupLayoutDesc ld;
        ld.entries = {{0, BindingType::SampledTexture}, {1, BindingType::Sampler}, {2, BindingType::UniformBuffer}};
        layout = device.create_bind_group_layout(ld);
        vs = device.create_shader({ShaderStage::Vertex, fake_spirv, "main", nullptr});
        ps = device.create_shader({ShaderStage::Pixel, fake_spirv, "main", nullptr});
        GraphicsPipelineDesc pd;
        pd.vertex_shader = vs;
        pd.pixel_shader = ps;
        pd.color_formats = {Format::RGBA8_SRGB};
        pd.bind_group_layouts = {layout};
        pipeline = device.create_graphics_pipeline(pd);
        texture = device.create_texture(sampled_texture(4, 4));
        sampler = device.create_sampler({});
        BufferDesc bd;
        bd.size = 256;
        bd.usage = BufferUsage::Uniform;
        bd.memory = MemoryLocation::CpuToGpu;
        uniform = device.create_buffer(bd);
        target = device.create_texture(color_target());
    }

    std::vector<BindGroupEntry> entries() const
    {
        std::vector<BindGroupEntry> e(3);
        e[0].binding = 0;
        e[0].texture = texture;
        e[1].binding = 1;
        e[1].sampler = sampler;
        e[2].binding = 2;
        e[2].buffer = uniform;
        e[2].size = 16;
        return e;
    }

    /// Records one frame that binds the pipeline, then runs `inside` and draws.
    template <typename Fn>
    void frame(Fn&& inside)
    {
        CommandList& cmd = device.begin_frame();
        RenderingDesc rd;
        ColorAttachment ca;
        ca.texture = target;
        rd.colors.push_back(ca);
        rd.area = {0, 0, 64, 32};
        const TextureBarrier to_attachment{
            target, {}, {PipelineStage::ColorAttachmentOutput, Access::ColorAttachmentWrite, TextureLayout::ColorAttachment}, {}};
        cmd.barrier(std::span(&to_attachment, 1));
        cmd.begin_rendering(rd);
        cmd.bind_pipeline(pipeline);
        inside(cmd);
        cmd.draw(3);
        cmd.end_rendering();
        (void)device.end_frame();
    }
};

} // namespace

TEST_CASE("NullDevice: bind group layouts and groups are validated like the Vulkan backend")
{
    BindFixture f;
    BindGroupLayoutDesc dup;
    dup.entries = {{0, BindingType::Sampler}, {0, BindingType::SampledTexture}};
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group_layout(dup); }));
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group_layout({}); }));

    const BindGroupHandle ok = f.device.create_bind_group(f.layout, f.entries());
    CHECK(ok.is_valid());
    auto missing = f.entries();
    missing.pop_back();
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(f.layout, missing); }));
    auto no_sampled_usage = f.entries();
    no_sampled_usage[0].texture = f.target;
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(f.layout, no_sampled_usage); }));
    auto misaligned = f.entries();
    misaligned[2].offset = 16;
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(f.layout, misaligned); }));
    auto too_long = f.entries();
    too_long[2].size = 4096;
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(f.layout, too_long); }));
    auto dead_sampler = f.entries();
    dead_sampler[1].sampler = SamplerHandle{};
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(f.layout, dead_sampler); }));
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(BindGroupLayoutHandle{}, f.entries()); }));
    CHECK(f.device.validation_error_count() == 0);
}

TEST_CASE("NullDevice: drawing needs a compatible bind group at every set the pipeline declares")
{
    BindFixture f;
    const BindGroupHandle group = f.device.create_bind_group(f.layout, f.entries());

    f.frame([&](CommandList& cmd) { cmd.bind_group(0, group); });
    CHECK(f.device.validation_error_count() == 0);
    bool recorded = false;
    for (const RecordedCommand& c : f.device.last_frame_commands()) {
        recorded |= c.type == CommandType::BindGroup && c.group == group && c.count == 0;
    }
    CHECK(recorded);

    const uint32_t before = f.device.validation_error_count();
    f.frame([&](CommandList&) {});  // draw without binding
    CHECK(f.device.validation_error_count() == before + 1);

    // A group made from a different (but identical-looking) layout is not compatible.
    BindGroupLayoutDesc ld;
    ld.entries = {{0, BindingType::SampledTexture}, {1, BindingType::Sampler}, {2, BindingType::UniformBuffer}};
    const BindGroupLayoutHandle other_layout = f.device.create_bind_group_layout(ld);
    const BindGroupHandle other_group = f.device.create_bind_group(other_layout, f.entries());
    const uint32_t before_mismatch = f.device.validation_error_count();
    f.frame([&](CommandList& cmd) { cmd.bind_group(0, other_group); });
    CHECK(f.device.validation_error_count() > before_mismatch);

    // Binding to a set the pipeline does not declare, and to a dead group, are errors too.
    const uint32_t before_set = f.device.validation_error_count();
    f.frame([&](CommandList& cmd) {
        cmd.bind_group(0, group);
        cmd.bind_group(3, group);
    });
    CHECK(f.device.validation_error_count() == before_set + 1);
}

TEST_CASE("NullDevice: bind groups and layouts retire after their frames and count as live")
{
    BindFixture f;
    const uint32_t base = f.device.live_resource_count();
    const BindGroupHandle group = f.device.create_bind_group(f.layout, f.entries());
    CHECK(f.device.live_resource_count() == base + 1);
    f.device.destroy(group);
    CHECK(f.device.pending_destruction_count() >= 1);
    CHECK(f.device.live_resource_count() == base + 1);  // alive until in-flight frames retire
    f.device.wait_idle();
    CHECK(f.device.live_resource_count() == base);
    f.device.destroy(group);  // stale handle: reported, not fatal
    CHECK(f.device.validation_error_count() == 1);
}

TEST_CASE("NullDevice: pipelines reject invalid or too many bind group layouts")
{
    BindFixture f;
    GraphicsPipelineDesc pd;
    pd.vertex_shader = f.vs;
    pd.pixel_shader = f.ps;
    pd.color_formats = {Format::RGBA8_SRGB};
    pd.bind_group_layouts = {BindGroupLayoutHandle{}};
    CHECK(throws_invalid([&] { (void)f.device.create_graphics_pipeline(pd); }));
    pd.bind_group_layouts.assign(max_bind_groups_per_pipeline + 1, f.layout);
    CHECK(throws_invalid([&] { (void)f.device.create_graphics_pipeline(pd); }));
}

TEST_CASE("NullDevice: depth/stencil textures cannot be uploaded")
{
    NullDevice device;
    for (const Format format : {Format::D16_UNORM, Format::D24_UNORM_S8_UINT, Format::D32_FLOAT, Format::D32_FLOAT_S8_UINT}) {
        TextureDesc td;
        td.format = format;
        td.width = td.height = 4;
        td.usage = TextureUsage::Sampled | TextureUsage::TransferDst | TextureUsage::DepthStencilAttachment;
        const TextureHandle t = device.create_texture(td);
        const std::vector<std::byte> data(compute_texture_data_size(td));
        CHECK(throws_invalid([&] { device.upload_texture(t, data); }));
        CHECK(device.uploaded_bytes(t) == 0);
    }
}

TEST_CASE("NullDevice: each timestamp index may be written once per recording")
{
    NullDevice device;
    CommandList& cmd = device.begin_frame();
    cmd.write_timestamp(3);
    CHECK(device.validation_error_count() == 0);
    cmd.write_timestamp(3);  // a second write to the same query would reuse one that was not reset
    CHECK(device.validation_error_count() == 1);
    cmd.write_timestamp(4);  // other indices are unaffected
    CHECK(device.validation_error_count() == 1);
    (void)device.end_frame();

    CommandList& next = device.begin_frame();  // a new recording starts with every index writable again
    next.write_timestamp(3);
    next.write_timestamp(4);
    CHECK(device.validation_error_count() == 1);
    (void)device.end_frame();
}

TEST_CASE("GpuTimestamps: intervals are computed modulo the counter width, and invalid ones are unavailable")
{
    GpuTimestamps t;
    t.period_ns = 2.0;  // 500,000 ticks = 1 ms
    t.available_mask = 0b111;
    t.valid_bits = 64;
    t.ticks[0] = 1000;
    t.ticks[1] = 1000 + 500'000;
    CHECK(std::fabs(*t.milliseconds_between(0, 1) - 1.0) < 1e-9);
    CHECK(t.milliseconds_between(0, 0).has_value());  // an empty interval is a valid zero
    CHECK(*t.milliseconds_between(0, 0) == 0.0);

    // A 36-bit counter (a common timestampValidBits) that wrapped between the two writes: still 500,000 ticks.
    t.valid_bits = 36;
    const uint64_t mask36 = (uint64_t{1} << 36) - 1u;
    t.ticks[0] = mask36 - 99;
    t.ticks[1] = 499'900;  // (mask36 - 99) + 500,000 wraps to 499,900
    REQUIRE(t.milliseconds_between(0, 1).has_value());
    CHECK(std::fabs(*t.milliseconds_between(0, 1) - 1.0) < 1e-9);

    // The full 64-bit counter wrapping.
    t.valid_bits = 64;
    t.ticks[0] = ~uint64_t{0} - 249'999;
    t.ticks[1] = 250'000;
    REQUIRE(t.milliseconds_between(0, 1).has_value());
    CHECK(std::fabs(*t.milliseconds_between(0, 1) - 1.0) < 1e-9);

    // Unmasked high bits in the stored ticks cannot corrupt a narrower counter's interval.
    t.valid_bits = 36;
    t.ticks[0] = (uint64_t{0xABC} << 36) | 100;
    t.ticks[1] = (uint64_t{0x123} << 36) | 500'100;
    CHECK(std::fabs(*t.milliseconds_between(0, 1) - 1.0) < 1e-9);

    // Invalid requests stay unavailable rather than inventing a number.
    t.valid_bits = 64;
    t.ticks[0] = 10;
    t.ticks[1] = 20;
    CHECK_FALSE(t.milliseconds_between(1, 0).has_value());                       // reversed index order
    CHECK_FALSE(t.milliseconds_between(0, 3).has_value());                       // 3 was never written
    CHECK_FALSE(t.milliseconds_between(3, 3).has_value());
    CHECK_FALSE(t.milliseconds_between(0, max_gpu_timestamps).has_value());      // out of range
    CHECK_FALSE(t.milliseconds_between(max_gpu_timestamps, 1).has_value());
    t.valid_bits = 0;
    CHECK_FALSE(t.milliseconds_between(0, 1).has_value());                       // counter width unknown
    t.valid_bits = 65;
    CHECK_FALSE(t.milliseconds_between(0, 1).has_value());
    t.valid_bits = 64;
    t.period_ns = 0.0;
    CHECK_FALSE(t.milliseconds_between(0, 1).has_value());                       // period unknown
    t.period_ns = -1.0;
    CHECK_FALSE(t.milliseconds_between(0, 1).has_value());
}

TEST_CASE("NullDevice: timestamps are validated and unavailable; memory is estimated from descriptions")
{
    NullDevice device;
    CHECK_FALSE(device.info().gpu_timestamps);
    CHECK_FALSE(device.completed_timestamps().has_value());

    CommandList& cmd = device.begin_frame();
    cmd.write_timestamp(0);
    cmd.write_timestamp(max_gpu_timestamps - 1);
    CHECK(device.validation_error_count() == 0);
    cmd.write_timestamp(max_gpu_timestamps);  // out of range
    CHECK(device.validation_error_count() == 1);
    (void)device.end_frame();
    uint32_t timestamps_recorded = 0;
    for (const RecordedCommand& c : device.last_frame_commands()) {
        timestamps_recorded += c.type == CommandType::Timestamp ? 1u : 0u;
    }
    CHECK(timestamps_recorded == 3);
    CHECK_FALSE(device.completed_timestamps().has_value());  // a null device never invents GPU time

    const DeviceMemoryStats before = device.memory_stats();
    CHECK(before.allocated_bytes == 0);
    CHECK_FALSE(before.budget_available);
    BufferDesc bd;
    bd.size = 1000;
    bd.usage = BufferUsage::Vertex;
    const BufferHandle b = device.create_buffer(bd);
    const TextureHandle t = device.create_texture(sampled_texture(4, 4, 1));
    const DeviceMemoryStats during = device.memory_stats();
    CHECK(during.allocated_bytes == 1000 + 64);
    CHECK(during.allocation_count == 2);
    device.destroy(b);
    device.destroy(t);
    device.wait_idle();
    CHECK(device.memory_stats().allocated_bytes == 0);
}

TEST_CASE("NullDevice: per-binding buffer ranges follow the portable limits")
{
    BindFixture f;
    BufferDesc big;
    big.size = 32768;  // twice the portable uniform range
    big.usage = BufferUsage::Uniform;
    big.memory = MemoryLocation::CpuToGpu;
    const BufferHandle big_uniform = f.device.create_buffer(big);
    auto entries = f.entries();
    entries[2].buffer = big_uniform;
    entries[2].size = ~uint64_t{0};  // whole buffer: 32 KiB
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(f.layout, entries); }));
    entries[2].size = max_uniform_binding_range + 1;
    CHECK(throws_invalid([&] { (void)f.device.create_bind_group(f.layout, entries); }));
    entries[2].size = max_uniform_binding_range;
    CHECK(f.device.create_bind_group(f.layout, entries).is_valid());
}

TEST_CASE("NullDevice: switching pipelines clears the bound groups, rebinding restores them")
{
    BindFixture f;
    const BindGroupHandle group = f.device.create_bind_group(f.layout, f.entries());
    GraphicsPipelineDesc pd;
    pd.vertex_shader = f.vs;
    pd.pixel_shader = f.ps;
    pd.color_formats = {Format::RGBA8_SRGB};
    pd.push_constant_size = 16;  // same set layout handle, different push constant range
    pd.bind_group_layouts = {f.layout};
    const PipelineHandle other = f.device.create_graphics_pipeline(pd);

    f.frame([&](CommandList& cmd) { cmd.bind_group(0, group); });
    CHECK(f.device.validation_error_count() == 0);

    // Same handle again: the group is retained.
    f.frame([&](CommandList& cmd) {
        cmd.bind_group(0, group);
        cmd.bind_pipeline(f.pipeline);
    });
    CHECK(f.device.validation_error_count() == 0);

    // A different pipeline starts with nothing bound.
    f.frame([&](CommandList& cmd) {
        cmd.bind_group(0, group);
        cmd.bind_pipeline(other);
    });
    CHECK(f.device.validation_error_count() == 1);

    // Rebinding after the switch restores it, and switching back requires rebinding again.
    f.frame([&](CommandList& cmd) {
        cmd.bind_pipeline(other);
        cmd.bind_group(0, group);
    });
    CHECK(f.device.validation_error_count() == 1);
    f.frame([&](CommandList& cmd) {
        cmd.bind_pipeline(other);
        cmd.bind_group(0, group);
        cmd.bind_pipeline(f.pipeline);
    });
    CHECK(f.device.validation_error_count() == 2);
}

namespace {

std::vector<BindGroupLayoutDesc> null_layouts_at_the_limits()
{
    std::vector<BindGroupLayoutDesc> out(3);
    out[0].entries = {{0, BindingType::SampledTexture}, {1, BindingType::Sampler}, {2, BindingType::UniformBuffer}};
    for (uint32_t b = 3; b < 16; ++b) out[0].entries.push_back({b, BindingType::SampledTexture});
    for (uint32_t b = 0; b < 2; ++b) out[1].entries.push_back({b, BindingType::SampledTexture});
    for (uint32_t b = 2; b < 16; ++b) out[1].entries.push_back({b, BindingType::Sampler});
    out[2].entries.push_back({0, BindingType::Sampler});
    for (uint32_t b = 1; b < 12; ++b) out[2].entries.push_back({b, BindingType::UniformBuffer});
    for (uint32_t b = 12; b < 16; ++b) out[2].entries.push_back({b, BindingType::StorageBuffer});
    return out;
}

} // namespace

TEST_CASE("NullDevice: aggregate descriptor totals are limited per pipeline")
{
    BindFixture f;
    std::vector<BindGroupLayoutHandle> handles;
    for (const BindGroupLayoutDesc& d : null_layouts_at_the_limits()) {
        handles.push_back(f.device.create_bind_group_layout(d));
    }
    auto make = [&](const std::vector<BindGroupLayoutHandle>& layouts) {
        GraphicsPipelineDesc pd;
        pd.vertex_shader = f.vs;
        pd.pixel_shader = f.ps;
        pd.color_formats = {Format::RGBA8_SRGB};
        pd.bind_group_layouts = layouts;
        return f.device.create_graphics_pipeline(pd);
    };
    CHECK(make(handles).is_valid());  // exactly at every limit
    for (const BindingType extra : {BindingType::SampledTexture, BindingType::Sampler, BindingType::UniformBuffer,
                                    BindingType::StorageBuffer}) {
        BindGroupLayoutDesc one;
        one.entries = {{0, extra}};
        auto over = handles;
        over.push_back(f.device.create_bind_group_layout(one));
        CHECK(throws_invalid([&] { (void)make(over); }));
    }
}

TEST_CASE("NullDevice: binding a group whose resources were destroyed is an error")
{
    BindFixture f;
    for (int victim = 0; victim < 3; ++victim) {
        const TextureHandle tex = f.device.create_texture(sampled_texture(4, 4));
        const SamplerHandle sampler = f.device.create_sampler({});
        BufferDesc ub;
        ub.size = 256;
        ub.usage = BufferUsage::Uniform;
        ub.memory = MemoryLocation::CpuToGpu;
        const BufferHandle uniform = f.device.create_buffer(ub);
        auto entries = f.entries();
        entries[0].texture = tex;
        entries[1].sampler = sampler;
        entries[2].buffer = uniform;
        const BindGroupHandle group = f.device.create_bind_group(f.layout, entries);

        const uint32_t before = f.device.validation_error_count();
        // Destroying after binding, in the same frame, is fine: destruction is deferred.
        f.frame([&](CommandList& cmd) {
            cmd.bind_group(0, group);
            if (victim == 0) f.device.destroy(tex);
            if (victim == 1) f.device.destroy(sampler);
            if (victim == 2) f.device.destroy(uniform);
        });
        CHECK(f.device.validation_error_count() == before);
        f.device.wait_idle();  // the destroyed resource is now really gone

        // A later frame reusing the group is rejected, and nothing is bound for the draw either.
        f.frame([&](CommandList& cmd) { cmd.bind_group(0, group); });
        CHECK(f.device.validation_error_count() == before + 2);
    }
}

// ---------------------------------------------------------------------------------------------------- compute

namespace {

struct ComputeFixture {
    NullDevice device;
    ShaderHandle cs;
    BindGroupLayoutHandle layout;  // set 0: a storage buffer and a storage texture, both compute-visible
    PipelineHandle pipeline;
    BufferHandle storage, indirect;
    TextureHandle image;
    BindGroupHandle group;

    ComputeFixture()
    {
        cs = device.create_shader({ShaderStage::Compute, fake_spirv, "CSMain", "cs"});
        BindGroupLayoutDesc ld;
        BindGroupLayoutEntry buf, img;
        buf.binding = 0;
        buf.type = BindingType::StorageBuffer;
        buf.visibility = StageMask::Compute;
        img.binding = 1;
        img.type = BindingType::StorageTexture;
        img.visibility = StageMask::Compute;
        ld.entries = {buf, img};
        layout = device.create_bind_group_layout(ld);
        ComputePipelineDesc pd;
        pd.compute_shader = cs;
        pd.push_constant_size = 16;
        pd.bind_group_layouts = {layout};
        pipeline = device.create_compute_pipeline(pd);
        BufferDesc bd;
        bd.size = 4096;
        bd.usage = BufferUsage::Storage;
        storage = device.create_buffer(bd);
        BufferDesc id;
        id.size = 256;
        id.usage = BufferUsage::Indirect | BufferUsage::Storage;
        indirect = device.create_buffer(id);
        TextureDesc td;
        td.format = Format::RGBA8_UNORM;
        td.width = td.height = 16;
        td.usage = TextureUsage::Storage | TextureUsage::Sampled;
        image = device.create_texture(td);
        BindGroupEntry e[2];
        e[0].binding = 0;
        e[0].buffer = storage;
        e[0].size = 1024;
        e[1].binding = 1;
        e[1].texture = image;
        group = device.create_bind_group(layout, e);
    }

    template <typename Fn>
    uint32_t errors_from(Fn&& record)
    {
        const uint32_t before = device.validation_error_count();
        CommandList& cmd = device.begin_frame();
        record(cmd);
        (void)device.end_frame();
        return device.validation_error_count() - before;
    }
};

} // namespace

TEST_CASE("compute: the null device reports compute support and a valid pipeline, group and dispatch record cleanly")
{
    ComputeFixture f;
    CHECK(f.device.info().compute);
    const uint32_t errors = f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);
        const std::array<float, 4> pc{1, 2, 3, 4};
        cmd.push_constant_struct(pc);
        cmd.dispatch(8, 4, 1);
        cmd.dispatch(0, 5, 5);  // zero groups on one axis is legal and does nothing
        cmd.dispatch_indirect(f.indirect, 16);
    });
    CHECK(errors == 0);
    const auto& cmds = f.device.last_frame_commands();
    int dispatches = 0, indirects = 0;
    for (const RecordedCommand& c : cmds) {
        if (c.type == CommandType::Dispatch) {
            if (dispatches == 0) {
                CHECK(c.group_count[0] == 8);
                CHECK(c.group_count[1] == 4);
                CHECK(c.group_count[2] == 1);
            }
            CHECK(c.pipeline == f.pipeline);
            ++dispatches;
        }
        if (c.type == CommandType::DispatchIndirect) {
            CHECK(c.buffer == f.indirect);
            CHECK(c.buffer_offset == 16);
            ++indirects;
        }
    }
    CHECK(dispatches == 2);
    CHECK(indirects == 1);
}

TEST_CASE("compute: pipeline creation is validated")
{
    ComputeFixture f;
    const ShaderHandle vs = f.device.create_shader({ShaderStage::Vertex, fake_spirv, "VSMain", "vs"});
    ComputePipelineDesc base;
    base.compute_shader = f.cs;
    base.bind_group_layouts = {f.layout};

    ComputePipelineDesc d = base;
    d.compute_shader = vs;  // wrong stage
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(d); }));
    d = base;
    d.compute_shader = ShaderHandle{};
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(d); }));
    d = base;
    d.push_constant_size = 132;
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(d); }));
    d = base;
    d.push_constant_size = 6;
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(d); }));
    d = base;
    d.bind_group_layouts = {f.layout, f.layout, f.layout, f.layout, f.layout};
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(d); }));
    d = base;
    d.bind_group_layouts = {BindGroupLayoutHandle::from_raw(999)};
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(d); }));
    // Per-stage totals for the compute stage: five storage buffers visible to it are over the portable four.
    BindGroupLayoutDesc five;
    for (uint32_t i = 0; i < 5; ++i) {
        BindGroupLayoutEntry e;
        e.binding = i;
        e.type = BindingType::StorageBuffer;
        e.visibility = StageMask::Compute;
        five.entries.push_back(e);
    }
    d = base;
    d.bind_group_layouts = {f.device.create_bind_group_layout(five)};
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(d); }));
    // ...but the same layout in a graphics pipeline costs nothing, since its entries are not visible to the graphics stages.
    PipelineFixture g;
    BindGroupLayoutHandle compute_only = g.device.create_bind_group_layout(five);
    GraphicsPipelineDesc gd;
    gd.vertex_shader = g.vs;
    gd.pixel_shader = g.ps;
    gd.color_formats = {Format::RGBA8_SRGB};
    gd.bind_group_layouts = {compute_only};
    CHECK_FALSE(throws_invalid_argument([&] { (void)g.device.create_graphics_pipeline(gd); }));
    // A pipeline that is valid is created and can be destroyed.
    CHECK_FALSE(throws_invalid_argument([&] {
        const PipelineHandle ok = f.device.create_compute_pipeline(base);
        f.device.destroy(ok);
    }));
}

TEST_CASE("compute: dispatch misuse is reported as validation errors")
{
    ComputeFixture f;
    // Nothing bound / a graphics pipeline bound / outside a frame.
    CHECK(f.errors_from([&](CommandList& cmd) { cmd.dispatch(1, 1, 1); }) == 1);
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(f.pipeline);
        cmd.dispatch(1, 1, 1);  // no compatible group bound at set 0
    }) == 1);
    // Group counts over the portable limit.
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);
        cmd.dispatch(65536, 1, 1);
    }) == 1);
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);
        cmd.dispatch(65535, 65535, 65535);  // the limit itself is fine
    }) == 0);
    // Switching to the same pipeline keeps the group; a different pipeline clears it (the existing rule applies to compute too).
    // Inside a rendering scope.
    const TextureHandle rt = f.device.create_texture(color_target());
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.begin_rendering({{{rt, AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0, 0, 0, 1}}}, std::nullopt, {0, 0, 64, 32}});
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);
        cmd.dispatch(1, 1, 1);
        cmd.end_rendering();
    }) == 1);
    // A draw while a compute pipeline is bound.
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.begin_rendering({{{rt, AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0, 0, 0, 1}}}, std::nullopt, {0, 0, 64, 32}});
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);
        cmd.draw(3);
        cmd.end_rendering();
    }) == 1);
    // A dispatch while a graphics pipeline (of the same device) is bound: exactly the stage error, nothing else.
    const ShaderHandle vs = f.device.create_shader({ShaderStage::Vertex, fake_spirv, "VSMain", "vs"});
    const ShaderHandle ps = f.device.create_shader({ShaderStage::Pixel, fake_spirv, "PSMain", "ps"});
    GraphicsPipelineDesc gd;
    gd.vertex_shader = vs;
    gd.pixel_shader = ps;
    gd.color_formats = {Format::RGBA8_SRGB};
    const PipelineHandle graphics = f.device.create_graphics_pipeline(gd);
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(graphics);
        cmd.dispatch(1, 1, 1);
    }) == 1);
}

TEST_CASE("compute: indirect dispatch arguments are validated")
{
    ComputeFixture f;
    auto run = [&](BufferHandle b, uint64_t offset) {
        return f.errors_from([&](CommandList& cmd) {
            cmd.bind_pipeline(f.pipeline);
            cmd.bind_group(0, f.group);
            cmd.dispatch_indirect(b, offset);
        });
    };
    CHECK(run(f.indirect, 0) == 0);
    CHECK(run(f.indirect, 244) == 0);   // 244 + 12 == 256: the last record that fits
    CHECK(run(f.indirect, 248) == 1);   // runs past the end
    CHECK(run(f.indirect, 2) == 1);     // not a multiple of 4
    CHECK(run(f.indirect, 100000) == 1);
    CHECK(run(f.storage, 0) == 1);      // no Indirect usage
    CHECK(run(BufferHandle{}, 0) == 1); // invalid buffer
}

TEST_CASE("compute: storage textures bind and a destroyed one cannot be bound again")
{
    ComputeFixture f;
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);
        cmd.dispatch(2, 2, 1);
    }) == 0);
    f.device.destroy(f.image);
    f.device.wait_idle();
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);  // the group refers to a destroyed texture
        cmd.dispatch(1, 1, 1);
    }) >= 1);
    // A storage texture needs Storage usage, and its layout entry must be compute-only.
    BindGroupEntry e[2];
    e[0].binding = 0;
    e[0].buffer = f.storage;
    e[0].size = 1024;
    TextureDesc sampled;
    sampled.format = Format::RGBA8_UNORM;
    sampled.width = sampled.height = 8;
    sampled.usage = TextureUsage::Sampled;
    e[1].binding = 1;
    e[1].texture = f.device.create_texture(sampled);
    CHECK(throws_invalid_argument([&] { (void)f.device.create_bind_group(f.layout, e); }));
    BindGroupLayoutDesc bad;
    BindGroupLayoutEntry be;
    be.binding = 0;
    be.type = BindingType::StorageTexture;  // default (graphics) visibility
    bad.entries = {be};
    CHECK(throws_invalid_argument([&] { (void)f.device.create_bind_group_layout(bad); }));
}

TEST_CASE("compute: a device without compute refuses every compute use")
{
    ComputeFixture f;
    f.device.deny_compute();
    CHECK_FALSE(f.device.info().compute);
    ComputePipelineDesc pd;
    pd.compute_shader = f.cs;
    CHECK(throws_invalid_argument([&] { (void)f.device.create_compute_pipeline(pd); }));
    BindGroupLayoutDesc ld;
    BindGroupLayoutEntry e;
    e.binding = 0;
    e.type = BindingType::UniformBuffer;
    e.visibility = StageMask::Compute;
    ld.entries = {e};
    CHECK(throws_invalid_argument([&] { (void)f.device.create_bind_group_layout(ld); }));
    CHECK(f.errors_from([&](CommandList& cmd) {
        cmd.bind_pipeline(f.pipeline);
        cmd.bind_group(0, f.group);
        cmd.dispatch(1, 1, 1);
    }) >= 1);
    // The graphics path is unaffected.
    PipelineFixture g;
    g.device.deny_compute();
    BindGroupLayoutDesc gl;
    BindGroupLayoutEntry ge;
    ge.binding = 0;
    ge.type = BindingType::UniformBuffer;
    gl.entries = {ge};
    CHECK_FALSE(throws_invalid_argument([&] { (void)g.device.create_bind_group_layout(gl); }));
    f.device.deny_compute(false);
    CHECK(f.device.info().compute);
}

// ------------------------------------------------------------------------------------------- indirect draws

namespace {

struct IndirectFixture {
    PipelineFixture f;  // a graphics pipeline without vertex input, push constants of 16 bytes
    TextureHandle rt;
    BufferHandle args, small_args, no_indirect, count, index, tiny;

    IndirectFixture()
    {
        rt = f.device.create_texture(color_target());
        BufferDesc bd;
        bd.size = 4096;
        bd.usage = BufferUsage::Indirect | BufferUsage::Storage;
        bd.memory = MemoryLocation::GpuOnly;
        args = f.device.create_buffer(bd);
        bd.size = 40;  // room for two indexed records (20 bytes each), nothing more
        small_args = f.device.create_buffer(bd);
        bd.size = 8;   // smaller than any argument record
        tiny = f.device.create_buffer(bd);
        bd.size = 4096;
        bd.usage = BufferUsage::Storage;
        no_indirect = f.device.create_buffer(bd);
        bd.size = 16;
        bd.usage = BufferUsage::Indirect | BufferUsage::Storage;
        count = f.device.create_buffer(bd);
        bd.size = 64;
        bd.usage = BufferUsage::Index;
        bd.memory = MemoryLocation::CpuToGpu;
        index = f.device.create_buffer(bd);
    }

    /// Errors reported while recording `record` inside a rendering scope with the graphics pipeline (and the index buffer, if asked for) bound.
    template <typename Fn>
    uint32_t errors(Fn&& record, bool bind_index = true)
    {
        const uint32_t before = f.device.validation_error_count();
        CommandList& cmd = f.device.begin_frame();
        cmd.begin_rendering({{{rt, AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0, 0, 0, 1}}}, std::nullopt, {0, 0, 64, 32}});
        cmd.bind_pipeline(f.pipeline);
        if (bind_index) cmd.bind_index_buffer(index, IndexType::Uint16);
        record(cmd);
        cmd.end_rendering();
        (void)f.device.end_frame();
        return f.device.validation_error_count() - before;
    }
};

} // namespace

TEST_CASE("indirect draws: the null device reports the capabilities, and valid draws record their arguments")
{
    IndirectFixture x;
    const DeviceInfo& info = x.f.device.info();
    CHECK(info.multi_draw_indirect);
    CHECK(info.draw_indirect_first_instance);
    CHECK(info.draw_indirect_count);
    CHECK(x.errors([&](CommandList& cmd) {
        cmd.draw_indirect(x.args, 0, 1, 0);        // one draw: the stride is ignored
        cmd.draw_indirect(x.args, 16, 3, 16);
        cmd.draw_indexed_indirect(x.args, 64, 4, 20);
        cmd.draw_indexed_indirect_count(x.args, 0, x.count, 8, 10, 20);
        cmd.draw_indirect(x.tiny, 0, 0, 0);  // zero draws: nothing is read, so nothing is checked against the buffer
    }) == 0);
    int seen = 0;
    for (const RecordedCommand& c : x.f.device.last_frame_commands()) {
        if (c.type == CommandType::DrawIndirect && c.count == 3) {
            CHECK(c.buffer == x.args);
            CHECK(c.buffer_offset == 16);
            CHECK(c.stride == 16);
            ++seen;
        }
        if (c.type == CommandType::DrawIndexedIndirect) {
            CHECK(c.count == 4);
            CHECK(c.buffer_offset == 64);
            CHECK(c.stride == 20);
            ++seen;
        }
        if (c.type == CommandType::DrawIndexedIndirectCount) {
            CHECK(c.count == 10);  // max draws
            CHECK(c.count_buffer == x.count);
            CHECK(c.count_offset == 8);
            ++seen;
        }
    }
    CHECK(seen == 3);
}

TEST_CASE("indirect draws: the argument range, usage, alignment and stride are validated")
{
    IndirectFixture x;
    auto one = [&](auto&& call) { return x.errors(call); };
    CHECK(one([&](CommandList& c) { c.draw_indirect(BufferHandle{}, 0, 1, 0); }) == 1);          // invalid buffer
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.no_indirect, 0, 1, 0); }) == 1);           // no Indirect usage
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.args, 2, 1, 0); }) == 1);                  // misaligned offset
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.args, 4088, 1, 0); }) == 1);               // 4088 + 16 > 4096
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.args, 4080, 1, 0); }) == 0);               // exactly fits
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.args, 0, 2, 12); }) == 1);                 // stride below the record size
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.args, 0, 2, 18); }) == 1);                 // stride not a multiple of 4
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.args, 0, 256, 16); }) == 0);               // 256 * 16 == 4096
    CHECK(one([&](CommandList& c) { c.draw_indirect(x.args, 0, 257, 16); }) == 1);               // one past the end
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect(x.small_args, 0, 2, 20); }) == 0);   // two indexed records fill 40 bytes
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect(x.small_args, 0, 3, 20); }) == 1);
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect(x.small_args, 0, 2, 16); }) == 1);   // an indexed record is 20 bytes, not 16
    // The count variant: its own buffer rules.
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, BufferHandle{}, 0, 4, 20); }) == 1);
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, x.no_indirect, 0, 4, 20); }) == 1);
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, x.count, 2, 4, 20); }) == 1);
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, x.count, 16, 4, 20); }) == 1);  // 16 + 4 > 16
    CHECK(one([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, x.count, 12, 4, 20); }) == 0);
}

TEST_CASE("indirect draws: state rules (rendering scope, pipeline kind, index buffer) are validated")
{
    IndirectFixture x;
    // Outside a rendering scope.
    {
        const uint32_t before = x.f.device.validation_error_count();
        CommandList& cmd = x.f.device.begin_frame();
        cmd.bind_pipeline(x.f.pipeline);
        cmd.draw_indirect(x.args, 0, 1, 0);
        (void)x.f.device.end_frame();
        CHECK(x.f.device.validation_error_count() == before + 1);
    }
    // Indexed draws need an index buffer.
    CHECK(x.errors([&](CommandList& c) { c.draw_indexed_indirect(x.args, 0, 1, 0); }, false) == 1);
    CHECK(x.errors([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, x.count, 0, 1, 20); }, false) == 1);
    // A compute pipeline bound.
    const ShaderHandle cs = x.f.device.create_shader({ShaderStage::Compute, fake_spirv, "CSMain", "cs"});
    ComputePipelineDesc cd;
    cd.compute_shader = cs;
    const PipelineHandle compute = x.f.device.create_compute_pipeline(cd);
    const uint32_t before = x.f.device.validation_error_count();
    CommandList& cmd = x.f.device.begin_frame();
    cmd.begin_rendering({{{x.rt, AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0, 0, 0, 1}}}, std::nullopt, {0, 0, 64, 32}});
    cmd.bind_pipeline(compute);
    cmd.draw_indirect(x.args, 0, 1, 0);
    cmd.end_rendering();
    (void)x.f.device.end_frame();
    CHECK(x.f.device.validation_error_count() == before + 1);
}

TEST_CASE("indirect draws: multi-draw and count draws are refused on a device without them, single draws are not")
{
    IndirectFixture x;
    x.f.device.set_indirect_draw_support(false, true, false);
    CHECK_FALSE(x.f.device.info().multi_draw_indirect);
    CHECK(x.f.device.info().draw_indirect_first_instance);
    CHECK_FALSE(x.f.device.info().draw_indirect_count);
    CHECK(x.errors([&](CommandList& c) { c.draw_indirect(x.args, 0, 1, 0); }) == 0);
    CHECK(x.errors([&](CommandList& c) { c.draw_indexed_indirect(x.args, 0, 1, 0); }) == 0);
    CHECK(x.errors([&](CommandList& c) { c.draw_indirect(x.args, 0, 2, 16); }) == 1);
    CHECK(x.errors([&](CommandList& c) { c.draw_indexed_indirect(x.args, 0, 2, 20); }) == 1);
    CHECK(x.errors([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, x.count, 0, 1, 20); }) == 1);
    x.f.device.set_indirect_draw_support(true, true, true);
    CHECK(x.errors([&](CommandList& c) { c.draw_indirect(x.args, 0, 2, 16); }) == 0);
    CHECK(x.errors([&](CommandList& c) { c.draw_indexed_indirect_count(x.args, 0, x.count, 0, 1, 20); }) == 0);
}
