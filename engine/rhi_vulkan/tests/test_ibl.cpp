// Image-based lighting on a real (validated) Vulkan device. The shader's lookup of the prefiltered environment and of the SH diffuse term is checked
// against the CPU references (Environment::sample, sh_diffuse) on spheres under procedural skies:
//  * a uniform environment gives exactly the image of a uniform ambient of the same radiance (the existing non-IBL path as the oracle);
//  * a sky that varies with z differs from the hemispherical ambient by exactly the documented 2/3 diffuse factor (up and down);
//  * the diffuse term follows an environment that varies with x and y (axes and orientation), and a mirror reflects the environment toward
//    the reflection vector (the equirect mapping and the roughness mips);
//  * without an environment nothing changes, and an environment with the wrong shading mode is refused. CI: lavapipe, validation required.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/gfx_assets/environment.hpp"
#include "saige/render/camera.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 192;
constexpr uint32_t kH = 192;
constexpr float kRadius = 1.0f;
constexpr Float3 kCenter{0.0f, 0.0f, 10.0f};  // well above the renderer's checkerboard ground (z = 0), which can neither hide nor tint the sphere

constexpr double pi_d = 3.14159265358979323846;

using Env = std::function<Float3(Float3)>;

/// A UV sphere of the given radius at the origin, counter-clockwise outward.
GpuModel make_sphere(GpuResourceRegistry& registry, GpuMaterial material, float radius, int rings = 48, int sectors = 96)
{
    std::vector<ModelVertex> v;
    std::vector<uint32_t> idx;
    for (int r = 0; r <= rings; ++r) {
        const double theta = pi_d * r / rings;
        for (int s = 0; s <= sectors; ++s) {
            const double phi = 2.0 * pi_d * s / sectors;
            const Float3 n{static_cast<float>(std::sin(theta) * std::cos(phi)), static_cast<float>(std::sin(theta) * std::sin(phi)), static_cast<float>(std::cos(theta))};
            v.push_back({{n.x * radius, n.y * radius, n.z * radius}, n, {static_cast<float>(s) / sectors, static_cast<float>(r) / rings}});
        }
    }
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            const uint32_t a = static_cast<uint32_t>(r * (sectors + 1) + s), b = a + static_cast<uint32_t>(sectors + 1);
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});  // (ring, sector) increases toward (-z, +y): this order is counter-clockwise seen from outside
        }
    }
    GpuModel m;
    m.name = "sphere";
    m.materials.push_back(std::move(material));
    m.meshes.push_back({make_primitive(registry, v, idx, 0)});
    m.placements.push_back({0, Mat4::identity()});
    return m;
}

std::shared_ptr<const EnvironmentLighting> make_environment(const Env& env, uint32_t base_width = 128, uint32_t levels = 4, float intensity = 1.0f)
{
    gfx::HdrImage image;
    image.width = 256;
    image.height = 128;
    image.rgb.resize(256 * 128 * 3);
    for (uint32_t y = 0; y < 128; ++y) {
        for (uint32_t x = 0; x < 256; ++x) {
            const Float3 c = env(gfx::equirect_direction((static_cast<float>(x) + 0.5f) / 256.0f, (static_cast<float>(y) + 0.5f) / 128.0f));
            float* p = image.rgb.data() + (static_cast<std::size_t>(y) * 256 + x) * 3;
            p[0] = c.x;
            p[1] = c.y;
            p[2] = c.z;
        }
    }
    gfx::EnvironmentSettings s;
    s.width = base_width;
    s.levels = levels;
    s.samples = 192;
    const gfx::EnvironmentResult r = gfx::precompute_environment(image, s);
    REQUIRE(r.ok());
    return std::make_shared<const EnvironmentLighting>(gfx::to_environment_lighting(*r.environment, intensity));
}

struct Setup {
    Float3 ambient_ground{0.2f, 0.18f, 0.16f};
    Float3 sky{0.3f, 0.5f, 0.82f};  // also the fog color; the hemispherical ambient's sky half is 0.6 times it
    float metallic = 0.0f, roughness = 1.0f;
    Float3 base{0.8f, 0.8f, 0.8f};
    std::shared_ptr<const EnvironmentLighting> environment;
};

struct Rendered {
    Image image;
    Mat4 view_proj;
    Camera camera;
};

