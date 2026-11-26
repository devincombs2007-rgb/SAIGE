// Clustered light culling on a real Vulkan device (shaders/scene/cull_lights.hlsl) against the CPU reference (render_reference light_culling). The lights come from the host class that validates and packs them
// (render::LocalLightSet), so the bytes the shader reads are the bytes the engine writes. What is asserted:
//   * no false negative: every light whose exact (double) sphere meets a cluster's box is listed in that cluster (a missing light is a visible hole in the lighting);
//   * no stray light: everything listed meets the box once the radius is widened by 0.1% + 1 mm (float rounding at the boundary);
//   * the lists are ascending and the truncation and overflow count follow the contract: the truncated list is the first entries of the full one, and listed + overflow = the lights that met the cluster;
//   * zero lights, a cluster count that is not a multiple of the workgroup size (nothing is written beyond it).
// CI: lavapipe, so correctness only: nothing here says how fast culling is.

#include "saige/gfx_test/saige_test.hpp"
#include "saige/render/local_light.hpp"
#include "saige/render_reference/light_culling.hpp"

#include "saige/shaders/scene_cull_lights.spv.hpp"
#include "vulkan_test_util.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
namespace rr = saige::render_reference;

namespace {

template <size_t N>
std::span<const uint32_t> words(const uint32_t (&array)[N])
{
    return {array, N};
}

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

struct Rng {
    uint32_t s = 0xC0FFEEu;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return static_cast<float>(s >> 8) / 16777216.0f; }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

/// A world to view transform (rows of a 3x4 matrix): the camera at (3, -2, 5) turned by `yaw` about the vertical, looking along the view's +z.
struct View {
    float row[3][4];
};

View make_view()
{
    const float yaw = 0.7f, c = std::cos(yaw), s = std::sin(yaw);
    // view x = world right, view y = world up (z), view z = forward in the horizontal plane.
    const std::array<float, 3> right{c, -s, 0.0f}, up{0.0f, 0.0f, 1.0f}, fwd{s, c, 0.0f};
    const std::array<float, 3> eye{3.0f, -2.0f, 5.0f};
    View v{};
    const auto dotv = [](const std::array<float, 3>& a, const std::array<float, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const std::array<std::array<float, 3>, 3> axes{right, up, fwd};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) v.row[r][k] = axes[static_cast<size_t>(r)][static_cast<size_t>(k)];
        v.row[r][3] = -dotv(axes[static_cast<size_t>(r)], eye);
    }
    return v;
}

std::array<double, 3> to_view(const View& v, const float* p)
{
    std::array<double, 3> out{};
    for (int r = 0; r < 3; ++r) out[static_cast<size_t>(r)] = static_cast<double>(v.row[r][0]) * static_cast<double>(p[0]) + static_cast<double>(v.row[r][1]) * static_cast<double>(p[1]) + static_cast<double>(v.row[r][2]) * static_cast<double>(p[2]) + static_cast<double>(v.row[r][3]);
    return out;
}

struct GpuResult {
    std::vector<uint32_t> counts, indices, overflow;
};

