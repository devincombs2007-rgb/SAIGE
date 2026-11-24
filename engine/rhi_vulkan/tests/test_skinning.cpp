// GPU skinning on a real (validated) Vulkan device, checked against the CPU reference (gfx::skin_vertex, the stateless sampling of gfx_assets).
//
// The oracle: a skinned tube is drawn through the skinned pipelines with a SkinPalette, and the same tube is drawn rigidly from the CPU-skinned
// vertices (positions, normals) through the ordinary pipelines, with the same material, camera and lighting. The two images must agree to
// rounding. That covers the vertex input (RGBA16_UINT joints, RGBA32_FLOAT weights), the palette upload and its frame slots, the blend of up to four
// joints, the cofactor normals under non-uniform scale, and (with the shadow pass on) the skinned depth stage, in every permutation of the pixel stage.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/gfx_assets/animation.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/skin_palette.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 160;
constexpr uint32_t kH = 160;
constexpr double pi_d = 3.14159265358979323846;
constexpr uint32_t kRings = 36;
constexpr uint32_t kSegments = 20;
constexpr uint32_t kJoints = 4;

struct Rig {
    gfx::Skin skin;
    gfx::Primitive primitive;
};

/// A tube along +Z from 0 to 3 m (radius 0.3), skinned to a chain of four joints at z = 0, 1, 2, 3. Weights blend the two nearest joints, plus a small
/// constant share of the last joint above z = 1 so that vertices carry four influences.
Rig make_rig()
{
    Rig rig;
    gfx::Skin& skin = rig.skin;
    skin.name = "chain";
    for (uint32_t j = 0; j < kJoints; ++j) {
        skin.joint_nodes.push_back(static_cast<int32_t>(j));
        skin.parent.push_back(j == 0 ? -1 : static_cast<int32_t>(j - 1));
        render::Transform rest;
        rest.position = {0.0f, 0.0f, j == 0 ? 0.0f : 1.0f};
        skin.rest.push_back(rest);
        skin.base.push_back(Mat4::identity());
        skin.inverse_bind.push_back(translation({0.0f, 0.0f, -static_cast<float>(j)}));
    }
    gfx::Primitive& p = rig.primitive;
    for (uint32_t r = 0; r <= kRings; ++r) {
        const float z = 3.0f * static_cast<float>(r) / static_cast<float>(kRings);
        // Tent weights over joints at 0, 1, 2, 3.
        float w[4] = {};
        const uint32_t lo = std::min(static_cast<uint32_t>(z), 2u);
        const float f = z - static_cast<float>(lo);
        w[lo] = 1.0f - f;
        w[lo + 1] = f;
        if (z > 1.0f) {  // a fourth, constant influence
            for (float& v : w) v *= 0.85f;
            w[3] += 0.15f;
        }
        for (uint32_t s = 0; s <= kSegments; ++s) {
            const double phi = 2.0 * pi_d * s / kSegments;
            const float nx = static_cast<float>(std::cos(phi)), ny = static_cast<float>(std::sin(phi));
            p.positions.insert(p.positions.end(), {0.3f * nx, 0.3f * ny, z});
            p.normals.insert(p.normals.end(), {nx, ny, 0.0f});
            p.tangents.insert(p.tangents.end(), {-ny, nx, 0.0f, 1.0f});
            p.uv0.insert(p.uv0.end(), {static_cast<float>(s) / static_cast<float>(kSegments), z / 3.0f});
            // Order the influences by joint index: the two nearest, then the extra share, with unused slots at weight 0.
            for (uint32_t k = 0; k < 4; ++k) {
                p.joints.push_back(static_cast<uint16_t>(k));
                p.weights.push_back(w[k]);
            }
        }
    }
    for (uint32_t r = 0; r < kRings; ++r) {
        for (uint32_t s = 0; s < kSegments; ++s) {
            const uint32_t a = r * (kSegments + 1) + s, b = a + kSegments + 1;
            p.indices.insert(p.indices.end(), {a, a + 1, b, a + 1, b + 1, b});  // counter-clockwise seen from outside
        }
    }
    return rig;
}

