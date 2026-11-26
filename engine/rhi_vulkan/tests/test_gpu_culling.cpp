// GPU frustum culling of instances on a real (validated) Vulkan device. CI: lavapipe. The compute pass must keep exactly the instances the CPU's `intersects_sphere` keeps (compared as sets of
// matrices, since the compaction order is undefined), fill the indirect draw arguments, and feed an indirect draw that gives the same image as drawing the CPU-culled list directly.
// Instances whose sphere lies within a hair of a frustum plane are left out of the random scene: the GPU and the CPU may round the last bit differently there, and that is not what is being tested.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/instance_culler.hpp"
#include "saige/shaders/test_cull_draw_probe.spv.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

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

using Row = std::array<float, 12>;

Row rows_of(const Mat4& m)
{
    Row r{};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 4; ++c) r[static_cast<size_t>(i * 4 + c)] = m.at(i, c);
    return r;
}

float max_axis_scale(const Mat4& w)
{
    float s = 0.0f;
    for (int col = 0; col < 3; ++col) s = std::max(s, std::sqrt(w.at(0, col) * w.at(0, col) + w.at(1, col) * w.at(1, col) + w.at(2, col) * w.at(2, col)));
    return s;
}

Float3 sphere_center(const Mat4& w, Float3 local)
{
    return {w.at(0, 0) * local.x + w.at(0, 1) * local.y + w.at(0, 2) * local.z + w.at(0, 3), w.at(1, 0) * local.x + w.at(1, 1) * local.y + w.at(1, 2) * local.z + w.at(1, 3),
            w.at(2, 0) * local.x + w.at(2, 1) * local.y + w.at(2, 2) * local.z + w.at(2, 3)};
}

/// The CPU answer: what ModelRenderer's frustum culling keeps.
bool cpu_visible(const Frustum& f, const Mat4& w, const CullSphere& s)
{
    if (!(s.radius > 0.0f)) return true;
    return intersects_sphere(f, sphere_center(w, s.center), s.radius * max_axis_scale(w));
}

/// The smallest signed margin of the sphere over the planes: |margin| tiny means "on the edge of the frustum".
float edge_margin(const Frustum& f, const Mat4& w, const CullSphere& s)
{
    const Float3 c = sphere_center(w, s.center);
    const float r = s.radius * max_axis_scale(w);
    float worst = std::numeric_limits<float>::max();
    for (const Plane& p : f.planes) {
        if (p.normal.x == 0.0f && p.normal.y == 0.0f && p.normal.z == 0.0f) continue;  // the degenerate far plane
        worst = std::min(worst, std::fabs(p.distance(c) + r));
    }
    return worst;
}