/// `camera_offset` is relative to the sphere's center.
Rendered render_sphere(const Setup& setup, Float3 camera_offset)
{
    const auto device = make_validated_device("ibl");
    RendererSettings settings = deterministic_settings();
    settings.fog_density = 0.0f;
    settings.sky = {setup.sky.x, setup.sky.y, setup.sky.z, 1.0f};
    settings.model_shading = ModelShading::MetallicRoughness;
    settings.lighting.sun_color = {0.0f, 0.0f, 0.0f};  // ambient only
    settings.lighting.ambient_ground = setup.ambient_ground;
    settings.environment = setup.environment;
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, settings);
    OffscreenTarget target(*device, kW, kH);
    MaterialParams mp;
    for (int i = 0; i < 3; ++i) mp.base_color[i] = (&setup.base.x)[i];
    mp.metallic = setup.metallic;
    mp.roughness = setup.roughness;
    const GpuModel sphere = make_sphere(renderer.registry(), renderer.model_renderer().create_material(mp, {}, {}, "sphere"), kRadius);
    CHECK(renderer.model_renderer().ibl_enabled() == (setup.environment != nullptr));
    const Camera camera = look_at_camera(kCenter + camera_offset, kCenter);
    const ModelDraw draws[] = {{&sphere, translation(kCenter)}};
    Rendered out;
    out.camera = camera;
    out.view_proj = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    out.image = target.render(renderer, {}, camera, 0, draws);
    CHECK(device->validation_error_count() == 0);
    return out;
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

/// Surface points on the camera-facing hemisphere, away from the silhouette (so the pixel's normal is the sample's normal to within a pixel).
std::vector<Float3> sample_normals(Float3 camera_position, float min_dot = 0.6f)
{
    std::vector<Float3> out;
    const Float3 view = normalize(camera_position);
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) {
            const Float3 up_hint = std::fabs(view.z) > 0.9f ? Float3{1, 0, 0} : Float3{0, 0, 1};
            const Float3 right = normalize(cross(up_hint, view)), up = cross(view, right);
            const Float3 n = normalize(view + right * (static_cast<float>(i) * 0.22f) + up * (static_cast<float>(j) * 0.22f));
            if (dot(n, view) > min_dot) out.push_back(n);
        }
    }
    return out;
}

} // namespace

TEST_CASE("IBL: a uniform environment is exactly a uniform ambient of the same radiance (dielectric and metal, several roughnesses)")
{
    const Float3 c{0.45f, 0.4f, 0.35f};
    const auto environment = make_environment([&](Float3) { return c; });
    for (const float roughness : {0.1f, 0.4f, 1.0f}) {
        for (const float metallic : {0.0f, 1.0f}) {
            Setup uniform;
            uniform.ambient_ground = c;
            uniform.sky = {c.x / 0.6f, c.y / 0.6f, c.z / 0.6f};  // ambient_radiance(d) = lerp(ground, sky * 0.6, ...) = c for every d
            uniform.roughness = roughness;
            uniform.metallic = metallic;
            const Rendered reference = render_sphere(uniform, {5, 0, 1});
            Setup ibl = uniform;
            ibl.environment = environment;
            const Rendered with = render_sphere(ibl, {5, 0, 1});
            int worst = 0;
            for (uint32_t y = 0; y < kH; ++y) {
                for (uint32_t x = 0; x < kW; ++x) worst = std::max(worst, max_channel_delta(reference.image.at(x, y), with.image.at(x, y)));
            }
            if (worst > 3) std::printf("  roughness %.1f metallic %.0f: worst channel difference %d\n", static_cast<double>(roughness), static_cast<double>(metallic), worst);
            CHECK(worst <= 3);  // half-float texels and SH9 of a constant are within a few 8-bit levels
        }
    }
}

