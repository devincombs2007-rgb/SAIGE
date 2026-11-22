// Sorted alpha blending in the model renderer, on a validated Vulkan device (lavapipe in CI): glTF BLEND materials composited
// source-over with straight alpha in a phase after all opaque draws, sorted back to front by the view-space depth of each
// primitive's bounding-sphere center, testing but not writing depth, double-sided surfaces in two passes (back faces, then front
// faces), working with metallic-roughness shading and HDR. Expected values come from the compositing equation applied to known
// colors; emissive materials (black base color, emissive factor) give exact lit colors so no lighting model is involved, except
// where a test needs distinct lit colors and uses the Legacy lighting oracle. Validation, including synchronization, fails a test.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/tonemap.hpp"
#include "saige/render/transform.hpp"
#include "model_test_util.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numeric>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 192;
constexpr uint32_t kH = 108;

enum class Mode { LdrLegacy, LdrMetallicRoughness, HdrLegacy, HdrMetallicRoughness };

RendererSettings settings_for(Mode mode)
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.sky = {0.30f, 0.50f, 0.82f, 1.0f};
    if (mode == Mode::LdrMetallicRoughness || mode == Mode::HdrMetallicRoughness) s.model_shading = ModelShading::MetallicRoughness;
    if (mode == Mode::HdrLegacy || mode == Mode::HdrMetallicRoughness) {
        s.hdr = true;
        s.tone_map.op = ToneMapOperator::Reinhard;  // distinct, invertible-ish values above 1
        s.tone_map.white_point = 8.0f;
        s.tone_map.exposure_ev = -0.5f;
    }
    return s;
}
bool is_hdr(Mode m) { return m == Mode::HdrLegacy || m == Mode::HdrMetallicRoughness; }

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;
    RendererSettings settings;
    Camera camera = look_at_camera({-6.0f, 0.0f, 1.5f}, {0.0f, 0.0f, 1.5f});

    Rig(const char* name, Mode mode, uint32_t w = kW, uint32_t h = kH) : settings(settings_for(mode))
    {
        device = make_validated_device(name, /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, settings);
        target = std::make_unique<OffscreenTarget>(*device, w, h);
    }
    ~Rig()
    {
        target.reset();
        renderer.reset();
    }

    /// Black base color with alpha `alpha` (blended) and emission `e`: the lit color is exactly `e` in either shading mode.
    GpuMaterial emissive(Float3 e, float alpha, bool blend, bool double_sided = false)
    {
        MaterialData d;
        d.base_color[0] = d.base_color[1] = d.base_color[2] = 0.0f;
        d.base_color[3] = alpha;
        d.emissive[0] = e.x;
        d.emissive[1] = e.y;
        d.emissive[2] = e.z;
        d.metallic = 1.0f;  // black metal with roughness 1 reflects almost nothing, so the emission is the whole color
        d.roughness = 1.0f;
        d.alpha_blend = blend;
        d.double_sided = double_sided;
        return renderer->model_renderer().create_material(d, MaterialTextures{}, "blend.test");
    }

    GpuModel quad(GpuMaterial m, float x, float zc = 1.5f, float half = 1.0f)
    {
        GpuModel model = make_quad(renderer->registry(), std::move(m), x, zc, half);
        model.meshes[0][0].bounds_center = {x, 0.0f, zc};
        model.meshes[0][0].bounds_radius = half * 1.4143f;
        return model;
    }

    test::Image render(std::span<const ModelDraw> draws, FrameStats* stats = nullptr)
    {
        return target->render(*renderer, {}, camera, 0, draws, stats);
    }

    Float3 center_pixel(const test::Image& img) const
    {
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(img.width) / static_cast<float>(img.height));
        const Projected p = project_point(vp, {0.0f, 0.0f, 1.5f});
        const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(img.width), static_cast<float>(img.height));
        const uint8_t* c = img.at(static_cast<uint32_t>(px.x), static_cast<uint32_t>(px.y));
        return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
    }

    /// What the output holds for a linear scene color: tone mapped in HDR, clamped otherwise.
    Float3 displayed(Float3 c, Mode mode) const
    {
        if (is_hdr(mode)) {
            return tone_map(c, settings.tone_map);
        }
        return {std::clamp(c.x, 0.0f, 1.0f), std::clamp(c.y, 0.0f, 1.0f), std::clamp(c.z, 0.0f, 1.0f)};
    }
};

