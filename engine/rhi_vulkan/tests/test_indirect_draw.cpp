// Indirect drawing on a real (validated) Vulkan device. CI: lavapipe. Three small triangles are laid out in separate regions of a 32 x 32 target; each test issues
// indirect draws whose argument records select triangles, and the image shows which regions were covered, so a wrong first vertex, index offset, instance count, draw
// count or stride changes the picture. Optional features (multi-draw, first instance, draw counts) are exercised only where the device reports them. Correctness only.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaders/test_draw_probe.spv.hpp"
#include "saige/shaders/test_compute_probe.spv.hpp"
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

constexpr uint32_t kSize = 32;

/// Triangle t (0, 1, 2) is centred in the top-left, top-right and bottom-left quadrant; the bottom-right one has none.
struct Probe {
    uint32_t x, y;
};
constexpr Probe kRegion[4] = {{8, 8}, {24, 8}, {8, 24}, {24, 24}};

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::vector<std::function<void()>> cleanup;
    rhi::PipelineHandle pipeline;
    rhi::BufferHandle vertices, indices, readback;
    rhi::TextureHandle target;

    template <typename Handle>
    Handle own(Handle h)
    {
        cleanup.push_back([this, h] { device->destroy(h); });
        return h;
    }

    explicit Rig(const char* name) : device(make_validated_device(name, true))
    {
        const rhi::ShaderHandle vs = own(device->create_shader({rhi::ShaderStage::Vertex, words(saige::shaders::test_draw_probe_vs_main), "VSMain", "ProbeVS"}));
        const rhi::ShaderHandle ps = own(device->create_shader({rhi::ShaderStage::Pixel, words(saige::shaders::test_draw_probe_ps_main), "PSMain", "ProbePS"}));
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs;
        pd.pixel_shader = ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {rhi::Format::RGBA8_UNORM};
        pd.push_constant_size = 16;
        pd.vertex_layout.stride = 8;
        pd.vertex_layout.attributes = {{0, rhi::Format::RG32_FLOAT, 0}};
        pd.debug_name = "IndirectProbe";
        pipeline = own(device->create_graphics_pipeline(pd));

        std::vector<float> v;
        const float centres[3][2] = {{-0.5f, 0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}};
        for (const auto& c : centres) {
            v.insert(v.end(), {c[0] - 0.3f, c[1] - 0.3f, c[0] + 0.3f, c[1] - 0.3f, c[0], c[1] + 0.3f});
        }
        rhi::BufferDesc bd;
        bd.size = v.size() * 4;
        bd.usage = rhi::BufferUsage::Vertex;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        vertices = own(device->create_buffer(bd, std::as_bytes(std::span<const float>(v))));
        std::array<uint16_t, 9> idx{0, 1, 2, 3, 4, 5, 6, 7, 8};
        bd.size = sizeof(idx);
        bd.usage = rhi::BufferUsage::Index;
        indices = own(device->create_buffer(bd, std::as_bytes(std::span<const uint16_t>(idx))));

        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = td.height = kSize;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        target = own(device->create_texture(td));
        rhi::BufferDesc rb;
        rb.size = uint64_t{kSize} * kSize * 4;
        rb.usage = rhi::BufferUsage::TransferDst;
        rb.memory = rhi::MemoryLocation::GpuToCpu;
        readback = own(device->create_buffer(rb));
    }

    ~Rig()
    {
        device->wait_idle();
        for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) (*it)();
        device->wait_idle();
    }

    /// A buffer with Indirect usage that the host filled in (readback buffers cannot be indirect).
    template <typename T>
    rhi::BufferHandle indirect_buffer(std::span<const T> records)
    {
        rhi::BufferDesc bd;
        bd.size = records.size_bytes();
        bd.usage = rhi::BufferUsage::Indirect;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        return own(device->create_buffer(bd, std::as_bytes(records)));
    }

    /// Records `draw` between begin and end of rendering with the probe pipeline, vertex and index buffers bound; returns, for each region, whether red was drawn.
    std::array<bool, 4> run(const std::function<void(rhi::CommandList&)>& draw)
    {
        rhi::CommandList& cmd = device->begin_frame();
        const rhi::TextureBarrier to_attachment{target, {}, {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment}, {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target;
        ca.clear = {0.0f, 0.0f, 0.0f, 1.0f};
        rd.colors.push_back(ca);
        rd.area = {0, 0, kSize, kSize};
        cmd.begin_rendering(rd);
        cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f});
        cmd.set_scissor({0, 0, kSize, kSize});
        cmd.bind_pipeline(pipeline);
        const std::array<float, 4> red{1.0f, 0.0f, 0.0f, 1.0f};
        cmd.push_constant_struct(red);
        cmd.bind_vertex_buffer(vertices);
        cmd.bind_index_buffer(indices, rhi::IndexType::Uint16);
        draw(cmd);
        cmd.end_rendering();
        const rhi::TextureBarrier to_transfer{target, to_attachment.after, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(target, readback);
        (void)device->end_frame();
        device->wait_idle();
        const std::span<std::byte> bytes = device->map(readback);
        std::array<bool, 4> covered{};
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data()) + (size_t{kRegion[i].y} * kSize + kRegion[i].x) * 4;
            covered[i] = p[0] > 200 && p[1] < 40;
        }
        return covered;
    }
};