/// `custom_boxes` (lo.xyz, pad, hi.xyz, pad per cluster) replaces the grid's boxes (a hand-made box for an exact boundary case).
GpuResult run_gpu(rhi::Device& device, const rr::ClusterGrid& grid, const View& view, const render::LocalLightSet& lights, uint32_t max_per_cluster, const std::vector<float>* custom_boxes = nullptr)
{
    const std::vector<rr::Aabb> boxes = custom_boxes != nullptr ? std::vector<rr::Aabb>() : rr::build_cluster_boxes(grid);
    std::vector<float> box_data(boxes.size() * 8u, 0.0f);
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t a = 0; a < 3; ++a) {
            box_data[i * 8u + a] = static_cast<float>(boxes[i].lo[a]);
            box_data[i * 8u + 4u + a] = static_cast<float>(boxes[i].hi[a]);
        }
    if (custom_boxes != nullptr) box_data = *custom_boxes;
    const uint32_t cluster_count = custom_boxes != nullptr ? static_cast<uint32_t>(box_data.size() / 8u) : static_cast<uint32_t>(grid.count());
    Cleanup own(device);
    const rhi::ShaderHandle cs = own.track(device.create_shader({rhi::ShaderStage::Compute, words(saige::shaders::scene_cull_lights_cs_main), "CSMain", "CullLights"}));
    rhi::BindGroupLayoutDesc ld;
    for (uint32_t i = 0; i < 4; ++i) {
        rhi::BindGroupLayoutEntry e;
        e.binding = i;
        e.type = rhi::BindingType::StorageBuffer;
        e.visibility = rhi::StageMask::Compute;
        ld.entries.push_back(e);
    }
    const rhi::BindGroupLayoutHandle layout = own.track(device.create_bind_group_layout(ld));
    rhi::ComputePipelineDesc pd;
    pd.compute_shader = cs;
    pd.push_constant_size = 64;
    pd.bind_group_layouts = {layout};
    pd.debug_name = "CullLights";
    const rhi::PipelineHandle pipeline = own.track(device.create_compute_pipeline(pd));
    const auto make = [&](std::span<const std::byte> init, uint64_t bytes, const char* name) {
        rhi::BufferDesc bd;
        bd.size = std::max<uint64_t>(bytes, 16);
        bd.usage = rhi::BufferUsage::Storage;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = name;
        std::vector<std::byte> padded(static_cast<size_t>(bd.size), std::byte{0});
        if (!init.empty()) std::memcpy(padded.data(), init.data(), init.size());
        return own.track(device.create_buffer(bd, padded));
    };
    const std::vector<uint32_t> sentinel_summary(static_cast<size_t>(cluster_count) * 2u + 64u, 0xDEADBEEFu), sentinel_indices(static_cast<size_t>(cluster_count) * max_per_cluster + 64u, 0xDEADBEEFu);
    const rhi::BufferHandle lights_b = make(lights.bytes(), lights.bytes().size(), "Lights");
    const rhi::BufferHandle boxes_b = make(std::as_bytes(std::span<const float>(box_data)), box_data.size() * 4u, "Boxes");
    const rhi::BufferHandle summary_b = make(std::as_bytes(std::span<const uint32_t>(sentinel_summary)), sentinel_summary.size() * 4u, "Summary");
    const rhi::BufferHandle indices_b = make(std::as_bytes(std::span<const uint32_t>(sentinel_indices)), sentinel_indices.size() * 4u, "Indices");
    std::vector<rhi::BindGroupEntry> entries(4);
    const rhi::BufferHandle bufs[4] = {lights_b, boxes_b, summary_b, indices_b};
    for (uint32_t i = 0; i < 4; ++i) {
        entries[i].binding = i;
        entries[i].buffer = bufs[i];
    }
    const rhi::BindGroupHandle group = own.track(device.create_bind_group(layout, entries));
    struct Push {
        uint32_t cluster_count, light_count, max_per_cluster, pad;
        float rows[12];
    } push{cluster_count, lights.size(), max_per_cluster, 0, {}};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) push.rows[r * 4 + k] = view.row[r][k];
    static_assert(sizeof(Push) == 64);

    rhi::CommandList& cmd = device.begin_frame();
    cmd.bind_pipeline(pipeline);
    cmd.bind_group(0, group);
    cmd.push_constant_struct(push);
    cmd.dispatch((cluster_count + 63u) / 64u, 1, 1);
    std::vector<rhi::BufferBarrier> barriers(2);
    const rhi::BufferHandle outs[2] = {summary_b, indices_b};
    for (size_t i = 0; i < 2; ++i) {
        barriers[i].buffer = outs[i];
        barriers[i].before = {rhi::PipelineStage::ComputeShader, rhi::Access::ShaderStorageWrite, rhi::TextureLayout::Undefined};
        barriers[i].after = {rhi::PipelineStage::Host, rhi::Access::HostRead, rhi::TextureLayout::Undefined};
    }
    cmd.barrier({}, barriers);
    (void)device.end_frame();
    device.wait_idle();
    GpuResult out;
    std::vector<uint32_t> summary(sentinel_summary.size());
    out.indices.resize(sentinel_indices.size());
    std::memcpy(summary.data(), device.map(summary_b).data(), summary.size() * 4u);
    std::memcpy(out.indices.data(), device.map(indices_b).data(), out.indices.size() * 4u);
    out.counts.resize(cluster_count + 64u);
    out.overflow.resize(cluster_count + 64u);
    for (size_t c = 0; c < out.counts.size(); ++c) {   // past the last cluster these hold the sentinel the buffer started with (nothing may be written there)
        out.counts[c] = summary[2 * c];
        out.overflow[c] = summary[2 * c + 1];
    }
    return out;
}