Float3 over(Float3 src, float alpha, Float3 dst) { return src * alpha + dst * (1.0f - alpha); }

float tolerance_for(Mode mode)
{
    switch (mode) {
    case Mode::LdrLegacy: return 0.012f;
    case Mode::LdrMetallicRoughness: return 0.02f;  // the black metal's small residual reflection
    case Mode::HdrLegacy: return 0.015f;
    case Mode::HdrMetallicRoughness: return 0.025f;
    }
    return 0.02f;
}

bool near(Float3 a, Float3 b, float tolerance) { return near3(a, b, tolerance); }

const char* mode_name(Mode m)
{
    switch (m) {
    case Mode::LdrLegacy: return "LDR legacy";
    case Mode::LdrMetallicRoughness: return "LDR metallic-roughness";
    case Mode::HdrLegacy: return "HDR legacy";
    case Mode::HdrMetallicRoughness: return "HDR metallic-roughness";
    }
    return "?";
}

constexpr std::array<Mode, 4> kModes{Mode::LdrLegacy, Mode::LdrMetallicRoughness, Mode::HdrLegacy, Mode::HdrMetallicRoughness};

} // namespace

TEST_CASE("Blend (model): the sort key is the view-space depth of the center, not its distance from the camera")
{
    const Camera camera = look_at_camera({-6.0f, 0.0f, 1.5f}, {0.0f, 0.0f, 1.5f});
    const Mat4 vp = view_projection_matrix(camera, 16.0f / 9.0f);
    // Points straight ahead: the key is the distance along the view direction.
    for (const float d : {1.0f, 5.0f, 40.0f}) {
        CHECK(std::fabs(ModelRenderer::blend_sort_depth(vp, {-6.0f + d, 0.0f, 1.5f}) - d) < 1e-3f * d);
    }
    // A sideways offset does not change the depth, though it changes the distance.
    CHECK(std::fabs(ModelRenderer::blend_sort_depth(vp, {-1.0f, 20.0f, 1.5f}) - 5.0f) < 1e-3f);
    // Primitive A is nearer along the view (depth 5) but far to the side (distance about 20.6); B is straight ahead at depth 6
    // (distance 6). By distance A would be "farther" and drawn first; by depth B is farther and must be drawn first.
    const float depth_a = ModelRenderer::blend_sort_depth(vp, {-1.0f, 20.0f, 1.5f});
    const float depth_b = ModelRenderer::blend_sort_depth(vp, {0.0f, 0.0f, 1.5f});
    CHECK(depth_b > depth_a);
    // Non-finite input sorts as depth 0 rather than breaking the ordering.
    CHECK(ModelRenderer::blend_sort_depth(vp, {std::nanf(""), 0.0f, 0.0f}) == 0.0f);
    CHECK(ModelRenderer::blend_sort_depth(vp, {INFINITY, 0.0f, 0.0f}) == 0.0f);
}

TEST_CASE("Blend (model): one blended quad over an opaque one follows source-over, in LDR, HDR and both shading modes")
{
    for (const Mode mode : kModes) {
        Rig rig("blend_model_over", mode);
        {
            const Float3 back_color = is_hdr(mode) ? Float3{3.0f, 1.0f, 0.5f} : Float3{0.7f, 0.4f, 0.2f};
            const Float3 front_color = is_hdr(mode) ? Float3{0.5f, 2.5f, 4.0f} : Float3{0.1f, 0.6f, 0.9f};
            for (const float alpha : {0.0f, 0.25f, 0.5f, 1.0f}) {
                const GpuModel back = rig.quad(rig.emissive(back_color, 1.0f, false), 2.0f);
                const GpuModel front = rig.quad(rig.emissive(front_color, alpha, true), 0.0f);
                const ModelDraw draws[2] = {{&front, Mat4::identity()}, {&back, Mat4::identity()}};  // blended listed first on purpose
                FrameStats stats;
                const Float3 got = rig.center_pixel(rig.render(draws, &stats));
                const Float3 want = rig.displayed(over(front_color, alpha, back_color), mode);
                if (!near(got, want, tolerance_for(mode))) {
                    std::printf("  %s alpha %.2f: got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", mode_name(mode), static_cast<double>(alpha),
                                static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(want.x),
                                static_cast<double>(want.y), static_cast<double>(want.z));
                }
                CHECK(near(got, want, tolerance_for(mode)));
                CHECK(stats.model_blended_primitives == 1);
            }
            // In HDR the composite happens in linear scene light before the tone map: blending already tone-mapped colors differs.
            if (is_hdr(mode)) {
                const float a = 0.5f;
                const Float3 right = rig.displayed(over(front_color, a, back_color), mode);
                const Float3 wrong = over(rig.displayed(front_color, mode), a, rig.displayed(back_color, mode));
                CHECK(!near(right, wrong, 0.04f));  // not vacuous
            }
        }
        CHECK(rig.device->validation_error_count() == 0);
    }
}