/// A pose: each joint bent about +Y by `angle * k`, the root twisted about Z, and (with `stretch`) the third joint scaled non-uniformly.
std::vector<Mat4> pose_matrices(const Rig& rig, float angle, bool stretch)
{
    gfx::Pose pose = gfx::rest_pose(rig.skin);
    const float bends[kJoints] = {0.0f, 0.55f, -0.8f, 0.35f};
    for (uint32_t j = 0; j < kJoints; ++j) {
        pose.local[j].rotation = from_axis_angle({0, 1, 0}, bends[j] * angle);
    }
    pose.local[0].rotation = from_axis_angle({0, 0, 1}, 0.3f * angle) * pose.local[0].rotation;
    if (stretch) pose.local[2].scale = {1.35f, 1.0f, 0.75f};
    return gfx::skin_matrices(rig.skin, pose);
}

rhi::BufferDesc buffer_desc(std::size_t bytes, rhi::BufferUsage usage, const char* name)
{
    rhi::BufferDesc d;
    d.size = bytes;
    d.usage = usage | rhi::BufferUsage::TransferDst;
    d.debug_name = name;
    return d;
}

/// The skinned model: SkinnedModelVertex buffer, the pipelines' skinned layout.
GpuModel skinned_model(GpuResourceRegistry& registry, GpuMaterial material, const gfx::Primitive& prim, uint32_t joint_limit = kJoints)
{
    std::vector<SkinnedModelVertex> v(prim.vertex_count());
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i].base.position = {prim.positions[i * 3], prim.positions[i * 3 + 1], prim.positions[i * 3 + 2]};
        v[i].base.normal = {prim.normals[i * 3], prim.normals[i * 3 + 1], prim.normals[i * 3 + 2]};
        v[i].base.uv[0] = prim.uv0[i * 2];
        v[i].base.uv[1] = prim.uv0[i * 2 + 1];
        for (int c = 0; c < 4; ++c) {
            v[i].base.tangent[c] = prim.tangents[i * 4 + static_cast<std::size_t>(c)];
            v[i].joints[c] = prim.joints[i * 4 + static_cast<std::size_t>(c)];
            v[i].weights[c] = prim.weights[i * 4 + static_cast<std::size_t>(c)];
        }
    }
    GpuPrimitive g;
    g.vertices = registry.create_buffer(buffer_desc(v.size() * sizeof(SkinnedModelVertex), rhi::BufferUsage::Vertex, "skinned.vertices"),
                                        std::as_bytes(std::span(v)));
    g.indices = registry.create_buffer(buffer_desc(prim.indices.size() * 4, rhi::BufferUsage::Index, "skinned.indices"), std::as_bytes(std::span(prim.indices)));
    g.index_count = static_cast<uint32_t>(prim.indices.size());
    g.index_type = rhi::IndexType::Uint32;
    g.skinned = true;
    g.joint_limit = joint_limit;
    g.bounds_center = {0, 0, 1.5f};
    g.bounds_radius = 4.0f;
    GpuModel m;
    m.name = "skinned";
    m.materials.push_back(std::move(material));
    m.meshes.push_back({std::move(g)});
    m.placements.push_back({0, Mat4::identity()});
    return m;
}

