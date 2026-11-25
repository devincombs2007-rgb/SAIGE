// Cascaded sun shadows on a real (validated) Vulkan device. Columns stand on a long ground at the middle of each cascade's slice of the view range, and
// every ground point the camera sees is compared with an independent CPU ray cast toward the sun: a point is shadowed exactly when that ray hits a column,
// and the pixel must be albedo * (ambient + visibility * sun) accordingly. Also: the blend across a split leaves no seam, the range ends at `distance`,
// the atlas geometry, the settings validation and the named quality presets. One cascade (the default) is covered by test_sun_shadows.cpp and is unchanged.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/shadow_cascades.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 320;
constexpr uint32_t kH = 240;
constexpr float kDistance = 60.0f;

Float3 sun() { return normalize(SceneLighting{}.sun_direction); }

RendererSettings cascade_settings(uint32_t cascades, float blend = 0.1f, uint32_t filter = 1, uint32_t map_size = 1024, float normal_offset = 1.5f)
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.ground_half_size = 0.01f;  // the renderer's own checkerboard ground is coplanar with the model ground below and would z-fight with it
    s.shadows.enabled = true;
    s.shadows.map_size = map_size;
    s.shadows.cascades = cascades;
    s.shadows.cascade_blend = blend;
    s.shadows.filter_radius = filter;
    s.shadows.normal_offset_texels = normal_offset;
    s.shadows.distance = kDistance;
    return s;
}

Float3 ambient_up(const RendererSettings& s)
{
    return {s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};  // the ground faces +Z, so the sky half of the hemispherical ambient
}

struct Box {
    Float3 lo, hi;
};

bool ray_hits_box(Float3 origin, Float3 dir, const Box& box)
{
    float t0 = 1e-3f, t1 = std::numeric_limits<float>::max();
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z}, l[3] = {box.lo.x, box.lo.y, box.lo.z}, h[3] = {box.hi.x, box.hi.y, box.hi.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-9f) {
            if (o[i] < l[i] || o[i] > h[i]) return false;
            continue;
        }
        float a = (l[i] - o[i]) / d[i], b = (h[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

bool shadowed(Float3 q, const std::vector<Box>& boxes)
{
    for (const Box& b : boxes) {
        if (ray_hits_box(q, sun(), b)) return true;
    }
    return false;
}

struct Pixel {
    int x, y;
};

struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;
    RendererSettings settings;
    GpuModel ground, column;
    Camera camera;
    Mat4 view_proj;

    explicit Fixture(const RendererSettings& s) : settings(s)
    {
        device = make_validated_device("cascades", /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, settings);
        target = std::make_unique<OffscreenTarget>(*device, kW, kH);
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.5f;
        const Float3 n{0, 0, 1};
        const float half = 120.0f;
        const std::vector<ModelVertex> v = {{{-half, -half, 0}, n, {0, 0}}, {{half, -half, 0}, n, {1, 0}}, {{half, half, 0}, n, {1, 1}}, {{-half, half, 0}, n, {0, 1}}};
        ground.name = "ground";
        ground.materials.push_back(renderer->model_renderer().create_material(grey, {}, {}, "ground"));
        ground.meshes.push_back({make_primitive(renderer->registry(), v, {0, 1, 2, 0, 2, 3}, 0)});
        ground.placements.push_back({0, Mat4::identity()});
        column = make_cube(renderer->registry(), renderer->model_renderer().create_material(grey, {}, {}, "column"));
        camera = look_at_camera({0.0f, -1.0f, 3.2f}, {0.0f, 40.0f, 0.0f});
        view_proj = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    }
    ~Fixture()
    {
        column = {};
        ground = {};
        target.reset();
        renderer.reset();
    }

    /// A column of the given footprint (x, y extents) and height, on the ground, centered at (x, y).
    ModelDraw column_draw(float x, float y, float sx, float sy, float height) const
    {
        return {&column, translation({x, y, height * 0.5f}) * scale_matrix({sx, sy, height})};
    }
    static Box column_box(float x, float y, float sx, float sy, float height) { return {{x - sx / 2, y - sy / 2, 0.0f}, {x + sx / 2, y + sy / 2, height}}; }

    Pixel pixel_of(Float3 world) const
    {
        const Projected p = project_point(view_proj, world);
        const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(kW), static_cast<float>(kH));
        return {static_cast<int>(px.x), static_cast<int>(px.y)};
    }
    float depth_of(Float3 world) const { return dot(world - camera.position, camera_forward(camera)); }
};

