// GPU timing and memory diagnostics on a real (validated) Vulkan device. Timestamps are either measured or reported
// unavailable, never invented; memory accounting is exact and must balance. CI: lavapipe.

#include "saige/gfx_test/saige_test.hpp"

#include "vulkan_test_util.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
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

TEST_CASE("Device memory: a bookkeeping failure frees the driver allocation and counts nothing")
{
    const uint32_t errors_before = rhi::testing::process_validation_error_count();
    auto device = make_validated_device("diag_memory_rollback");
    idle_frame(*device);
    const rhi::DeviceMemoryStats base = device->memory_stats();
    const auto unchanged = [&](const char* what) {
        const rhi::DeviceMemoryStats now = device->memory_stats();
        CHECK(now.allocated_bytes == base.allocated_bytes);
        CHECK(now.allocation_count == base.allocation_count);
        if (now.allocated_bytes != base.allocated_bytes || now.allocation_count != base.allocation_count) {
            std::printf("  ledger changed after a failed %s\n", what);
        }
    };
    const auto fails = [&](uint32_t nth, auto&& create) {
        rhi::testing::fail_memory_bookkeeping_after(nth);
        bool threw = false;
        try {
            create();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        rhi::testing::fail_memory_bookkeeping_after(0);
        return threw;
    };

    rhi::BufferDesc host;
    host.size = 4096;
    host.usage = rhi::BufferUsage::Uniform;
    host.memory = rhi::MemoryLocation::CpuToGpu;
    CHECK(fails(1, [&] { (void)device->create_buffer(host); }));
    unchanged("host-visible buffer");

    rhi::BufferDesc gpu_only;
    gpu_only.size = 4096;
    gpu_only.usage = rhi::BufferUsage::Vertex;
    gpu_only.memory = rhi::MemoryLocation::GpuOnly;
    const std::vector<std::byte> bytes(4096, std::byte{1});
    CHECK(fails(1, [&] { (void)device->create_buffer(gpu_only, bytes); }));  // the buffer's own memory
    unchanged("device buffer");
    // The staged upload allocates twice: the buffer, then the staging buffer. Failing the second leaves a fully created
    // destination with no handle, which must be released along with the staging buffer.
    CHECK(fails(2, [&] { (void)device->create_buffer(gpu_only, bytes); }));
    unchanged("staged buffer upload");

    CHECK(fails(1, [&] { (void)device->create_texture(big_texture()); }));
    unchanged("texture");

    // The device is fully usable afterwards, and everything created now is accounted for and released exactly.
    const rhi::BufferHandle b = device->create_buffer(gpu_only, bytes);
    const rhi::TextureHandle tex = device->create_texture(big_texture());
    CHECK(device->memory_stats().allocation_count == base.allocation_count + 2);
    device->destroy(b);
    device->destroy(tex);
    idle_frame(*device);
    unchanged("recovery");
    CHECK(device->validation_error_count() == 0);
    // The ledger cannot see an allocation the driver made and nobody recorded. The validation layer can: it reports any
    // device memory still alive when the device is destroyed.
    device.reset();
    CHECK(rhi::testing::process_validation_error_count() == errors_before);
}

TEST_CASE("GPU timestamps: an index is written at most once per recording, supported device or not")
{
    const auto device = make_validated_device("diag_timestamp_duplicates");
    rhi::CommandList& cmd = device->begin_frame();
    cmd.write_timestamp(2);
    bool second_threw = false;
    try {
        cmd.write_timestamp(2);  // re-using a query that has not been reset is invalid Vulkan: refused before any call
    } catch (const std::invalid_argument&) {
        second_threw = true;
    }
    CHECK(second_threw);
    cmd.write_timestamp(5);  // other indices are unaffected, and the refused write left the recording usable
    (void)device->end_frame();

    rhi::CommandList& next = device->begin_frame();  // a new recording may write every index again
    next.write_timestamp(2);
    next.write_timestamp(5);
    (void)device->end_frame();
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);  // nothing invalid ever reached Vulkan
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
        CHECK(t->valid_bits > 0);
        CHECK(t->valid_bits <= 64);
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
    t.valid_bits = 64;
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
