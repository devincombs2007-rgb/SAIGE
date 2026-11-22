// Metallic-roughness shading (milestone M2) on a validated Vulkan device, checked against the CPU reference BRDF
// (engine/gfx_test_support/.../brdf_reference.hpp), which implements the glTF specification independently of the shader.
//
// Oracle: for a flat quad whose center pixel is sampled, the shader's output is the sum of the sun's contribution
// (shade_directional with the sun's irradiance, color * pi), hemispherical ambient light through the environment BRDF (the
// reference integrates it numerically; the renderer uses a 32x32 table of the same integral) and emission. Fog is off. CI:
// lavapipe. Validation errors fail each test.

#include "saige/gfx_test/brdf_reference.hpp"
#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/env_brdf.hpp"
#include "saige/render/transform.hpp"
#include "model_test_util.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 384;
constexpr uint32_t kH = 216;
constexpr float kPi = 3.14159265358979323846f;

RendererSettings pbr_settings()
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.sky = {0.30f, 0.50f, 0.82f, 1.0f};
    s.model_shading = ModelShading::MetallicRoughness;
    return s;
}

// The renderer's fixed sun and ambient (ModelRenderer constructor), restated for the oracle.
Float3 sun_direction() { return normalize(Float3{-0.40f, -0.55f, 0.73f}); }
constexpr Float3 kSunColor{1.00f, 0.95f, 0.85f};
Float3 ambient_radiance(Float3 d, const RendererSettings& s)
{
    const float t = d.z * 0.5f + 0.5f;
    const Float3 ground{0.20f, 0.18f, 0.16f};
    const Float3 sky{s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};
    return ground * (1.0f - t) + sky * t;
}

struct Pixel {
    int x, y;
};
Pixel to_pixel(const Mat4& vp, Float3 world)
{
    const Projected p = project_point(vp, world);
    const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(kW), static_cast<float>(kH));
    return {static_cast<int>(px.x), static_cast<int>(px.y)};
}
Float3 linear_at(const Image& img, Pixel p)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

/// Rotation about the origin taking the unit vector `a` to the unit vector `b` (Rodrigues; a and b not antiparallel).
Mat4 rotation_between(Float3 a, Float3 b)
{
    const Float3 axis = cross(a, b);
    const float s = length(axis);
    const float c = dot(a, b);
    if (s < 1e-6f) {
        return Mat4::identity();
    }
    const Float3 k = axis * (1.0f / s);
    const float t = 1.0f - c;
    Mat4 m = Mat4::identity();
    m.at(0, 0) = c + k.x * k.x * t;
    m.at(0, 1) = k.x * k.y * t - k.z * s;
    m.at(0, 2) = k.x * k.z * t + k.y * s;
    m.at(1, 0) = k.y * k.x * t + k.z * s;
    m.at(1, 1) = c + k.y * k.y * t;
    m.at(1, 2) = k.y * k.z * t - k.x * s;
    m.at(2, 0) = k.z * k.x * t - k.y * s;
    m.at(2, 1) = k.z * k.y * t + k.x * s;
    m.at(2, 2) = c + k.z * k.z * t;
    return m;
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;
    RendererSettings settings = pbr_settings();

    explicit Rig(const char* name, ModelShading shading = ModelShading::MetallicRoughness)
    {
        settings.model_shading = shading;
        device = make_validated_device(name, /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, settings);
        target = std::make_unique<OffscreenTarget>(*device, kW, kH);
    }
    ~Rig()
    {
        target.reset();
        renderer.reset();
    }
    GpuMaterial material(const MaterialData& data, const MaterialTextures& textures = {}, const char* name = "pbr.material")
    {
        return renderer->model_renderer().create_material(data, textures, name);
    }
};

/// One quad to compare: its model, where it sits, and what the oracle expects at its center.
struct Probe {
    GpuModel model;
    Mat4 world;
    brdf::Surface surface;
    Float3 normal;
    Float3 view;
    Float3 emissive{};
    float occlusion = 1.0f;
    Pixel pixel{};
};