Float3 linear_at(const Image& img, Pixel p)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

struct Probe {
    Float3 q;
    bool shadow;
};

/// Ground points around the occluders whose classification is the same for every neighbor within `margin`, visible to the camera (no occluder in
/// front), and inside the image.
std::vector<Probe> probes_around(const Fixture& f, const std::vector<Box>& boxes, float x0, float x1, float y0, float y1, float step, float margin)
{
    std::vector<Probe> out;
    for (float x = x0; x <= x1; x += step) {
        for (float y = y0; y <= y1; y += step) {
            const Float3 q{x, y, 0.0f};
            bool visible = true;
            for (const Float3 d : {Float3{0, 0, 0}, Float3{margin, 0, 0}, Float3{-margin, 0, 0}, Float3{0, margin, 0}, Float3{0, -margin, 0}}) {
                for (const Box& b : boxes) {
                    visible = visible && !ray_hits_box(q + d, normalize(f.camera.position - (q + d)), b);
                    // Not beside the column either: its silhouette pixels (and the filter's footprint) are the column's, not the ground's.
                    const float pad = std::max(0.6f, 0.03f * f.depth_of(q));
                    visible = visible && !(q.x + d.x > b.lo.x - pad && q.x + d.x < b.hi.x + pad && q.y + d.y > b.lo.y - pad && q.y + d.y < b.hi.y + pad);
                }
            }
            if (!visible) continue;
            const bool s = shadowed(q, boxes);
            bool interior = true;
            for (const Float3 d : {Float3{margin, 0, 0}, Float3{-margin, 0, 0}, Float3{0, margin, 0}, Float3{0, -margin, 0}}) {
                interior = interior && (shadowed(q + d, boxes) == s);
            }
            if (!interior) continue;
            const Pixel px = f.pixel_of(q);
            if (px.x < 1 || px.y < 1 || px.x >= static_cast<int>(kW) - 1 || px.y >= static_cast<int>(kH) - 1) continue;
            out.push_back({q, s});
        }
    }
    return out;
}

struct Tally {
    int shadowed = 0, lit = 0, wrong = 0;
};

Tally check_probes(const Fixture& f, const Image& img, const std::vector<Probe>& probes, const char* what)
{
    const Float3 up{0, 0, 1};
    const float ndl = std::max(0.0f, dot(up, sun()));
    const Float3 ambient = ambient_up(f.settings);
    const Float3 lit = (ambient + Float3{1.00f * ndl, 0.95f * ndl, 0.85f * ndl}) * 0.5f;
    const Float3 dark = ambient * 0.5f;
    Tally t;
    for (const Probe& p : probes) {
        const Float3 got = linear_at(img, f.pixel_of(p.q));
        const bool ok = near3(got, p.shadow ? dark : lit, 0.035f);
        (p.shadow ? t.shadowed : t.lit) += 1;
        t.wrong += ok ? 0 : 1;
        if (!ok && t.wrong <= 6) {
            std::printf("  %s: wrong at (%.2f, %.2f) depth %.1f: oracle says %s, pixel (%.3f %.3f %.3f)\n", what, static_cast<double>(p.q.x), static_cast<double>(p.q.y),
                        static_cast<double>(f.depth_of(p.q)), p.shadow ? "shadow" : "lit", static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z));
        }
    }
    return t;
}

} // namespace

