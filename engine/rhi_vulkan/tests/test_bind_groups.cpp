// Bind group tests on a real (validated) Vulkan device: textures, samplers and uniform buffers reach the shader and the
// pixels are what was uploaded. CI: lavapipe. Validation errors fail each test.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaders/test_textured_fullscreen.spv.hpp"
#include "vulkan_test_util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;

namespace {

constexpr uint32_t kSize = 8;

std::byte b(uint32_t v) { return static_cast<std::byte>(v & 0xFFu); }

std::vector<std::byte> pattern(uint32_t width, uint32_t height)
{
    std::vector<std::byte> bytes;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            bytes.push_back(b(x * 30 + 5));
            bytes.push_back(b(y * 31 + 9));
            bytes.push_back(b(x * 7 + y * 11 + 20));
            bytes.push_back(std::byte{255});
        }
    }
    return bytes;
}

std::vector<std::byte> solid(uint32_t width, uint32_t height, uint32_t r, uint32_t g, uint32_t bl)
{
    std::vector<std::byte> bytes;
    for (uint32_t i = 0; i < width * height; ++i) {
        bytes.push_back(b(r));
        bytes.push_back(b(g));
        bytes.push_back(b(bl));
        bytes.push_back(std::byte{255});
    }
    return bytes;
}

template <typename T, size_t N>
std::span<const uint32_t> words(const T (&a)[N])
{
    return std::span<const uint32_t>(a, N);
}

/// Everything needed to draw the test shader into an 8x8 RGBA8 target and read it back.
struct Fixture {
    rhi::Device& device;
    rhi::BindGroupLayoutHandle layout;
    rhi::ShaderHandle vs, ps;
    rhi::PipelineHandle pipeline;
    rhi::TextureHandle target;
    rhi::BufferHandle readback;
    rhi::BufferHandle tint;

    explicit Fixture(rhi::Device& d) : device(d)
    {
        rhi::BindGroupLayoutDesc ld;
        ld.entries = {{0, rhi::BindingType::SampledTexture}, {1, rhi::BindingType::Sampler}, {2, rhi::BindingType::UniformBuffer}};
        ld.debug_name = "TestTexturedLayout";
        layout = device.create_bind_group_layout(ld);
        vs = device.create_shader({rhi::ShaderStage::Vertex, words(shaders::test_textured_vs_main), "VSMain", "TestTextured.VS"});
        ps = device.create_shader({rhi::ShaderStage::Pixel, words(shaders::test_textured_ps_main), "PSMain", "TestTextured.PS"});
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs;
        pd.pixel_shader = ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {rhi::Format::RGBA8_UNORM};
        pd.bind_group_layouts = {layout};
        pd.debug_name = "TestTextured";
        pipeline = device.create_graphics_pipeline(pd);

        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = td.height = kSize;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "TestTarget";
        target = device.create_texture(td);
        rhi::BufferDesc bd;
        bd.size = uint64_t{kSize} * kSize * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "TestReadback";
        readback = device.create_buffer(bd);
        set_tint(1.0f, 1.0f, 1.0f, 1.0f);
    }

    ~Fixture()
    {
        device.destroy(tint);
        device.destroy(readback);
        device.destroy(target);
        device.destroy(pipeline);
        device.destroy(ps);
        device.destroy(vs);
        device.destroy(layout);
    }

    void set_tint(float r, float g, float bl, float a)
    {
        if (tint.is_valid()) {
            device.destroy(tint);
        }
        const float color[4] = {r, g, bl, a};
        rhi::BufferDesc ub;
        ub.size = 256;  // the uniform block is 16 bytes; 256 keeps any offset/size rule trivially satisfied
        ub.usage = rhi::BufferUsage::Uniform;
        ub.memory = rhi::MemoryLocation::CpuToGpu;
        ub.debug_name = "TestTint";
        tint = device.create_buffer(ub);
        std::memcpy(device.map(tint).data(), color, sizeof(color));
    }

