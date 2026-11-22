// The GPU resource registry on a real (validated) Vulkan device: scene resources created with data, replaced while earlier
// frames are still in flight, a failed replacement, and a teardown that leaks. CI: lavapipe. Validation errors, including
// synchronization hazards, fail each test.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/gpu_resources.hpp"
#include "vulkan_test_util.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kSize = 16;

std::vector<std::byte> texels(uint32_t seed)
{
    std::vector<std::byte> bytes;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            bytes.push_back(static_cast<std::byte>((x * 23 + seed) & 0xFF));
            bytes.push_back(static_cast<std::byte>((y * 31 + seed * 3) & 0xFF));
            bytes.push_back(static_cast<std::byte>((x + y + seed) & 0xFF));
            bytes.push_back(std::byte{255});
        }
    }
    return bytes;
}

rhi::TextureDesc scene_texture(const char* name)
{
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = td.height = kSize;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst | rhi::TextureUsage::TransferSrc;
    td.debug_name = name;
    return td;
}

rhi::BufferDesc vertex_data(const char* name)
{
    rhi::BufferDesc bd;
    bd.size = 4096;
    bd.usage = rhi::BufferUsage::Vertex;
    bd.debug_name = name;
    return bd;
}

/// One scene's GPU resources, as a loaded scene holds them (non-owning readback targets are kept by the test).
struct SceneResources {
    TextureRef texture;
    BufferRef vertices;
    SamplerRef sampler;
};

SceneResources make_scene(GpuResourceRegistry& registry, uint32_t seed, const char* prefix)
{
    const std::string p = prefix;
    SceneResources s;
    s.texture = registry.create_texture(scene_texture((p + ".texture").c_str()), texels(seed));
    s.vertices = registry.create_buffer(vertex_data((p + ".vertices").c_str()));
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    s.sampler = registry.get_sampler(sd);  // shared by every scene that asks for the same sampler
    return s;
}

rhi::BufferHandle make_readback(rhi::Device& device)
{
    rhi::BufferDesc bd;
    bd.size = kSize * kSize * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    bd.debug_name = "Readback";
    return device.create_buffer(bd);
}