/// The same tube as a rigid model whose vertices are the CPU reference's output.
GpuModel reference_model(GpuResourceRegistry& registry, GpuMaterial material, const gfx::Primitive& prim, std::span<const Mat4> matrices, bool tangents)
{
    std::vector<ModelVertex> v(prim.vertex_count());
    for (std::size_t i = 0; i < v.size(); ++i) {
        const gfx::SkinnedVertex s = gfx::skin_vertex(prim, i, matrices);
        v[i].position = s.position;
        v[i].normal = normalize(s.normal);
        v[i].uv[0] = prim.uv0[i * 2];
        v[i].uv[1] = prim.uv0[i * 2 + 1];
        if (tangents) {
            const Float3 t = normalize(s.tangent);
            v[i].tangent[0] = t.x;
            v[i].tangent[1] = t.y;
            v[i].tangent[2] = t.z;
            v[i].tangent[3] = prim.tangents[i * 4 + 3];
        }
    }
    GpuModel m;
    m.name = "reference";
    m.materials.push_back(std::move(material));
    m.meshes.push_back({make_primitive(registry, v, prim.indices, 0)});
    m.placements.push_back({0, Mat4::identity()});
    return m;
}

struct Options {
    bool shadows = false;
    bool ibl = false;
    bool blend = false;
    bool skinning = true;
    uint32_t joint_limit = kJoints;
};

/// The test scene's scaffolding on one device: the renderer, the floor that receives shadows, the target, and helpers to render draws.
struct Scene {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;
    GpuModel floor;
    Camera camera;
    Mat4 view_proj;

    explicit Scene(const Options& o, const std::shared_ptr<const EnvironmentLighting>& environment = nullptr)
    {
        device = make_validated_device("skinning");
        RendererSettings s = deterministic_settings();
        s.fog_density = 0.0f;
        s.model_shading = ModelShading::MetallicRoughness;
        s.shadows.enabled = o.shadows;
        s.shadows.map_size = 1024;
        s.alpha_blending = false;
        s.skinning = o.skinning;
        if (o.ibl) s.environment = environment;
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, s);
        target = std::make_unique<OffscreenTarget>(*device, kW, kH);
        MaterialParams mp;
        mp.base_color[0] = mp.base_color[1] = mp.base_color[2] = 0.7f;
        mp.roughness = 1.0f;
        const CubeMesh cube = make_unit_cube();
        std::vector<ModelVertex> v;
        for (const CubeVertex& cv : cube.vertices) v.push_back({cv.position, cv.normal, {0, 0}});
        std::vector<uint32_t> idx(cube.indices, cube.indices + 36);
        floor.name = "floor";
        floor.materials.push_back(renderer->model_renderer().create_material(mp, {}, {}, "floor"));
        floor.meshes.push_back({make_primitive(renderer->registry(), v, idx, 0)});
        floor.placements.push_back({0, translation({0.0f, 0.0f, -0.05f}) * scale_matrix({6.0f, 6.0f, 0.1f})});
        camera = look_at_camera({-5.5f, -4.0f, 2.6f}, {0.0f, 0.0f, 1.3f});
        view_proj = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    }
    ~Scene()
    {
        if (device) device->wait_idle();
        target.reset();
    }
    GpuMaterial material(float r, float g, float b, float metallic = 0.0f, float roughness = 0.7f)
    {
        MaterialParams mp;
        mp.base_color[0] = r;
        mp.base_color[1] = g;
        mp.base_color[2] = b;
        mp.metallic = metallic;
        mp.roughness = roughness;
        return renderer->model_renderer().create_material(mp, {}, {}, "tube");
    }
    Image render(std::span<const ModelDraw> draws, FrameStats* stats = nullptr)
    {
        try {
            return target->render(*renderer, {}, camera, 0, draws, stats);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  render threw: %s\n", e.what());
            throw;
        }
    }
};

struct Diff {
    int worst = 0;
    uint32_t over = 0;  // pixels above the threshold
};

Diff compare(const Image& a, const Image& b, int threshold)
{
    Diff d;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const int delta = max_channel_delta(a.at(x, y), b.at(x, y));
            d.worst = std::max(d.worst, delta);
            d.over += delta > threshold ? 1u : 0u;
        }
    }
    return d;
}