    rhi::BindGroupHandle make_group(rhi::TextureHandle texture, rhi::SamplerHandle sampler)
    {
        rhi::BindGroupEntry entries[3];
        entries[0].binding = 0;
        entries[0].texture = texture;
        entries[1].binding = 1;
        entries[1].sampler = sampler;
        entries[2].binding = 2;
        entries[2].buffer = tint;
        entries[2].size = 16;
        return device.create_bind_group(layout, entries);
    }

    /// Records one frame that draws the full-screen triangle with `group`. With `wait` it also copies the target to the
    /// readback buffer, waits, and returns the pixels. Without it nothing is read back and the frame is left running, so
    /// the GPU may still be sampling the group's texture when the caller touches it again.
    std::vector<uint8_t> draw(rhi::BindGroupHandle group, bool wait = true)
    {
        rhi::CommandList& cmd = device.begin_frame();
        // Discard the previous contents, ordered after everything earlier (an earlier unwaited frame may still run).
        const rhi::TextureBarrier to_attachment{
            target,
            {rhi::PipelineStage::AllCommands, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::Undefined},
            {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment},
            {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target;
        ca.clear = {0.0f, 0.0f, 0.0f, 0.0f};
        rd.colors.push_back(ca);
        rd.area = {0, 0, kSize, kSize};
        cmd.begin_rendering(rd);
        cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f});
        cmd.set_scissor({0, 0, kSize, kSize});
        cmd.bind_pipeline(pipeline);
        cmd.bind_group(0, group);
        cmd.draw(3);
        cmd.end_rendering();
        if (wait) {
            const rhi::TextureBarrier to_transfer{
                target, to_attachment.after,
                {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
            cmd.barrier(std::span(&to_transfer, 1));
            cmd.copy_texture_to_buffer(target, readback);
        }
        (void)device.end_frame();
        if (!wait) {
            return {};
        }
        device.wait_idle();
        return pixels();
    }

    std::vector<uint8_t> pixels()
    {
        const auto mapped = device.map(readback);
        return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(mapped.data()),
                                    reinterpret_cast<const uint8_t*>(mapped.data()) + size_t{kSize} * kSize * 4);
    }
};

rhi::TextureDesc sampled(uint32_t mips)
{
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = td.height = kSize;
    td.mip_levels = mips;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "TestSource";
    return td;
}

rhi::SamplerDesc nearest(float min_lod = 0.0f, float max_lod = rhi::lod_clamp_none)
{
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    sd.mipmap_mode = rhi::MipmapMode::Nearest;
    sd.address_u = sd.address_v = rhi::AddressMode::ClampToEdge;
    sd.min_lod = min_lod;
    sd.max_lod = max_lod;
    return sd;
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

TEST_CASE("Vulkan bind groups: a sampled texture reaches the shader texel for texel")
{
    const auto device = make_validated_device("bind_group_texels");
    {
        Fixture f(*device);
        const rhi::TextureHandle tex = device->create_texture(sampled(1));
        device->upload_texture(tex, pattern(kSize, kSize));
        const rhi::SamplerHandle sampler = device->create_sampler(nearest());
        const rhi::BindGroupHandle group = f.make_group(tex, sampler);
        const std::vector<uint8_t> got = f.draw(group);
        const auto want = pattern(kSize, kSize);
        REQUIRE(got.size() == want.size());
        // Exact: nearest filtering at pixel centers, UNORM in and out. A flipped row, swapped channel or wrong
        // texel would fail here.
        size_t mismatches = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            mismatches += got[i] != static_cast<uint8_t>(want[i]) ? 1u : 0u;
        }
        CHECK(mismatches == 0);
        device->destroy(group);
        device->destroy(sampler);
        device->destroy(tex);
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan bind groups: the uniform buffer binding scales the output")
{
    const auto device = make_validated_device("bind_group_uniform");
    {
        Fixture f(*device);
        const rhi::TextureHandle tex = device->create_texture(sampled(1));
        device->upload_texture(tex, solid(kSize, kSize, 200, 100, 50));
        const rhi::SamplerHandle sampler = device->create_sampler(nearest());
        f.set_tint(0.5f, 1.0f, 0.0f, 1.0f);
        const rhi::BindGroupHandle group = f.make_group(tex, sampler);
        const std::vector<uint8_t> got = f.draw(group);
        bool ok = true;
        for (size_t p = 0; p < size_t{kSize} * kSize; ++p) {
            ok &= std::abs(int{got[p * 4 + 0]} - 100) <= 1;  // 200 * 0.5
            ok &= std::abs(int{got[p * 4 + 1]} - 100) <= 1;  // 100 * 1.0
            ok &= got[p * 4 + 2] == 0;                       // 50 * 0.0
            ok &= got[p * 4 + 3] == 255;
        }
        CHECK(ok);
        device->destroy(group);
        device->destroy(sampler);
        device->destroy(tex);
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan bind groups: each mip level of an upload lands where it belongs")
{
    const auto device = make_validated_device("bind_group_mips");
    {
        Fixture f(*device);
        // 8x8 with 4 mips (8, 4, 2, 1). Every level is a different solid color, so a mis-placed level shows up as the
        // wrong color when a sampler clamps its LOD to that level.
        const uint32_t colors[4][3] = {{250, 10, 10}, {10, 250, 10}, {10, 10, 250}, {240, 240, 10}};
        std::vector<std::byte> data;
        for (uint32_t level = 0; level < 4; ++level) {
            const uint32_t s = kSize >> level;
            const auto p = solid(s, s, colors[level][0], colors[level][1], colors[level][2]);
            data.insert(data.end(), p.begin(), p.end());
        }
        const rhi::TextureHandle tex = device->create_texture(sampled(4));
        device->upload_texture(tex, data);
        for (uint32_t level = 0; level < 4; ++level) {
            const float lod = static_cast<float>(level);
            const rhi::SamplerHandle sampler = device->create_sampler(nearest(lod, lod));
            const rhi::BindGroupHandle group = f.make_group(tex, sampler);
            const std::vector<uint8_t> got = f.draw(group);
            size_t wrong = 0;
            for (size_t p = 0; p < size_t{kSize} * kSize; ++p) {
                wrong += (got[p * 4 + 0] != colors[level][0] || got[p * 4 + 1] != colors[level][1] ||
                          got[p * 4 + 2] != colors[level][2])
                             ? 1u
                             : 0u;
            }
            CHECK(wrong == 0);
            if (wrong != 0) {
                std::printf("  mip %u: %zu of %u pixels have the wrong color\n", level, wrong, kSize * kSize);
            }
            device->destroy(group);
            device->destroy(sampler);
        }
        device->destroy(tex);
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan bind groups: destroying a group and its resources right after submit is safe")
{
    const auto device = make_validated_device("bind_group_lifetime");
    {
        Fixture f(*device);
        for (int round = 0; round < 6; ++round) {
            const rhi::TextureHandle tex = device->create_texture(sampled(1));
            device->upload_texture(tex, solid(kSize, kSize, 20u + 30u * static_cast<uint32_t>(round), 60, 90));
            const rhi::SamplerHandle sampler = device->create_sampler(nearest());
            const rhi::BindGroupHandle group = f.make_group(tex, sampler);
            (void)f.draw(group, /*wait=*/false);
            // The submission may still be running: retirement must wait for it.
            device->destroy(group);
            device->destroy(sampler);
            device->destroy(tex);
        }
        device->wait_idle();
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan bind groups: invalid layouts and groups are rejected")
{
    const auto device = make_validated_device("bind_group_errors");
    Fixture f(*device);
    const rhi::TextureHandle tex = device->create_texture(sampled(1));
    device->upload_texture(tex, pattern(kSize, kSize));
    const rhi::SamplerHandle sampler = device->create_sampler(nearest());

    rhi::BindGroupLayoutDesc empty;
    CHECK(throws_invalid([&] { (void)device->create_bind_group_layout(empty); }));
    rhi::BindGroupLayoutDesc dup;
    dup.entries = {{0, rhi::BindingType::Sampler}, {0, rhi::BindingType::SampledTexture}};
    CHECK(throws_invalid([&] { (void)device->create_bind_group_layout(dup); }));

    const auto entries = [&](uint32_t n) {
        std::vector<rhi::BindGroupEntry> e(n);
        if (n > 0) { e[0].binding = 0; e[0].texture = tex; }
        if (n > 1) { e[1].binding = 1; e[1].sampler = sampler; }
        if (n > 2) { e[2].binding = 2; e[2].buffer = f.tint; e[2].size = 16; }
        return e;
    };
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, entries(2)); }));  // missing the uniform
    auto wrong_binding = entries(3);
    wrong_binding[1].binding = 7;
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, wrong_binding); }));
    auto repeated = entries(3);
    repeated[2].binding = 1;
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, repeated); }));
    auto bad_texture = entries(3);
    bad_texture[0].texture = {};
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, bad_texture); }));
    auto not_sampled = entries(3);
    not_sampled[0].texture = f.target;  // ColorAttachment | TransferSrc: no Sampled usage
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, not_sampled); }));
    auto bad_sampler = entries(3);
    bad_sampler[1].sampler = {};
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, bad_sampler); }));
    auto wrong_buffer_kind = entries(3);
    wrong_buffer_kind[2].buffer = f.readback;  // TransferDst only
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, wrong_buffer_kind); }));
    auto misaligned = entries(3);
    misaligned[2].offset = 16;
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, misaligned); }));
    auto too_long = entries(3);
    too_long[2].size = 1024;
    CHECK(throws_invalid([&] { (void)device->create_bind_group(f.layout, too_long); }));
    CHECK(throws_invalid([&] { (void)device->create_bind_group({}, entries(3)); }));

    // None of that corrupted the device: a correct group still works.
    const rhi::BindGroupHandle group = device->create_bind_group(f.layout, entries(3));
    const std::vector<uint8_t> got = f.draw(group);
    CHECK(got.size() == size_t{kSize} * kSize * 4);
    device->destroy(group);
    device->destroy(sampler);
    device->destroy(tex);
    CHECK(device->validation_error_count() == 0);
}