TEST_CASE("Cascades: shadows at every depth match an independent ray cast, with two, three and four cascades")
{
    for (const uint32_t count : {2u, 3u, 4u}) {
        Fixture f(cascade_settings(count));
        const std::array<float, kMaxShadowCascades + 1> splits = cascade_splits(0.1f, kDistance, count, 0.75f);
        // One column in the middle of each cascade's slice (view depth along +Y is y + 1), plus one in the nearest part of the first slice.
        std::vector<Box> boxes;
        std::vector<ModelDraw> draws{{&f.ground, Mat4::identity()}};
        std::vector<float> depths;
        for (uint32_t c = 0; c < count; ++c) depths.push_back(0.5f * (splits[c] + splits[c + 1]));
        float x = -3.0f;
        std::vector<float> sizes, heights;
        for (const float depth : depths) {  // far columns are bigger, so their shadows span many pixels and the shadow map's far, coarser texels
            const float y = depth - 1.0f, size = std::max(1.0f, 0.06f * depth), height = 3.0f + 0.1f * depth;
            draws.push_back(f.column_draw(x, y, size, size, height));
            boxes.push_back(Fixture::column_box(x, y, size, size, height));
            sizes.push_back(size);
            heights.push_back(height);
            x += 2.0f + 0.04f * depth;
        }
        FrameStats stats;
        const Image img = f.target->render(*f.renderer, {}, f.camera, 0, draws, &stats);
        CHECK(f.device->validation_error_count() == 0);
        CHECK(stats.shadow_draw_calls == (1 + count) * count);  // the ground and each column, once per cascade
        int shadowed_total = 0, lit_total = 0, wrong_total = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const Box& b = boxes[i];
            const float margin = std::max(0.2f, 0.02f * depths[i]);  // a pixel's footprint and the filter grow with depth
            const float reach = heights[i];
            const std::vector<Probe> probes = probes_around(f, boxes, b.lo.x - 1.5f, b.hi.x + 0.6f * reach + 1.0f, b.lo.y - 1.5f, b.hi.y + 0.8f * reach + 1.0f, std::max(0.25f, 0.02f * depths[i]), margin);
            const Tally t = check_probes(f, img, probes, "cascade");
            shadowed_total += t.shadowed;
            lit_total += t.lit;
            wrong_total += t.wrong;
            if (t.shadowed < 2 && depths[i] > 6.0f) std::printf("  column %zu (depth %.1f): only %d shadowed probes\n", i, static_cast<double>(depths[i]), t.shadowed);
            CHECK(t.shadowed >= 2 || depths[i] <= 6.0f);  // every visible column's shadow is really probed (the camera sees the ground from 4.6 m on)
        }
        std::printf("  %u cascades: %d shadowed and %d lit probes, %d wrong\n", count, shadowed_total, lit_total, wrong_total);
        CHECK(shadowed_total >= static_cast<int>(8 * count) && lit_total >= static_cast<int>(40 * count));
        CHECK(wrong_total == 0);
    }
}