std::shared_ptr<const EnvironmentLighting> sky_environment()
{
    auto e = std::make_shared<EnvironmentLighting>();
    e->width = 16;
    e->height = 8;
    e->levels = 2;
    e->sh[0] = {1.4f, 1.5f, 1.7f};
    e->sh[2] = {0.3f, 0.35f, 0.4f};  // brighter above
    e->rgba16f.assign((16 * 8 + 8 * 4) * 4, 0x3800);
    return e;
}

} // namespace

TEST_CASE("Skinning: the GPU result equals rigid drawing of the CPU-skinned vertices, across poses (including non-uniform scale and four influences)")
{
    const Rig rig = make_rig();
    Scene scene({});
    ModelRenderer& models = scene.renderer->model_renderer();
    CHECK(models.skinning_enabled());
    const GpuModel skinned = skinned_model(scene.renderer->registry(), scene.material(0.8f, 0.55f, 0.3f), rig.primitive);
    SkinPalette palette = models.create_skin_palette(kJoints, "palette");
    CHECK(palette.valid() && palette.joint_count() == kJoints);
    Image previous;
    for (const bool stretch : {false, true}) {
        for (const float angle : {0.0f, 0.6f, 1.0f, -0.8f}) {
            const std::vector<Mat4> matrices = pose_matrices(rig, angle, stretch);
            palette.update(matrices);
            const GpuModel reference = reference_model(scene.renderer->registry(), scene.material(0.8f, 0.55f, 0.3f), rig.primitive, matrices, false);
            FrameStats stats;
            const ModelDraw skinned_draw[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity(), &palette}};
            const Image gpu = scene.render(skinned_draw, &stats);
            CHECK(stats.model_skinned_draws == 1);
            CHECK(stats.model_skinned_skipped == 0);
            const ModelDraw rigid_draw[] = {{&scene.floor, Mat4::identity()}, {&reference, Mat4::identity()}};
            const Image cpu = scene.render(rigid_draw);
            const Diff d = compare(gpu, cpu, 3);
            if (d.worst > 3 || d.over > 0) {
                std::printf("  stretch %d angle %.1f: worst channel delta %d, %u pixels over 3\n", stretch ? 1 : 0, static_cast<double>(angle), d.worst, d.over);
            }
            CHECK(d.over <= 6);  // silhouette pixels may flip on the last bit of a transformed vertex; everything else is exact to rounding
            CHECK(d.worst <= 40);
            if (angle == 0.0f && !stretch) previous = gpu;  // the bind pose
            if (angle == 1.0f && !stretch) {
                const Diff moved = compare(gpu, previous, 12);
                CHECK(moved.over > 300);  // the pose really changes the picture, so the comparison above could fail
            }
        }
    }
    CHECK(scene.device->validation_error_count() == 0);
}