TEST_CASE("IBL: a sky that varies with z differs from the hemispherical ambient by the diffuse factor 2/3, in both directions")
{
    // ambient_radiance(d) = ground + (sky * 0.6 - ground) * (d.z / 2 + 1 / 2) = a + b d.z with a = (ground + 0.6 sky) / 2 and b = (0.6 sky - ground) / 2.
    Setup base;
    const Float3 ground = base.ambient_ground, sky06 = base.sky * 0.6f;
    const Float3 a = (ground + sky06) * 0.5f, b = (sky06 - ground) * 0.5f;
    const auto environment = make_environment([&](Float3 d) { return Float3{a.x + b.x * d.z, a.y + b.y * d.z, a.z + b.z * d.z}; });
    Setup ibl = base;
    ibl.environment = environment;
    for (const float height : {6.0f, -6.0f}) {  // looking down at the top of the sphere (n = +z), then up at the bottom (n = -z)
        const Float3 camera{0.3f, 0.2f, height};  // not exactly on the up axis (look_at with an up vector parallel to the view is degenerate)
        const Rendered old = render_sphere(base, camera), now = render_sphere(ibl, camera);
        const Pixel center = to_pixel(now.view_proj, kCenter + Float3{0, 0, height > 0 ? kRadius : -kRadius});
        const Float3 before = linear_at(old.image, center), after = linear_at(now.image, center);
        // The diffuse factor at n.z = +-1: the old path uses L(n) = a +- b, the SH path a +- 2/3 b; the difference carries the diffuse color 0.8.
        const float sign = height > 0 ? 1.0f : -1.0f;
        const Float3 expected{0.8f * (-b.x / 3.0f) * sign, 0.8f * (-b.y / 3.0f) * sign, 0.8f * (-b.z / 3.0f) * sign};
        const Float3 got = after - before;
        if (std::fabs(got.x - expected.x) > 0.02f || std::fabs(got.z - expected.z) > 0.02f) {
            std::printf("  height %.0f: got (%.3f %.3f %.3f) expected (%.3f %.3f %.3f)\n", static_cast<double>(height), static_cast<double>(got.x), static_cast<double>(got.y),
                        static_cast<double>(got.z), static_cast<double>(expected.x), static_cast<double>(expected.y), static_cast<double>(expected.z));
        }
        CHECK(near3(got, expected, 0.02f));
        CHECK(std::fabs(expected.z) > 0.04f);  // the effect is well above the tolerance, so the test could fail
    }
}

TEST_CASE("IBL: the diffuse term follows an environment that varies with x, y and z (axes and orientation)")
{
    const Env env = [](Float3 d) { return Float3{0.5f + 0.3f * d.x, 0.45f + 0.25f * d.y, 0.4f + 0.2f * d.z}; };
    Setup s;
    s.roughness = 1.0f;
    s.environment = make_environment(env);
    int checked = 0;
    for (const Float3 camera : {Float3{5, 0, 0}, Float3{0, 5, 0}, Float3{0.3f, 0.2f, 5}, Float3{-4, 3, 2}}) {
        const Rendered r = render_sphere(s, camera);
        for (const Float3 n : sample_normals(camera)) {
            // Diffuse color 0.8 times the radiance factor; for a linear environment the factor is a + (2/3) times the linear part. The specular
            // term of a rough dielectric is about 0.04 * L, so the tolerance covers it.
            const Float3 diffuse = {0.5f + 0.3f * n.x * 2.0f / 3.0f, 0.45f + 0.25f * n.y * 2.0f / 3.0f, 0.4f + 0.2f * n.z * 2.0f / 3.0f};
            const Float3 expected = diffuse * 0.8f;
            const Float3 got = linear_at(r.image, to_pixel(r.view_proj, kCenter + n * kRadius));
            if (!near3(got, expected, 0.05f)) {
                std::printf("  camera (%.0f %.0f %.0f) normal (%.2f %.2f %.2f): got (%.3f %.3f %.3f) expected (%.3f %.3f %.3f)\n", static_cast<double>(camera.x),
                            static_cast<double>(camera.y), static_cast<double>(camera.z), static_cast<double>(n.x), static_cast<double>(n.y), static_cast<double>(n.z),
                            static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(expected.x),
                            static_cast<double>(expected.y), static_cast<double>(expected.z));
            }
            CHECK(near3(got, expected, 0.05f));
            ++checked;
        }
    }
    CHECK(checked >= 80);
}