struct Lcg {
    uint32_t state;
    float next() { state = state * 1664525u + 1013904223u; return static_cast<float>(state >> 8) / 16777216.0f; }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

Mat4 rotation_about_z(float a)
{
    Mat4 m = Mat4::identity();
    m.at(0, 0) = std::cos(a);
    m.at(0, 1) = -std::sin(a);
    m.at(1, 0) = std::sin(a);
    m.at(1, 1) = std::cos(a);
    return m;
}

/// `count` random instances in a box around the origin, with rotations, non-uniform scales (some mirrored), none within `margin` of a plane of `f`.
std::vector<Mat4> random_instances(uint32_t count, const Frustum& f, const CullSphere& s, float lo, float hi, uint32_t seed, float margin = 0.03f)
{
    Lcg rng{seed};
    std::vector<Mat4> out;
    while (out.size() < count) {
        const Float3 t{rng.range(lo, hi), rng.range(lo, hi), rng.range(-2.0f, 4.0f)};
        const float sx = rng.range(0.4f, 1.6f) * (rng.next() < 0.2f ? -1.0f : 1.0f), sy = rng.range(0.4f, 1.6f), sz = rng.range(0.4f, 1.6f);
        const Mat4 w = translation(t) * rotation_about_z(rng.range(0.0f, 6.28f)) * scale_matrix({sx, sy, sz});
        if (edge_margin(f, w, s) < margin) continue;
        out.push_back(w);
    }
    return out;
}

Frustum camera_frustum()
{
    const Camera c = look_at_camera({0.0f, 0.0f, 1.0f}, {10.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, 1.0f);
    return extract_frustum(view_projection_matrix(c, 16.0f / 9.0f));
}

/// Six planes that keep the box |x| <= 5, |y| <= 5 (near and far always inside).
Frustum box_frustum()
{
    Frustum f;
    f.planes[0] = {{1, 0, 0}, 5.0f};
    f.planes[1] = {{-1, 0, 0}, 5.0f};
    f.planes[2] = {{0, 1, 0}, 5.0f};
    f.planes[3] = {{0, -1, 0}, 5.0f};
    f.planes[4] = {{0, 0, 0}, 1.0f};
    f.planes[5] = {{0, 0, 0}, 1.0f};
    return f;
}

Frustum always_inside()
{
    Frustum f;
    for (Plane& p : f.planes) p = {{0, 0, 0}, 1.0f};
    return f;
}

Frustum always_outside()
{
    Frustum f;
    for (Plane& p : f.planes) p = {{0, 0, 1}, -1.0e6f};
    return f;
}

struct Rig {
    std::unique_ptr<rhi::Device> device = make_validated_device("gpu_culling", true);
    std::unique_ptr<GpuResourceRegistry> registry = std::make_unique<GpuResourceRegistry>(*device);
    Rig() = default;
    ~Rig()
    {
        device->wait_idle();
        registry.reset();
    }
};

/// Culls on the GPU and waits; returns the compacted rows (sorted for set comparison) and the arguments.
struct GpuResult {
    rhi::DrawIndexedIndirectCommand args;
    std::vector<Row> rows;
};
GpuResult gpu_cull(Rig& rig, const Frustum& f, const std::vector<Mat4>& worlds, const CullSphere& s, uint32_t index_count = 36, uint32_t capacity = 4096)
{
    InstanceCullerSettings st;
    st.max_instances = capacity;
    st.host_visible_outputs = true;
    InstanceCuller culler(*rig.device, *rig.registry, st);
    rhi::CommandList& cmd = rig.device->begin_frame();
    culler.cull(cmd, f, worlds, s, index_count);
    (void)rig.device->end_frame();
    rig.device->wait_idle();
    GpuResult r;
    r.args = culler.arguments_after_wait();
    r.rows = culler.visible_rows_after_wait();
    std::sort(r.rows.begin(), r.rows.end());
    return r;
}

std::vector<Row> cpu_rows(const Frustum& f, const std::vector<Mat4>& worlds, const CullSphere& s)
{
    std::vector<Row> out;
    for (const Mat4& w : worlds)
        if (cpu_visible(f, w, s)) out.push_back(rows_of(w));
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("GPU culling: the compacted set is exactly what the CPU frustum test keeps, and the arguments carry the count")
{
    Rig rig;
    const Frustum f = camera_frustum();
    const CullSphere s{{0.1f, 0.0f, 0.2f}, 0.9f};
    const std::vector<Mat4> worlds = random_instances(600, f, s, -40.0f, 40.0f, 12345u);
    const std::vector<Row> expected = cpu_rows(f, worlds, s);
    CHECK(expected.size() > 30);   // the scene has visible instances...
    CHECK(expected.size() < 400);  // ...and culls a good share
    const GpuResult g = gpu_cull(rig, f, worlds, s);
    CHECK(g.args.instance_count == expected.size());
    CHECK(g.args.index_count == 36);
    CHECK(g.args.first_index == 0);
    CHECK(g.args.vertex_offset == 0);
    CHECK(g.args.first_instance == 0);
    CHECK(g.rows == expected);
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("GPU culling: edge cases (nothing, everything, a radius of zero, a partial workgroup)")
{
    Rig rig;
    const CullSphere s{{0, 0, 0}, 1.0f};
    const std::vector<Mat4> worlds = random_instances(130, camera_frustum(), s, -30.0f, 30.0f, 7u);  // 130 = two workgroups and two threads
    // Nothing recorded for zero instances, but the arguments are initialised to an empty draw.
    GpuResult none = gpu_cull(rig, camera_frustum(), {}, s, 24);
    CHECK(none.args.index_count == 24);
    CHECK(none.args.instance_count == 0);
    CHECK(none.rows.empty());
    // Everything inside / everything outside.
    const GpuResult all = gpu_cull(rig, always_inside(), worlds, s);
    CHECK(all.args.instance_count == worlds.size());
    std::vector<Row> every;
    for (const Mat4& w : worlds) every.push_back(rows_of(w));
    std::sort(every.begin(), every.end());
    CHECK(all.rows == every);
    CHECK(gpu_cull(rig, always_outside(), worlds, s).args.instance_count == 0);
    // A radius of zero is never culled, even when every plane rejects.
    const GpuResult unbounded = gpu_cull(rig, always_outside(), worlds, CullSphere{{0, 0, 0}, 0.0f});
    CHECK(unbounded.args.instance_count == worlds.size());
    CHECK(unbounded.rows == every);
    // Exactly one full workgroup, and the capacity exactly.
    const std::vector<Mat4> sixty_four(worlds.begin(), worlds.begin() + 64);
    CHECK(gpu_cull(rig, always_inside(), sixty_four, s).args.instance_count == 64);
    CHECK(gpu_cull(rig, always_inside(), worlds, s, 36, 130).args.instance_count == 130);
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("GPU culling: the scale of an instance grows its sphere, the same as on the CPU")
{
    Rig rig;
    // A sphere at x = 6 (just outside the |x| <= 5 box) is culled at unit scale and kept when scaled up enough for its sphere to reach the box.
    const Frustum f = box_frustum();
    const CullSphere s{{0, 0, 0}, 1.0f};
    const std::vector<Mat4> worlds{translation({6.5f, 0, 0}), translation({6.5f, 0, 0}) * scale_matrix({3.0f, 1.0f, 1.0f}), translation({6.5f, 0, 0}) * scale_matrix({1.0f, 1.0f, 3.0f}),
                                   translation({0, 0, 0}) * scale_matrix({0.2f, 0.2f, 0.2f})};
    CHECK(gpu_cull(rig, f, worlds, s).rows == cpu_rows(f, worlds, s));
    CHECK(cpu_rows(f, worlds, s).size() == 3);  // the unit one is culled; the two scaled ones (largest axis scale 3) and the small central one are kept
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("GPU culling: an indirect draw of the compacted instances gives the image of drawing the CPU-culled list directly")
{
    Rig rig;
    const Frustum f = box_frustum();
    const CullSphere s{{0, 0, 0}, 0.5f};
    // Small triangles scattered over [-9, 9]^2: the box keeps roughly a third of them.
    std::vector<Mat4> worlds = random_instances(300, f, s, -9.0f, 9.0f, 99u, 0.05f);
    for (Mat4& w : worlds) {  // keep the instances small and unmirrored so triangles are easy to see; the cull test above covers mirrors and scales
        const Float3 t{w.at(0, 3), w.at(1, 3), 0.0f};
        w = translation(t);
    }
    constexpr uint32_t kSize = 128;
    const auto render = [&](bool gpu_path, bool cull_at_all) {
        auto& device = *rig.device;
        std::vector<std::function<void()>> cleanup;
        const auto own = [&](auto h) {
            cleanup.push_back([&device, h] { device.destroy(h); });
            return h;
        };
        const rhi::ShaderHandle vs = own(device.create_shader({rhi::ShaderStage::Vertex, words(saige::shaders::test_cull_draw_probe_vs_main), "VSMain", "CullProbeVS"}));
        const rhi::ShaderHandle ps = own(device.create_shader({rhi::ShaderStage::Pixel, words(saige::shaders::test_cull_draw_probe_ps_main), "PSMain", "CullProbePS"}));
        rhi::BindGroupLayoutDesc ld;
        rhi::BindGroupLayoutEntry e;
        e.binding = 0;
        e.type = rhi::BindingType::StorageBuffer;
        e.visibility = rhi::StageMask::Vertex;
        ld.entries = {e};
        const rhi::BindGroupLayoutHandle layout = own(device.create_bind_group_layout(ld));
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs;
        pd.pixel_shader = ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {rhi::Format::RGBA8_UNORM};
        pd.vertex_layout.stride = 8;
        pd.vertex_layout.attributes = {{0, rhi::Format::RG32_FLOAT, 0}};
        pd.bind_group_layouts = {layout};
        const rhi::PipelineHandle pipeline = own(device.create_graphics_pipeline(pd));
        const float tri[6] = {-0.4f, -0.4f, 0.4f, -0.4f, 0.0f, 0.4f};
        rhi::BufferDesc bd;
        bd.size = sizeof(tri);
        bd.usage = rhi::BufferUsage::Vertex;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        const rhi::BufferHandle vb = own(device.create_buffer(bd, std::as_bytes(std::span(tri))));
        const uint16_t idx[3] = {0, 1, 2};
        bd.size = sizeof(idx);
        bd.usage = rhi::BufferUsage::Index;
        const rhi::BufferHandle ib = own(device.create_buffer(bd, std::as_bytes(std::span(idx))));
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = td.height = kSize;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        const rhi::TextureHandle target = own(device.create_texture(td));
        rhi::BufferDesc rb;
        rb.size = uint64_t{kSize} * kSize * 4;
        rb.usage = rhi::BufferUsage::TransferDst;
        rb.memory = rhi::MemoryLocation::GpuToCpu;
        const rhi::BufferHandle readback = own(device.create_buffer(rb));

        InstanceCullerSettings st;
        st.max_instances = 1024;
        st.host_visible_outputs = true;
        InstanceCuller culler(device, *rig.registry, st);
        // The CPU-side alternatives: the visible list written to a host-visible buffer (or the whole list).
        std::vector<Row> cpu_list;
        for (const Mat4& w : worlds)
            if (!cull_at_all || cpu_visible(f, w, s)) cpu_list.push_back(rows_of(w));
        rhi::BufferDesc cb;
        cb.size = std::max<size_t>(256, cpu_list.size() * 48);
        cb.usage = rhi::BufferUsage::Storage;
        cb.memory = rhi::MemoryLocation::CpuToGpu;
        const rhi::BufferHandle cpu_rows_buffer = own(device.create_buffer(cb, std::as_bytes(std::span<const Row>(cpu_list))));
        rhi::BindGroupEntry ge;
        ge.binding = 0;
        ge.buffer = gpu_path ? culler.rows() : cpu_rows_buffer;
        ge.size = std::max<uint64_t>(48, gpu_path ? uint64_t{1024} * 48u : cpu_list.size() * 48u);
        const rhi::BindGroupHandle group = own(device.create_bind_group(layout, std::span(&ge, 1)));

        rhi::CommandList& cmd = device.begin_frame();
        if (gpu_path) culler.cull(cmd, f, worlds, s, 3);
        const rhi::TextureBarrier to_attachment{target, {}, {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment}, {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target;
        ca.clear = {0, 0, 0, 1};
        rd.colors.push_back(ca);
        rd.area = {0, 0, kSize, kSize};
        cmd.begin_rendering(rd);
        cmd.set_viewport({0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1});
        cmd.set_scissor({0, 0, kSize, kSize});
        cmd.bind_pipeline(pipeline);
        cmd.bind_group(0, group);
        cmd.bind_vertex_buffer(vb);
        cmd.bind_index_buffer(ib, rhi::IndexType::Uint16);
        if (gpu_path) {
            cmd.draw_indexed_indirect(culler.arguments(), 0, 1, 0);
        } else {
            cmd.draw_indexed(3, static_cast<uint32_t>(cpu_list.size()));
        }
        cmd.end_rendering();
        const rhi::TextureBarrier to_transfer{target, to_attachment.after, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(target, readback);
        (void)device.end_frame();
        device.wait_idle();
        const std::span<std::byte> bytes = device.map(readback);
        std::vector<uint8_t> px(bytes.size());
        std::memcpy(px.data(), bytes.data(), px.size());
        for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) (*it)();
        return px;
    };
    const std::vector<uint8_t> gpu_image = render(true, true);
    const std::vector<uint8_t> cpu_image = render(false, true);
    const std::vector<uint8_t> everything = render(false, false);
    size_t red = 0, differing_from_all = 0;
    for (size_t i = 0; i < gpu_image.size(); i += 4) {
        red += gpu_image[i] > 200 ? 1u : 0u;
        differing_from_all += gpu_image[i] != everything[i] ? 1u : 0u;
    }
    CHECK(gpu_image == cpu_image);          // the indirect draw of the GPU-culled instances is the CPU-culled draw
    CHECK(red > 100);                        // the picture shows something
    CHECK(differing_from_all > 100);         // and culling changed it relative to drawing everything
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("GPU culling: invalid use is rejected before anything is recorded")
{
    Rig rig;
    InstanceCullerSettings small;
    small.max_instances = 4;
    InstanceCuller culler(*rig.device, *rig.registry, small);
    const std::vector<Mat4> five(5, Mat4::identity());
    rhi::CommandList& cmd = rig.device->begin_frame();
    CHECK(throws_invalid_argument([&] { culler.cull(cmd, always_inside(), five, CullSphere{{0, 0, 0}, 1.0f}, 3); }));
    CHECK(throws_invalid_argument([&] { culler.cull(cmd, always_inside(), std::span<const Mat4>(five.data(), 2), CullSphere{{0, 0, 0}, -1.0f}, 3); }));
    CHECK(throws_invalid_argument([&] { culler.cull(cmd, always_inside(), std::span<const Mat4>(five.data(), 2), CullSphere{{0, 0, 0}, std::nanf("")}, 3); }));
    CHECK(throws_invalid_argument([&] { culler.cull(cmd, always_inside(), std::span<const Mat4>(five.data(), 2), CullSphere{{std::numeric_limits<float>::infinity(), 0, 0}, 1.0f}, 3); }));
    culler.cull(cmd, always_inside(), std::span<const Mat4>(five.data(), 4), CullSphere{{0, 0, 0}, 1.0f}, 3);  // a valid cull still works, at exactly the capacity
    (void)rig.device->end_frame();
    rig.device->wait_idle();
    CHECK(culler.arguments_after_wait().instance_count == 4);
    bool logic = false;
    try {
        (void)culler.visible_rows_after_wait();  // the outputs were not made host visible
    } catch (const std::logic_error&) {
        logic = true;
    }
    CHECK(logic);
    bool bad_settings = false;
    try {
        InstanceCullerSettings bad;
        bad.max_instances = 0;
        InstanceCuller c(*rig.device, *rig.registry, bad);
    } catch (const std::invalid_argument&) {
        bad_settings = true;
    }
    CHECK(bad_settings);
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("GPU culling: each cull starts from an empty draw, so a second cull on the same culler does not accumulate")
{
    Rig rig;
    InstanceCullerSettings st;
    st.max_instances = 1024;
    st.host_visible_outputs = true;
    InstanceCuller culler(*rig.device, *rig.registry, st);
    const CullSphere s{{0, 0, 0}, 1.0f};
    const Frustum f = camera_frustum();
    const std::vector<Mat4> many = random_instances(300, f, s, -30.0f, 30.0f, 5u);
    const std::vector<Mat4> few(many.begin(), many.begin() + 40);
    for (const std::vector<Mat4>* list : {&many, &few, &many}) {
        rhi::CommandList& cmd = rig.device->begin_frame();
        culler.cull(cmd, f, *list, s, 36);
        (void)rig.device->end_frame();
        rig.device->wait_idle();
        CHECK(culler.arguments_after_wait().instance_count == cpu_rows(f, *list, s).size());
        CHECK(culler.arguments_after_wait().index_count == 36);
    }
    CHECK(rig.device->validation_error_count() == 0);
}
