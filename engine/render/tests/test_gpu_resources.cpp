#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/gpu_resources.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace saige::render;
using namespace saige::rhi;

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

struct GroupFixture {
    NullDevice device;
    GpuResourceRegistry registry{device};
    BindGroupLayoutHandle layout;

    GroupFixture()
    {
        BindGroupLayoutDesc ld;
        ld.entries = {{0, BindingType::SampledTexture}, {1, BindingType::Sampler}, {2, BindingType::UniformBuffer}};
        layout = device.create_bind_group_layout(ld);
    }

    BufferRef uniform()
    {
        BufferDesc ub;
        ub.size = 256;
        ub.usage = BufferUsage::Uniform;
        ub.memory = MemoryLocation::CpuToGpu;
        ub.debug_name = "material.params";
        return registry.create_buffer(ub);
    }

    std::vector<BindGroupBinding> bindings(const TextureRef& t, const SamplerRef& s, const BufferRef& b)
    {
        std::vector<BindGroupBinding> out(3);
        out[0].binding = 0;
        out[0].texture = t;
        out[1].binding = 1;
        out[1].sampler = s;
        out[2].binding = 2;
        out[2].buffer = b;
        out[2].size = 32;
        return out;
    }
};

} // namespace

TEST_CASE("Registry bind groups: a group keeps what it binds alive and releases it after itself")
{
    GroupFixture f;
    BindGroupRef group;
    {
        TextureRef tex = f.registry.create_texture(sampled_texture(4, 4, 1, "material.base_color"));
        SamplerRef sampler = f.registry.get_sampler({});
        BufferRef params = f.uniform();
        group = f.registry.create_bind_group(f.layout, f.bindings(tex, sampler, params), "material");
        CHECK(tex.use_count() == 2);      // the caller's and the group's
        CHECK(sampler.use_count() == 2);
        CHECK(params.use_count() == 2);
        CHECK(group.use_count() == 1);
    }  // the caller lets go of everything
    CHECK(f.registry.stats(ResourceKind::Texture).live == 1);  // ...but the group still holds the texture
    CHECK(f.registry.stats(ResourceKind::Sampler).live == 1);
    CHECK(f.registry.stats(ResourceKind::Buffer).live == 1);
    CHECK(f.registry.stats(ResourceKind::BindGroup).live == 1);
    CHECK(f.device.pending_destruction_count() == 0);

    BindGroupRef copy = group;
    group.reset();
    CHECK(f.registry.stats(ResourceKind::BindGroup).live == 1);  // another reference to the group remains
    copy.reset();                                                // the last one
    CHECK(f.registry.live_count() == 0);                         // group, texture, sampler and buffer all released
    CHECK(f.device.pending_destruction_count() == 4);
    f.device.wait_idle();
    CHECK(f.device.live_resource_count() == 1);  // only the layout (owned by the test, not the registry)
    CHECK(f.device.validation_error_count() == 0);
}

TEST_CASE("Registry bind groups: resources bound by several groups are shared and outlive all of them")
{
    GroupFixture f;
    TextureRef shared_texture = f.registry.create_texture(sampled_texture(4, 4, 1, "shared.texture"));
    SamplerRef sampler = f.registry.get_sampler({});
    BindGroupRef a = f.registry.create_bind_group(f.layout, f.bindings(shared_texture, sampler, f.uniform()), "a");
    BindGroupRef b = f.registry.create_bind_group(f.layout, f.bindings(shared_texture, sampler, f.uniform()), "b");
    CHECK(shared_texture.use_count() == 3);
    shared_texture.reset();
    a.reset();
    CHECK(f.registry.stats(ResourceKind::Texture).live == 1);  // group b still binds it
    b.reset();
    CHECK(f.registry.stats(ResourceKind::Texture).live == 0);
    CHECK(sampler.use_count() == 1);                           // only the caller's reference is left
}

TEST_CASE("Registry bind groups: a failed creation holds nothing, and foreign references are rejected")
{
    GroupFixture f;
    TextureRef tex = f.registry.create_texture(sampled_texture(4, 4, 1));
    SamplerRef sampler = f.registry.get_sampler({});
    BufferRef params = f.uniform();
    auto missing_buffer = f.bindings(tex, sampler, params);
    missing_buffer[2].buffer = {};  // the layout needs a buffer here
    const uint32_t tex_refs = tex.use_count();  // includes the copies held by the vector above
    const uint32_t sampler_refs = sampler.use_count();
    bool threw = false;
    try {
        (void)f.registry.create_bind_group(f.layout, missing_buffer);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(tex.use_count() == tex_refs);  // nothing was retained by the failed attempt
    CHECK(sampler.use_count() == sampler_refs);
    CHECK(f.registry.stats(ResourceKind::BindGroup).created == 0);

    // A reference from another registry cannot be bound: it would be released against the wrong table.
    GpuResourceRegistry other(f.device);
    TextureRef foreign = other.create_texture(sampled_texture(4, 4, 1, "foreign"));
    threw = false;
    try {
        (void)f.registry.create_bind_group(f.layout, f.bindings(foreign, sampler, params));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(foreign.use_count() == 1);
}

TEST_CASE("Registry bind groups: a leak report names the group")
{
    NullDevice device;
    std::string report;
    BindGroupRef kept;
    {
        GpuResourceRegistry registry(device);
        registry.set_leak_sink([&](std::string_view m) { report = std::string(m); });
        BindGroupLayoutDesc ld;
        ld.entries = {{0, BindingType::SampledTexture}};
        const BindGroupLayoutHandle layout = device.create_bind_group_layout(ld);
        std::vector<BindGroupBinding> b(1);
        b[0].binding = 0;
        b[0].texture = registry.create_texture(sampled_texture(4, 4, 1, "leaky.texture"));
        kept = registry.create_bind_group(layout, b, "leaky.material");
    }
    CHECK(report.find("BindGroup 'leaky.material'") != std::string::npos);
    CHECK(report.find("Texture 'leaky.texture'") != std::string::npos);  // held by the group, reported too
    kept.reset();  // inert after teardown
}