TEST_CASE("IBL: the diffuse term weights the second SH band by the clamped cosine's 1/4")
{
    // Each channel is a pure band-2 harmonic (traceless quadratic) plus a constant, so the diffuse factor is the constant plus 1/4 of the harmonic at the
    // normal. This is what keeps l = 2 from being exercised only through the CPU reference: a wrong band weight changes these by tens of levels.
    const auto harmonic_r = [](Float3 d) { return d.x * d.y; };
    const auto harmonic_g = [](Float3 d) { return d.z * d.z - 1.0f / 3.0f; };
    const auto harmonic_b = [](Float3 d) { return d.x * d.x - d.y * d.y; };
    const Env env = [&](Float3 d) { return Float3{0.5f + 0.6f * harmonic_r(d), 0.5f + 0.5f * harmonic_g(d), 0.5f + 0.5f * harmonic_b(d)}; };
    Setup s;
    s.roughness = 1.0f;
    s.environment = make_environment(env);
    int checked = 0;
    float strongest = 0.0f;
    for (const Float3 camera : {Float3{5, 0, 0}, Float3{0, 5, 0}, Float3{0.3f, 0.2f, 5}, Float3{-4, 3, 2}, Float3{3, 3, -3}}) {
        const Rendered r = render_sphere(s, camera);
        for (const Float3 n : sample_normals(camera)) {
            const Float3 expected = Float3{0.5f + 0.15f * harmonic_r(n), 0.5f + 0.125f * harmonic_g(n), 0.5f + 0.125f * harmonic_b(n)} * 0.8f;
            const Float3 got = linear_at(r.image, to_pixel(r.view_proj, kCenter + n * kRadius));
            strongest = std::max(strongest, std::fabs(0.6f * harmonic_r(n) * 0.75f * 0.8f));
            if (!near3(got, expected, 0.05f)) {
                std::printf("  normal (%.2f %.2f %.2f): got (%.3f %.3f %.3f) expected (%.3f %.3f %.3f)\n", static_cast<double>(n.x), static_cast<double>(n.y), static_cast<double>(n.z),
                            static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(expected.x), static_cast<double>(expected.y),
                            static_cast<double>(expected.z));
            }
            CHECK(near3(got, expected, 0.05f));
            ++checked;
        }
    }
    CHECK(checked >= 100);
    CHECK(strongest > 0.1f);  // the band-2 term moves the image by far more than the tolerance (with weight 1 instead of 1/4 the red error is about 0.2)
}

TEST_CASE("IBL: a mirror reflects the prefiltered environment toward the reflection vector")
{
    // A metal sphere (white, so the specular color is 1): the pixel is k * L(r) with k = scale + bias of the split-sum table, the same for all
    // channels. Solve k per pixel from the sum of channels and check the channel ratios against L(r): that is what proves the mapping.
    const Env env = [](Float3 d) { return Float3{0.5f + 0.4f * d.x, 0.5f + 0.4f * d.y, 0.5f + 0.4f * d.z}; };
    Setup s;
    s.metallic = 1.0f;
    s.roughness = 0.05f;
    s.base = {1.0f, 1.0f, 1.0f};
    s.environment = make_environment(env, 128, 4);
    int checked = 0;
    double k_min = 10.0, k_max = 0.0;
    for (const Float3 camera : {Float3{5, 0, 0}, Float3{0, 5, 0}, Float3{-3, -3, 3}}) {
        const Rendered r = render_sphere(s, camera);
        for (const Float3 n : sample_normals(camera)) {
            const Float3 view = normalize(camera - n * kRadius);  // toward the camera from this surface point (the camera is close, so it differs per point)
            const Float3 refl = n * (2.0f * dot(n, view)) - view;  // reflect(-v, n)
            const Float3 expected = env(refl);
            const Float3 got = linear_at(r.image, to_pixel(r.view_proj, kCenter + n * kRadius));
            const float k = (got.x + got.y + got.z) / (expected.x + expected.y + expected.z);
            k_min = std::min(k_min, static_cast<double>(k));
            k_max = std::max(k_max, static_cast<double>(k));
            const Float3 scaled = expected * k;
            if (!near3(got, scaled, 0.045f)) {
                std::printf("  camera (%.0f %.0f %.0f) normal (%.2f %.2f %.2f): got (%.3f %.3f %.3f) vs k * L(r) = (%.3f %.3f %.3f), k %.3f\n", static_cast<double>(camera.x),
                            static_cast<double>(camera.y), static_cast<double>(camera.z), static_cast<double>(n.x), static_cast<double>(n.y), static_cast<double>(n.z),
                            static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(scaled.x), static_cast<double>(scaled.y),
                            static_cast<double>(scaled.z), static_cast<double>(k));
            }
            CHECK(near3(got, scaled, 0.045f));
            ++checked;
        }
    }
    CHECK(checked >= 60);
    CHECK(k_min > 0.7 && k_max < 1.15);  // a white metal reflects about all of the environment (the split-sum scale plus bias is close to 1)
}

