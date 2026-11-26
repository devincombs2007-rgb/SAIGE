// Local lights (point and spot) on a real Vulkan device: the shading function in shaders/scene/local_lights.hlsli, run by a compute shader over probe points, is compared with the CPU reference
// (saige::render_reference lights: units, windowed inverse-square falloff, spot cone) combined with the BRDF oracle (gfx_test brdf_reference), the same oracle that checks the sun's shading. CI: lavapipe,
// so this is correctness only: no performance claim, and nothing about how many lights a frame can afford (that is the clustered culling that comes next).

#include "saige/gfx_test/brdf_reference.hpp"
#include "saige/gfx_test/saige_test.hpp"
#include "saige/render_reference/lights.hpp"

#include "saige/shaders/test_local_light_probe.spv.hpp"
#include "vulkan_test_util.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
namespace gfx_test = saige::test;
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
    uint32_t s;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return static_cast<float>(s >> 8) / 16777216.0f; }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

struct Probe {
    std::array<float, 3> position, normal, view, base_color;
    float metallic, roughness;
};

enum class Kind { Point, Spot };

struct Light {
    Kind kind;
    std::array<float, 3> position, direction, color;
    float intensity, radius, inner, outer;  // inner and outer are half angles in radians (spot)
};

std::array<float, 3> unit(Rng& rng)
{
    for (;;) {
        const std::array<float, 3> v{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)};
        const float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (l2 > 0.01f && l2 <= 1.0f) {
            const float inv = 1.0f / std::sqrt(l2);
            return {v[0] * inv, v[1] * inv, v[2] * inv};
        }
    }
}

/// What the host puts in the light buffer for `light`: the same derived terms the CPU reference computes, in float.
void pack_light(const Light& l, float* out)
{
    out[0] = l.position[0]; out[1] = l.position[1]; out[2] = l.position[2]; out[3] = l.radius;
    out[4] = l.color[0]; out[5] = l.color[1]; out[6] = l.color[2]; out[7] = l.intensity;
    if (l.kind == Kind::Point) {
        out[8] = 0; out[9] = 0; out[10] = 0; out[11] = -2.0f;
        out[12] = 1.0f; out[13] = 1.0f; out[14] = 0; out[15] = 0;
        return;
    }
    const double outer = std::max<double>(l.outer, 0.0);
    const double inner = std::min(std::max<double>(l.inner, 0.0), outer);
    const double cos_outer = std::cos(outer), cos_inner = std::cos(inner);
    out[8] = l.direction[0]; out[9] = l.direction[1]; out[10] = l.direction[2]; out[11] = static_cast<float>(cos_outer);
    out[12] = static_cast<float>(cos_inner); out[13] = static_cast<float>(1.0 / std::max(cos_inner - cos_outer, 1e-4)); out[14] = 0; out[15] = 0;
}