TEST_CASE("Skinning: no palette draws the bind pose, and a palette updated every frame is current in every frame slot")
{
    const Rig rig = make_rig();
    Scene scene({});
    ModelRenderer& models = scene.renderer->model_renderer();
    const GpuModel skinned = skinned_model(scene.renderer->registry(), scene.material(0.3f, 0.6f, 0.85f), rig.primitive);
    const std::vector<Mat4> bind = pose_matrices(rig, 0.0f, false);
    {
        const GpuModel reference = reference_model(scene.renderer->registry(), scene.material(0.3f, 0.6f, 0.85f), rig.primitive, bind, false);
        const ModelDraw a[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity()}};  // no palette
        const ModelDraw b[] = {{&scene.floor, Mat4::identity()}, {&reference, Mat4::identity()}};
        const Diff d = compare(scene.render(a), scene.render(b), 3);
        CHECK(d.over <= 6 && d.worst <= 40);
    }
    // Several frames in a row, a new pose each time (more frames than frame slots), each compared with its own reference.
    SkinPalette palette = models.create_skin_palette(kJoints, "animated");
    for (int frame = 0; frame < 6; ++frame) {
        const std::vector<Mat4> matrices = pose_matrices(rig, 0.25f * static_cast<float>(frame + 1), frame % 2 == 1);
        palette.update(matrices);
        palette.update(matrices);  // updating twice before a frame is the same as once
        const GpuModel reference = reference_model(scene.renderer->registry(), scene.material(0.3f, 0.6f, 0.85f), rig.primitive, matrices, false);
        const ModelDraw a[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity(), &palette}};
        const ModelDraw b[] = {{&scene.floor, Mat4::identity()}, {&reference, Mat4::identity()}};
        const Diff d = compare(scene.render(a), scene.render(b), 3);
        if (d.over > 6) std::printf("  frame %d: %u pixels differ (worst %d)\n", frame, d.over, d.worst);
        CHECK(d.over <= 6 && d.worst <= 40);
    }
    // Not updating keeps the last pose (the draw re-uploads it for whichever frame slot comes up).
    const std::vector<Mat4> last = pose_matrices(rig, 1.5f, false);
    palette.update(last);
    const GpuModel reference = reference_model(scene.renderer->registry(), scene.material(0.3f, 0.6f, 0.85f), rig.primitive, last, false);
    for (int frame = 0; frame < 4; ++frame) {
        const ModelDraw a[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity(), &palette}};
        const ModelDraw b[] = {{&scene.floor, Mat4::identity()}, {&reference, Mat4::identity()}};
        const Diff d = compare(scene.render(a), scene.render(b), 3);
        CHECK(d.over <= 6 && d.worst <= 40);
    }
    CHECK(scene.device->validation_error_count() == 0);
}

TEST_CASE("Skinning: two instances with different palettes in one frame, with sun shadows and image-based lighting together")
{
    const Rig rig = make_rig();
    const auto environment = sky_environment();
    Options o;
    o.shadows = true;
    o.ibl = true;
    Scene scene(o, environment);
    ModelRenderer& models = scene.renderer->model_renderer();
    CHECK(models.shadows_enabled() && models.ibl_enabled());
    const GpuModel skinned = skinned_model(scene.renderer->registry(), scene.material(0.8f, 0.8f, 0.75f), rig.primitive);
    SkinPalette left = models.create_skin_palette(kJoints, "left"), right = models.create_skin_palette(kJoints, "right");
    const std::vector<Mat4> ma = pose_matrices(rig, 0.9f, true), mb = pose_matrices(rig, -0.7f, false);
    left.update(ma);
    right.update(mb);
    const Mat4 at_left = translation({0.0f, 1.2f, 0.0f}), at_right = translation({0.4f, -1.3f, 0.0f}) * rotation_z(0.8f);
    const GpuModel ref_left = reference_model(scene.renderer->registry(), scene.material(0.8f, 0.8f, 0.75f), rig.primitive, ma, false);
    const GpuModel ref_right = reference_model(scene.renderer->registry(), scene.material(0.8f, 0.8f, 0.75f), rig.primitive, mb, false);
    FrameStats stats;
    const ModelDraw a[] = {{&scene.floor, Mat4::identity()}, {&skinned, at_left, &left}, {&skinned, at_right, &right}};
    const Image gpu = scene.render(a, &stats);
    const ModelDraw b[] = {{&scene.floor, Mat4::identity()}, {&ref_left, at_left}, {&ref_right, at_right}};
    const Image cpu = scene.render(b);
    CHECK(stats.model_skinned_draws == 4);  // two instances, each drawn in the shadow pass and the color pass
    const Diff d = compare(gpu, cpu, 3);
    std::printf("  shadows + IBL, two instances: worst channel delta %d, %u pixels over 3\n", d.worst, d.over);
    CHECK(d.over <= 12 && d.worst <= 40);
    // The shadows are really there (so the skinned depth stage is exercised): without shadow casting from the skinned instances the floor is brighter.
    const ModelDraw only_floor[] = {{&scene.floor, Mat4::identity()}};
    const Image bare = scene.render(only_floor);
    const Diff shadowed = compare(gpu, bare, 10);
    CHECK(shadowed.over > 800);
    CHECK(scene.device->validation_error_count() == 0);
}