TEST_CASE("IBL: rougher metal reflects a blurrier environment (the roughness selects the mip)")
{
    // A high-frequency sky (bands around the z axis): a smooth mirror shows its full contrast, rough metal averages it out. The contrast is the
    // spread of the reflected color across the sphere, measured from the rendered image.
    const Env env = [](Float3 d) {
        const float band = 0.5f + 0.5f * std::sin(14.0f * d.z);
        return Float3{0.1f + 0.8f * band, 0.1f + 0.8f * band, 0.1f + 0.8f * band};
    };
    const auto environment = make_environment(env, 256, 5);
    const auto spread = [&](float roughness) {
        Setup s;
        s.metallic = 1.0f;
        s.base = {1.0f, 1.0f, 1.0f};
        s.roughness = roughness;
        s.environment = environment;
        const Rendered r = render_sphere(s, {5, 0, 0});
        float lo = 10.0f, hi = -10.0f;
        for (const Float3 n : sample_normals({5, 0, 0})) {
            const float v = linear_at(r.image, to_pixel(r.view_proj, kCenter + n * kRadius)).x;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return hi - lo;
    };
    const float mirror = spread(0.04f), mid = spread(0.5f), rough = spread(1.0f);
    std::printf("  reflected contrast: roughness 0.04 -> %.3f, 0.5 -> %.3f, 1.0 -> %.3f\n", static_cast<double>(mirror), static_cast<double>(mid), static_cast<double>(rough));
    CHECK(mirror > 0.4f);
    CHECK(mid < mirror * 0.3f);  // (the rough floor is the split-sum scale varying across the sphere, not the sky)
    CHECK(rough < mid * 0.8f);
}

TEST_CASE("IBL: off changes nothing, intensity scales the sky, and an environment with the wrong shading mode is refused")
{
    Setup off;
    const Rendered a = render_sphere(off, {5, 0, 1}), b = render_sphere(off, {5, 0, 1});
    CHECK(a.image.rgba == b.image.rgba);  // deterministic with no environment
    // Intensity: doubling it doubles a uniform environment's contribution.
    const Float3 c{0.2f, 0.2f, 0.2f};
    Setup one, two;
    one.environment = make_environment([&](Float3) { return c; }, 128, 4, 1.0f);
    two.environment = make_environment([&](Float3) { return c; }, 128, 4, 2.0f);
    one.roughness = two.roughness = 1.0f;
    const Pixel center = to_pixel(render_sphere(one, {5, 0, 0}).view_proj, kCenter + Float3{kRadius, 0, 0});
    const float v1 = linear_at(render_sphere(one, {5, 0, 0}).image, center).x, v2 = linear_at(render_sphere(two, {5, 0, 0}).image, center).x;
    CHECK(std::fabs(v2 / v1 - 2.0f) < 0.08f);
    // The specular term scales too: a white metal (all specular) under the same two environments, kept inside the displayable range.
    one.metallic = two.metallic = 1.0f;
    one.base = two.base = {1.0f, 1.0f, 1.0f};
    one.roughness = two.roughness = 0.3f;
    const float m1 = linear_at(render_sphere(one, {5, 0, 0}).image, center).x, m2 = linear_at(render_sphere(two, {5, 0, 0}).image, center).x;
    CHECK(m1 > 0.1f);
    CHECK(std::fabs(m2 / m1 - 2.0f) < 0.08f);
    // Refused with another shading mode (silently ignoring the environment would be worse), and with invalid data.
    const auto device = make_validated_device("ibl_refused");
    RendererSettings legacy = deterministic_settings();
    legacy.environment = one.environment;  // model_shading stays Legacy
    bool threw = false;
    try {
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, legacy);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    RendererSettings broken = deterministic_settings();
    broken.model_shading = ModelShading::MetallicRoughness;
    auto bad = std::make_shared<EnvironmentLighting>(*one.environment);
    bad->rgba16f.pop_back();
    broken.environment = bad;
    threw = false;
    try {
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, broken);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(device->validation_error_count() == 0);
}