/// Two viewpoints 20 m away, both looking at the grid. A sits on the sun's side (v.l about +0.4); B sits on the opposite
/// side (v.l about -0.68), where Fresnel weights are large enough for a wrong exponent to show.
constexpr Float3 kCameraA{-20.0f, 0.0f, 1.5f};
constexpr Float3 kCameraB{11.76f, 16.18f, 1.5f};
constexpr Float3 kTarget{0.0f, 0.0f, 4.0f};

/// reflect(-v, n): the mirror direction of the view vector about the normal.
Float3 reflect_direction(Float3 v, Float3 n) { return n * (2.0f * dot(n, v)) - v; }

Float3 expected_color(const Probe& p, const RendererSettings& s)
{
    const Float3 n = p.normal;
    const Float3 v = p.view;
    const Float3 sun = brdf::shade_directional(p.surface, n, v, sun_direction(), kSunColor * kPi);
    const Float3 r = reflect_direction(v, n);
    const float n_dot_v = std::max(dot(n, v), 1e-4f);
    const float roughness = std::clamp(p.surface.roughness, brdf::min_roughness, 1.0f);
    const brdf::EnvBrdf ab = brdf::integrate_env_brdf(roughness, n_dot_v);
    const Float3 ambient = brdf::shade_ambient(p.surface, p.occlusion, ab, ambient_radiance(n, s), ambient_radiance(r, s));
    return sun + ambient + p.emissive;
}

} // namespace

namespace {

/// Probes sit on a grid: one per (material, orientation), each a unit quad rotated so its normal is `tilt` away from the half
/// vector between the view and the sun (a highlight the camera can see) or, for tilt < 0, facing the camera (diffuse dominated).
Float3 grid_position(size_t index)
{
    const size_t cols = 9;
    const float col = static_cast<float>(index % cols) - 4.0f;
    const float row = static_cast<float>(index / cols) - 2.5f;
    return {0.0f, col * 1.4f, 4.0f + row * 1.4f};  // above the ground plane
}

Probe make_probe(Rig& rig, size_t index, Float3 camera_position, const MaterialData& data, const MaterialTextures& textures, brdf::Surface surface,
                 float tilt, float occlusion = 1.0f)
{
    Probe p;
    const Float3 position = grid_position(index);
    p.view = normalize(camera_position - position);
    Float3 normal = p.view;  // tilt < 0: facing the camera, so the quad is lit mostly diffusely
    if (tilt >= 0.0f) {
        const Float3 h = normalize(p.view + sun_direction());
        const Float3 e = normalize(cross(h, Float3{0.0f, 0.0f, 1.0f}));  // a direction perpendicular to h
        normal = normalize(h + e * tilt);
    }
    p.normal = normal;
    p.surface = surface;
    p.occlusion = occlusion;
    const GpuMaterial material = rig.material(data, textures);
    p.model = make_quad(rig.renderer->registry(), material, 0.0f, 0.0f, 0.5f);
    p.world = translation(position) * rotation_between({-1.0f, 0.0f, 0.0f}, normal);
    return p;
}

Image render_probes(Rig& rig, const Camera& camera, std::vector<Probe>& probes)
{
    std::vector<ModelDraw> draws;
    for (const Probe& p : probes) {
        draws.push_back({&p.model, p.world});
    }
    return rig.target->render(*rig.renderer, {}, camera, 0, draws);
}

} // namespace