TEST_CASE("Skinning: refusals and skips are explicit; a bad pose leaves the previous pose; skinning off draws nothing for skinned primitives")
{
    const Rig rig = make_rig();
    {
        Options off;
        off.skinning = false;
        Scene scene(off);
        ModelRenderer& models = scene.renderer->model_renderer();
        CHECK(!models.skinning_enabled());
        bool threw = false;
        try {
            (void)models.create_skin_palette(kJoints, "none");
        } catch (const std::logic_error&) {
            threw = true;
        }
        CHECK(threw);
        const GpuModel skinned = skinned_model(scene.renderer->registry(), scene.material(0.8f, 0.5f, 0.3f), rig.primitive);
        const ModelDraw only_floor[] = {{&scene.floor, Mat4::identity()}};
        const ModelDraw with_skinned[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity()}};
        FrameStats stats;
        const Image a = scene.render(only_floor), b = scene.render(with_skinned, &stats);
        CHECK(stats.model_skinned_skipped == 1 && stats.model_skinned_draws == 0);
        CHECK(compare(a, b, 0).over == 0);  // the rigid scene is untouched by the skipped primitive
    }
    Scene scene({});
    ModelRenderer& models = scene.renderer->model_renderer();
    const auto refused = [&](uint32_t count) {
        try {
            (void)models.create_skin_palette(count, "bad");
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(refused(0) && refused(kMaxSkinJoints + 1));
    CHECK(!refused(kMaxSkinJoints));
    // A palette with fewer joints than the primitive references is skipped, not read past.
    {
        const GpuModel skinned = skinned_model(scene.renderer->registry(), scene.material(0.8f, 0.5f, 0.3f), rig.primitive);
        SkinPalette small = models.create_skin_palette(2, "small");
        const ModelDraw d[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity(), &small}};
        FrameStats stats;
        (void)scene.render(d, &stats);
        CHECK(stats.model_skinned_skipped == 1 && stats.model_skinned_draws == 0);
    }
    // update(): wrong count, NaN and a non-affine matrix are refused and change nothing; an empty palette cannot be updated.
    SkinPalette palette = models.create_skin_palette(kJoints, "pose");
    const GpuModel skinned = skinned_model(scene.renderer->registry(), scene.material(0.8f, 0.5f, 0.3f), rig.primitive);
    const std::vector<Mat4> good = pose_matrices(rig, 0.8f, false);
    palette.update(good);
    const ModelDraw draw[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity(), &palette}};
    const Image before = scene.render(draw);
    const auto update_refused = [&](std::vector<Mat4> m) {
        try {
            palette.update(m);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    std::vector<Mat4> nan_pose = good, projective = good, short_pose(good.begin(), good.begin() + 2);
    nan_pose[2].at(0, 3) = std::numeric_limits<float>::quiet_NaN();
    projective[1].at(3, 0) = 0.5f;
    CHECK(update_refused(nan_pose) && update_refused(projective) && update_refused(short_pose));
    CHECK(compare(before, scene.render(draw), 0).over == 0);  // still the previous pose
    SkinPalette empty;
    bool logic = false;
    try {
        empty.update(good);
    } catch (const std::logic_error&) {
        logic = true;
    }
    CHECK(logic && !empty.valid());
    CHECK(scene.device->validation_error_count() == 0);
}

TEST_CASE("Skinning: sorted alpha-blended skinned primitives match the CPU-skinned blended reference")
{
    const Rig rig = make_rig();
    Options o;
    Scene scene(o);
    // A second renderer is needed with alpha blending on: the scene above has it off, so build this one by hand.
    auto device = make_validated_device("skinning_blend");
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.model_shading = ModelShading::MetallicRoughness;
    s.alpha_blending = true;
    s.skinning = true;
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, s);
    ModelRenderer& models = renderer.model_renderer();
    CHECK(models.blending_enabled());
    OffscreenTarget target(*device, kW, kH);
    MaterialParams mp;
    mp.base_color[0] = 0.9f;
    mp.base_color[1] = 0.3f;
    mp.base_color[2] = 0.3f;
    mp.base_color[3] = 0.5f;
    mp.roughness = 0.6f;
    mp.metallic = 0.0f;
    mp.alpha_blend = true;
    mp.double_sided = true;
    const std::vector<Mat4> matrices = pose_matrices(rig, 0.9f, false);
    const GpuModel skinned = skinned_model(renderer.registry(), models.create_material(mp, {}, {}, "blend"), rig.primitive);
    const GpuModel reference = reference_model(renderer.registry(), models.create_material(mp, {}, {}, "blend_ref"), rig.primitive, matrices, false);
    SkinPalette palette = models.create_skin_palette(kJoints, "blend_palette");
    palette.update(matrices);
    const Camera camera = scene.camera;
    FrameStats stats;
    const ModelDraw a[] = {{&skinned, Mat4::identity(), &palette}};
    const Image gpu = target.render(renderer, {}, camera, 0, a, &stats);
    const ModelDraw b[] = {{&reference, Mat4::identity()}};
    const Image cpu = target.render(renderer, {}, camera, 0, b);
    CHECK(stats.model_blended_primitives == 1);
    CHECK(stats.model_skinned_draws == 2);  // a double-sided blended primitive draws its back faces and then its front faces
    const Diff d = compare(gpu, cpu, 3);
    std::printf("  blended skinned: worst channel delta %d, %u pixels over 3\n", d.worst, d.over);
    CHECK(d.over <= 8 && d.worst <= 40);
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Skinning: a normal-mapped skinned surface keeps its tangent frame (rotation-only pose)")
{
    const Rig rig = make_rig();
    Scene scene({});
    ModelRenderer& models = scene.renderer->model_renderer();
    // A normal map with strong, varied texels, nearest sampling so each texel is a constant tilt.
    std::vector<std::byte> texels;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            texels.push_back(std::byte{static_cast<uint8_t>(40 + 60 * i)});
            texels.push_back(std::byte{static_cast<uint8_t>(200 - 50 * j)});
            texels.push_back(std::byte{200});
            texels.push_back(std::byte{255});
        }
    }
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_UNORM;
    td.width = td.height = 4;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "skin.normal_map";
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    sd.mipmap_mode = rhi::MipmapMode::Nearest;
    sd.address_u = sd.address_v = rhi::AddressMode::Repeat;
    MaterialTextures textures;
    textures[MaterialSlot::Normal] = {scene.renderer->registry().create_texture(td, texels), scene.renderer->registry().get_sampler(sd)};
    MaterialParams mp;
    mp.base_color[0] = 0.8f;
    mp.base_color[1] = 0.7f;
    mp.base_color[2] = 0.6f;
    mp.metallic = 0.0f;
    mp.roughness = 0.5f;
    const std::vector<Mat4> matrices = pose_matrices(rig, 0.9f, false);  // rotations and translations only: the tangent frame stays orthonormal
    const GpuModel skinned = skinned_model(scene.renderer->registry(), models.create_material(mp, textures, "mapped"), rig.primitive);
    const GpuModel reference = reference_model(scene.renderer->registry(), models.create_material(mp, textures, "mapped_ref"), rig.primitive, matrices, true);
    SkinPalette palette = models.create_skin_palette(kJoints, "mapped_palette");
    palette.update(matrices);
    const ModelDraw a[] = {{&scene.floor, Mat4::identity()}, {&skinned, Mat4::identity(), &palette}};
    const ModelDraw b[] = {{&scene.floor, Mat4::identity()}, {&reference, Mat4::identity()}};
    const Image gpu = scene.render(a), cpu = scene.render(b);
    const Diff d = compare(gpu, cpu, 4);
    std::printf("  normal-mapped skinned: worst channel delta %d, %u pixels over 4\n", d.worst, d.over);
    // Skinned tangents are blended and renormalized by the shader, the reference normalizes per vertex: allow a little more than rounding.
    CHECK(d.over <= 40 && d.worst <= 60);
    // Without the tangents the map is skipped: the picture must differ, so the test above checks the tangent path.
    const GpuModel reference_untangented = reference_model(scene.renderer->registry(), models.create_material(mp, textures, "mapped_ref2"), rig.primitive, matrices, false);
    const ModelDraw c[] = {{&scene.floor, Mat4::identity()}, {&reference_untangented, Mat4::identity()}};
    CHECK(compare(gpu, scene.render(c), 12).over > 200);
    CHECK(scene.device->validation_error_count() == 0);
}