TEST_CASE("Cascades: a shadow across a split has no seam, with and without the blend, and the range ends at the distance")
{
    for (const float blend : {0.0f, 0.2f}) {
        Fixture f(cascade_settings(2, blend));
        const std::array<float, kMaxShadowCascades + 1> splits = cascade_splits(0.1f, kDistance, 2, 0.75f);
        const float split = splits[1];
        // A wall 14 m long in depth, 3 m wide and 6 m high, straddling the split: its shadow lies on both sides of the split (and across the blend band).
        const float y = split - 1.0f, length = 14.0f;
        std::vector<Box> boxes{Fixture::column_box(-2.0f, y, 3.0f, length, 6.0f)};
        std::vector<ModelDraw> draws{{&f.ground, Mat4::identity()}, f.column_draw(-2.0f, y, 3.0f, length, 6.0f)};
        const Image img = f.target->render(*f.renderer, {}, f.camera, 0, draws);
        CHECK(f.device->validation_error_count() == 0);
        const float margin = 0.8f;  // the far cascade's edges are coarser (and ragged under the filter): judge the interior of the shadow and of the light
        const std::vector<Probe> probes = probes_around(f, boxes, -6.0f, 9.0f, y - length / 2 - 2.0f, y + length / 2 + 9.0f, std::max(0.25f, 0.02f * split), margin);
        const Tally t = check_probes(f, img, probes, blend > 0.0f ? "blend" : "hard split");
        int before = 0, after = 0;  // shadowed probes on each side of the split
        for (const Probe& p : probes) {
            if (!p.shadow) continue;
            (f.depth_of(p.q) < split ? before : after) += 1;
        }
        std::printf("  blend %.1f: %d shadowed probes before the split, %d after, %d wrong\n", static_cast<double>(blend), before, after, t.wrong);
        CHECK(before >= 5 && after >= 5);
        CHECK(t.wrong == 0);
    }
    // Beyond `distance` the sun is not occluded, even where a ray cast says it is and where the last cascade's sphere still covers the point: a tall column just
    // past the range, and its shadow a few meters farther, still inside the range's last (rounded) cascade.
    {
        Fixture f(cascade_settings(3));
        const float depth = kDistance + 2.5f;
        std::vector<Box> boxes{Fixture::column_box(0.0f, depth - 1.0f, 2.0f, 2.0f, 14.0f)};
        std::vector<ModelDraw> draws{{&f.ground, Mat4::identity()}, f.column_draw(0.0f, depth - 1.0f, 2.0f, 2.0f, 14.0f)};
        const Image img = f.target->render(*f.renderer, {}, f.camera, 0, draws);
        const Float3 up{0, 0, 1};
        const float ndl = std::max(0.0f, dot(up, sun()));
        const Float3 lit = (ambient_up(f.settings) + Float3{1.00f * ndl, 0.95f * ndl, 0.85f * ndl}) * 0.5f;
        int beyond = 0;
        for (const Float3 q : {Float3{3.0f, depth + 3.0f, 0.0f}, Float3{4.0f, depth + 4.5f, 0.0f}, Float3{5.0f, depth + 6.0f, 0.0f}}) {
            CHECK(shadowed(q, boxes));
            CHECK(f.depth_of(q) > kDistance);
            CHECK(near3(linear_at(img, f.pixel_of(q)), lit, 0.035f));
            ++beyond;
        }
        CHECK(beyond == 3);
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("Cascades: the blend replaces the jump between two cascades that disagree with a ramp")
{
    // A large receiver normal offset (in texels) moves receivers by a different distance in each cascade (their texels differ in size), so the two cascades
    // place the wall's shadow edge differently near the split and a hard switch shows a step. Find the ground column where that step is largest, then
    // measure the same column with the blend: its largest step between neighboring rows is a fraction of it.
    const std::array<float, kMaxShadowCascades + 1> splits = cascade_splits(0.1f, kDistance, 2, 0.75f);
    const float y_split = splits[1] - 1.0f, length = 16.0f;
    Fixture hard_fixture(cascade_settings(2, 0.0f, 1, 1024, 40.0f)), soft_fixture(cascade_settings(2, 0.2f, 1, 1024, 40.0f));
    const auto image = [&](Fixture& f) {
        const std::vector<ModelDraw> draws{{&f.ground, Mat4::identity()}, f.column_draw(-2.0f, y_split, 3.0f, length, 6.0f)};
        return f.target->render(*f.renderer, {}, f.camera, 0, draws);
    };
    const Image hard = image(hard_fixture), soft = image(soft_fixture);
    CHECK(hard_fixture.device->validation_error_count() == 0 && soft_fixture.device->validation_error_count() == 0);
    const auto shade = [&](const Fixture& f, const Image& img, float x, float y) { return linear_at(img, f.pixel_of({x, y, 0.0f})).y; };
    float best_x = 0.0f, best_jump = 0.0f;
    for (float x = -4.0f; x <= 8.0f; x += 0.1f) {
        const float jump = std::fabs(shade(hard_fixture, hard, x, y_split + 0.3f) - shade(hard_fixture, hard, x, y_split - 0.3f));
        if (jump > best_jump) {
            best_jump = jump;
            best_x = x;
        }
    }
    const float band = 0.2f * (splits[1] - splits[0]);  // the blend band's depth range
    const auto largest_step = [&](const Fixture& f, const Image& img) {
        float worst = 0.0f;
        float previous = shade(f, img, best_x, y_split - band - 0.5f);
        for (float y = y_split - band - 0.4f; y <= y_split + 0.5f; y += 0.1f) {
            const float now = shade(f, img, best_x, y);
            worst = std::max(worst, std::fabs(now - previous));
            previous = now;
        }
        return worst;
    };
    const float hard_step = largest_step(hard_fixture, hard), soft_step = largest_step(soft_fixture, soft);
    std::printf("  at x = %.1f: the split jumps by %.3f; the largest step between neighboring rows is %.3f hard and %.3f blended\n", static_cast<double>(best_x),
                static_cast<double>(best_jump), static_cast<double>(hard_step), static_cast<double>(soft_step));
    CHECK(best_jump > 0.05f);             // the two cascades really disagree here, so the test could fail
    CHECK(soft_step < 0.6f * hard_step);  // the blend spreads the jump over a ramp
}

TEST_CASE("Cascades: filter taps at a tile's edges stay inside the tile, whatever the neighbouring tiles hold")
{
    // Three cascades in a 2 x 2 atlas. Tile 0 is covered end to end by a caster; the tiles to its right and below it are empty, so a tap that strays into their first column or
    // row reads "lit" while every tap inside tile 0 reads "shadowed". The receivers sit in tile 0's last texel column and row: their filter taps (radius 1 and 2) reach past the edge, and must
    // clamp to the last texel of their own tile. (Clamping the tile-local coordinate to exactly 1 lands on the next tile's first texel; no clamp at all lands further out.)
    const auto device = make_validated_device("cascade_tile_edge", /*synchronization=*/true);
    GpuResourceRegistry registry(*device);
    for (const uint32_t variant : {0u, 1u, 2u, 3u}) {
        const uint32_t radius = 1 + variant % 2;
        const bool receiver_in_cascade_1 = variant >= 2;  // variant 2 and 3: the receiver reads the empty tile 1, whose left and top neighbours hold the caster's tile
        ModelRenderSettings ms;
        ms.shadows.enabled = true;
        ms.shadows.map_size = 64;
        ms.shadows.cascades = 3;  // a 2 x 2 atlas: tile 0 has a neighbour to its right (cascade 1) and one below it (cascade 2)
        ms.fog_density = 0.0f;
        ModelRenderer models(*device, registry, rhi::Format::RGBA8_SRGB, rhi::Format::D32_FLOAT, ms);
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.5f;
        const GpuMaterial material = models.create_material(grey, {}, {}, "grey");
        const auto slab = [&](float half, float z) {
            const Float3 n{0, 0, 1};
            const std::vector<ModelVertex> v = {{{-half, -half, z}, n, {0, 0}}, {{half, -half, z}, n, {1, 0}}, {{half, half, z}, n, {1, 1}}, {{-half, half, z}, n, {0, 1}}};
            GpuModel m;
            m.name = "slab";
            m.materials.push_back(material);
            m.meshes.push_back({make_primitive(registry, v, {0, 1, 2, 0, 2, 3}, 0)});
            m.placements.push_back({0, Mat4::identity()});
            return m;
        };
        const GpuModel ground = slab(60.0f, 0.0f), caster = slab(30.0f, 5.0f);
        // A straight-down sun: x and y in [-8, 8] map to the tile, and height z to reverse-Z depth z / 40 + 0.5 (higher is nearer the sun, so greater).
        Mat4 light = Mat4::identity();
        light.m[0] = 1.0f / 8.0f;
        light.m[5] = 1.0f / 8.0f;
        light.m[10] = 1.0f / 40.0f;
        light.m[14] = 0.5f;
        ModelShadowFrame sf;
        sf.light_view_proj = light;
        sf.more_view_proj[0] = sf.more_view_proj[1] = light;
        sf.texel_world_size = 0.25f;
        sf.more_texel_world_size[0] = sf.more_texel_world_size[1] = 0.25f;
        sf.normal_offset_texels = 0.0f;
        sf.filter_radius = radius;
        sf.cascade_count = 3;
        sf.split_far = receiver_in_cascade_1 ? std::array<float, 4>{10.0f, 1000.0f, 3000.0f, 0.0f} : std::array<float, 4>{1000.0f, 2000.0f, 3000.0f, 0.0f};  // the ground is 20 m deep
        sf.camera_position = {0.0f, 0.0f, 20.0f};
        sf.camera_forward = {0.0f, 0.0f, -1.0f};
        sf.blend = 0.0f;

        const Camera camera = look_at_camera({0.0f, 0.0f, 20.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});  // straight down, +X right, +Y up
        const Mat4 view_proj = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));

        rhi::TextureDesc cd;
        cd.format = rhi::Format::RGBA8_SRGB;
        cd.width = kW;
        cd.height = kH;
        cd.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        const rhi::TextureHandle color = device->create_texture(cd);
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width = kW;
        dd.height = kH;
        dd.usage = rhi::TextureUsage::DepthStencilAttachment;
        const rhi::TextureHandle depth = device->create_texture(dd);
        rhi::BufferDesc bd;
        bd.size = uint64_t{kW} * kH * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        const rhi::BufferHandle readback = device->create_buffer(bd);

        const rhi::ResourceState none{rhi::PipelineStage::AllCommands, rhi::Access::None, rhi::TextureLayout::Undefined};
        const rhi::ResourceState depth_write{rhi::PipelineStage::EarlyFragmentTests | rhi::PipelineStage::LateFragmentTests, rhi::Access::DepthStencilWrite,
                                             rhi::TextureLayout::DepthStencilAttachment};
        const rhi::ResourceState color_write{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment};
        rhi::CommandList& cmd = device->begin_frame();
        models.set_shadow_frame(&sf);
        const rhi::TextureBarrier init[] = {{models.shadow_map(), none, depth_write, {}}, {depth, none, depth_write, {}}, {color, none, color_write, {}}};
        cmd.barrier(init);
        {  // the shadow pass: the caster into tile 0 only
            rhi::RenderingDesc rd;
            rhi::DepthAttachment da;
            da.texture = models.shadow_map();
            da.load = rhi::AttachmentLoadOp::Clear;
            da.store = rhi::AttachmentStoreOp::Store;
            da.clear_depth = 0.0f;
            rd.depth = da;
            rd.area = {0, 0, models.shadow_atlas_width(), models.shadow_atlas_height()};
            cmd.begin_rendering(rd);
            const uint32_t tile = models.shadow_map_size();
            cmd.set_viewport({0.0f, 0.0f, static_cast<float>(tile), static_cast<float>(tile), 0.0f, 1.0f});
            cmd.set_scissor({0, 0, tile, tile});
            ModelFrameStats shadow_stats;
            const ModelDraw casters[] = {{&caster, Mat4::identity()}};
            models.draw_shadow(cmd, light, casters, shadow_stats);
            cmd.end_rendering();
        }
        const rhi::TextureBarrier to_sampled{models.shadow_map(), depth_write, rhi::sampled_texture_state, {}};
        cmd.barrier(std::span(&to_sampled, 1));
        {  // the color pass: only the receiver
            rhi::RenderingDesc rd;
            rhi::ColorAttachment ca;
            ca.texture = color;
            ca.clear = {0.0f, 0.0f, 0.0f, 1.0f};
            rd.colors.push_back(ca);
            rhi::DepthAttachment da;
            da.texture = depth;
            da.clear_depth = 0.0f;
            rd.depth = da;
            rd.area = {0, 0, kW, kH};
            cmd.begin_rendering(rd);
            cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f});
            cmd.set_scissor({0, 0, kW, kH});
            ModelFrameStats stats;
            const ModelDraw receivers[] = {{&ground, Mat4::identity()}};
            models.draw(cmd, view_proj, camera.position, receivers, stats);
            cmd.end_rendering();
        }
        const rhi::TextureBarrier to_transfer{color, color_write, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(color, readback);
        (void)device->end_frame();
        device->wait_idle();
        const auto mapped = device->map(readback);
        Image img;
        img.width = kW;
        img.height = kH;
        img.rgba.assign(reinterpret_cast<const uint8_t*>(mapped.data()), reinterpret_cast<const uint8_t*>(mapped.data()) + size_t{kW} * kH * 4);

        const Float3 up{0, 0, 1};
        const float ndl = std::max(0.0f, dot(up, sun()));
        const Float3 sky{ms.sky.r * 0.6f, ms.sky.g * 0.6f, ms.sky.b * 0.6f};
        const Float3 lit = (sky + Float3{1.00f * ndl, 0.95f * ndl, 0.85f * ndl}) * 0.5f, dark = sky * 0.5f;
        const auto pixel_of = [&](Float3 world) {
            const Projected p = project_point(view_proj, world);
            const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(kW), static_cast<float>(kH));
            return Pixel{static_cast<int>(px.x), static_cast<int>(px.y)};
        };
        if (!receiver_in_cascade_1) {
            // Inside tile 0 everything is shadowed, including the last texel column and row, the corner and the receivers whose taps reach beyond the edge.
            for (const Float3 q : {Float3{7.86f, 0.0f, 0.0f}, Float3{7.86f, 3.0f, 0.0f}, Float3{0.0f, -7.86f, 0.0f}, Float3{-3.0f, -7.86f, 0.0f}, Float3{7.86f, -7.86f, 0.0f},
                                   Float3{7.0f, 0.0f, 0.0f}, Float3{0.0f, 0.0f, 0.0f}}) {
                const Float3 got = linear_at(img, pixel_of(q));
                const bool ok = near3(got, dark, 0.03f);
                if (!ok) std::printf("  radius %u at (%.2f, %.2f): got (%.3f %.3f %.3f), shadowed expected (%.3f %.3f %.3f)\n", radius, static_cast<double>(q.x), static_cast<double>(q.y),
                                     static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(dark.x), static_cast<double>(dark.y), static_cast<double>(dark.z));
                CHECK(ok);
            }
            // Beyond the tile there is no shadow data: the sun is not occluded (so the test is not simply "everything is dark").
            for (const Float3 q : {Float3{9.0f, 0.0f, 0.0f}, Float3{0.0f, -9.0f, 0.0f}}) CHECK(near3(linear_at(img, pixel_of(q)), lit, 0.03f));
        } else {
            // The receivers use cascade 1, whose tile is empty: everything in it is lit, also at its left and top edge, where taps must not read tile 0's caster.
            for (const Float3 q : {Float3{-7.86f, 0.0f, 0.0f}, Float3{-7.86f, 3.0f, 0.0f}, Float3{0.0f, 7.86f, 0.0f}, Float3{-3.0f, 7.86f, 0.0f}, Float3{-7.86f, 7.86f, 0.0f},
                                   Float3{0.0f, 0.0f, 0.0f}, Float3{7.0f, -7.0f, 0.0f}}) {
                const Float3 got = linear_at(img, pixel_of(q));
                const bool ok = near3(got, lit, 0.03f);
                if (!ok) std::printf("  cascade 1, radius %u at (%.2f, %.2f): got (%.3f %.3f %.3f), lit expected (%.3f %.3f %.3f)\n", radius, static_cast<double>(q.x), static_cast<double>(q.y),
                                     static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(lit.x), static_cast<double>(lit.y), static_cast<double>(lit.z));
                CHECK(ok);
            }
        }
        CHECK(device->validation_error_count() == 0);
        device->destroy(readback);
        device->destroy(depth);
        device->destroy(color);
    }
}