/// The CPU oracle: the reference light's irradiance at the probe, then the BRDF oracle: f(l, v) * E * max(n . l, 0).
std::array<double, 4> expected(const Probe& p, const std::vector<Light>& lights)
{
    std::array<double, 4> sum{0, 0, 0, 0};
    brdf::Surface s;
    s.base_color = {p.base_color[0], p.base_color[1], p.base_color[2]};
    s.metallic = p.metallic;
    s.roughness = p.roughness;
    for (const Light& l : lights) {
        rr::LightSample sample;
        bool reached = false;
        const rr::Vec3 point{p.position[0], p.position[1], p.position[2]};
        if (l.kind == Kind::Point) {
            rr::PointLight pl;
            pl.position = {l.position[0], l.position[1], l.position[2]};
            pl.color = {l.color[0], l.color[1], l.color[2]};
            pl.intensity = l.intensity;
            pl.radius = l.radius;
            reached = rr::sample_point_light(pl, point, sample);
        } else {
            rr::SpotLight sl;
            sl.position = {l.position[0], l.position[1], l.position[2]};
            sl.direction = {l.direction[0], l.direction[1], l.direction[2]};
            sl.color = {l.color[0], l.color[1], l.color[2]};
            sl.intensity = l.intensity;
            sl.radius = l.radius;
            sl.inner_angle = l.inner;
            sl.outer_angle = l.outer;
            reached = rr::sample_spot_light(sl, point, sample);
        }
        if (!reached) continue;
        const render::Float3 n{p.normal[0], p.normal[1], p.normal[2]}, v{p.view[0], p.view[1], p.view[2]};
        const render::Float3 ldir{static_cast<float>(sample.to_light[0]), static_cast<float>(sample.to_light[1]), static_cast<float>(sample.to_light[2])};
        const render::Float3 f = brdf::evaluate(s, n, v, ldir);
        const double n_dot_l = std::max<double>(render::dot(n, ldir), 0.0);
        sum[0] += static_cast<double>(f.x) * sample.irradiance[0] * n_dot_l;
        sum[1] += static_cast<double>(f.y) * sample.irradiance[1] * n_dot_l;
        sum[2] += static_cast<double>(f.z) * sample.irradiance[2] * n_dot_l;
        sum[3] += (sample.irradiance[0] > 0 || sample.irradiance[1] > 0 || sample.irradiance[2] > 0) ? 1.0 : 0.0;
    }
    return sum;
}

struct Scene {
    std::vector<Probe> probes;
    std::vector<Light> lights;
    size_t edge_begin = 0;  // probes from here on are the hand-placed edge cases
};

Scene make_scene(uint32_t seed)
{
    Rng rng{seed};
    Scene scene;
    for (int i = 0; i < 28; ++i) {
        Light l;
        l.kind = i % 3 == 2 ? Kind::Spot : Kind::Point;
        l.position = {rng.range(-8, 8), rng.range(-8, 8), rng.range(0.5f, 8)};
        l.direction = unit(rng);
        l.color = {rng.range(0.1f, 1.0f), rng.range(0.1f, 1.0f), rng.range(0.1f, 1.0f)};
        l.intensity = rng.range(20.0f, 2000.0f);
        l.radius = rng.range(3.0f, 25.0f);
        l.outer = rng.range(0.15f, 1.45f);
        l.inner = l.outer * rng.range(0.0f, 1.0f);
        scene.lights.push_back(l);
    }
    for (int i = 0; i < 700; ++i) {
        Probe p;
        p.position = {rng.range(-10, 10), rng.range(-10, 10), rng.range(0.0f, 6)};
        p.normal = unit(rng);
        std::array<float, 3> v = unit(rng);
        const float d = v[0] * p.normal[0] + v[1] * p.normal[1] + v[2] * p.normal[2];
        if (d < 0.05f && i % 5 != 0) v = {v[0] + p.normal[0] * 1.2f, v[1] + p.normal[1] * 1.2f, v[2] + p.normal[2] * 1.2f};
        const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        p.view = {v[0] / vl, v[1] / vl, v[2] / vl};
        p.base_color = {rng.range(0.02f, 1.0f), rng.range(0.02f, 1.0f), rng.range(0.02f, 1.0f)};
        p.metallic = i % 4 == 0 ? 1.0f : (i % 4 == 1 ? 0.0f : rng.range(0, 1));
        p.roughness = rng.range(0.05f, 1.0f);
        scene.probes.push_back(p);
    }
    scene.edge_begin = scene.probes.size();
    // Hand-placed edges against one point light and one spot light at known places (the lights are appended last; the general lights stay in the list too, so the CPU sum is the oracle).
    return scene;
}

struct Result {
    std::vector<std::array<float, 4>> values;
};