TEST_CASE("Skinning: an imported (staged) skinned asset draws like the same asset with the pose baked into its vertices")
{
    const Rig rig = make_rig();
    Scene scene({});
    ModelRenderer& models = scene.renderer->model_renderer();
    const std::vector<Mat4> matrices = pose_matrices(rig, 0.8f, true);

    // The skinned asset: the tube, its skin, one skinned instance (a non-identity node transform that must be ignored).
    gfx::GltfAsset skinned;
    gfx::Mesh mesh;
    mesh.primitives.push_back(rig.primitive);
    skinned.meshes.push_back(mesh);
    skinned.skins.push_back(rig.skin);
    gfx::MeshInstance inst;
    inst.mesh = 0;
    inst.skin = 0;
    inst.world = translation({3.0f, 0.0f, 0.0f});
    skinned.instances.push_back(inst);
    // The reference asset: the same tube with the CPU reference's positions and normals baked in, no skin, placed at the identity.
    gfx::GltfAsset baked;
    gfx::Mesh baked_mesh;
    gfx::Primitive bp = rig.primitive;
    bp.joints.clear();
    bp.weights.clear();
    bp.tangents.clear();
    for (std::size_t v = 0; v < bp.vertex_count(); ++v) {
        const gfx::SkinnedVertex s = gfx::skin_vertex(rig.primitive, v, matrices);
        const Float3 n = normalize(s.normal);
        bp.positions[v * 3] = s.position.x;
        bp.positions[v * 3 + 1] = s.position.y;
        bp.positions[v * 3 + 2] = s.position.z;
        bp.normals[v * 3] = n.x;
        bp.normals[v * 3 + 1] = n.y;
        bp.normals[v * 3 + 2] = n.z;
    }
    baked_mesh.primitives.push_back(bp);
    baked.meshes.push_back(baked_mesh);
    gfx::MeshInstance binst;
    binst.mesh = 0;
    binst.world = Mat4::identity();
    baked.instances.push_back(binst);

    gfx::StageReport report;
    const GpuModel staged = gfx::stage_model(*scene.renderer, skinned, "staged-skinned", {}, &report);
    const GpuModel reference = gfx::stage_model(*scene.renderer, baked, "staged-baked");
    CHECK(report.skinned_primitives == 1);
    CHECK(staged.skin_joint_count == kJoints);
    SkinPalette palette = models.create_skin_palette(staged.skin_joint_count, "staged");
    palette.update(matrices);
    const ModelDraw a[] = {{&scene.floor, Mat4::identity()}, {&staged, Mat4::identity(), &palette}};
    const ModelDraw b[] = {{&scene.floor, Mat4::identity()}, {&reference, Mat4::identity()}};
    FrameStats stats;
    const Image gpu = scene.render(a, &stats);
    const Image cpu = scene.render(b);
    CHECK(stats.model_skinned_draws >= 1 && stats.model_skinned_skipped == 0);
    const Diff d = compare(gpu, cpu, 3);
    std::printf("  staged skinned asset: worst channel delta %d, %u pixels over 3\n", d.worst, d.over);
    CHECK(d.over <= 6 && d.worst <= 40);
    CHECK(scene.device->validation_error_count() == 0);
}