// End-to-end check that a texture re-uploaded while draws that sampled it may still be running shows the new contents
// and stays validation-clean (synchronization validation is on, which already caught a hazard in this test's own
// fixture). It does NOT prove the upload's ordering against *shader* reads: the Khronos validation layer shipped with
// Vulkan SDK 1.3.275 does not track descriptor reads, and this test still passes if the upload's first barrier loses its
// execution dependency. That ordering is proved by "re-uploading a texture that an in-flight frame is reading is
// synchronized" in test_texture_upload.cpp, which uses a transfer read the layer does track; the barrier is the same
// whichever operation read the texture.
TEST_CASE("Vulkan bind groups: a texture re-uploaded while draws that sampled it are in flight shows the new contents")
{
    const auto device = make_validated_device("bind_group_reupload", /*synchronization=*/true);
    {
        Fixture f(*device);
        const rhi::TextureHandle tex = device->create_texture(sampled(1));
        device->upload_texture(tex, solid(kSize, kSize, 10, 20, 30));
        const rhi::SamplerHandle sampler = device->create_sampler(nearest());
        const rhi::BindGroupHandle group = f.make_group(tex, sampler);
        for (uint32_t round = 1; round <= 6; ++round) {
            (void)f.draw(group, /*wait=*/false);  // a fragment shader samples the texture; the frame is not waited for
            device->upload_texture(tex, solid(kSize, kSize, 10 + round * 30, 20, 30));
        }
        const std::vector<uint8_t> got = f.draw(group);
        size_t wrong = 0;
        for (size_t p = 0; p < size_t{kSize} * kSize; ++p) {
            wrong += (got[p * 4 + 0] != 190 || got[p * 4 + 1] != 20 || got[p * 4 + 2] != 30) ? 1u : 0u;  // 10 + 6 * 30
        }
        CHECK(wrong == 0);
        device->destroy(group);
        device->destroy(sampler);
        device->destroy(tex);
    }
    CHECK(device->validation_error_count() == 0);
}
