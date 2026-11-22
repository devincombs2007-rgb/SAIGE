#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/gpu_resources.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace saige::render;
using namespace saige::rhi;

#if !defined(_MSC_VER)
// ------------------------------------------------------------------------------------------------ allocation counting
// Counts heap allocations while `armed`, to show that releasing a reference allocates nothing in the registry. It never makes
// an allocation fail: failure injection goes through the registry's own test hook, because failing allocations inside the
// device and the STL (whose behavior on failure is outside the registry's contract, and which allocate even in operations
// declared noexcept under MSVC's checked iterators) tests nothing about the registry. Replacing the global allocation
// functions affects only this executable.
namespace alloc_probe {
std::size_t count = 0;
bool armed = false;
} // namespace alloc_probe

void* operator new(std::size_t size)
{
    if (alloc_probe::armed) {
        ++alloc_probe::count;
    }
    if (void* p = std::malloc(size != 0 ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return operator new(size, tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif  // !_MSC_VER

namespace {

BufferDesc vertex_buffer(uint64_t size, const char* name = "buf")
{
    BufferDesc d;
    d.size = size;
    d.usage = BufferUsage::Vertex;
    d.memory = MemoryLocation::GpuOnly;
    d.debug_name = name;
    return d;
}

TextureDesc sampled_texture(uint32_t w, uint32_t h, uint32_t mips, const char* name = "tex")
{
    TextureDesc d;
    d.format = Format::RGBA8_UNORM;
    d.width = w;
    d.height = h;
    d.mip_levels = mips;
    d.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    d.debug_name = name;
    return d;
}

} // namespace

TEST_CASE("Registry: references share a resource, the last one releases it, and counters follow")
{
    NullDevice device;
    GpuResourceRegistry registry(device);
    CHECK(registry.live_count() == 0);
    {
        BufferRef a = registry.create_buffer(vertex_buffer(1024, "mesh.vertices"));
        REQUIRE(a.valid());
        CHECK(a.use_count() == 1);
        CHECK(a.bytes() == 1024);
        CHECK(a.name() == "mesh.vertices");
        CHECK(device.live_resource_count() == 1);
        {
            BufferRef b = a;  // shared, same device resource
            CHECK(a.use_count() == 2);
            CHECK(b.handle() == a.handle());
            CHECK(device.live_resource_count() == 1);
            BufferRef c = std::move(b);
            CHECK_FALSE(b.valid());
            CHECK(a.use_count() == 2);
            c.reset();
            CHECK(a.use_count() == 1);
        }
        CHECK(registry.stats(ResourceKind::Buffer).live == 1);
        CHECK(registry.stats(ResourceKind::Buffer).live_bytes == 1024);
        a = a;  // self-assignment must not release it
        CHECK(a.use_count() == 1);
        CHECK(a.valid());
    }
    const ResourceStats s = registry.stats(ResourceKind::Buffer);
    CHECK(s.live == 0);
    CHECK(s.live_bytes == 0);
    CHECK(s.created == 1);
    CHECK(s.destroyed == 1);
    CHECK(s.peak_bytes == 1024);
}

TEST_CASE("Registry: releasing the last reference defers destruction to the device until frames retire")
{
    NullDevice device;
    GpuResourceRegistry registry(device);
    BufferRef buffer = registry.create_buffer(vertex_buffer(256));
    (void)device.begin_frame();  // a frame is in flight when the reference goes away
    buffer.reset();
    CHECK(registry.live_count() == 0);                  // no longer referenced...
    CHECK(device.pending_destruction_count() == 1);     // ...but the device still owns it until the GPU is done
    CHECK(device.live_resource_count() == 1);
    (void)device.end_frame();
    device.wait_idle();
    CHECK(device.pending_destruction_count() == 0);
    CHECK(device.live_resource_count() == 0);
    CHECK(device.validation_error_count() == 0);
}

TEST_CASE("Registry: textures are sized by their full mip chain and can be created with data")
{
    NullDevice device;
    GpuResourceRegistry registry(device);
    const TextureDesc desc = sampled_texture(13, 7, full_mip_chain);
    const uint64_t expected = compute_texture_data_size(desc);
    std::vector<std::byte> data(expected);
    TextureRef tex = registry.create_texture(desc, data);
    CHECK(tex.bytes() == expected);
    CHECK(device.uploaded_bytes(tex.handle()) == expected);
    CHECK(registry.stats(ResourceKind::Texture).live_bytes == expected);

    // Wrong data size: the device rejects the upload, so the texture is destroyed and nothing is registered.
    const uint64_t created_before = registry.stats(ResourceKind::Texture).created;
    bool threw = false;
    try {
        (void)registry.create_texture(desc, std::span<const std::byte>(data).first(expected - 1));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(registry.stats(ResourceKind::Texture).created == created_before);
    CHECK(registry.stats(ResourceKind::Texture).live == 1);
    CHECK(device.pending_destruction_count() == 1);  // the failed texture was handed back to the device
}

TEST_CASE("Registry: invalid descriptions throw and register nothing")
{
    NullDevice device;
    GpuResourceRegistry registry(device);
    BufferDesc bad;  // size 0
    bool threw = false;
    try {
        (void)registry.create_buffer(bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(registry.live_count() == 0);
    CHECK(registry.total_stats().created == 0);
}

TEST_CASE("Registry: identical sampler descriptions share one sampler, different ones do not")
{
    NullDevice device;
    GpuResourceRegistry registry(device);
    SamplerDesc linear;
    SamplerDesc nearest;
    nearest.min_filter = nearest.mag_filter = Filter::Nearest;
    SamplerRef a = registry.get_sampler(linear);
    SamplerRef b = registry.get_sampler(linear);
    SamplerRef c = registry.get_sampler(nearest);
    CHECK(a.handle() == b.handle());
    CHECK(a.handle() != c.handle());
    CHECK(a.use_count() == 2);
    CHECK(device.live_resource_count() == 2);  // two samplers on the device, not three
    CHECK(registry.stats(ResourceKind::Sampler).live == 2);

    a.reset();
    b.reset();  // the last reference to the linear sampler
    CHECK(registry.stats(ResourceKind::Sampler).live == 1);
    SamplerRef again = registry.get_sampler(linear);  // a fresh sampler, not a stale one
    CHECK(again.valid());
    CHECK(again.use_count() == 1);
    CHECK(registry.stats(ResourceKind::Sampler).created == 3);
}

TEST_CASE("Registry: slots are reused after churn without stale references leaking through")
{
    NullDevice device;
    GpuResourceRegistry registry(device);
    BufferRef keeper = registry.create_buffer(vertex_buffer(64, "keeper"));
    for (int i = 0; i < 500; ++i) {
        BufferRef temp = registry.create_buffer(vertex_buffer(32, "temp"));
        BufferRef copy = temp;
        CHECK(temp.use_count() == 2);
    }
    CHECK(registry.live_count() == 1);
    CHECK(keeper.use_count() == 1);
    CHECK(keeper.name() == "keeper");
    CHECK(registry.stats(ResourceKind::Buffer).created == 501);
    CHECK(registry.stats(ResourceKind::Buffer).destroyed == 500);
    CHECK(registry.live_resources().size() == 1);
}

TEST_CASE("Registry: peak bytes are the peak of the total across kinds, not the sum of per-kind peaks")
{
    NullDevice device;
    GpuResourceRegistry registry(device);
    {
        BufferRef b = registry.create_buffer(vertex_buffer(1000));
        CHECK(registry.total_stats().live_bytes == 1000);
    }
    {
        TextureRef t = registry.create_texture(sampled_texture(4, 4, 1));  // 64 bytes, after the buffer is gone
        CHECK(registry.total_stats().live_bytes == 64);
    }
    BufferRef small = registry.create_buffer(vertex_buffer(10));
    TextureRef small_tex = registry.create_texture(sampled_texture(2, 2, 1));  // 16 bytes
    const ResourceStats total = registry.total_stats();
    CHECK(total.live_bytes == 26);
    // The buffer (1000) and the texture (64) were never alive together, so the real peak is 1000. The sum of the per-kind
    // peaks would be 1064.
    CHECK(total.peak_bytes == 1000);
    CHECK(registry.stats(ResourceKind::Buffer).peak_bytes == 1000);
    CHECK(registry.stats(ResourceKind::Texture).peak_bytes == 64);
}

TEST_CASE("Registry: destroying it with references outstanding reports the leaks, and late releases are harmless")
{
    NullDevice device;
    std::string report;
    BufferRef leaked_buffer;
    TextureRef leaked_texture;
    {
        GpuResourceRegistry registry(device);
        registry.set_leak_sink([&](std::string_view message) { report = std::string(message); });
        leaked_buffer = registry.create_buffer(vertex_buffer(2048, "scene.mesh.vertices"));
        leaked_texture = registry.create_texture(sampled_texture(8, 8, 1, "scene.checker"));
        BufferRef copy = leaked_buffer;  // two references to the buffer
        CHECK(registry.leak_report().find("scene.mesh.vertices") != std::string::npos);
    }
    CHECK(report.find("2 resource(s) still referenced") != std::string::npos);
    CHECK(report.find("Buffer 'scene.mesh.vertices', 2048 bytes, 1 reference(s)") != std::string::npos);
    CHECK(report.find("Texture 'scene.checker', 256 bytes, 1 reference(s)") != std::string::npos);
    // The registry destroyed them on the device (so the device does not also report a leak) ...
    CHECK(device.pending_destruction_count() == 2);
    // ... and the surviving references are inert: reading and releasing them is safe.
    CHECK(leaked_buffer.use_count() == 0);
    CHECK(leaked_buffer.bytes() == 0);
    leaked_buffer.reset();
    leaked_texture.reset();
    device.wait_idle();
    CHECK(device.live_resource_count() == 0);
    CHECK(device.validation_error_count() == 0);
}

TEST_CASE("Registry: a clean shutdown reports nothing")
{
    NullDevice device;
    bool reported = false;
    {
        GpuResourceRegistry registry(device);
        registry.set_leak_sink([&](std::string_view) { reported = true; });
        BufferRef b = registry.create_buffer(vertex_buffer(16));
        SamplerRef s = registry.get_sampler({});
    }
    CHECK_FALSE(reported);
}

namespace {

/// Runs `op` with the n-th registry bookkeeping step failing, for n = 1, 2, ... until `op` completes. After every failed
/// attempt `verify` must hold (nothing leaked, nothing half-registered). Returns how many steps the successful run passed.
template <typename Op, typename Verify>
std::size_t for_each_bookkeeping_failure(Op&& op, Verify&& verify)
{
    for (uint32_t n = 1; n < 64; ++n) {
        saige::render::testing::fail_registry_bookkeeping_after(n);
        bool failed = false;
        try {
            op();
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        saige::render::testing::fail_registry_bookkeeping_after(0);
        if (!failed) {
            return n - 1;
        }
        verify();
    }
    return 0;
}

} // namespace

TEST_CASE("Registry: a bookkeeping failure during registration leaves the device and the registry untouched")
{
    const std::string long_name = "a debug name too long for the small-string buffer";
    SamplerDesc unusual;
    unusual.max_anisotropy = 4.0f;

    // One phase: fail every bookkeeping step of `op` in turn; each failed attempt must leave device and registry as they were.
    const auto phase = [&](NullDevice& device, GpuResourceRegistry& registry, auto&& op) {
        device.wait_idle();
        const uint32_t device_before = device.live_resource_count();
        const uint32_t live_before = registry.live_count();
        const std::size_t made = for_each_bookkeeping_failure(op, [&] {
            CHECK(device.live_resource_count() == device_before);
            CHECK(registry.live_count() == live_before);
            CHECK(device.validation_error_count() == 0);
        });
        CHECK(made > 0);  // the operation has bookkeeping steps, so there was something to inject into
    };

    // A fresh registry: the first registration has to allocate the slot tables, the hardest place to fail.
    {
        NullDevice device;
        GpuResourceRegistry registry(device);
        BufferRef buffer;
        phase(device, registry, [&] { buffer = registry.create_buffer(vertex_buffer(256, long_name.c_str())); });
        CHECK(buffer.valid());
        CHECK(buffer.name() == long_name);
    }
    {
        NullDevice device;
        GpuResourceRegistry registry(device);
        TextureRef texture;
        phase(device, registry, [&] { texture = registry.create_texture(sampled_texture(4, 4, 1, long_name.c_str())); });
        CHECK(texture.valid());
    }
    {
        NullDevice device;
        GpuResourceRegistry registry(device);
        SamplerRef sampler;
        phase(device, registry, [&] { sampler = registry.get_sampler(unusual); });
        CHECK(sampler.valid());
    }
    // A churned registry: slots are reused and the free list is non-empty.
    {
        NullDevice device;
        GpuResourceRegistry registry(device);
        std::vector<BufferRef> keep;
        for (int i = 0; i < 6; ++i) keep.push_back(registry.create_buffer(vertex_buffer(64)));
        keep.erase(keep.begin() + 1);
        keep.erase(keep.begin() + 2);
        BufferRef buffer;
        phase(device, registry, [&] { buffer = registry.create_buffer(vertex_buffer(256, long_name.c_str())); });
        CHECK(buffer.valid());
        SamplerRef sampler;
        phase(device, registry, [&] { sampler = registry.get_sampler(unusual); });
        CHECK(sampler.valid());
    }
}

#if !defined(_MSC_VER)
TEST_CASE("Registry: releasing references allocates nothing beyond the device's own destruction")
{
    constexpr int count = 40;
    const auto arm = [] {
        alloc_probe::count = 0;
        alloc_probe::armed = true;
    };

    // Baseline: what the device itself allocates when the same resources are destroyed directly (its deferred queue).
    std::size_t device_only = 0;
    {
        NullDevice device;
        std::vector<BufferHandle> buffers;
        std::vector<TextureHandle> textures;
        for (int i = 0; i < count; ++i) {
            buffers.push_back(device.create_buffer(vertex_buffer(64)));
            textures.push_back(device.create_texture(sampled_texture(2, 2, 1)));
        }
        const SamplerHandle sampler = device.create_sampler(SamplerDesc{});
        device.wait_idle();
        arm();
        for (const BufferHandle b : buffers) device.destroy(b);
        for (const TextureHandle t : textures) device.destroy(t);
        device.destroy(sampler);
        device_only = alloc_probe::count;
        alloc_probe::armed = false;
    }

    NullDevice device;
    GpuResourceRegistry registry(device);
    std::vector<BufferRef> buffers;
    std::vector<TextureRef> textures;
    for (int i = 0; i < count; ++i) {
        buffers.push_back(registry.create_buffer(vertex_buffer(64)));
        textures.push_back(registry.create_texture(sampled_texture(2, 2, 1)));
    }
    SamplerRef sampler = registry.get_sampler(SamplerDesc{});
    std::vector<BufferRef> copies = buffers;  // shared references: only the last release destroys
    device.wait_idle();

    arm();
    copies.clear();                          // not the last references: retains and releases only
    for (BufferRef& b : buffers) b.reset();  // last references: device destruction and slot recycling
    textures.clear();
    sampler.reset();
    const std::size_t through_registry = alloc_probe::count;
    alloc_probe::armed = false;
    CHECK(through_registry == device_only);  // the registry's own bookkeeping contributed no allocation
    CHECK(registry.live_count() == 0);
    CHECK(registry.total_stats().destroyed == 2 * count + 1);
}
#else
TEST_CASE("Registry: releasing references allocates nothing beyond the device's own destruction (not run on MSVC)")
{
    // Counting needs a replacement of the global allocation functions. MSVC's checked iterators allocate container proxies in
    // places the standard does not, and the debug CRT keeps its own heap bookkeeping, so the count is not a property of the
    // registry there. The check runs on the Linux sanitizer and Vulkan lanes; this line keeps its absence visible here.
    std::printf("  skipped on MSVC: allocation counting (covered by the other platforms)\n");
}
#endif