render::LocalLightSet make_lights(uint32_t count)
{
    Rng rng;
    render::LocalLightSet set(count + 8);
    for (uint32_t i = 0; i < count; ++i) {
        const std::array<float, 3> pos{rng.range(-120, 120), rng.range(-120, 120), rng.range(-5, 60)};
        const float radius = i % 17 == 0 ? rng.range(80.0f, 400.0f) : rng.range(0.5f, 45.0f);
        if (i % 3 == 2) {
            render::SpotLightDesc s;
            s.position = pos;
            s.radius = radius;
            s.outer_angle = 0.5f;
            s.inner_angle = 0.3f;
            set.add_spot(s);
        } else {
            render::PointLightDesc p;
            p.position = pos;
            p.radius = radius;
            set.add_point(p);
        }
    }
    return set;
}

} // namespace

TEST_CASE("Light culling on the GPU: no light that meets a cluster is missing, nothing is listed that is not within rounding, lists are ascending")
{
    const auto device = make_validated_device("light_culling", true);
    CHECK(device->info().compute);
    rr::ClusterGrid grid;   // 16 x 9 x 24
    const View view = make_view();
    const render::LocalLightSet lights = make_lights(300);
    const uint32_t max_per_cluster = 512;   // no truncation in this case
    const GpuResult gpu = run_gpu(*device, grid, view, lights, max_per_cluster);
    const std::vector<rr::Aabb> boxes = rr::build_cluster_boxes(grid);
    std::vector<rr::CullLight> exact, widened;
    for (const render::LocalLightGpu& l : lights.lights()) {
        const std::array<double, 3> v = to_view(view, l.position_radius);
        exact.push_back({v, static_cast<double>(l.position_radius[3])});
        widened.push_back({v, static_cast<double>(l.position_radius[3]) * 1.001 + 0.001});
    }
    const rr::CullResult r0 = rr::cull_lights(grid, boxes, exact, max_per_cluster), r1 = rr::cull_lights(grid, boxes, widened, max_per_cluster);
    size_t false_negatives = 0, strays = 0, equal_clusters = 0, listed = 0, ordering_errors = 0, unwritten = 0;
    for (size_t c = 0; c < boxes.size(); ++c) {
        std::vector<uint32_t> got(gpu.indices.begin() + static_cast<std::ptrdiff_t>(c * max_per_cluster), gpu.indices.begin() + static_cast<std::ptrdiff_t>(c * max_per_cluster + gpu.counts[c]));
        listed += got.size();
        for (size_t k = 1; k < got.size(); ++k) ordering_errors += got[k - 1] < got[k] ? size_t{0} : size_t{1};
        const auto in = [&](const rr::CullResult& r, uint32_t light) {
            for (uint32_t k = 0; k < r.counts[c]; ++k)
                if (r.indices[c * max_per_cluster + k] == light) return true;
            return false;
        };
        for (uint32_t k = 0; k < r0.counts[c]; ++k) {
            bool found = false;
            for (uint32_t g : got) found = found || g == r0.indices[c * max_per_cluster + k];
            false_negatives += found ? size_t{0} : size_t{1};
        }
        for (uint32_t g : got) strays += in(r1, g) ? size_t{0} : size_t{1};
        equal_clusters += got.size() == r0.counts[c] ? size_t{1} : size_t{0};
        for (uint32_t k = gpu.counts[c]; k < std::min<uint32_t>(gpu.counts[c] + 3u, max_per_cluster); ++k) unwritten += gpu.indices[c * max_per_cluster + k] == 0xFFFFFFFFu ? size_t{0} : size_t{1};
        CHECK(gpu.overflow[c] == 0);
    }
    std::printf("light culling: %zu clusters, %zu entries listed, %zu of %zu clusters equal the exact list, %zu false negatives, %zu strays\n", boxes.size(), listed, equal_clusters, boxes.size(), false_negatives, strays);
    CHECK(listed > 3000);              // the case is not vacuous: many clusters list many lights
    CHECK(false_negatives == 0);       // THE property: a missing light is a hole in the picture
    CHECK(strays == 0);
    CHECK(equal_clusters >= boxes.size() * 995 / 1000);   // float against double differ only at a boundary
    CHECK(ordering_errors == 0);
    CHECK(unwritten == 0);             // slots after the listed ones hold the sentinel the shader writes
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Light culling on the GPU: truncation keeps the first entries and counts the rest; zero lights; a cluster count that is not a multiple of 64")
{
    const auto device = make_validated_device("light_culling_edges", true);
    rr::ClusterGrid grid;
    grid.nx = 15; grid.ny = 9; grid.nz = 23;   // 3105 clusters: the last workgroup is partly empty
    const View view = make_view();
    const render::LocalLightSet lights = make_lights(200);
    const GpuResult full = run_gpu(*device, grid, view, lights, 256);
    const GpuResult cut = run_gpu(*device, grid, view, lights, 4);
    const size_t n = static_cast<size_t>(grid.count());
    size_t truncated = 0, bad = 0;
    for (size_t c = 0; c < n; ++c) {
        const uint32_t met = full.counts[c];
        bad += cut.counts[c] == std::min<uint32_t>(met, 4u) ? size_t{0} : size_t{1};
        bad += cut.counts[c] + cut.overflow[c] == met ? size_t{0} : size_t{1};
        for (uint32_t k = 0; k < cut.counts[c]; ++k) bad += cut.indices[c * 4 + k] == full.indices[c * 256 + k] ? size_t{0} : size_t{1};
        for (uint32_t k = cut.counts[c]; k < 4; ++k) bad += cut.indices[c * 4 + k] == 0xFFFFFFFFu ? size_t{0} : size_t{1};
        truncated += cut.overflow[c] > 0 ? size_t{1} : size_t{0};
    }
    std::printf("light culling edges: %zu clusters, %zu truncated at 4, %zu contract violations\n", n, truncated, bad);
    CHECK(truncated > 100);
    CHECK(bad == 0);
    // Nothing is written beyond the last cluster (the buffers were filled with a sentinel there).
    for (size_t k = n; k < n + 32; ++k) CHECK(full.counts[k] == 0xDEADBEEFu && full.overflow[k] == 0xDEADBEEFu && cut.counts[k] == 0xDEADBEEFu);
    for (size_t k = n * 4; k < n * 4 + 32; ++k) CHECK(cut.indices[k] == 0xDEADBEEFu);

    // Zero lights: every list is empty, every slot holds the sentinel, nothing overflows.
    const render::LocalLightSet none(4);
    const GpuResult empty = run_gpu(*device, grid, view, none, 8);
    size_t nonempty = 0;
    for (size_t c = 0; c < n; ++c) {
        nonempty += empty.counts[c] != 0 || empty.overflow[c] != 0 ? size_t{1} : size_t{0};
        for (uint32_t k = 0; k < 8; ++k) nonempty += empty.indices[c * 8 + k] == 0xFFFFFFFFu ? size_t{0} : size_t{1};
    }
    CHECK(nonempty == 0);
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Light culling on the GPU: a sphere that exactly touches a cluster's box is listed (the boundary is inclusive, as in the reference)")
{
    const auto device = make_validated_device("light_culling_touch", true);
    rr::ClusterGrid grid;
    // One box [0,1]^3 and the identity view; lights touching a face, an edge and a corner exactly, and one a hair outside.
    const std::vector<float> box{0, 0, 0, 0, 1, 1, 1, 0};
    View identity{};
    for (int r = 0; r < 3; ++r) identity.row[r][r] = 1.0f;
    render::LocalLightSet lights(8);
    const auto add = [&](float x, float y, float z, float r) {
        render::PointLightDesc p;
        p.position = {x, y, z};
        p.radius = r;
        return lights.add_point(p);
    };
    add(-2.0f, 0.5f, 0.5f, 2.0f);              // touches the face x = 0
    add(0.5f, 3.0f, 0.5f, 2.0f);               // touches the face y = 1
    add(-3.0f, -4.0f, 0.5f, 5.0f);             // touches the edge x = 0, y = 0 (3-4-5)
    add(-2.0f, 0.5f, 0.5f, 1.999f);            // a hair short
    add(0.5f, 0.5f, 0.5f, 0.01f);              // inside
    const GpuResult gpu = run_gpu(*device, grid, identity, lights, 8, &box);
    CHECK(gpu.counts[0] == 4);
    CHECK(gpu.indices[0] == 0 && gpu.indices[1] == 1 && gpu.indices[2] == 2 && gpu.indices[3] == 4 && gpu.indices[4] == 0xFFFFFFFFu);
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}