TEST_CASE("Blend (model): overlapping blended quads composite back to front whatever the submission order")
{
    for (const Mode mode : {Mode::LdrLegacy, Mode::HdrMetallicRoughness}) {
        Rig rig("blend_model_sort", mode);
        {
            const bool hdr = is_hdr(mode);
            const Float3 colors[3] = {hdr ? Float3{4.0f, 0.2f, 0.2f} : Float3{0.9f, 0.1f, 0.1f}, hdr ? Float3{0.2f, 3.0f, 0.2f} : Float3{0.1f, 0.8f, 0.1f},
                                      hdr ? Float3{0.2f, 0.2f, 5.0f} : Float3{0.1f, 0.1f, 0.9f}};
            const float alphas[3] = {0.5f, 0.6f, 0.4f};
            const Float3 background = hdr ? Float3{1.0f, 1.0f, 0.5f} : Float3{0.5f, 0.5f, 0.3f};
            // Local x positions 0 (nearest), 1, 2 (farthest) and an opaque wall at 3.
            const GpuModel wall = rig.quad(rig.emissive(background, 1.0f, false), 3.0f);
            const GpuModel layers[3] = {rig.quad(rig.emissive(colors[0], alphas[0], true), 0.0f), rig.quad(rig.emissive(colors[1], alphas[1], true), 1.0f),
                                        rig.quad(rig.emissive(colors[2], alphas[2], true), 2.0f)};
            const Float3 want_linear = over(colors[0], alphas[0], over(colors[1], alphas[1], over(colors[2], alphas[2], background)));
            const Float3 want = rig.displayed(want_linear, mode);

            std::array<int, 3> order{0, 1, 2};
            std::vector<uint8_t> first_bytes;
            int permutations = 0;
            do {
                const ModelDraw draws[4] = {{&layers[order[0]], Mat4::identity()}, {&wall, Mat4::identity()}, {&layers[order[1]], Mat4::identity()},
                                            {&layers[order[2]], Mat4::identity()}};
                const test::Image img = rig.render(draws);
                if (first_bytes.empty()) {
                    first_bytes = img.rgba;
                }
                CHECK(img.rgba == first_bytes);  // identical for every permutation of the submission order
                CHECK(near(rig.center_pixel(img), want, tolerance_for(mode)));
                ++permutations;
            } while (std::next_permutation(order.begin(), order.end()));
            CHECK(permutations == 6);
            // Not vacuous: drawing them in the wrong (near to far) order would give a clearly different color.
            const Float3 wrong = rig.displayed(over(colors[2], alphas[2], over(colors[1], alphas[1], over(colors[0], alphas[0], background))), mode);
            CHECK(!near(want, wrong, 0.05f));
        }
        CHECK(rig.device->validation_error_count() == 0);
    }
}