Result run_gpu(rhi::Device& device, const Scene& scene)
{
    Result out;
    out.values.resize(scene.probes.size());
    Cleanup own(device);
    const rhi::ShaderHandle cs = own.track(device.create_shader({rhi::ShaderStage::Compute, words(saige::shaders::test_local_light_probe_cs_main), "CSMain", "LocalLightProbe"}));
    rhi::BindGroupLayoutDesc ld;
    for (uint32_t i = 0; i < 3; ++i) {
        rhi::BindGroupLayoutEntry e;
        e.binding = i;
        e.type = rhi::BindingType::StorageBuffer;
        e.visibility = rhi::StageMask::Compute;
        ld.entries.push_back(e);
    }
    const rhi::BindGroupLayoutHandle layout = own.track(device.create_bind_group_layout(ld));
    rhi::ComputePipelineDesc pd;
    pd.compute_shader = cs;
    pd.push_constant_size = 8;
    pd.bind_group_layouts = {layout};
    pd.debug_name = "LocalLightProbe";
    const rhi::PipelineHandle pipeline = own.track(device.create_compute_pipeline(pd));

    std::vector<float> probe_data(scene.probes.size() * 16u, 0.0f), light_data(scene.lights.size() * 16u, 0.0f);
    for (size_t i = 0; i < scene.probes.size(); ++i) {
        const Probe& p = scene.probes[i];
        float* d = &probe_data[i * 16u];
        d[0] = p.position[0]; d[1] = p.position[1]; d[2] = p.position[2]; d[3] = p.metallic;
        d[4] = p.normal[0]; d[5] = p.normal[1]; d[6] = p.normal[2]; d[7] = p.roughness;
        d[8] = p.view[0]; d[9] = p.view[1]; d[10] = p.view[2]; d[11] = 0;
        d[12] = p.base_color[0]; d[13] = p.base_color[1]; d[14] = p.base_color[2]; d[15] = 0;
    }
    for (size_t i = 0; i < scene.lights.size(); ++i) pack_light(scene.lights[i], &light_data[i * 16u]);
    const auto make = [&](std::span<const float> data, uint64_t bytes, const char* name) {
        rhi::BufferDesc bd;
        bd.size = bytes;
        bd.usage = rhi::BufferUsage::Storage;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = name;
        return own.track(device.create_buffer(bd, std::as_bytes(data)));
    };
    const rhi::BufferHandle probes = make(probe_data, probe_data.size() * 4u, "Probes");
    const rhi::BufferHandle lights = make(light_data, light_data.size() * 4u, "Lights");
    std::vector<float> zeros(scene.probes.size() * 4u, -1.0f);
    const rhi::BufferHandle results = make(zeros, zeros.size() * 4u, "Results");
    std::vector<rhi::BindGroupEntry> entries(3);
    entries[0].binding = 0; entries[0].buffer = probes;
    entries[1].binding = 1; entries[1].buffer = lights;
    entries[2].binding = 2; entries[2].buffer = results;
    const rhi::BindGroupHandle group = own.track(device.create_bind_group(layout, entries));

    rhi::CommandList& cmd = device.begin_frame();
    cmd.bind_pipeline(pipeline);
    cmd.bind_group(0, group);
    const std::array<uint32_t, 2> pc{static_cast<uint32_t>(scene.probes.size()), static_cast<uint32_t>(scene.lights.size())};
    cmd.push_constant_struct(pc);
    cmd.dispatch((pc[0] + 63u) / 64u, 1, 1);
    rhi::BufferBarrier bb;
    bb.buffer = results;
    bb.before = {rhi::PipelineStage::ComputeShader, rhi::Access::ShaderStorageWrite, rhi::TextureLayout::Undefined};
    bb.after = {rhi::PipelineStage::Host, rhi::Access::HostRead, rhi::TextureLayout::Undefined};
    cmd.barrier({}, std::span(&bb, 1));
    (void)device.end_frame();
    device.wait_idle();
    const std::span<std::byte> mapped = device.map(results);
    std::memcpy(out.values.data(), mapped.data(), out.values.size() * 16u);
    return out;
}

} // namespace

