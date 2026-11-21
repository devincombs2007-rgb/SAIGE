#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi_null/null_device.hpp"

#include <array>
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