TEST_CASE("Cascades: the atlas is one tile per cascade, and the pass writes every tile")
{
    for (const uint32_t count : {1u, 2u, 3u, 4u}) {
        Fixture f(cascade_settings(count));
        const ModelRenderer& models = f.renderer->model_renderer();
        CHECK(models.shadow_cascade_count() == count);
        CHECK(models.shadow_atlas_columns() == (count == 1 ? 1u : 2u));
        CHECK(models.shadow_atlas_rows() == (count <= 2 ? 1u : 2u));
        const auto desc = f.device->texture_desc(models.shadow_map());
        REQUIRE(desc.has_value());
        CHECK(desc->width == models.shadow_atlas_width() && desc->height == models.shadow_atlas_height());
        CHECK(desc->width == 1024 * models.shadow_atlas_columns() && desc->height == 1024 * models.shadow_atlas_rows());
    }
    CHECK(true);
}

TEST_CASE("Cascades: settings are validated, and the quality presets are valid and ordered")
{
    const auto rejected = [](auto&& mutate) {
        RendererSettings s = cascade_settings(2);
        mutate(s);
        try {
            auto device = make_validated_device("cascade_invalid");
            SceneRenderer r(*device, rhi::Format::RGBA8_SRGB, s);
            (void)r;
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(rejected([](RendererSettings& s) { s.shadows.cascades = 0; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.cascades = 5; }));
    CHECK(rejected([&](RendererSettings& s) { s.shadows.split_lambda = nan; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.split_lambda = 1.5f; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.split_lambda = -0.1f; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.cascade_blend = 0.6f; }));
    CHECK(rejected([&](RendererSettings& s) { s.shadows.cascade_blend = nan; }));
    CHECK(!rejected([](RendererSettings& s) { s.shadows.cascades = 4; s.shadows.cascade_blend = 0.5f; s.shadows.split_lambda = 1.0f; }));
    CHECK(!rejected([](RendererSettings& s) { s.shadows.enabled = false; s.shadows.cascades = 9; }));  // ignored while shadows are off

    uint32_t last_cascades = 0, last_size = 0;
    for (const ShadowQuality q : {ShadowQuality::Low, ShadowQuality::Medium, ShadowQuality::High, ShadowQuality::Ultra}) {
        const SunShadowSettings s = shadow_quality_preset(q, 80.0f);
        CHECK(s.enabled && s.distance == 80.0f);
        CHECK(s.cascades >= last_cascades && s.map_size >= last_size);
        last_cascades = s.cascades;
        last_size = s.map_size;
        RendererSettings r = deterministic_settings();
        r.shadows = s;
        auto device = make_validated_device("cascade_preset");
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, r);
        CHECK(renderer.model_renderer().shadow_cascade_count() == s.cascades);
    }
    CHECK(shadow_quality_preset(ShadowQuality::Low).cascades == 1 && shadow_quality_preset(ShadowQuality::Ultra).cascades == 4);
}