TEST_CASE("Local lights on the GPU: the shading function equals the CPU reference (units, falloff, window, cone, BRDF) over 700 probes and 28 point and spot lights")
{
    const auto device = make_validated_device("local_lights", true);
    CHECK(device->info().compute);
    const Scene scene = make_scene(0x5EEDu);
    const Result gpu = run_gpu(*device, scene);
    int bad = 0, nonzero = 0;
    double worst = 0.0;
    for (size_t i = 0; i < scene.probes.size(); ++i) {
        const std::array<double, 4> want = expected(scene.probes[i], scene.lights);
        for (int c = 0; c < 3; ++c) {
            const double got = static_cast<double>(gpu.values[i][static_cast<size_t>(c)]);
            const double err = std::fabs(got - want[static_cast<size_t>(c)]);
            const double tol = 2e-3 * std::max(std::fabs(want[static_cast<size_t>(c)]), 0.5) + 2e-3;
            worst = std::max(worst, err / tol);
            if (err > tol) ++bad;
            if (want[static_cast<size_t>(c)] > 0.0) ++nonzero;
        }
    }
    std::printf("local lights: %zu probes, %d non-zero channels, worst error %.3f of the tolerance, %d channels out of tolerance\n", scene.probes.size(), nonzero, worst, bad);
    CHECK(nonzero > 300);  // the comparison is not vacuous: most probes are lit by something
    CHECK(bad == 0);
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Local lights on the GPU: exact cases (outside the radius, outside the cone, on the light, a probe facing away)")
{
    const auto device = make_validated_device("local_lights_edges", true);
    Scene scene;
    Light point;
    point.kind = Kind::Point;
    point.position = {0, 0, 5};
    point.direction = {0, 0, -1};
    point.color = {1, 1, 1};
    point.intensity = 800.0f;
    point.radius = 10.0f;
    point.inner = point.outer = 0.0f;
    Light spot = point;
    spot.kind = Kind::Spot;
    spot.position = {40, 0, 5};
    spot.direction = {0, 0, -1};
    spot.inner = 0.3f;
    spot.outer = 0.5f;
    scene.lights = {point};
    const auto probe = [&](std::array<float, 3> pos, std::array<float, 3> n) {
        Probe p;
        p.position = pos;
        p.normal = n;
        p.view = n;
        p.base_color = {0.8f, 0.6f, 0.4f};
        p.metallic = 0.0f;
        p.roughness = 0.6f;
        return p;
    };
    // Point light: straight below at 2 m (lit), at exactly the radius (zero), beyond it (zero), a surface facing away (zero), on the light itself (finite).
    scene.probes = {probe({0, 0, 3}, {0, 0, 1}), probe({0, 0, -5}, {0, 0, 1}), probe({0, 0, -6}, {0, 0, 1}), probe({0, 0, 3}, {0, 0, -1}), probe({0, 0, 5}, {0, 0, 1})};
    Result gpu = run_gpu(*device, scene);
    const auto want = [&](size_t i) { return expected(scene.probes[i], scene.lights); };
    CHECK(gpu.values[0][0] > 1.0f);
    CHECK(std::fabs(static_cast<double>(gpu.values[0][0]) - want(0)[0]) < 2e-3 * want(0)[0] + 1e-3);
    CHECK(gpu.values[1][0] == 0.0f && gpu.values[1][1] == 0.0f && gpu.values[1][2] == 0.0f);   // at the radius: exactly zero
    CHECK(gpu.values[2][0] == 0.0f && gpu.values[2][3] == 0.0f);                               // beyond it
    CHECK(gpu.values[3][0] == 0.0f);                                                           // facing away from the light
    CHECK(std::isfinite(gpu.values[4][0]) && std::isfinite(gpu.values[4][1]));                 // on the light: finite (distance clamped at 1 cm)
    {
        // The clamp is a value, not just "finite": at 0 m and at 5 cm the irradiance is intensity / max(d, 1 cm)^2 (the reference), within float rounding.
        scene.probes = {probe({0, 0, 5}, {0, 0, 1}), probe({0, 0, 4.95f}, {0, 0, 1}), probe({0, 0, 4.995f}, {0, 0, 1})};
        const Result near = run_gpu(*device, scene);
        for (size_t i = 0; i < scene.probes.size(); ++i) {
            const double w = expected(scene.probes[i], scene.lights)[0];
            CHECK(w > 1e4);
            CHECK(std::fabs(static_cast<double>(near.values[i][0]) - w) < 3e-3 * w);
        }
        // A light whose radius is zero reaches nothing, not even a probe standing on it (no 0 / 0).
        Light dead = point;
        dead.radius = 0.0f;
        scene.lights = {dead};
        scene.probes = {probe({0, 0, 5}, {0, 0, 1}), probe({0, 0, 4}, {0, 0, 1})};
        const Result none = run_gpu(*device, scene);
        for (size_t i = 0; i < scene.probes.size(); ++i) CHECK(none.values[i][0] == 0.0f && none.values[i][1] == 0.0f && none.values[i][2] == 0.0f && none.values[i][3] == 0.0f);
        scene.lights = {point};
    }

    // Spot light: inside the inner cone (full strength), between inner and outer (partial), outside the outer cone (zero).
    scene.lights = {spot};
    const float rx = 40.0f;
    scene.probes = {probe({rx, 0, 0}, {0, 0, 1}), probe({rx + 5.0f * std::tan(0.4f), 0, 0}, {0, 0, 1}), probe({rx + 5.0f * std::tan(0.7f), 0, 0}, {0, 0, 1})};
    gpu = run_gpu(*device, scene);
    CHECK(gpu.values[0][0] > gpu.values[1][0] && gpu.values[1][0] > 0.0f);
    CHECK(gpu.values[2][0] == 0.0f && gpu.values[2][3] == 0.0f);
    for (size_t i = 0; i < scene.probes.size(); ++i) {
        const double w = expected(scene.probes[i], scene.lights)[0];
        CHECK(std::fabs(static_cast<double>(gpu.values[i][0]) - w) < 2e-3 * std::max(w, 0.5) + 2e-3);
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

namespace {

std::array<uint8_t, 3> tonemap_pixel(double r, double g, double b, double exposure)
{
    const auto one = [&](double v) {
        const double x = std::max(v, 0.0) * exposure;
        const double mapped = x / (1.0 + x);                                   // Reinhard
        const double srgb = mapped <= 0.0031308 ? 12.92 * mapped : 1.055 * std::pow(mapped, 1.0 / 2.4) - 0.055;
        return static_cast<uint8_t>(std::clamp(srgb * 255.0 + 0.5, 0.0, 255.0));
    };
    return {one(r), one(g), one(b)};
}

} // namespace

TEST_CASE("Local lights on the GPU: a lit floor (two point lights and a spot) equals the CPU reference pixel by pixel; with SAIGE_CAPTURE_DIR the three images are written")
{
    const auto device = make_validated_device("local_lights_floor", true);
    constexpr uint32_t size = 256;
    Scene scene;
    const auto make_point = [](std::array<float, 3> pos, std::array<float, 3> color, float intensity, float radius) {
        Light l;
        l.kind = Kind::Point;
        l.position = pos;
        l.direction = {0, 0, -1};
        l.color = color;
        l.intensity = intensity;
        l.radius = radius;
        l.inner = l.outer = 0.0f;
        return l;
    };
    Light warm = make_point({-4.0f, 0.0f, 2.5f}, {1.0f, 0.62f, 0.3f}, 500.0f, 14.0f);
    Light cool = make_point({5.0f, 3.0f, 1.5f}, {0.35f, 0.6f, 1.0f}, 400.0f, 10.0f);
    Light spot = make_point({0.0f, -6.0f, 6.0f}, {1.0f, 1.0f, 1.0f}, 30000.0f, 20.0f);
    spot.kind = Kind::Spot;
    {
        const float dx = 0.0f - spot.position[0], dy = 3.0f - spot.position[1], dz = 0.0f - spot.position[2];
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        spot.direction = {dx / len, dy / len, dz / len};
        spot.inner = 0.22f;
        spot.outer = 0.45f;
    }
    scene.lights = {warm, cool, spot};
    const std::array<float, 3> eye{0.0f, -14.0f, 12.0f};
    for (uint32_t j = 0; j < size; ++j)
        for (uint32_t i = 0; i < size; ++i) {
            Probe p;
            p.position = {-12.0f + 24.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(size), 12.0f - 24.0f * (static_cast<float>(j) + 0.5f) / static_cast<float>(size), 0.0f};
            p.normal = {0, 0, 1};
            const std::array<float, 3> d{eye[0] - p.position[0], eye[1] - p.position[1], eye[2] - p.position[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            p.view = {d[0] / len, d[1] / len, d[2] / len};
            p.base_color = {0.72f, 0.70f, 0.66f};
            p.metallic = 0.0f;
            p.roughness = 0.55f;
            scene.probes.push_back(p);
        }
    const Result gpu = run_gpu(*device, scene);
    gfx_test::Image gpu_img = gfx_test::make_image(size, size, 0, 0, 0), cpu_img = gpu_img;
    double peak = 0.0;
    std::vector<std::array<double, 3>> want(scene.probes.size());
    for (size_t k = 0; k < scene.probes.size(); ++k) {
        const std::array<double, 4> w = expected(scene.probes[k], scene.lights);
        want[k] = {w[0], w[1], w[2]};
        peak = std::max({peak, w[0], w[1], w[2]});
    }
    const double exposure = 0.8 / std::max(peak * 0.35, 1e-6);  // the brightest pixels clip a little: dim regions stay visible
    int bad = 0, lit = 0;
    for (size_t k = 0; k < scene.probes.size(); ++k) {
        for (int c = 0; c < 3; ++c) {
            const double g = static_cast<double>(gpu.values[k][static_cast<size_t>(c)]), w = want[k][static_cast<size_t>(c)];
            if (std::fabs(g - w) > 2e-3 * std::max(std::fabs(w), 0.5) + 2e-3) ++bad;
        }
        if (want[k][0] + want[k][1] + want[k][2] > 0.05) ++lit;
        const auto g8 = tonemap_pixel(static_cast<double>(gpu.values[k][0]), static_cast<double>(gpu.values[k][1]), static_cast<double>(gpu.values[k][2]), exposure);
        const auto c8 = tonemap_pixel(want[k][0], want[k][1], want[k][2], exposure);
        for (int c = 0; c < 3; ++c) {
            gpu_img.rgba[k * 4 + static_cast<size_t>(c)] = g8[static_cast<size_t>(c)];
            cpu_img.rgba[k * 4 + static_cast<size_t>(c)] = c8[static_cast<size_t>(c)];
        }
    }
    std::printf("local lights floor: %zu pixels, %d lit by at least 0.05, %d channels out of tolerance, peak radiance %.1f\n", scene.probes.size(), lit, bad, peak);
    CHECK(bad == 0);
    CHECK(lit > 15000);                                             // the three lights cover much of the floor
    CHECK(lit < static_cast<int>(scene.probes.size()) - 2000);     // and not all of it: beyond the radii the floor is black
    // The spot is the brightest source on the floor where it points (the middle of the picture, a little above the centre).
    const size_t centre = (size / 2 - 12) * size + size / 2;
    CHECK(want[centre][0] > 0.0);
    CHECK(static_cast<double>(gpu.values[centre][0]) > 0.5 * peak * 0.2);
    if (const char* dir = std::getenv("SAIGE_CAPTURE_DIR"); dir != nullptr && dir[0] != '\0') {
        std::filesystem::create_directories(dir);
        gfx_test::write_ppm(std::filesystem::path(dir) / "local_lights_floor_gpu.ppm", gpu_img);
        gfx_test::write_ppm(std::filesystem::path(dir) / "local_lights_floor_cpu_reference.ppm", cpu_img);
        gfx_test::write_ppm(std::filesystem::path(dir) / "local_lights_floor_difference_x8.ppm", gfx_test::make_diff_image(gpu_img, cpu_img, 8));
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}