TEST_CASE("Blend (model): the sort uses the placement's world matrix, so one model drawn at several places sorts by place")
{
    Rig rig("blend_model_world_sort", Mode::LdrLegacy);
    {
        // Two models with identical local geometry and bounds, told apart only by color, placed at different depths by their
        // draw matrices; a wall behind. Sorting must follow the world positions, not the (equal) local centers.
        const Float3 red{0.9f, 0.1f, 0.1f}, blue{0.1f, 0.1f, 0.9f}, wall_color{0.4f, 0.4f, 0.4f};
        const GpuModel wall = rig.quad(rig.emissive(wall_color, 1.0f, false), 4.0f);
        const GpuModel a = rig.quad(rig.emissive(red, 0.5f, true), 0.0f);
        const GpuModel b = rig.quad(rig.emissive(blue, 0.5f, true), 0.0f);
        for (const bool a_is_near : {true, false}) {
            const Mat4 near_place = Mat4::identity();
            const Mat4 far_place = translation({1.5f, 0.0f, 0.0f});
            const ModelDraw draws[3] = {{&a, a_is_near ? near_place : far_place}, {&wall, Mat4::identity()}, {&b, a_is_near ? far_place : near_place}};
            const Float3 near_c = a_is_near ? red : blue, far_c = a_is_near ? blue : red;
            const Float3 want = over(near_c, 0.5f, over(far_c, 0.5f, wall_color));
            CHECK(near(rig.center_pixel(rig.render(draws)), want, 0.012f));
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Blend (model): blended draws test depth against opaque ones and never write it")
{
    Rig rig("blend_model_depth", Mode::LdrLegacy);
    {
        const Float3 opaque_c{0.2f, 0.8f, 0.3f}, glass_c{0.9f, 0.2f, 0.2f}, floor_c{0.3f, 0.3f, 0.6f};
        const GpuModel floor = rig.quad(rig.emissive(floor_c, 1.0f, false), 4.0f);
        const GpuModel glass_far = rig.quad(rig.emissive(glass_c, 0.5f, true), 2.0f);
        const GpuModel opaque_near = rig.quad(rig.emissive(opaque_c, 1.0f, false), 1.0f);
        {
            // An opaque quad in front hides a blended quad behind it completely, whatever the draw order.
            const ModelDraw draws[3] = {{&glass_far, Mat4::identity()}, {&opaque_near, Mat4::identity()}, {&floor, Mat4::identity()}};
            CHECK(near(rig.center_pixel(rig.render(draws)), opaque_c, 0.012f));
        }
        {
            // A blended quad in front of the opaque one blends with it.
            const GpuModel glass_near = rig.quad(rig.emissive(glass_c, 0.5f, true), 0.0f);
            const ModelDraw draws[3] = {{&glass_near, Mat4::identity()}, {&opaque_near, Mat4::identity()}, {&floor, Mat4::identity()}};
            CHECK(near(rig.center_pixel(rig.render(draws)), over(glass_c, 0.5f, opaque_c), 0.012f));
        }
        {
            // Depth is not written: two coplanar blended quads (same depth, so the second would fail a `greater` depth test if the
            // first had written it) both contribute, in submission order.
            const Float3 c1{0.9f, 0.1f, 0.1f}, c2{0.1f, 0.1f, 0.9f};
            const GpuModel q1 = rig.quad(rig.emissive(c1, 0.5f, true), 1.0f);
            const GpuModel q2 = rig.quad(rig.emissive(c2, 0.5f, true), 1.0f);
            const ModelDraw draws[3] = {{&q1, Mat4::identity()}, {&q2, Mat4::identity()}, {&floor, Mat4::identity()}};
            const Float3 want = over(c2, 0.5f, over(c1, 0.5f, floor_c));  // equal depth: submission order breaks the tie
            CHECK(near(rig.center_pixel(rig.render(draws)), want, 0.012f));
            const ModelDraw swapped[3] = {{&q2, Mat4::identity()}, {&q1, Mat4::identity()}, {&floor, Mat4::identity()}};
            CHECK(near(rig.center_pixel(rig.render(swapped)), over(c1, 0.5f, over(c2, 0.5f, floor_c)), 0.012f));
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

namespace {

/// One double-sided surface made of two quads in a single primitive: the near one faces the camera, the far one faces away (its
/// winding is reversed), and the near one is first in the index buffer, so a single unsorted pass would draw it before the far
/// one. Normals differ so the two lit colors differ; the shader flips the far one's normal because it is seen from behind.
GpuModel two_layer_surface(Rig& rig, GpuMaterial material, Float3 near_normal, Float3 far_normal)
{
    std::vector<ModelVertex> v;
    const auto add = [&](float x, Float3 n, bool) {
        const float h = 1.0f, zc = 1.5f;
        v.push_back({{x, +h, zc + h}, n, {0, 0}});
        v.push_back({{x, -h, zc + h}, n, {1, 0}});
        v.push_back({{x, +h, zc - h}, n, {0, 1}});
        v.push_back({{x, -h, zc - h}, n, {1, 1}});
    };
    add(0.0f, near_normal, true);
    add(1.0f, far_normal, false);
    const std::vector<uint32_t> indices = {0, 2, 3, 0, 3, 1,   // near quad, counter-clockwise seen from -X: front facing
                                           4, 7, 6, 4, 5, 7};  // far quad, reversed: back facing
    GpuModel m;
    m.name = "two layers";
    m.materials.push_back(std::move(material));
    m.meshes.push_back({make_primitive(rig.renderer->registry(), v, indices, 0)});
    m.meshes[0][0].bounds_center = {0.5f, 0.0f, 1.5f};
    m.meshes[0][0].bounds_radius = 1.6f;
    m.placements.push_back({0, Mat4::identity()});
    return m;
}

Float3 legacy_k(Float3 n, const RendererSettings& s)
{
    const Float3 sun = normalize(Float3{-0.40f, -0.55f, 0.73f});
    const float ndl = std::max(0.0f, dot(n, sun));
    const float t = n.z * 0.5f + 0.5f;
    const Float3 ground{0.20f, 0.18f, 0.16f};
    const Float3 sky{s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};
    const Float3 ambient = ground * (1.0f - t) + sky * t;
    return {ambient.x + 1.00f * ndl, ambient.y + 0.95f * ndl, ambient.z + 0.85f * ndl};
}

} // namespace

TEST_CASE("Blend (model): a double-sided blended surface draws its back faces first, then its front faces")
{
    Rig rig("blend_model_two_pass", Mode::LdrLegacy);
    {
        const Float3 near_normal{-1.0f, 0.0f, 0.0f};
        const Float3 far_normal = normalize(Float3{0.8f, 0.4f, 0.3f});  // outward, away from the camera; flipped when seen from behind
        const Float3 floor_c{0.3f, 0.3f, 0.5f};
        const float alpha = 0.5f;
        const GpuModel floor = rig.quad(rig.emissive(floor_c, 1.0f, false), 4.0f);
        const Float3 k_near = legacy_k(near_normal, rig.settings);
        const Float3 k_far = legacy_k(far_normal * -1.0f, rig.settings);  // the back face lights with the reversed normal
        CHECK(!near(k_near, k_far, 0.05f));                               // not vacuous

        const auto surface = [&](bool double_sided) {
            MaterialData d;  // white base: the lit color is the lighting term itself
            d.base_color[3] = alpha;
            d.metallic = 0.0f;
            d.alpha_blend = true;
            d.double_sided = double_sided;
            return two_layer_surface(rig, rig.renderer->model_renderer().create_material(d, MaterialTextures{}, "two.layer"), near_normal, far_normal);
        };
        const GpuModel two_sided = surface(true);
        const GpuModel one_sided = surface(false);

        {
            const ModelDraw draws[2] = {{&two_sided, Mat4::identity()}, {&floor, Mat4::identity()}};
            FrameStats stats;
            const Float3 got = rig.center_pixel(rig.render(draws, &stats));
            const Float3 want = over(k_near, alpha, over(k_far, alpha, floor_c));  // far (back face) first, near (front face) last
            const Float3 wrong = over(k_far, alpha, over(k_near, alpha, floor_c)); // what one unsorted pass in index order would give
            CHECK(!near(want, wrong, 0.02f));
            CHECK(near(got, want, 0.012f));
            CHECK(stats.model_blended_primitives == 1);
        }
        {
            // Single-sided: only the front face is drawn; the far quad, seen from behind, is culled.
            const ModelDraw draws[2] = {{&one_sided, Mat4::identity()}, {&floor, Mat4::identity()}};
            const Float3 got = rig.center_pixel(rig.render(draws));
            CHECK(near(got, over(k_near, alpha, floor_c), 0.012f));
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Blend (model): a mirrored instance blends like its unmirrored twin (winding and face passes)")
{
    for (const bool double_sided : {false, true}) {
        Rig rig("blend_model_mirror", Mode::LdrLegacy);
        {
            const Float3 c{0.2f, 0.7f, 0.9f}, floor_c{0.5f, 0.3f, 0.3f};
            const GpuModel floor = rig.quad(rig.emissive(floor_c, 1.0f, false), 4.0f);
            const GpuModel glass = rig.quad(rig.emissive(c, 0.5f, true, double_sided), 0.0f);
            const ModelDraw plain[2] = {{&glass, Mat4::identity()}, {&floor, Mat4::identity()}};
            const ModelDraw mirrored[2] = {{&glass, scale_matrix({1.0f, -1.0f, 1.0f})}, {&floor, Mat4::identity()}};
            const test::Image a = rig.render(plain);
            const test::Image b = rig.render(mirrored);
            CHECK(a.rgba == b.rgba);  // the quad is symmetric about y = 0, so the mirror image is identical
            CHECK(near(rig.center_pixel(b), over(c, 0.5f, floor_c), 0.012f));
        }
        CHECK(rig.device->validation_error_count() == 0);
    }
}

TEST_CASE("Blend (model): frames submitted back to back with blended draws, in LDR and HDR, stay validation clean")
{
    for (const Mode mode : {Mode::LdrMetallicRoughness, Mode::HdrMetallicRoughness}) {
        Rig rig("blend_model_in_flight", mode);
        {
            const GpuModel wall = rig.quad(rig.emissive({0.5f, 0.5f, 0.5f}, 1.0f, false), 3.0f);
            const GpuModel a = rig.quad(rig.emissive({0.9f, 0.1f, 0.1f}, 0.5f, true), 0.0f);
            const GpuModel b = rig.quad(rig.emissive({0.1f, 0.9f, 0.1f}, 0.5f, true, true), 1.0f);
            rhi::TextureDesc td;
            td.format = rhi::Format::RGBA8_SRGB;
            td.width = kW;
            td.height = kH;
            td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
            td.debug_name = "InFlight";
            const rhi::TextureHandle color = rig.device->create_texture(td);
            const rhi::ResourceState written{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite,
                                             rhi::TextureLayout::ColorAttachment};
            for (uint64_t frame = 0; frame < 8; ++frame) {
                const ModelDraw draws[3] = {{&a, Mat4::identity()}, {&wall, Mat4::identity()}, {&b, Mat4::identity()}};
                SceneFrame sf;
                sf.tick = frame;
                sf.camera = &rig.camera;
                sf.models = draws;
                RenderTarget target;
                target.texture = color;
                target.width = kW;
                target.height = kH;
                if (frame > 0) target.initial_state = written;
                target.final_state = written;
                rhi::CommandList& cmd = rig.device->begin_frame();
                const FrameStats stats = rig.renderer->render(cmd, sf, target);
                CHECK(stats.model_blended_primitives == 2);
                (void)rig.device->end_frame();
            }
            rig.device->wait_idle();
            rig.device->destroy(color);
        }
        CHECK(rig.device->validation_error_count() == 0);
    }
}

TEST_CASE("Blend (model): many blended primitives sort deterministically; the CPU cost of the sorted phase is measured")
{
    Rig rig("blend_model_many", Mode::LdrLegacy);
    {
        const GpuModel glass = rig.quad(rig.emissive({0.3f, 0.5f, 0.8f}, 0.3f, true), 0.0f, 1.5f, 0.2f);
        const GpuModel solid = rig.quad(rig.emissive({0.3f, 0.5f, 0.8f}, 1.0f, false), 0.0f, 1.5f, 0.2f);
        const auto place = [](int i) {
            return translation({static_cast<float>(i % 37) * 0.3f, static_cast<float>((i * 7) % 41) * 0.05f - 1.0f, 0.0f});
        };
        std::vector<ModelDraw> blended, opaque;
        for (int i = 0; i < 1000; ++i) {
            blended.push_back({&glass, place(i)});
            opaque.push_back({&solid, place(i)});
        }
        FrameStats stats;
        const test::Image a = rig.render(blended, &stats);
        CHECK(stats.model_blended_primitives == 1000);
        const test::Image b = rig.render(blended);
        CHECK(a.rgba == b.rgba);  // reproducible frame to frame

        // CPU time to record the frame (what the sort adds), without rasterizing: record, then wait outside the timed region.
        const auto record_ms = [&](std::span<const ModelDraw> draws) {
            double total = 0.0;
            for (int i = 0; i < 20; ++i) {
                SceneFrame sf;
                sf.camera = &rig.camera;
                sf.models = draws;
                RenderTarget target;
                target.texture = rig.target->color_texture();
                target.width = kW;
                target.height = kH;
                rhi::CommandList& cmd = rig.device->begin_frame();
                const auto started = std::chrono::steady_clock::now();
                (void)rig.renderer->render(cmd, sf, target);
                total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                (void)rig.device->end_frame();
                rig.device->wait_idle();
            }
            return total / 20.0;
        };
        const double blended_ms = record_ms(blended);
        const double opaque_ms = record_ms(opaque);
        std::printf("  recording 1000 primitives on the CPU: blended (sorted) %.3f ms, opaque %.3f ms\n", blended_ms, opaque_ms);
    }
    CHECK(rig.device->validation_error_count() == 0);
}
