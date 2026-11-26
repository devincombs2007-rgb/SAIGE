// Compute on a real (validated) Vulkan device. CI: lavapipe. Each test compares what the GPU wrote with plain C++, so a wrong dispatch size, a missing barrier,
// a wrong bind point or a wrong descriptor type fails the comparison, and a validation error fails the test. Correctness only: nothing here is a performance
// measurement, and lavapipe is a CPU implementation.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaders/test_compute_probe.spv.hpp"
#include "saige/shaders/test_depth_only.spv.hpp"
#include "vulkan_test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;

namespace {

template <size_t N>
std::span<const uint32_t> words(const uint32_t (&array)[N])
{
    return {array, N};
}

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

uint32_t expected_fill(uint32_t i, uint32_t seed) { return (i * 2654435761u) ^ seed; }

/// Destroys everything it was given, newest first, when it goes out of scope (so no resource outlives the device).
class Cleanup {
public:
    explicit Cleanup(rhi::Device& device) : device_(device) {}
    ~Cleanup()
    {
        device_.wait_idle();
        for (auto it = fns_.rbegin(); it != fns_.rend(); ++it) (*it)();
    }
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;
    template <typename Handle>
    Handle track(Handle h)
    {
        fns_.push_back([this, h] { device_.destroy(h); });
        return h;
    }

private:
    rhi::Device& device_;
    std::vector<std::function<void()>> fns_;
};

/// A host-visible buffer (so the test can read results back directly), filled with `fill`. Not for indirect arguments: readback buffers cannot be indirect.
rhi::BufferHandle make_buffer(Cleanup& own, rhi::Device& device, uint64_t size, rhi::BufferUsage usage, const char* name, uint32_t fill = 0u)
{
    rhi::BufferDesc bd;
    bd.size = size;
    bd.usage = usage;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    bd.debug_name = name;
    std::vector<uint32_t> initial(size / 4, fill);
    return own.track(device.create_buffer(bd, std::as_bytes(std::span<const uint32_t>(initial))));
}

/// A device-local buffer that only the GPU touches (the arguments of an indirect dispatch, written by another dispatch).
rhi::BufferHandle make_gpu_buffer(Cleanup& own, rhi::Device& device, uint64_t size, rhi::BufferUsage usage, const char* name)
{
    rhi::BufferDesc bd;
    bd.size = size;
    bd.usage = usage;
    bd.memory = rhi::MemoryLocation::GpuOnly;
    bd.debug_name = name;
    return own.track(device.create_buffer(bd));
}

rhi::BindGroupLayoutHandle storage_buffer_layout(Cleanup& own, rhi::Device& device, uint32_t count)
{
    rhi::BindGroupLayoutDesc ld;
    for (uint32_t i = 0; i < count; ++i) {
        rhi::BindGroupLayoutEntry e;
        e.binding = i;
        e.type = rhi::BindingType::StorageBuffer;
        e.visibility = rhi::StageMask::Compute;
        ld.entries.push_back(e);
    }
    return own.track(device.create_bind_group_layout(ld));
}

rhi::BindGroupHandle buffer_group(Cleanup& own, rhi::Device& device, rhi::BindGroupLayoutHandle layout, std::initializer_list<rhi::BufferHandle> buffers)
{
    std::vector<rhi::BindGroupEntry> entries;
    uint32_t binding = 0;
    for (const rhi::BufferHandle b : buffers) {
        rhi::BindGroupEntry e;
        e.binding = binding++;
        e.buffer = b;
        entries.push_back(e);
    }
    return own.track(device.create_bind_group(layout, entries));
}

/// Makes writes of a compute pass visible to the host (read after wait_idle).
rhi::BufferBarrier to_host(rhi::BufferHandle b)
{
    rhi::BufferBarrier bb;
    bb.buffer = b;
    bb.before = {rhi::PipelineStage::ComputeShader, rhi::Access::ShaderStorageWrite, rhi::TextureLayout::Undefined};
    bb.after = {rhi::PipelineStage::Host, rhi::Access::HostRead, rhi::TextureLayout::Undefined};
    return bb;
}

} // namespace