TEST_CASE("PBR: metallic-roughness shading matches the CPU reference across materials, orientations and viewpoints")
{
    Rig rig("pbr_reference");
    {
        size_t compared_total = 0;
        for (const Float3 camera_position : {kCameraA, kCameraB}) {
            const Camera camera = look_at_camera(camera_position, kTarget);
            const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
            std::vector<Probe> probes;
            for (const Float3 base : {Float3{0.80f, 0.50f, 0.20f}, Float3{0.95f, 0.64f, 0.54f}}) {
                for (const float metallic : {0.0f, 0.5f, 1.0f}) {
                    for (const float roughness : {0.3f, 0.6f, 1.0f}) {
                        for (const float tilt : {0.10f, 0.25f, -1.0f}) {
                            MaterialData data;
                            data.base_color[0] = base.x;
                            data.base_color[1] = base.y;
                            data.base_color[2] = base.z;
                            data.metallic = metallic;
                            data.roughness = roughness;
                            probes.push_back(make_probe(rig, probes.size(), camera_position, data, {},
                                                        brdf::Surface{base, metallic, roughness}, tilt));
                        }
                    }
                }
            }
            // Roughness below the clamp (0.045) behaves like the clamp. Facing the camera the highlight is off to the side, so
            // this checks the diffuse and ambient terms stay finite and equal to the oracle's.
            for (const float roughness : {0.0f, 0.02f}) {
                for (const float metallic : {0.0f, 1.0f}) {
                    MaterialData data;
                    data.metallic = metallic;
                    data.roughness = roughness;
                    probes.push_back(make_probe(rig, probes.size(), camera_position, data, {},
                                                brdf::Surface{{1.0f, 1.0f, 1.0f}, metallic, roughness}, -1.0f));
                }
            }
            // Exactly at the highlight's peak, a perfectly smooth surface has a lobe so narrow that without the clamp it is zero
            // (alpha = 0) or NaN: with the clamp it is finite and far above 1, so the target saturates to white.
            const size_t first_peak = probes.size();
            for (const float roughness : {0.0f, 0.02f}) {
                for (const float metallic : {0.0f, 1.0f}) {
                    MaterialData data;
                    data.metallic = metallic;
                    data.roughness = roughness;
                    probes.push_back(make_probe(rig, probes.size(), camera_position, data, {},
                                                brdf::Surface{{1.0f, 1.0f, 1.0f}, metallic, roughness}, 0.0f));
                }
            }
            REQUIRE(probes.size() == 62);
            const Image img = render_probes(rig, camera, probes);

            size_t compared = 0, bad = 0, skipped_saturated = 0;
            double worst = 0.0;
            for (size_t i = 0; i < probes.size(); ++i) {
                const Probe& p = probes[i];
                const Float3 got = linear_at(img, to_pixel(vp, grid_position(i)));
                const Float3 want = expected_color(p, rig.settings);
                if (i >= first_peak) {
                    CHECK(std::min({want.x, want.y, want.z}) > 1.0f);  // the oracle's value is far above the display range
                    CHECK(std::min({got.x, got.y, got.z}) > 0.95f);    // and the shader reaches it: saturated white, not black/NaN
                    continue;
                }
                if (std::max({want.x, want.y, want.z}) > 0.9f) {
                    ++skipped_saturated;  // the display-referred target clips here (HDR arrives with M4)
                    continue;
                }
                const float tolerance = 0.012f + 0.015f * std::max({want.x, want.y, want.z});
                const float error = std::max({std::fabs(got.x - want.x), std::fabs(got.y - want.y), std::fabs(got.z - want.z)});
                worst = std::max(worst, static_cast<double>(error / tolerance));
                ++compared;
                if (error > tolerance) {
                    ++bad;
                    std::printf("  probe %zu: got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", i, static_cast<double>(got.x),
                                static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(want.x),
                                static_cast<double>(want.y), static_cast<double>(want.z));
                }
            }
            std::printf("  camera (%.1f %.1f %.1f): compared %zu probes (%zu clipped and skipped), worst error / tolerance = %.2f\n",
                        static_cast<double>(camera_position.x), static_cast<double>(camera_position.y),
                        static_cast<double>(camera_position.z), compared, skipped_saturated, worst);
            CHECK(compared >= 40);  // not vacuous: most of the grid is inside the displayable range
            CHECK(bad == 0);
            compared_total += compared;
        }
        CHECK(compared_total >= 80);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("PBR: the metallic-roughness texture is read as linear data, G for roughness and B for metallic, times the factors")
{
    Rig rig("pbr_mr_texture");
    {
        const Camera camera = look_at_camera(kCameraA, kTarget);
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
        std::vector<Probe> probes;
        struct Case {
            uint8_t g, b;       // texel channels
            float f_rough, f_metal;  // factors
        };
        const Case cases[] = {{128, 64, 1.0f, 1.0f}, {64, 200, 1.0f, 1.0f}, {128, 255, 0.5f, 0.5f}, {255, 128, 0.6f, 1.0f}};
        for (const Case& c : cases) {
            rhi::TextureDesc td;
            td.format = rhi::Format::RGBA8_UNORM;  // linear data
            td.width = td.height = 1;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
            td.debug_name = "test.mr";
            const std::byte texel[4] = {std::byte{255}, std::byte{c.g}, std::byte{c.b}, std::byte{255}};  // R is ignored
            MaterialTextures textures;
            textures[MaterialSlot::MetallicRoughness].texture = rig.renderer->registry().create_texture(td, texel);
            MaterialData data;
            data.base_color[0] = 0.8f; data.base_color[1] = 0.5f; data.base_color[2] = 0.2f;
            data.roughness = c.f_rough;
            data.metallic = c.f_metal;
            const float roughness = c.f_rough * static_cast<float>(c.g) / 255.0f;
            const float metallic = c.f_metal * static_cast<float>(c.b) / 255.0f;
            probes.push_back(make_probe(rig, probes.size(), kCameraA, data, textures, brdf::Surface{{0.8f, 0.5f, 0.2f}, metallic, roughness}, 0.25f));
            // The swapped reading (G as metallic, B as roughness) must differ clearly, or the check proves nothing.
            Probe swapped = probes.back();
            swapped.surface.roughness = c.f_rough * static_cast<float>(c.b) / 255.0f;
            swapped.surface.metallic = c.f_metal * static_cast<float>(c.g) / 255.0f;
            const Float3 a = expected_color(probes.back(), rig.settings);
            const Float3 b = expected_color(swapped, rig.settings);
            if (c.g != c.b && std::fabs(static_cast<float>(c.g) - static_cast<float>(c.b)) > 100.0f) {
                CHECK(std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) > 0.08f);
            }
        }
        const Image img = render_probes(rig, camera, probes);
        for (size_t i = 0; i < probes.size(); ++i) {
            const Float3 want = expected_color(probes[i], rig.settings);
            if (std::max({want.x, want.y, want.z}) > 0.9f) {
                continue;
            }
            const Float3 got = linear_at(img, to_pixel(vp, grid_position(i)));
            const float tolerance = 0.012f + 0.015f * std::max({want.x, want.y, want.z});
            CHECK(std::fabs(got.x - want.x) <= tolerance);
            CHECK(std::fabs(got.y - want.y) <= tolerance);
            CHECK(std::fabs(got.z - want.z) <= tolerance);
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("PBR: the environment BRDF table matches the independent integral, at texels and between them")
{
    const auto started = std::chrono::steady_clock::now();
    const EnvBrdfTable table = make_env_brdf_table();
    const double build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("  env BRDF table built in %.1f ms\n", build_ms);
    REQUIRE(table.size == 32);
    double worst = 0.0;
    for (uint32_t j = 0; j < table.size; j += 5) {
        for (uint32_t i = 0; i < table.size; i += 5) {
            const float n_dot_v = (static_cast<float>(i) + 0.5f) / static_cast<float>(table.size);
            const float roughness = (static_cast<float>(j) + 0.5f) / static_cast<float>(table.size);
            const EnvBrdfValue got = table.at(i, j);
            const brdf::EnvBrdf want = brdf::integrate_env_brdf(roughness, n_dot_v);
            worst = std::max({worst, static_cast<double>(std::fabs(got.scale - want.scale)), static_cast<double>(std::fabs(got.bias - want.bias))});
            CHECK(std::fabs(got.scale - want.scale) < 0.007f);
            CHECK(std::fabs(got.bias - want.bias) < 0.007f);
        }
    }
    // Between texels, linear interpolation of the table (what the sampler does) stays close to the true integral.
    double worst_between = 0.0;
    for (const float roughness : {0.12f, 0.37f, 0.74f}) {
        for (const float n_dot_v : {0.08f, 0.33f, 0.61f, 0.93f}) {
            const float fx = n_dot_v * static_cast<float>(table.size) - 0.5f;
            const float fy = roughness * static_cast<float>(table.size) - 0.5f;
            const uint32_t x0 = static_cast<uint32_t>(std::floor(fx)), y0 = static_cast<uint32_t>(std::floor(fy));
            const float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
            const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
            const EnvBrdfValue p00 = table.at(x0, y0), p10 = table.at(x0 + 1, y0), p01 = table.at(x0, y0 + 1), p11 = table.at(x0 + 1, y0 + 1);
            const float scale = lerp(lerp(p00.scale, p10.scale, tx), lerp(p01.scale, p11.scale, tx), ty);
            const float bias = lerp(lerp(p00.bias, p10.bias, tx), lerp(p01.bias, p11.bias, tx), ty);
            const brdf::EnvBrdf want = brdf::integrate_env_brdf(roughness, n_dot_v);
            worst_between = std::max({worst_between, static_cast<double>(std::fabs(scale - want.scale)), static_cast<double>(std::fabs(bias - want.bias))});
            CHECK(std::fabs(scale - want.scale) < 0.01f);
            CHECK(std::fabs(bias - want.bias) < 0.01f);
        }
    }
    std::printf("  env BRDF table: worst error at texels %.4f, between texels %.4f\n", worst, worst_between);
}

TEST_CASE("PBR: binary16 conversion is exact for every half value and rounds to nearest even")
{
    size_t mismatches = 0;
    for (uint32_t bits = 0; bits < 0x10000u; ++bits) {
        const uint16_t h = static_cast<uint16_t>(bits);
        const bool nan = (h & 0x7C00u) == 0x7C00u && (h & 0x3FFu) != 0;
        if (nan) {
            continue;
        }
        mismatches += float_to_half(half_to_float(h)) == h ? 0u : 1u;
    }
    CHECK(mismatches == 0);
    CHECK(float_to_half(1.0f) == 0x3C00);
    CHECK(float_to_half(0.5f) == 0x3800);
    CHECK(float_to_half(-2.0f) == 0xC000);
    CHECK(float_to_half(65504.0f) == 0x7BFF);
    CHECK(float_to_half(65520.0f) == 0x7C00);  // rounds up to infinity
    CHECK(float_to_half(5.960464477539063e-8f) == 0x0001);  // the smallest subnormal
    CHECK(float_to_half(1.0f + 1.0f / 2048.0f) == 0x3C00);  // halfway between 1 and 1 + 2^-10: ties to even (down)
    CHECK(float_to_half(1.0f + 3.0f / 2048.0f) == 0x3C02);  // halfway between 1 + 2^-10 and 1 + 2^-9: ties to even (up)
}

// ------------------------------------------------------------------------------------------ normal mapping (M3)
namespace {

constexpr int kCells = 4;

/// The tangent-space normal stored in cell (i, j) of the 4x4 test map: tilts grow away from the center, i counts from the
/// image's left and j from its top (glTF: +Y in the image is up). Bytes are what the texture holds; the oracle decodes them.
void cell_bytes(int i, int j, uint8_t out[3])
{
    const float x = (static_cast<float>(i) - 1.5f) / 1.5f * 0.55f;
    const float y = (1.5f - static_cast<float>(j)) / 1.5f * 0.55f;
    const float z = std::sqrt(1.0f - x * x - y * y);
    const float v[3] = {x, y, z};
    for (int c = 0; c < 3; ++c) {
        out[c] = static_cast<uint8_t>(std::lround((v[c] * 0.5f + 0.5f) * 255.0f));
    }
}
Float3 decode_cell(int i, int j)
{
    uint8_t b[3];
    cell_bytes(i, j, b);
    return {static_cast<float>(b[0]) / 255.0f * 2.0f - 1.0f, static_cast<float>(b[1]) / 255.0f * 2.0f - 1.0f,
            static_cast<float>(b[2]) / 255.0f * 2.0f - 1.0f};
}

TextureRef normal_map(Rig& rig)
{
    std::vector<std::byte> texels;
    for (int j = 0; j < kCells; ++j) {
        for (int i = 0; i < kCells; ++i) {
            uint8_t b[3];
            cell_bytes(i, j, b);
            texels.push_back(std::byte{b[0]});
            texels.push_back(std::byte{b[1]});
            texels.push_back(std::byte{b[2]});
            texels.push_back(std::byte{255});
        }
    }
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;  // linear data
    td.width = td.height = kCells;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "test.normal_map";
    return rig.renderer->registry().create_texture(td, texels);
}

SamplerRef nearest_clamp(Rig& rig)
{
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    sd.mipmap_mode = rhi::MipmapMode::Nearest;
    sd.address_u = sd.address_v = rhi::AddressMode::ClampToEdge;
    return rig.renderer->registry().get_sampler(sd);
}

/// World position of cell (i, j)'s center on the unmirrored quad (x = 0, centered on z = 1.5, half extent 1): u grows toward -Y.
Float3 cell_center(int i, int j)
{
    return {0.0f, 1.0f - (static_cast<float>(i) + 0.5f) * 0.5f, 1.5f + 1.0f - (static_cast<float>(j) + 0.5f) * 0.5f};
}

struct Frame {
    Float3 t, b, n;  ///< world-space tangent (increasing u), bitangent (up in the image) and outward normal
};

/// The expected shading normal for cell (i, j) with a world frame: x*T + y*B + z*N, x and y scaled by the material's scale.
Float3 mapped_normal(const Frame& f, int i, int j, float scale)
{
    const Float3 d = decode_cell(i, j);
    return normalize(f.t * (d.x * scale) + f.b * (d.y * scale) + f.n * d.z);
}

struct MappedResult {
    size_t compared = 0;
    size_t bad = 0;
    size_t differs_from_flat = 0;  ///< cells where mapping changes the expected color: the check is not vacuous
};

/// Renders the quad (its `world` placement, optional mirror) with the normal map and checks every cell against the oracle.
MappedResult check_normal_map(Rig& rig, Float3 camera_position, const Mat4& world_matrix, Float3 cell_mirror, const Frame& frame,
                              bool double_sided, bool flip_for_back_face, float normal_scale, brdf::Surface surface)
{
    const Camera camera = look_at_camera(camera_position, {0.0f, 0.0f, 1.5f});
    const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    MaterialData data;
    data.base_color[0] = surface.base_color.x;
    data.base_color[1] = surface.base_color.y;
    data.base_color[2] = surface.base_color.z;
    data.metallic = surface.metallic;
    data.roughness = surface.roughness;
    data.normal_scale = normal_scale;
    data.double_sided = double_sided;
    MaterialTextures textures;
    textures[MaterialSlot::Normal] = {normal_map(rig), nearest_clamp(rig)};
    const GpuModel quad = make_quad(rig.renderer->registry(), rig.material(data, textures), 0.0f, 1.5f, 1.0f, /*tangents=*/true);
    const ModelDraw draw{&quad, world_matrix};
    const Image img = rig.target->render(*rig.renderer, {}, camera, 0, std::span(&draw, 1));

    MappedResult result;
    for (int j = 0; j < kCells; ++j) {
        for (int i = 0; i < kCells; ++i) {
            const Float3 local = cell_center(i, j);
            const Float3 center{local.x * cell_mirror.x, local.y * cell_mirror.y, local.z * cell_mirror.z};
            Probe p;
            p.surface = surface;
            p.view = normalize(camera_position - center);
            Float3 n = mapped_normal(frame, i, j, normal_scale);
            if (flip_for_back_face) {
                n = n * -1.0f;
            }
            p.normal = n;
            const Float3 want = expected_color(p, rig.settings);
            Probe flat = p;
            flat.normal = flip_for_back_face ? frame.n * -1.0f : frame.n;
            const Float3 unmapped = expected_color(flat, rig.settings);
            if (std::fabs(want.x - unmapped.x) + std::fabs(want.y - unmapped.y) + std::fabs(want.z - unmapped.z) > 0.04f) {
                ++result.differs_from_flat;
            }
            if (std::max({want.x, want.y, want.z}) > 0.9f) {
                continue;
            }
            const Float3 got = linear_at(img, to_pixel(vp, center));
            const float tolerance = 0.012f + 0.015f * std::max({want.x, want.y, want.z});
            ++result.compared;
            if (std::fabs(got.x - want.x) > tolerance || std::fabs(got.y - want.y) > tolerance || std::fabs(got.z - want.z) > tolerance) {
                ++result.bad;
                std::printf("  cell (%d,%d): got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", i, j, static_cast<double>(got.x),
                            static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(want.x),
                            static_cast<double>(want.y), static_cast<double>(want.z));
            }
        }
    }
    return result;
}

const brdf::Surface kDielectric{{0.8f, 0.6f, 0.4f}, 0.0f, 0.6f};
const brdf::Surface kMetal{{0.95f, 0.64f, 0.54f}, 1.0f, 0.5f};

} // namespace

TEST_CASE("PBR: a tangent-space normal map follows the glTF frame, scale and color space")
{
    Rig rig("pbr_normal_map");
    {
        const Frame frame{{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}};
        for (const brdf::Surface& surface : {kDielectric, kMetal}) {
            for (const float scale : {1.0f, 0.5f}) {
                const MappedResult r = check_normal_map(rig, {-6.0f, 0.0f, 1.5f}, Mat4::identity(), {1, 1, 1}, frame, false, false, scale, surface);
                std::printf("  metallic %.0f scale %.1f: compared %zu cells, %zu differ from the unmapped shading\n",
                            static_cast<double>(surface.metallic), static_cast<double>(scale), r.compared, r.differs_from_flat);
                CHECK(r.compared >= 12);
                CHECK(r.differs_from_flat >= 10);
                CHECK(r.bad == 0);
            }
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("PBR: a mirrored instance keeps a consistent frame, so the relief is the mirror image and lit correctly")
{
    Rig rig("pbr_normal_map_mirror");
    {
        // Mirroring Y maps the tangent (-Y) to +Y, the bitangent and normal stay put, and the image appears mirrored.
        const Frame frame{{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}};
        const Mat4 mirror = scale_matrix({1.0f, -1.0f, 1.0f});
        for (const brdf::Surface& surface : {kDielectric, kMetal}) {
            const MappedResult r = check_normal_map(rig, {-6.0f, 0.0f, 1.5f}, mirror, {1, -1, 1}, frame, false, false, 1.0f, surface);
            CHECK(r.compared >= 12);
            CHECK(r.differs_from_flat >= 10);
            CHECK(r.bad == 0);
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("PBR: the back of a double-sided quad uses the reversed frame")
{
    Rig rig("pbr_normal_map_back");
    {
        // Seen from +X the quad's back faces the camera: tangent, bitangent and normal are all negated (as the glTF reference
        // viewer does), so the shading normal is the front's mapped normal reversed.
        const Frame frame{{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}};
        for (const brdf::Surface& surface : {kDielectric, kMetal}) {
            const MappedResult r = check_normal_map(rig, {6.0f, 0.0f, 1.5f}, Mat4::identity(), {1, 1, 1}, frame, true, true, 1.0f, surface);
            CHECK(r.compared >= 12);
            CHECK(r.differs_from_flat >= 8);
            CHECK(r.bad == 0);
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("PBR: without tangents, or in Legacy or Extended shading, a normal map changes nothing")
{
    const Camera camera = look_at_camera({-6.0f, 0.0f, 1.5f}, {0.0f, 0.0f, 1.5f});
    for (const ModelShading shading : {ModelShading::MetallicRoughness, ModelShading::Legacy, ModelShading::Extended}) {
        Rig rig("pbr_normal_map_ignored", shading);
        {
            MaterialData data;
            data.base_color[0] = 0.8f; data.base_color[1] = 0.6f; data.base_color[2] = 0.4f;
            data.metallic = 0.0f;
            data.roughness = 0.6f;
            MaterialTextures with_map;
            with_map[MaterialSlot::Normal] = {normal_map(rig), nearest_clamp(rig)};
            GpuResourceRegistry& registry = rig.renderer->registry();
            const GpuModel plain = make_quad(registry, rig.material(data), 0.0f, 1.5f, 1.0f, true);
            const GpuModel mapped = make_quad(registry, rig.material(data, with_map), 0.0f, 1.5f, 1.0f, true);
            const GpuModel no_tangents = make_quad(registry, rig.material(data, with_map), 0.0f, 1.5f, 1.0f, false);
            const auto render = [&](const GpuModel& m) {
                const ModelDraw d{&m, Mat4::identity()};
                return rig.target->render(*rig.renderer, {}, camera, 0, std::span(&d, 1)).rgba;
            };
            const auto baseline = render(plain);
            CHECK(render(no_tangents) == baseline);  // no tangent frame: the map is skipped
            if (shading != ModelShading::MetallicRoughness) {
                CHECK(render(mapped) == baseline);   // the 0.2 look and Extended never read a normal map
            } else {
                CHECK(render(mapped) != baseline);   // not vacuous: with tangents the map does change the image
            }
        }
        CHECK(rig.device->validation_error_count() == 0);
    }
}

TEST_CASE("PBR: a tangent parallel to the normal has no frame, so the surface keeps its geometric normal (no NaN)")
{
    const Camera camera = look_at_camera({-6.0f, 0.0f, 1.5f}, {0.0f, 0.0f, 1.5f});
    Rig rig("pbr_degenerate_tangent", ModelShading::MetallicRoughness);
    MaterialData data;
    data.base_color[0] = 0.8f; data.base_color[1] = 0.6f; data.base_color[2] = 0.4f;
    data.metallic = 0.0f;
    data.roughness = 0.6f;
    MaterialTextures with_map;
    with_map[MaterialSlot::Normal] = {normal_map(rig), nearest_clamp(rig)};
    GpuResourceRegistry& registry = rig.renderer->registry();

    // The quad faces -X (normal n = (-1, 0, 0)); a tangent along n projects to zero against it.
    const auto quad_with_tangent = [&](const GpuMaterial& material, Float3 tangent) {
        const Float3 n{-1, 0, 0};
        std::vector<ModelVertex> v = {
            {{0.0f, +1.0f, 2.5f}, n, {0, 0}}, {{0.0f, -1.0f, 2.5f}, n, {1, 0}},
            {{0.0f, +1.0f, 0.5f}, n, {0, 1}}, {{0.0f, -1.0f, 0.5f}, n, {1, 1}},
        };
        for (ModelVertex& vertex : v) {
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
            vertex.tangent[3] = 1.0f;
        }
        GpuModel m;
        m.name = "degenerate quad";
        m.materials.push_back(material);
        m.meshes.push_back({make_primitive(registry, v, {0, 2, 3, 0, 3, 1}, 0)});
        m.placements.push_back({0, Mat4::identity()});
        return m;
    };
    const auto render = [&](const GpuModel& m) {
        const ModelDraw d{&m, Mat4::identity()};
        return rig.target->render(*rig.renderer, {}, camera, 0, std::span(&d, 1)).rgba;
    };

    const GpuMaterial plain = rig.material(data);
    const GpuMaterial mapped = rig.material(data, with_map);
    const auto baseline = render(quad_with_tangent(plain, {0.0f, -1.0f, 0.0f}));
    // Not vacuous: with a proper tangent the map changes the image.
    CHECK(render(quad_with_tangent(mapped, {0.0f, -1.0f, 0.0f})) != baseline);
    // Exactly parallel to the normal, and parallel to within 1e-4: both fall back to the unmapped surface.
    CHECK(render(quad_with_tangent(mapped, {-1.0f, 0.0f, 0.0f})) == baseline);
    CHECK(render(quad_with_tangent(mapped, {-1.0f, -1.0e-4f, 0.0f})) == baseline);
    CHECK(rig.device->validation_error_count() == 0);
}