using Cover = std::array<bool, 4>;

} // namespace

TEST_CASE("Vulkan indirect draw: the device reports which indirect features it has, and the argument records select what is drawn")
{
    Rig rig("indirect_draw");
    const rhi::DeviceInfo& info = rig.device->info();
    std::printf("  indirect drawing on this device: multi-draw %s, first instance %s, draw count %s\n", info.multi_draw_indirect ? "yes" : "no",
                info.draw_indirect_first_instance ? "yes" : "no", info.draw_indirect_count ? "yes" : "no");

    // lavapipe (the CI device) offers all three features; a software device that stops offering one fails here loudly instead of letting the tests below skip silently.
    if (info.software_device) {
        CHECK(info.multi_draw_indirect);
        CHECK(info.draw_indirect_first_instance);
        CHECK(info.draw_indirect_count);
    }

    // One draw per call: the second record selects triangle 2 (first vertex 6), the first triangle 0.
    const std::array<rhi::DrawIndirectCommand, 2> records{{{3, 1, 0, 0}, {3, 1, 6, 0}}};
    const rhi::BufferHandle args = rig.indirect_buffer<rhi::DrawIndirectCommand>(records);
    CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indirect(args, 0, 1, 0); }) == Cover{true, false, false, false}));
    CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indirect(args, sizeof(rhi::DrawIndirectCommand), 1, 0); }) == Cover{false, false, true, false}));
    CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indirect(args, 0, 0, 16); }) == Cover{false, false, false, false}));  // zero draws

    // Several draws from one buffer, skipping triangle 1.
    if (info.multi_draw_indirect) {
        CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indirect(args, 0, 2, sizeof(rhi::DrawIndirectCommand)); }) == Cover{true, false, true, false}));
        // A stride larger than the record: records 32 bytes apart.
        struct Padded {
            rhi::DrawIndirectCommand cmd;
            uint32_t pad[4];
        };
        const std::array<Padded, 2> padded{{{{3, 1, 3, 0}, {}}, {{3, 1, 6, 0}, {}}}};  // triangles 1 and 2
        const rhi::BufferHandle strided = rig.indirect_buffer<Padded>(padded);
        CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indirect(strided, 0, 2, sizeof(Padded)); }) == Cover{false, true, true, false}));
    }

    // The instance count of a record: the vertex shader shifts each instance one NDC unit to the right, so two instances of triangle 0 cover regions 0 and 1.
    const std::array<rhi::DrawIndirectCommand, 1> two_instances{{{3, 2, 0, 0}}};
    const rhi::BufferHandle inst = rig.indirect_buffer<rhi::DrawIndirectCommand>(two_instances);
    CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indirect(inst, 0, 1, 0); }) == Cover{true, true, false, false}));

    // The first instance of a record (SV_InstanceID includes it): instance 1 of triangle 0 lands in region 1 and nothing is left in region 0.
    if (info.draw_indirect_first_instance) {
        const std::array<rhi::DrawIndirectCommand, 1> first{{{3, 1, 0, 1}}};
        const rhi::BufferHandle fi = rig.indirect_buffer<rhi::DrawIndirectCommand>(first);
        CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indirect(fi, 0, 1, 0); }) == Cover{false, true, false, false}));
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Vulkan indirect draw: indexed records honour the first index, the vertex offset and the stride")
{
    Rig rig("indirect_indexed");
    const rhi::DeviceInfo& info = rig.device->info();
    // {index_count, instance_count, first_index, vertex_offset, first_instance}
    const std::array<rhi::DrawIndexedIndirectCommand, 3> records{{{3, 1, 6, 0, 0},    // indices 6..8: triangle 2
                                                                  {3, 1, 0, 3, 0},    // indices 0..2 plus a vertex offset of 3: triangle 1
                                                                  {3, 1, 0, 0, 0}}};  // triangle 0
    const rhi::BufferHandle args = rig.indirect_buffer<rhi::DrawIndexedIndirectCommand>(records);
    CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indexed_indirect(args, 0, 1, 0); }) == Cover{false, false, true, false}));
    CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indexed_indirect(args, sizeof(rhi::DrawIndexedIndirectCommand), 1, 0); }) == Cover{false, true, false, false}));
    if (info.multi_draw_indirect) {
        CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indexed_indirect(args, 0, 2, sizeof(rhi::DrawIndexedIndirectCommand)); }) == Cover{false, true, true, false}));
        CHECK((rig.run([&](rhi::CommandList& c) { c.draw_indexed_indirect(args, 0, 3, sizeof(rhi::DrawIndexedIndirectCommand)); }) == Cover{true, true, true, false}));
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Vulkan indirect draw: the draw count read from a buffer limits how many records run")
{
    Rig rig("indirect_count");
    if (!rig.device->info().draw_indirect_count) {
        // Without the feature the call is refused before any Vulkan call is made (a valid pipeline, buffers and scope are bound, so the capability is the only thing wrong).
        std::printf("  this device has no indirect draw count support: checking the refusal instead\n");
        const std::array<rhi::DrawIndexedIndirectCommand, 1> records{{{3, 1, 0, 0, 0}}};
        const rhi::BufferHandle args = rig.indirect_buffer<rhi::DrawIndexedIndirectCommand>(records);
        const rhi::BufferHandle count = rig.indirect_buffer<uint32_t>(std::array<uint32_t, 1>{1u});
        rig.run([&](rhi::CommandList& c) { CHECK(throws_invalid_argument([&] { c.draw_indexed_indirect_count(args, 0, count, 0, 1, 20); })); });
        CHECK(rig.device->validation_error_count() == 0);
        return;
    }
    const std::array<rhi::DrawIndexedIndirectCommand, 3> records{{{3, 1, 0, 0, 0}, {3, 1, 3, 0, 0}, {3, 1, 6, 0, 0}}};  // triangles 0, 1, 2
    const rhi::BufferHandle args = rig.indirect_buffer<rhi::DrawIndexedIndirectCommand>(records);
    auto run_with_count = [&](uint32_t n, uint32_t max_draws) {
        const std::array<uint32_t, 4> counts{0u, n, 0u, 0u};  // the count lives at byte offset 4, as a non-zero offset is part of the rule
        const rhi::BufferHandle count = rig.indirect_buffer<uint32_t>(counts);
        return rig.run([&](rhi::CommandList& c) { c.draw_indexed_indirect_count(args, 0, count, 4, max_draws, sizeof(rhi::DrawIndexedIndirectCommand)); });
    };
    CHECK((run_with_count(0, 3) == Cover{false, false, false, false}));
    CHECK((run_with_count(1, 3) == Cover{true, false, false, false}));
    CHECK((run_with_count(2, 3) == Cover{true, true, false, false}));
    CHECK((run_with_count(3, 3) == Cover{true, true, true, false}));
    CHECK((run_with_count(3, 2) == Cover{true, true, false, false}));  // clamped to max_draws
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Vulkan indirect draw: misuse is rejected before a Vulkan call is made")
{
    Rig rig("indirect_misuse");
    const std::array<rhi::DrawIndirectCommand, 2> records{{{3, 1, 0, 0}, {3, 1, 6, 0}}};
    const rhi::BufferHandle args = rig.indirect_buffer<rhi::DrawIndirectCommand>(records);
    rhi::BufferDesc bd;
    bd.size = 256;
    bd.usage = rhi::BufferUsage::Storage;
    bd.memory = rhi::MemoryLocation::GpuOnly;
    const rhi::BufferHandle not_indirect = rig.own(rig.device->create_buffer(bd));
    const rhi::BufferHandle count = rig.indirect_buffer<uint32_t>(std::array<uint32_t, 1>{1u});

    rig.run([&](rhi::CommandList& c) {
        CHECK(throws_invalid_argument([&] { c.draw_indirect(rhi::BufferHandle{}, 0, 1, 0); }));
        CHECK(throws_invalid_argument([&] { c.draw_indirect(not_indirect, 0, 1, 0); }));
        CHECK(throws_invalid_argument([&] { c.draw_indirect(args, 2, 1, 0); }));          // misaligned offset
        CHECK(throws_invalid_argument([&] { c.draw_indirect(args, 24, 1, 0); }));         // 24 + 16 > 32
        CHECK(throws_invalid_argument([&] { c.draw_indirect(args, 0, 3, 16); }));         // three records do not fit (or multi-draw is missing)
        CHECK(throws_invalid_argument([&] { c.draw_indirect(args, 0, 2, 12); }));         // stride below the record size (or multi-draw is missing)
        CHECK(throws_invalid_argument([&] { c.draw_indexed_indirect(args, 0, 2, 16); }));  // an indexed record is 20 bytes
        CHECK(throws_invalid_argument([&] { c.draw_indexed_indirect_count(args, 0, not_indirect, 0, 1, 20); }));
        CHECK(throws_invalid_argument([&] { c.draw_indexed_indirect_count(args, 0, count, 2, 1, 20); }));
        CHECK(throws_invalid_argument([&] { c.draw_indexed_indirect_count(args, 0, count, 4, 1, 20); }));  // the count would be past the end of a 4-byte buffer
        c.draw_indirect(args, 0, 1, 0);  // and a valid draw still works afterwards
    });
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Vulkan indirect draw: multi-draw and draw counts are refused where the device lacks them, single draws are not")
{
    Rig rig("indirect_denied");
    const std::array<rhi::DrawIndirectCommand, 2> records{{{3, 1, 0, 0}, {3, 1, 6, 0}}};
    const rhi::BufferHandle args = rig.indirect_buffer<rhi::DrawIndirectCommand>(records);
    const std::array<rhi::DrawIndexedIndirectCommand, 1> indexed{{{3, 1, 0, 0, 0}}};
    const rhi::BufferHandle iargs = rig.indirect_buffer<rhi::DrawIndexedIndirectCommand>(indexed);
    const rhi::BufferHandle count = rig.indirect_buffer<uint32_t>(std::array<uint32_t, 1>{1u});
    rhi::testing::deny_indirect_draw_features(true, true);
    Cover single{};
    rig.run([&](rhi::CommandList& c) {
        CHECK(throws_invalid_argument([&] { c.draw_indirect(args, 0, 2, sizeof(rhi::DrawIndirectCommand)); }));
        CHECK(throws_invalid_argument([&] { c.draw_indexed_indirect(iargs, 0, 2, sizeof(rhi::DrawIndexedIndirectCommand)); }));
        CHECK(throws_invalid_argument([&] { c.draw_indexed_indirect_count(iargs, 0, count, 0, 1, sizeof(rhi::DrawIndexedIndirectCommand)); }));
        c.draw_indirect(args, sizeof(rhi::DrawIndirectCommand), 1, 0);  // one draw needs neither feature
    });
    single = rig.run([&](rhi::CommandList& c) { c.draw_indirect(args, sizeof(rhi::DrawIndirectCommand), 1, 0); });
    rhi::testing::deny_indirect_draw_features(false, false);
    CHECK((single == Cover{false, false, true, false}));
    CHECK(rig.device->validation_error_count() == 0);
}