TEST_CASE("Vulkan compute: the device reports compute and a dispatch fills a storage buffer with the CPU-computed pattern")
{
    const auto device = make_validated_device("compute_fill", true);
    CHECK(device->info().compute);
    constexpr uint32_t count = 1000, seed = 0xBEEFu;  // not a multiple of the workgroup size: the tail workgroup's guard is exercised
    constexpr uint32_t sentinel = 0xFFFFFFFFu;
    {
        Cleanup own(*device);
        const rhi::ShaderHandle cs = own.track(device->create_shader({rhi::ShaderStage::Compute, words(saige::shaders::test_compute_probe_fill_main), "FillMain", "Fill"}));
        const rhi::BindGroupLayoutHandle layout = storage_buffer_layout(own, *device, 1);
        rhi::ComputePipelineDesc pd;
        pd.compute_shader = cs;
        pd.push_constant_size = 8;
        pd.bind_group_layouts = {layout};
        pd.debug_name = "Fill";
        const rhi::PipelineHandle pipeline = own.track(device->create_compute_pipeline(pd));
        const rhi::BufferHandle data = make_buffer(own, *device, 2048 * 4, rhi::BufferUsage::Storage, "FillData", sentinel);
        const rhi::BindGroupHandle group = buffer_group(own, *device, layout, {data});

        rhi::CommandList& cmd = device->begin_frame();
        cmd.bind_pipeline(pipeline);
        cmd.bind_group(0, group);
        const std::array<uint32_t, 2> pc{count, seed};
        cmd.push_constant_struct(pc);
        cmd.dispatch((count + 63) / 64, 1, 1);
        const rhi::BufferBarrier barrier = to_host(data);
        cmd.barrier({}, std::span(&barrier, 1));
        (void)device->end_frame();
        device->wait_idle();

        const std::span<std::byte> mapped = device->map(data);
        std::vector<uint32_t> out(2048);
        std::memcpy(out.data(), mapped.data(), out.size() * 4);
        uint32_t wrong = 0, untouched_ok = 0;
        for (uint32_t i = 0; i < count; ++i) wrong += out[i] != expected_fill(i, seed) ? 1u : 0u;
        for (uint32_t i = count; i < 2048; ++i) untouched_ok += out[i] == sentinel ? 1u : 0u;
        CHECK(wrong == 0);
        CHECK(untouched_ok == 2048 - count);  // nothing past `count` was written: the dispatch size and the shader guard are both right
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan compute: a dispatch whose size the GPU wrote runs exactly that many workgroups")
{
    const auto device = make_validated_device("compute_indirect", true);
    constexpr uint32_t sentinel = 0xFFFFFFFFu;
    {
        Cleanup own(*device);
        const rhi::ShaderHandle args_cs = own.track(device->create_shader({rhi::ShaderStage::Compute, words(saige::shaders::test_compute_probe_args_main), "ArgsMain", "Args"}));
        const rhi::ShaderHandle fill_cs = own.track(device->create_shader({rhi::ShaderStage::Compute, words(saige::shaders::test_compute_probe_fill_main), "FillMain", "Fill"}));
        const rhi::BindGroupLayoutHandle two = storage_buffer_layout(own, *device, 2);
        const rhi::BindGroupLayoutHandle one = storage_buffer_layout(own, *device, 1);
        rhi::ComputePipelineDesc ad;
        ad.compute_shader = args_cs;
        ad.bind_group_layouts = {two};
        const rhi::PipelineHandle args_pipeline = own.track(device->create_compute_pipeline(ad));
        rhi::ComputePipelineDesc fd;
        fd.compute_shader = fill_cs;
        fd.push_constant_size = 8;
        fd.bind_group_layouts = {one};
        const rhi::PipelineHandle fill_pipeline = own.track(device->create_compute_pipeline(fd));

        constexpr uint32_t wanted = 300;  // 300 elements need ceil(300 / 64) = 5 workgroups = 320 invocations
        std::array<uint32_t, 64> counts_init{};
        counts_init[0] = wanted;
        rhi::BufferDesc cd;
        cd.size = 256;
        cd.usage = rhi::BufferUsage::Storage;
        cd.memory = rhi::MemoryLocation::GpuToCpu;
        const rhi::BufferHandle counts = own.track(device->create_buffer(cd, std::as_bytes(std::span<const uint32_t>(counts_init))));
        const rhi::BufferHandle args = make_gpu_buffer(own, *device, 256, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect, "Args");
        const rhi::BufferHandle data = make_buffer(own, *device, 1024 * 4, rhi::BufferUsage::Storage, "Data", sentinel);
        const rhi::BindGroupHandle args_group = buffer_group(own, *device, two, {counts, args});
        const rhi::BindGroupHandle fill_group = buffer_group(own, *device, one, {data});

        rhi::CommandList& cmd = device->begin_frame();
        cmd.bind_pipeline(args_pipeline);
        cmd.bind_group(0, args_group);
        cmd.dispatch(1, 1, 1);
        rhi::BufferBarrier to_indirect;
        to_indirect.buffer = args;
        to_indirect.before = {rhi::PipelineStage::ComputeShader, rhi::Access::ShaderStorageWrite, rhi::TextureLayout::Undefined};
        to_indirect.after = {rhi::PipelineStage::DrawIndirect, rhi::Access::IndirectCommandRead, rhi::TextureLayout::Undefined};
        cmd.barrier({}, std::span(&to_indirect, 1));
        cmd.bind_pipeline(fill_pipeline);
        cmd.bind_group(0, fill_group);
        // The fill shader guards with its own count; give it the full capacity so only the number of workgroups limits what is written.
        const std::array<uint32_t, 2> pc{1024u, 7u};
        cmd.push_constant_struct(pc);
        cmd.dispatch_indirect(args, 0);
        const rhi::BufferBarrier barrier = to_host(data);
        cmd.barrier({}, std::span(&barrier, 1));
        (void)device->end_frame();
        device->wait_idle();

        const std::span<std::byte> mapped = device->map(data);
        std::vector<uint32_t> out(1024);
        std::memcpy(out.data(), mapped.data(), out.size() * 4);
        const uint32_t groups = (wanted + 63) / 64;
        uint32_t wrong = 0, past_end = 0;
        for (uint32_t i = 0; i < groups * 64; ++i) wrong += out[i] != expected_fill(i, 7u) ? 1u : 0u;
        for (uint32_t i = groups * 64; i < 1024; ++i) past_end += out[i] != sentinel ? 1u : 0u;
        CHECK(wrong == 0);     // every invocation of the GPU-chosen number of workgroups ran
        CHECK(past_end == 0);  // and no more than that
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan compute: a compute shader writes a storage image that reads back as computed")
{
    const auto device = make_validated_device("compute_image", true);
    {
        Cleanup own(*device);
        const rhi::ShaderHandle cs = own.track(device->create_shader({rhi::ShaderStage::Compute, words(saige::shaders::test_compute_probe_image_main), "ImageMain", "Image"}));
        rhi::BindGroupLayoutDesc ld;
        rhi::BindGroupLayoutEntry e;
        e.binding = 0;
        e.type = rhi::BindingType::StorageTexture;
        e.visibility = rhi::StageMask::Compute;
        ld.entries = {e};
        const rhi::BindGroupLayoutHandle layout = own.track(device->create_bind_group_layout(ld));
        rhi::ComputePipelineDesc pd;
        pd.compute_shader = cs;
        pd.bind_group_layouts = {layout};
        const rhi::PipelineHandle pipeline = own.track(device->create_compute_pipeline(pd));

        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA32_FLOAT;
        td.width = td.height = 16;
        td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSrc;
        td.debug_name = "StorageImage";
        const rhi::TextureHandle image = own.track(device->create_texture(td));
        rhi::BufferDesc bd;
        bd.size = 16u * 16u * 16u;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        const rhi::BufferHandle readback = own.track(device->create_buffer(bd));
        rhi::BindGroupEntry ge;
        ge.binding = 0;
        ge.texture = image;
        const rhi::BindGroupHandle group = own.track(device->create_bind_group(layout, std::span(&ge, 1)));

        rhi::CommandList& cmd = device->begin_frame();
        const rhi::TextureBarrier to_general{image, {}, {rhi::PipelineStage::ComputeShader, rhi::Access::ShaderStorageWrite, rhi::TextureLayout::General}, {}};
        cmd.barrier(std::span(&to_general, 1));
        cmd.bind_pipeline(pipeline);
        cmd.bind_group(0, group);
        cmd.dispatch(2, 2, 1);  // 2 x 2 workgroups of 8 x 8 cover the 16 x 16 image
        const rhi::TextureBarrier to_transfer{image, to_general.after, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(image, readback);
        (void)device->end_frame();
        device->wait_idle();

        const std::span<std::byte> mapped = device->map(readback);
        std::vector<float> px(16u * 16u * 4u);
        std::memcpy(px.data(), mapped.data(), px.size() * sizeof(float));
        uint32_t wrong = 0;
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                const float* p = &px[(y * 16 + x) * 4];
                const float ex[4] = {static_cast<float>(x) / 15.0f, static_cast<float>(y) / 15.0f, 0.5f, 1.0f};
                for (int c = 0; c < 4; ++c) wrong += std::abs(p[c] - ex[c]) > 1e-6f ? 1u : 0u;
            }
        }
        CHECK(wrong == 0);
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan compute: misuse is rejected before a Vulkan call is made")
{
    const auto device = make_validated_device("compute_misuse");
    {
        Cleanup own(*device);
        const rhi::ShaderHandle cs = own.track(device->create_shader({rhi::ShaderStage::Compute, words(saige::shaders::test_compute_probe_fill_main), "FillMain", "Fill"}));
        const rhi::ShaderHandle vs = own.track(device->create_shader({rhi::ShaderStage::Vertex, words(saige::shaders::test_compute_probe_fill_main), "FillMain", "NotVertex"}));
        const rhi::BindGroupLayoutHandle layout = storage_buffer_layout(own, *device, 1);
        rhi::ComputePipelineDesc pd;
        pd.compute_shader = cs;
        pd.push_constant_size = 8;
        pd.bind_group_layouts = {layout};
        const rhi::PipelineHandle pipeline = own.track(device->create_compute_pipeline(pd));
        rhi::ComputePipelineDesc bad = pd;
        bad.compute_shader = vs;
        CHECK(throws_invalid_argument([&] { (void)device->create_compute_pipeline(bad); }));
        bad = pd;
        bad.push_constant_size = 6;
        CHECK(throws_invalid_argument([&] { (void)device->create_compute_pipeline(bad); }));
        bad = pd;
        bad.bind_group_layouts = {rhi::BindGroupLayoutHandle::from_raw(999)};
        CHECK(throws_invalid_argument([&] { (void)device->create_compute_pipeline(bad); }));

        const rhi::BufferHandle data = make_buffer(own, *device, 4096, rhi::BufferUsage::Storage, "Data");
        const rhi::BufferHandle indirect = make_gpu_buffer(own, *device, 256, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect, "Args");
        const rhi::BindGroupHandle group = buffer_group(own, *device, layout, {data});
        rhi::CommandList& cmd = device->begin_frame();
        CHECK(throws_invalid_argument([&] { cmd.dispatch(1, 1, 1); }));  // no pipeline
        cmd.bind_pipeline(pipeline);
        CHECK(throws_invalid_argument([&] { cmd.dispatch(1, 1, 1); }));  // no group at set 0
        cmd.bind_group(0, group);
        CHECK(throws_invalid_argument([&] { cmd.dispatch(65536, 1, 1); }));
        CHECK(throws_invalid_argument([&] { cmd.dispatch_indirect(data, 0); }));       // no Indirect usage
        CHECK(throws_invalid_argument([&] { cmd.dispatch_indirect(indirect, 2); }));   // misaligned
        CHECK(throws_invalid_argument([&] { cmd.dispatch_indirect(indirect, 248); }));  // does not fit
        CHECK(throws_invalid_argument([&] { cmd.draw(3); }));  // a compute pipeline is bound
        const std::array<uint32_t, 2> pc{1u, 0u};
        cmd.push_constant_struct(pc);  // the shader reads its push constants
        cmd.dispatch(1, 1, 1);         // a valid dispatch still works after the rejected ones

        // A dispatch inside a rendering scope is rejected (compute cannot run inside a render pass), and the scope can still be ended normally.
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = td.height = 8;
        td.usage = rhi::TextureUsage::ColorAttachment;
        const rhi::TextureHandle target = own.track(device->create_texture(td));
        const rhi::TextureBarrier to_attachment{target, {}, {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment}, {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target;
        rd.colors.push_back(ca);
        rd.area = {0, 0, 8, 8};
        cmd.begin_rendering(rd);
        CHECK(throws_invalid_argument([&] { cmd.dispatch(1, 1, 1); }));
        cmd.end_rendering();
        cmd.dispatch(1, 1, 1);  // and compute works again once the scope has ended

        // A dispatch with a graphics pipeline bound is rejected, and so is a draw with a compute pipeline bound (checked above).
        const rhi::ShaderHandle depth_vs = own.track(device->create_shader({rhi::ShaderStage::Vertex, words(saige::shaders::test_depth_only_vs_main), "VSMain", "DepthVS"}));
        rhi::GraphicsPipelineDesc gd;
        gd.vertex_shader = depth_vs;  // no pixel shader: a depth-only pipeline
        gd.depth.test = true;
        gd.depth.write = true;
        gd.depth_format = rhi::Format::D32_FLOAT;
        gd.push_constant_size = 64;
        gd.vertex_layout.stride = 12;
        gd.vertex_layout.attributes = {{0, rhi::Format::RGB32_FLOAT, 0}};
        const rhi::PipelineHandle graphics = own.track(device->create_graphics_pipeline(gd));
        cmd.bind_pipeline(graphics);
        CHECK(throws_invalid_argument([&] { cmd.dispatch(1, 1, 1); }));
        (void)device->end_frame();
        device->wait_idle();
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}