/// Submits a frame that reads the scene's texture into `readback` (a transfer copy), without waiting for it.
void submit_use(rhi::Device& device, const SceneResources& scene, rhi::BufferHandle readback)
{
    rhi::CommandList& cmd = device.begin_frame();
    const rhi::TextureBarrier to_transfer{
        scene.texture.handle(), rhi::sampled_texture_state,
        {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
    cmd.barrier(std::span(&to_transfer, 1));
    cmd.copy_texture_to_buffer(scene.texture.handle(), readback);
    // Put the texture back in the state shaders expect, so the next frame (and any later draw) starts from it.
    const rhi::TextureBarrier back{
        scene.texture.handle(), to_transfer.after, rhi::sampled_texture_state, {}};
    cmd.barrier(std::span(&back, 1));
    (void)device.end_frame();
}

bool holds(rhi::Device& device, rhi::BufferHandle readback, const std::vector<std::byte>& expected)
{
    const auto mapped = device.map(readback);
    return mapped.size() >= expected.size() && std::memcmp(mapped.data(), expected.data(), expected.size()) == 0;
}

} // namespace

TEST_CASE("Registry on Vulkan: a texture created with data reads back exactly and is counted")
{
    const auto device = make_validated_device("registry_roundtrip");
    {
        GpuResourceRegistry registry(*device);
        SceneResources scene = make_scene(registry, 5, "scene");
        const rhi::BufferHandle readback = make_readback(*device);
        submit_use(*device, scene, readback);
        device->wait_idle();
        CHECK(holds(*device, readback, texels(5)));
        CHECK(scene.texture.bytes() == kSize * kSize * 4);
        CHECK(registry.stats(ResourceKind::Texture).live == 1);
        CHECK(registry.stats(ResourceKind::Buffer).live_bytes == 4096);
        device->destroy(readback);
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Registry on Vulkan: replacing a scene while its frames are in flight keeps the old resources valid until they retire")
{
    const auto device = make_validated_device("registry_replace", /*synchronization=*/true);
    {
        GpuResourceRegistry registry(*device);
        std::string leaked;
        registry.set_leak_sink([&](std::string_view m) { leaked = std::string(m); });

        SceneResources active = make_scene(registry, 11, "sceneA");
        // Stage the replacement completely BEFORE A's frames are submitted. Creating a texture uploads it, and an upload
        // waits for the device timeline, so staging after the frames would retire them and leave nothing in flight.
        SceneResources candidate = make_scene(registry, 77, "sceneB");
        CHECK(registry.stats(ResourceKind::Texture).live == 2);        // both scenes exist during the swap
        CHECK(candidate.sampler.handle() == active.sampler.handle());  // one shared sampler
        CHECK(active.sampler.use_count() == 2);

        std::vector<rhi::BufferHandle> old_readbacks;
        for (int i = 0; i < 3; ++i) {  // three frames using scene A, none waited for
            old_readbacks.push_back(make_readback(*device));
            submit_use(*device, active, old_readbacks.back());
        }

        // The swap: A's references drop here, while its frames may still be running.
        active = std::move(candidate);
        CHECK(registry.stats(ResourceKind::Texture).live == 1);   // A's texture is released; the device still owns it
        CHECK(registry.stats(ResourceKind::Buffer).live == 1);
        CHECK(active.sampler.use_count() == 1);

        const rhi::BufferHandle new_readback = make_readback(*device);
        submit_use(*device, active, new_readback);                // and the new scene draws
        device->wait_idle();

        // The old frames saw scene A's data intact; the new frame sees scene B's.
        for (const rhi::BufferHandle r : old_readbacks) {
            CHECK(holds(*device, r, texels(11)));
        }
        CHECK(holds(*device, new_readback, texels(77)));

        // After idle: counters balance exactly.
        const ResourceStats textures = registry.stats(ResourceKind::Texture);
        CHECK(textures.created == 2);
        CHECK(textures.destroyed == 1);
        CHECK(textures.live == 1);
        CHECK(registry.stats(ResourceKind::Sampler).created == 1);

        for (const rhi::BufferHandle r : old_readbacks) device->destroy(r);
        device->destroy(new_readback);
        active = {};
        CHECK(registry.live_count() == 0);
        CHECK(leaked.empty());
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Registry on Vulkan: a failed replacement leaves the active scene intact and leaks nothing")
{
    const auto device = make_validated_device("registry_failed_candidate");
    {
        GpuResourceRegistry registry(*device);
        SceneResources active = make_scene(registry, 21, "active");
        const uint64_t live_before = registry.live_count();

        // A candidate whose texture data is the wrong size: the upload is rejected, the texture is released, nothing is
        // registered, and the caller simply never swaps.
        bool failed = false;
        try {
            SceneResources candidate;
            candidate.vertices = registry.create_buffer(vertex_data("candidate.vertices"));  // created first, then...
            const auto short_data = std::vector<std::byte>(kSize * kSize * 4 - 1);
            candidate.texture = registry.create_texture(scene_texture("candidate.texture"), short_data);  // ...this throws
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        CHECK(failed);
        CHECK(registry.live_count() == live_before);  // the half-built candidate released everything it had created

        const rhi::BufferHandle readback = make_readback(*device);
        submit_use(*device, active, readback);  // the active scene still draws
        device->wait_idle();
        CHECK(holds(*device, readback, texels(21)));
        device->destroy(readback);
    }
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Registry on Vulkan: tearing the registry down with a frame in flight and live references reports and stays clean")
{
    const auto device = make_validated_device("registry_leak", /*synchronization=*/true);
    std::string report;
    SceneResources late;
    rhi::BufferHandle readback;
    {
        GpuResourceRegistry registry(*device);
        registry.set_leak_sink([&](std::string_view m) { report = std::string(m); });
        late = make_scene(registry, 33, "leaky");
        readback = make_readback(*device);
        submit_use(*device, late, readback);  // still running when the registry goes away
    }
    CHECK(report.find("leaky.texture") != std::string::npos);
    CHECK(report.find("leaky.vertices") != std::string::npos);
    CHECK(report.find("3 resource(s) still referenced") != std::string::npos);
    late = {};  // late references are inert
    device->wait_idle();
    CHECK(holds(*device, readback, texels(33)));  // the in-flight frame completed correctly despite the teardown
    device->destroy(readback);
    CHECK(device->validation_error_count() == 0);
}
