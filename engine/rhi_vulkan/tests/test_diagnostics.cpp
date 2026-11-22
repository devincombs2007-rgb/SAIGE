// GPU timing and memory diagnostics on a real (validated) Vulkan device. Timestamps are either measured or reported
// unavailable, never invented; memory accounting is exact and must balance. CI: lavapipe.

#include "saige/gfx_test/saige_test.hpp"

#include "vulkan_test_util.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;

namespace {

rhi::TextureDesc big_texture()
{
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = td.height = 256;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "DiagTexture";
    return td;
}

/// Runs an empty frame so the device collects garbage and finished frames' results.
void idle_frame(rhi::Device& device)
{
    device.wait_idle();
    (void)device.begin_frame();
    (void)device.end_frame();
    device.wait_idle();
}

} // namespace

TEST_CASE("Device memory: backend allocations are counted exactly and balance after destruction")
{
    const auto device = make_validated_device("diag_memory");
    idle_frame(*device);
    const rhi::DeviceMemoryStats base = device->memory_stats();

    // A texture with data: the staging buffer used by the upload must not stay counted.
    const rhi::TextureHandle tex = device->create_texture(big_texture());
    device->upload_texture(tex, std::vector<std::byte>(256 * 256 * 4));
    const rhi::DeviceMemoryStats with_texture = device->memory_stats();
    CHECK(with_texture.allocated_bytes >= base.allocated_bytes + 256 * 256 * 4);
    CHECK(with_texture.allocation_count == base.allocation_count + 1);  // the texture only: staging was freed

    rhi::BufferDesc bd;
    bd.size = 1u << 20;
    bd.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    const rhi::BufferHandle buffer = device->create_buffer(bd, std::vector<std::byte>(bd.size));
    const rhi::DeviceMemoryStats with_both = device->memory_stats();
    CHECK(with_both.allocated_bytes >= with_texture.allocated_bytes + (1u << 20));
    CHECK(with_both.allocation_count == base.allocation_count + 2);

    device->destroy(tex);
    device->destroy(buffer);
    // Destruction is deferred until the GPU is done, so the bytes are still counted until retirement...
    CHECK(device->memory_stats().allocated_bytes == with_both.allocated_bytes);
    idle_frame(*device);
    // ...and then they all go: exactly back to the baseline.
    const rhi::DeviceMemoryStats after = device->memory_stats();
    CHECK(after.allocated_bytes == base.allocated_bytes);
    CHECK(after.allocation_count == base.allocation_count);
    if (after.budget_available) {  // reported only where the driver provides it, and then it must be sane
        CHECK(after.budget_bytes > 0);
        CHECK(after.usage_bytes <= after.budget_bytes);
    }
    std::printf("  memory budget %s\n", after.budget_available ? "reported by the driver" : "not reported (unavailable)");
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("GPU timestamps: measured where supported, absent where not, and written ones are ordered")
{
    const auto device = make_validated_device("diag_timestamps");
    const rhi::DeviceInfo& info = device->info();
    std::printf("  adapter '%s', timestamps %s (period %.3f ns)\n", info.device_name.c_str(),
                info.gpu_timestamps ? "supported" : "unavailable", info.timestamp_period_ns);

    CHECK_FALSE(device->completed_timestamps().has_value());  // nothing has completed yet

    // Record frames that bracket a texture-to-buffer copy with timestamps 0 and 1; timestamp 2 is never written.
    const rhi::TextureHandle tex = device->create_texture([] {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = td.height = 512;
        td.usage = rhi::TextureUsage::TransferSrc | rhi::TextureUsage::TransferDst | rhi::TextureUsage::Sampled;
        return td;
    }());
    device->upload_texture(tex, std::vector<std::byte>(512 * 512 * 4));
    rhi::BufferDesc bd;
    bd.size = 512 * 512 * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    std::vector<rhi::BufferHandle> readbacks;
    std::vector<uint64_t> recorded_frames;
    for (int i = 0; i < 8; ++i) {
        readbacks.push_back(device->create_buffer(bd));
        recorded_frames.push_back(device->frame_number());
        rhi::CommandList& cmd = device->begin_frame();
        cmd.write_timestamp(0);
        const rhi::TextureBarrier to_src{tex, rhi::sampled_texture_state,
                                         {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_src, 1));
        cmd.copy_texture_to_buffer(tex, readbacks.back());
        const rhi::TextureBarrier back{tex, to_src.after, rhi::sampled_texture_state, {}};
        cmd.barrier(std::span(&back, 1));
        cmd.write_timestamp(1);
        (void)device->end_frame();
    }
    bool threw = false;
    rhi::CommandList& bad_cmd = device->begin_frame();
    try {
        bad_cmd.write_timestamp(rhi::max_gpu_timestamps);  // out of range is a programming error on every device
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    (void)device->end_frame();  // the frame is still valid; finish it
    device->wait_idle();

    if (!info.gpu_timestamps) {
        CHECK_FALSE(device->completed_timestamps().has_value());  // unavailable stays unavailable: no invented numbers
    } else {
        // A few more frames so the earlier slots are reused and collected.
        for (int i = 0; i < 4; ++i) {
            (void)device->begin_frame();
            (void)device->end_frame();
        }
        device->wait_idle();
        const std::optional<rhi::GpuTimestamps> t = device->completed_timestamps();
        REQUIRE(t.has_value());
        CHECK(t->period_ns > 0.0);
        CHECK((t->available_mask & 0b11u) == 0b11u);   // timestamps 0 and 1 were written
        CHECK((t->available_mask & (1u << 2)) == 0);   // 2 was not: it must not appear as a measurement
        CHECK(t->ticks[1] >= t->ticks[0]);
        const std::optional<double> ms = t->milliseconds_between(0, 1);
        REQUIRE(ms.has_value());
        CHECK(*ms >= 0.0);
        CHECK(*ms < 10'000.0);                           // not garbage: under ten seconds for a 1 MiB copy
        CHECK_FALSE(t->milliseconds_between(1, 0).has_value());  // reversed
        CHECK_FALSE(t->milliseconds_between(0, 2).has_value());  // the unwritten one
        bool belongs_to_a_recorded_frame = false;
        for (const uint64_t f : recorded_frames) belongs_to_a_recorded_frame |= (f == t->frame_number);
        CHECK(belongs_to_a_recorded_frame);               // it is the timing of a frame that really wrote them
    }
    for (const rhi::BufferHandle b : readbacks) device->destroy(b);
    device->destroy(tex);
    idle_frame(*device);
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("GpuTimestamps::milliseconds_between validates its inputs")
{
    rhi::GpuTimestamps t;
    t.period_ns = 2.0;
    t.available_mask = 0b11;
    t.ticks[0] = 1000;
    t.ticks[1] = 1000 + 500'000;  // 500,000 ticks * 2 ns = 1 ms
    CHECK(t.milliseconds_between(0, 1).has_value());
    CHECK(std::fabs(*t.milliseconds_between(0, 1) - 1.0) < 1e-9);
    CHECK_FALSE(t.milliseconds_between(1, 0).has_value());
    CHECK_FALSE(t.milliseconds_between(0, 2).has_value());
    CHECK_FALSE(t.milliseconds_between(0, rhi::max_gpu_timestamps).has_value());
    CHECK_FALSE(t.milliseconds_between(rhi::max_gpu_timestamps, 1).has_value());
    CHECK(t.milliseconds_between(0, 0).has_value());  // an empty interval is a valid zero
}
