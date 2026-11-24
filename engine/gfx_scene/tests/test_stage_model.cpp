#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/tangents.hpp"
#include "saige/gfx_scene/prepared_model.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace saige;
using namespace saige::gfx;

namespace {

render::RendererSettings rig_settings(render::ModelShading shading, bool alpha_blending, bool skinning)
{
    render::RendererSettings s;
    s.model_shading = shading;
    s.alpha_blending = alpha_blending;
    s.skinning = skinning;
    return s;
}

struct Rig {
    rhi::NullDevice device;
    render::SceneRenderer renderer;
    /// Legacy shading and no alpha blending are the defaults everywhere (the 0.2 behavior); tests of the extra material slots opt in to
    /// Extended or MetallicRoughness, and tests of sorted blending opt in to alpha blending.
    explicit Rig(render::ModelShading shading = render::ModelShading::Legacy, bool alpha_blending = false, bool skinning = false)
        : renderer(device, rhi::Format::RGBA8_SRGB, rig_settings(shading, alpha_blending, skinning))
    {
    }
};

GltfAsset load_fixture()
{
    const GltfLoadResult r = load_gltf_file(std::filesystem::path(SAIGE_TEXTURED_SCENE_DIR) / "checker-box.gltf");
    if (!r.ok()) {
        std::printf("  cannot load the fixture: %s\n", r.error.c_str());
        std::abort();
    }
    return *r.asset;
}

bool has_warning(const StageReport& report, const std::string& fragment)
{
    for (const std::string& w : report.warnings) {
        if (w.find(fragment) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("Stage: Core's textured fixture becomes a model with its mesh, texture and placement")
{
    Rig rig;
    const GltfAsset asset = load_fixture();
    StageReport report;
    const render::GpuModel model = stage_model(rig.renderer, asset, "checker-box", {}, &report);

    REQUIRE(model.meshes.size() == 1);
    REQUIRE(model.meshes[0].size() == 1);
    const render::GpuPrimitive& prim = model.meshes[0][0];
    CHECK(prim.index_count == 36);
    CHECK(prim.index_type == rhi::IndexType::Uint16);           // 24 vertices: 16-bit indices
    CHECK(prim.vertices.bytes() == 24 * sizeof(render::ModelVertex));
    CHECK(prim.indices.bytes() == 36 * sizeof(uint16_t));
    CHECK(prim.vertices.name() == "checker-box/mesh[0] primitive 0 vertices");

    REQUIRE(model.materials.size() == 2);                        // the file's material plus the default one
    CHECK(prim.material == 0);
    CHECK(model.placements.size() == 1);                         // the asset's single node
    CHECK(model.triangle_count() == 12);

    CHECK(report.textures == 1);
    CHECK(report.texture_bytes == (64 + 16 + 4 + 1) * 4);       // 8x8 with a full mip chain, RGBA8
    CHECK(report.primitives == 1);
    CHECK(report.warnings.empty());

    // The registry accounts for everything: no leaks, and the checker texture is there by name.
    bool found_texture = false;
    for (const render::ResourceInfo& r : rig.renderer.registry().live_resources()) {
        found_texture |= r.kind == render::ResourceKind::Texture && r.name.find("checker-box/image[0]") == 0;
    }
    CHECK(found_texture);
    CHECK(rig.device.validation_error_count() == 0);
}

TEST_CASE("Stage: dropping a model releases everything it staged (and only that)")
{
    Rig rig;
    const GltfAsset asset = load_fixture();
    (void)rig.renderer.model_renderer();  // the renderer's own resources exist before any model
    const render::ResourceStats baseline = rig.renderer.registry().total_stats();
    {
        const render::GpuModel model = stage_model(rig.renderer, asset, "scene");
        CHECK(rig.renderer.registry().total_stats().live > baseline.live);
        CHECK(rig.renderer.registry().total_stats().live_bytes > baseline.live_bytes);
    }
    const render::ResourceStats after = rig.renderer.registry().total_stats();
    CHECK(after.live == baseline.live);
    CHECK(after.live_bytes == baseline.live_bytes);
    rig.device.wait_idle();
    CHECK(rig.device.pending_destruction_count() == 0);
}

TEST_CASE("Stage: a failure part-way through leaves nothing behind and names what failed")
{
    Rig rig;
    GltfAsset asset = load_fixture();
    (void)rig.renderer.model_renderer();
    const uint32_t live_before = rig.renderer.registry().total_stats().live;

    // Corrupt the image after the mesh would have been uploaded: the texture comes first, so also corrupt a later stage.
    asset.images[0].bytes.resize(20);  // truncated PNG
    std::string message;
    try {
        (void)stage_model(rig.renderer, asset, "broken");
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    CHECK(message.find("broken/image[0]") != std::string::npos);
    CHECK(rig.renderer.registry().total_stats().live == live_before);

    // A failure in the mesh stage, after the material (and its texture) was already built, also unwinds cleanly.
    asset = load_fixture();
    asset.meshes[0].primitives[0].indices[0] = 9999;  // refers past the last vertex
    message.clear();
    try {
        (void)stage_model(rig.renderer, asset, "bad-index");
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    CHECK(message.find("bad-index/mesh[0]") != std::string::npos);
    CHECK(message.find("past the last vertex") != std::string::npos);
    CHECK(rig.renderer.registry().total_stats().live == live_before);  // the texture and materials were released too
    CHECK(rig.device.validation_error_count() == 0);
}

TEST_CASE("Stage: one image used by several materials is decoded and uploaded once")
{
    Rig rig;
    GltfAsset asset = load_fixture();
    asset.materials.push_back(asset.materials[0]);
    asset.materials.back().name = "second";
    asset.materials.back().base_color[0] = 0.5f;
    StageReport report;
    const render::GpuModel model = stage_model(rig.renderer, asset, "shared", {}, &report);
    CHECK(model.materials.size() == 3);
    CHECK(report.textures == 1);  // two materials, one texture
    uint32_t checker_textures = 0;
    for (const render::ResourceInfo& r : rig.renderer.registry().live_resources()) {
        checker_textures += (r.kind == render::ResourceKind::Texture && r.name.find("shared/image[0]") == 0) ? 1u : 0u;
    }
    CHECK(checker_textures == 1);
}

TEST_CASE("Stage: index width follows the vertex count")
{
    Rig rig;
    GltfAsset asset = load_fixture();
    // 70,000 vertices need 32-bit indices.
    Primitive big;
    const uint32_t n = 70000;
    big.positions.assign(std::size_t{n} * 3, 0.0f);
    big.normals.assign(std::size_t{n} * 3, 0.0f);
    for (uint32_t v = 0; v < n; ++v) big.normals[v * 3 + 2] = 1.0f;
    big.indices = {0, 1, 2, n - 3, n - 2, n - 1};
    asset.meshes.push_back({"big", {big}, {}});
    asset.instances.push_back({1, -1, render::Mat4::identity(), "big"});
    const render::GpuModel model = stage_model(rig.renderer, asset, "wide");
    REQUIRE(model.meshes.size() == 2);
    CHECK(model.meshes[0][0].index_type == rhi::IndexType::Uint16);
    CHECK(model.meshes[1][0].index_type == rhi::IndexType::Uint32);
    CHECK(model.meshes[1][0].material == 1);  // no material in the file: the default one
    CHECK(model.placements.size() == 2);

    // A non-indexed primitive is drawn as a plain triangle list.
    GltfAsset plain = load_fixture();
    plain.meshes[0].primitives[0].indices.clear();
    plain.meshes[0].primitives[0].positions.resize(9);
    plain.meshes[0].primitives[0].normals.resize(9);
    plain.meshes[0].primitives[0].uv0.resize(6);
    const render::GpuModel tri = stage_model(rig.renderer, plain, "plain");
    CHECK(tri.meshes[0][0].index_count == 3);
}

TEST_CASE("Stage: malformed primitives are rejected before any GPU work")
{
    Rig rig;
    for (int variant = 0; variant < 4; ++variant) {
        GltfAsset asset = load_fixture();
        Primitive& p = asset.meshes[0].primitives[0];
        if (variant == 0) p.normals.pop_back();                        // mismatched normals
        if (variant == 1) p.uv0.pop_back();                            // mismatched uvs
        if (variant == 2) p.indices.pop_back();                        // not a multiple of 3
        if (variant == 3) p.positions.clear(), p.normals.clear();      // no vertices
        bool threw = false;
        try {
            (void)stage_model(rig.renderer, asset, "bad");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
}

TEST_CASE("Stage: material factors and modes map onto the model's materials, with honest warnings")
{
    Rig rig;
    GltfAsset asset = load_fixture();
    Material mask = asset.materials[0];
    mask.alpha_mode = AlphaMode::Mask;
    mask.alpha_cutoff = 0.0f;  // glTF: renders when alpha >= cutoff, so 0 passes everything (the shader's "no test")
    mask.double_sided = true;
    mask.name = "leaves";
    Material blend = asset.materials[0];
    blend.alpha_mode = AlphaMode::Blend;
    blend.normal_texture.texture = 0;  // an unsampled texture slot
    blend.name = "glass";
    asset.materials.push_back(mask);
    asset.materials.push_back(blend);
    StageReport report;
    const render::GpuModel model = stage_model(rig.renderer, asset, "materials", {}, &report);
    REQUIRE(model.materials.size() == 4);
    CHECK_FALSE(model.materials[0].double_sided);
    CHECK(model.materials[1].double_sided);
    // The default renderer has alpha blending off: BLEND draws opaque exactly as in 0.2, with a warning saying so.
    CHECK(has_warning(report, "alpha blending is off"));
    CHECK(has_warning(report, "not sampled in 0.2"));  // the default Legacy renderer: a normal texture is ignored, as in 0.2
    CHECK_FALSE(model.materials[0].blend);
    CHECK_FALSE(model.materials[1].blend);  // the mask material
    CHECK_FALSE(model.materials[2].blend);  // "glass": glTF BLEND, but blending was not asked for
    // Shadow casting follows the authored alpha mode, not how the surface is drawn: the opaque material casts; the MASK (even with cutoff 0)
    // and the BLEND (even though it is drawn opaque here) do not.
    CHECK(model.materials[0].casts_sun_shadow);
    CHECK_FALSE(model.materials[1].casts_sun_shadow);
    CHECK_FALSE(model.materials[2].casts_sun_shadow);
    // Every primitive carries a bounding sphere for sorting blended draws (used only when blending is on).
    for (const auto& primitives : model.meshes) {
        for (const render::GpuPrimitive& prim : primitives) {
            CHECK(prim.bounds_radius > 0.0f);
        }
    }
    // With alpha blending asked for, the same asset blends the BLEND material and needs no warning about blending.
    Rig blending_rig(render::ModelShading::Legacy, /*alpha_blending=*/true);
    StageReport blending_report;
    const render::GpuModel blending_model = stage_model(blending_rig.renderer, asset, "materials", {}, &blending_report);
    CHECK_FALSE(has_warning(blending_report, "alpha blending"));
    CHECK_FALSE(blending_model.materials[0].blend);
    CHECK_FALSE(blending_model.materials[1].blend);
    CHECK(blending_model.materials[2].blend);
}

TEST_CASE("Stage: every slot gets its own texture upload per color space and the sampler its glTF texture names")
{
    Rig rig(render::ModelShading::Extended);
    GltfAsset asset = load_fixture();
    REQUIRE(!asset.textures.empty());
    REQUIRE(!asset.images.empty());
    // Two glTF samplers, and two glTF textures that share the fixture's one image.
    Sampler nearest;
    nearest.mag_filter = 9728;
    nearest.min_filter = 9728;
    asset.samplers.clear();
    asset.samplers.push_back(Sampler{});  // default trilinear/repeat
    asset.samplers.push_back(nearest);
    asset.textures[0].sampler = 0;
    Texture second = asset.textures[0];
    second.sampler = 1;
    asset.textures.push_back(second);

    Material& m = asset.materials[0];
    m.base_color_texture.texture = 0;  // sRGB, sampler 0
    m.emissive_texture.texture = 1;    // sRGB, sampler 1, the same image as base color
    m.occlusion_texture.texture = 1;   // linear: the same image again, a second upload
    m.normal_texture.texture = 0;      // linear: shares the occlusion upload
    StageReport report;
    (void)rig.renderer.model_renderer();  // created on first use; its neutral textures are not part of this staging
    const render::ResourceStats before = rig.renderer.registry().stats(render::ResourceKind::Texture);
    const render::ResourceStats samplers_before = rig.renderer.registry().stats(render::ResourceKind::Sampler);
    const render::GpuModel model = stage_model(rig.renderer, asset, "slots", {}, &report);
    const render::ResourceStats after = rig.renderer.registry().stats(render::ResourceKind::Texture);
    REQUIRE(!model.materials.empty());
    CHECK(report.textures == 2);                       // one sRGB and one linear upload of the one image, not four
    CHECK(after.created - before.created == 2);
    // Two sampler descriptions across the five slots; the default one is the renderer's own and is shared, not created again.
    CHECK(rig.renderer.registry().stats(render::ResourceKind::Sampler).created - samplers_before.created == 1);
    CHECK(has_warning(report, "not shaded yet"));      // normal is bound but not shaded
    // An out-of-range texture index in an optional slot leaves that slot neutral, with a warning that names it (Extended only: Legacy
    // never reads these slots). The same fault in the base color slot fails the staging (tested below).
    asset.materials[0].emissive_texture.texture = 99;
    StageReport bad_slot;
    bool staged = true;
    try {
        (void)stage_model(rig.renderer, asset, "bad slot", {}, &bad_slot);
    } catch (const std::runtime_error&) {
        staged = false;
    }
    CHECK(staged);
    CHECK(has_warning(bad_slot, "emissive texture index is out of range"));
    CHECK(has_warning(bad_slot, "left neutral"));
}

// ----------------------------------------------------------------------------------------------- review finding on #50: 0.2 compatibility

TEST_CASE("Stage (Legacy, the default): all five slots populated stage exactly what 0.2 staged, the base color texture alone")
{
    Rig rig;  // ModelShading::Legacy
    GltfAsset asset = load_fixture();
    Material& m = asset.materials[0];
    m.metallic_roughness_texture.texture = 0;
    m.normal_texture.texture = 0;
    m.occlusion_texture.texture = 0;
    m.emissive_texture.texture = 0;
    StageReport report;
    const render::GpuModel model = stage_model(rig.renderer, asset, "legacy", {}, &report);
    CHECK(report.textures == 1);  // the one base color upload; before the fix a second (linear) upload for the other slots
    CHECK(has_warning(report, "not sampled in 0.2"));
    CHECK_FALSE(has_warning(report, "not shaded yet"));

    // The same asset with the other slots empty stages identically.
    const GltfAsset plain = load_fixture();
    StageReport plain_report;
    const render::ResourceStats before = rig.renderer.registry().total_stats();
    (void)stage_model(rig.renderer, plain, "plain", {}, &plain_report);
    const render::ResourceStats after = rig.renderer.registry().total_stats();
    CHECK(plain_report.textures == report.textures);
    CHECK(plain_report.texture_bytes == report.texture_bytes);
    CHECK(plain_report.warnings.empty());
    (void)before;
    (void)after;
}

TEST_CASE("Stage: an undecodable image in an optional slot does not reject 0.2 content, and is neutral with a warning in Extended")
{
    for (const render::ModelShading shading : {render::ModelShading::Legacy, render::ModelShading::Extended}) {
        Rig rig(shading);
        GltfAsset asset = load_fixture();
        Image broken;
        broken.name = "broken";
        broken.mime_type = "image/png";
        broken.bytes = {1, 2, 3, 4, 5};  // not an image
        asset.images.push_back(broken);
        asset.textures.push_back({static_cast<int32_t>(asset.images.size() - 1), -1});
        const int32_t bad = static_cast<int32_t>(asset.textures.size() - 1);
        Material& m = asset.materials[0];
        m.emissive_texture.texture = bad;
        m.occlusion_texture.texture = bad;
        m.normal_texture.texture = bad;
        m.metallic_roughness_texture.texture = bad;
        StageReport report;
        bool staged = true;
        try {
            const render::GpuModel model = stage_model(rig.renderer, asset, "optional", {}, &report);
            CHECK(model.materials.size() >= 1);
        } catch (const std::runtime_error&) {
            staged = false;
        }
        CHECK(staged);  // before the fix the Legacy case threw "decode failed" for an image 0.2 never read
        CHECK(report.textures == 1);  // only the base color texture was uploaded in both modes
        if (shading == render::ModelShading::Extended) {
            CHECK(has_warning(report, "left neutral"));
        } else {
            CHECK(has_warning(report, "not sampled in 0.2"));
        }
    }
}

TEST_CASE("Stage: an undecodable or missing base color texture still fails the staging in both modes, as in 0.2")
{
    for (const render::ModelShading shading : {render::ModelShading::Legacy, render::ModelShading::Extended}) {
        {
            Rig rig(shading);
            GltfAsset asset = load_fixture();
            asset.images[0].bytes = {9, 9, 9};  // the base color image
            bool threw = false;
            try {
                (void)stage_model(rig.renderer, asset, "bad base", {}, nullptr);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
        }
        {
            Rig rig(shading);
            GltfAsset asset = load_fixture();
            asset.materials[0].base_color_texture.texture = 99;
            bool threw = false;
            try {
                (void)stage_model(rig.renderer, asset, "bad index", {}, nullptr);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
        }
    }
}

TEST_CASE("Stage: tangents reach the vertices, and a normal map on a primitive without them is reported")
{
    Rig rig;
    GltfAsset asset = load_fixture();
    Primitive& primitive = asset.meshes[0].primitives[0];
    // The loader only generates tangents for primitives whose material has a normal texture, so generate them here.
    asset.materials[0].normal_texture.texture = 0;
    REQUIRE(generate_tangents(primitive).result == TangentResult::Generated);
    REQUIRE(primitive.tangents.size() == std::size_t{primitive.vertex_count()} * 4);
    StageReport with_tangents;
    (void)stage_model(rig.renderer, asset, "tangents", {}, &with_tangents);
    CHECK_FALSE(has_warning(with_tangents, "no tangents"));

    primitive.tangents.clear();
    StageReport without;
    (void)stage_model(rig.renderer, asset, "no tangents", {}, &without);
    CHECK(has_warning(without, "its normal map is ignored"));

    primitive.tangents.assign(5, 0.0f);  // a count that does not match the vertices is a malformed primitive
    bool threw = false;
    try {
        (void)stage_model(rig.renderer, asset, "bad tangents");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("Stage: a normal texture on TEXCOORD_1 is ignored with a warning, because only TEXCOORD_0 is sampled")
{
    Rig rig(render::ModelShading::MetallicRoughness);  // the normal map is staged (and shaded) only in this mode
    GltfAsset asset = load_fixture();
    Primitive& primitive = asset.meshes[0].primitives[0];
    REQUIRE(generate_tangents(primitive).result == TangentResult::Generated);

    StageReport without_normal;  // the uploads every other slot of the fixture needs
    (void)stage_model(rig.renderer, asset, "no normal", {}, &without_normal);

    asset.materials[0].normal_texture.texture = 0;
    StageReport on_set_zero;
    (void)stage_model(rig.renderer, asset, "uv0 normal", {}, &on_set_zero);
    CHECK(on_set_zero.textures == without_normal.textures + 1);  // the normal map is uploaded (linear)
    CHECK_FALSE(has_warning(on_set_zero, "TEXCOORD_1 is ignored"));

    asset.materials[0].normal_texture.tex_coord = 1;
    StageReport on_set_one;
    (void)stage_model(rig.renderer, asset, "uv1 normal", {}, &on_set_one);
    CHECK(on_set_one.textures == without_normal.textures);  // not uploaded: the slot stays empty, so the shader keeps the geometric normal
    CHECK(has_warning(on_set_one, "TEXCOORD_1 is ignored"));
    CHECK_FALSE(has_warning(on_set_one, "no tangents"));
}

TEST_CASE("Stage: glTF samplers map to RHI sampler descriptions")
{
    Sampler s;
    s.mag_filter = 9728;  // NEAREST
    s.min_filter = 9984;  // NEAREST_MIPMAP_NEAREST
    s.wrap_s = 33071;     // CLAMP_TO_EDGE
    s.wrap_t = 33648;     // MIRRORED_REPEAT
    rhi::SamplerDesc d = to_sampler_desc(s, 8.0f);
    CHECK(d.mag_filter == rhi::Filter::Nearest);
    CHECK(d.min_filter == rhi::Filter::Nearest);
    CHECK(d.mipmap_mode == rhi::MipmapMode::Nearest);
    CHECK(d.address_u == rhi::AddressMode::ClampToEdge);
    CHECK(d.address_v == rhi::AddressMode::MirroredRepeat);
    CHECK(d.max_anisotropy == 1.0f);  // nearest filtering: no anisotropy

    s = {};  // unspecified filters: glTF leaves them to the implementation; we use trilinear and repeat
    d = to_sampler_desc(s, 8.0f);
    CHECK(d.min_filter == rhi::Filter::Linear);
    CHECK(d.mipmap_mode == rhi::MipmapMode::Linear);
    CHECK(d.address_u == rhi::AddressMode::Repeat);
    CHECK(d.max_anisotropy == 8.0f);
    CHECK(to_sampler_desc(s, 1.0f).max_anisotropy == 1.0f);
    CHECK(to_sampler_desc(s, 100.0f).max_anisotropy == 16.0f);  // clamped to the RHI's range

    s.min_filter = 9729;  // LINEAR, no mipmaps: only the base level is sampled
    d = to_sampler_desc(s, 8.0f);
    CHECK(d.max_lod == 0.0f);
    CHECK(d.max_anisotropy == 1.0f);
    s.min_filter = 9986;
    d = to_sampler_desc(s, 1.0f);
    CHECK(d.min_filter == rhi::Filter::Nearest);
    CHECK(d.mipmap_mode == rhi::MipmapMode::Linear);
    s.wrap_s = 12345;  // unknown: repeat
    CHECK(to_sampler_desc(s, 1.0f).address_u == rhi::AddressMode::Repeat);
    CHECK(rhi::validate(to_sampler_desc(s, 16.0f)).error == nullptr);
}

namespace {

/// The checker box as a skinned asset: every vertex bound to joint (vertex index mod 3) with the rest of the weight on joint 0, a skin of `joints` joints,
/// and its one instance skinned (world left as a non-identity to show it is replaced).
GltfAsset skinned_fixture(uint16_t joints)
{
    GltfAsset asset = load_fixture();
    Primitive& p = asset.meshes[0].primitives[0];
    const uint32_t n = p.vertex_count();
    for (uint32_t v = 0; v < n; ++v) {
        p.joints.insert(p.joints.end(), {static_cast<uint16_t>(v % 3), 0, 0, 0});
        p.weights.insert(p.weights.end(), {0.75f, 0.25f, 0.0f, 0.0f});
    }
    Skin skin;
    skin.name = "rig";
    for (uint16_t j = 0; j < joints; ++j) {
        skin.joint_nodes.push_back(j);
        skin.parent.push_back(j == 0 ? -1 : 0);
        skin.rest.push_back({});
        skin.base.push_back(render::Mat4::identity());
        skin.inverse_bind.push_back(render::Mat4::identity());
    }
    asset.skins.push_back(std::move(skin));
    asset.instances[0].skin = 0;
    asset.instances[0].world.at(0, 3) = 5.0f;  // a skinned node's own transform is irrelevant
    return asset;
}

} // namespace

TEST_CASE("Stage (skinning): skinned primitives get skinned vertices, the skin's joint count and an identity placement")
{
    Rig rig(render::ModelShading::Legacy, false, /*skinning=*/true);
    const GltfAsset asset = skinned_fixture(8);
    StageReport report;
    const render::GpuModel model = stage_model(rig.renderer, asset, "skinned-box", {}, &report);
    REQUIRE(model.meshes.size() == 1 && model.meshes[0].size() == 1);
    const render::GpuPrimitive& prim = model.meshes[0][0];
    CHECK(prim.skinned);
    CHECK(prim.joint_limit == 3);  // joints 0..2 carry weight
    CHECK(prim.vertices.bytes() == 24 * sizeof(render::SkinnedModelVertex));
    CHECK(model.skin_joint_count == 8);
    CHECK(report.skinned_primitives == 1);
    REQUIRE(model.placements.size() == 1);
    {
        const render::Mat4 id = render::Mat4::identity();
        bool identity = true;
        for (int i = 0; i < 16; ++i) identity = identity && model.placements[0].local.m[i] == id.m[i];
        CHECK(identity);  // not the node's translation
    }
    CHECK(!has_warning(report, "skinning is off"));
    // The staged vertices carry what the importer read.
    CHECK(prim.bounds_radius > 0.0f);
}

TEST_CASE("Stage (skinning): a zero-weight influence does not count toward the joint limit, and a joint beyond the skin fails the staging cleanly")
{
    Rig rig(render::ModelShading::Legacy, false, true);
    {
        GltfAsset asset = skinned_fixture(8);
        Primitive& p = asset.meshes[0].primitives[0];
        p.joints[3] = 200;  // slot 3 of vertex 0 has weight 0
        const render::GpuModel model = stage_model(rig.renderer, asset, "zero-weight");
        CHECK(model.meshes[0][0].joint_limit == 3);
    }
    rig.device.wait_idle();  // retire the first block's model before taking the baseline
    const uint32_t baseline = rig.device.live_resource_count();
    {
        GltfAsset asset = skinned_fixture(2);  // joints 0..2 are used but the skin has two
        bool threw = false;
        std::string what;
        try {
            (void)stage_model(rig.renderer, asset, "short-skin");
        } catch (const std::runtime_error& e) {
            threw = true;
            what = e.what();
        }
        CHECK(threw);
        CHECK(what.find("bound to joint 2") != std::string::npos && what.find("skin has 2 joints") != std::string::npos);
    }
    rig.device.wait_idle();
    CHECK(rig.device.live_resource_count() == baseline);  // nothing leaked by the failed staging
    {
        GltfAsset asset = skinned_fixture(300);  // more than kMaxSkinJoints
        bool threw = false;
        try {
            (void)stage_model(rig.renderer, asset, "big-skin");
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("at most 256") != std::string::npos;
        }
        CHECK(threw);
    }
}

TEST_CASE("Stage (skinning): each joint gets the bind-space box of the vertices it influences, and a primitive whose weights do not sum to one gets none")
{
    Rig rig(render::ModelShading::Legacy, false, true);
    GltfAsset asset = skinned_fixture(8);
    const Primitive& src = asset.meshes[0].primitives[0];
    const uint32_t n = src.vertex_count();
    {
        const render::GpuModel model = stage_model(rig.renderer, asset, "bounds");
        const render::GpuPrimitive& prim = model.meshes[0][0];
        REQUIRE(prim.joint_bounds.size() == prim.joint_limit);
        REQUIRE(prim.joint_bounds.size() == 3);
        for (uint32_t j = 0; j < 3; ++j) {
            // Vertex v is bound to joint v % 3 (weight 0.75) and to joint 0 (0.25): joint 0 influences every vertex, joint j > 0 the vertices with v % 3 == j.
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            uint32_t count = 0;
            for (uint32_t v = 0; v < n; ++v) {
                if (j != 0 && v % 3 != j) continue;
                ++count;
                for (uint32_t c = 0; c < 3; ++c) {
                    lo[c] = std::min(lo[c], src.positions[v * 3 + c]);
                    hi[c] = std::max(hi[c], src.positions[v * 3 + c]);
                }
            }
            const render::JointBounds& jb = prim.joint_bounds[j];
            CHECK(jb.used == (count > 0));
            CHECK(jb.min.x == lo[0] && jb.min.y == lo[1] && jb.min.z == lo[2]);
            CHECK(jb.max.x == hi[0] && jb.max.y == hi[1] && jb.max.z == hi[2]);
        }
        CHECK(!prim.joint_bounds[1].used || prim.joint_bounds[1].min.x >= prim.joint_bounds[0].min.x);  // a subset's box lies inside joint 0's
    }
    {
        // A zero-weight influence contributes to no joint's box (the vertex's slot 3 names joint 200 with weight 0).
        GltfAsset a = skinned_fixture(8);
        a.meshes[0].primitives[0].joints[3] = 200;
        const render::GpuModel model = stage_model(rig.renderer, a, "zero-weight-bounds");
        CHECK(model.meshes[0][0].joint_bounds.size() == 3);
    }
    {
        // A joint nobody uses (joints 0 and 2 only) stays unused.
        GltfAsset a = skinned_fixture(8);
        Primitive& p = a.meshes[0].primitives[0];
        for (uint32_t v = 0; v < n; ++v) p.joints[v * 4] = (v % 2 == 0) ? 0 : 2;
        const render::GpuModel model = stage_model(rig.renderer, a, "gap");
        REQUIRE(model.meshes[0][0].joint_bounds.size() == 3);
        CHECK(model.meshes[0][0].joint_bounds[0].used);
        CHECK(!model.meshes[0][0].joint_bounds[1].used);
        CHECK(model.meshes[0][0].joint_bounds[2].used);
    }
    {
        // Weights that do not sum to one are not convex combinations: no bound holds, so the primitive carries none (and is drawn but never culled).
        GltfAsset a = skinned_fixture(8);
        a.meshes[0].primitives[0].weights[0] = 0.5f;  // vertex 0 sums to 0.75
        const render::GpuModel model = stage_model(rig.renderer, a, "unnormalized");
        CHECK(model.meshes[0][0].skinned);
        CHECK(model.meshes[0][0].joint_bounds.empty());
    }
    {
        // The tolerance is 1e-3: a sum of 1.005 is not a convex combination to within it.
        GltfAsset a = skinned_fixture(8);
        a.meshes[0].primitives[0].weights[0] = 0.75f + 5e-3f;
        const render::GpuModel model = stage_model(rig.renderer, a, "off-by-half-percent");
        CHECK(model.meshes[0][0].joint_bounds.empty());
    }
    {
        // Within float noise of one is fine.
        GltfAsset a = skinned_fixture(8);
        a.meshes[0].primitives[0].weights[0] = 0.75f + 5e-4f;
        const render::GpuModel model = stage_model(rig.renderer, a, "noise");
        CHECK(model.meshes[0][0].joint_bounds.size() == 3);
    }
}

TEST_CASE("Stage (skinning): with skinning off a skinned mesh is staged rigid in the bind pose, with a warning; a second skin is not placed")
{
    {
        Rig rig;  // skinning off
        const GltfAsset asset = skinned_fixture(8);
        StageReport report;
        const render::GpuModel model = stage_model(rig.renderer, asset, "off", {}, &report);
        CHECK(!model.meshes[0][0].skinned);
        CHECK(model.meshes[0][0].vertices.bytes() == 24 * sizeof(render::ModelVertex));
        CHECK(model.skin_joint_count == 0 && report.skinned_primitives == 0);
        CHECK(has_warning(report, "skinning is off"));
        REQUIRE(model.placements.size() == 1);
        CHECK(model.placements[0].local.at(0, 3) == 5.0f);  // rigid: the node's transform applies
    }
    {
        Rig rig(render::ModelShading::Legacy, false, true);
        GltfAsset asset = skinned_fixture(8);
        Skin other = asset.skins[0];
        other.name = "other";
        asset.skins.push_back(other);
        MeshInstance second = asset.instances[0];
        second.skin = 1;
        asset.instances.push_back(second);
        StageReport report;
        const render::GpuModel model = stage_model(rig.renderer, asset, "two-skins", {}, &report);
        CHECK(model.placements.size() == 1);  // the second skin's instance is not placed
        CHECK(has_warning(report, "second skin"));
        GltfAsset bad = skinned_fixture(8);
        bad.instances[0].skin = 7;  // refers to no skin
        bool threw = false;
        try {
            (void)stage_model(rig.renderer, bad, "bad-skin");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
}

// ---- Prepared models: CPU preparation, then upload ----------------------------------------------------------------------------------------------

namespace {

/// Everything observable about a staged model that two stagings of the same asset must agree on.
struct Fingerprint {
    std::size_t meshes = 0, materials = 0, placements = 0;
    std::vector<uint32_t> index_counts;
    std::vector<uint64_t> vertex_bytes, index_bytes;
    std::vector<float> radii;
    std::vector<std::string> names;
    uint32_t live = 0;
    uint64_t live_bytes = 0;
    bool operator==(const Fingerprint&) const = default;
};

Fingerprint fingerprint(Rig& rig, const render::GpuModel& model)
{
    Fingerprint f;
    f.meshes = model.meshes.size();
    f.materials = model.materials.size();
    f.placements = model.placements.size();
    for (const auto& mesh : model.meshes) {
        for (const render::GpuPrimitive& p : mesh) {
            f.index_counts.push_back(p.index_count);
            f.vertex_bytes.push_back(p.vertices.bytes());
            f.index_bytes.push_back(p.indices.bytes());
            f.radii.push_back(p.bounds_radius);
            f.names.push_back(p.vertices.name());
        }
    }
    const render::ResourceStats s = rig.renderer.registry().total_stats();
    f.live = s.live;
    f.live_bytes = s.live_bytes;
    return f;
}

}  // namespace

TEST_CASE("Prepared: preparation needs no renderer or device, and the payload accounts for its CPU memory and time")
{
    const GltfAsset asset = load_fixture();
    StageCapabilities caps;  // the defaults: Legacy shading, no skinning, no blending
    const PreparedModel prepared = prepare_model(asset, "checker-box", caps);
    CHECK(prepared.name == "checker-box");
    CHECK(prepared.capabilities == caps);
    REQUIRE(prepared.images.size() == 1);
    CHECK(prepared.images[0].levels == 4);
    CHECK(prepared.images[0].data.size() == (64 + 16 + 4 + 1) * 4);
    REQUIRE(prepared.meshes.size() == 1);
    CHECK(prepared.meshes[0][0].vertices.size() == 24 * sizeof(render::ModelVertex));
    CHECK(prepared.meshes[0][0].indices.size() == 36 * sizeof(uint16_t));
    CHECK(prepared.materials.size() == 2);  // the file's, then the default
    CHECK(prepared.cpu_bytes() == prepared.images[0].data.size() + prepared.meshes[0][0].vertices.size() + prepared.meshes[0][0].indices.size());
    CHECK(prepared.report.primitives == 1);
    CHECK(prepared.report.vertex_bytes == 24 * sizeof(render::ModelVertex));
    CHECK(prepared.report.warnings.empty());
    CHECK(prepared.prepare_seconds >= 0.0);
    CHECK(prepared.decode_seconds >= 0.0);
    CHECK(prepared.decode_seconds <= prepared.prepare_seconds);
    CHECK(prepared.report.textures == 0);  // nothing is on a GPU yet
    Rig rig;
    StageReport report;
    double upload_seconds = 0.0;
    (void)upload_model(rig.renderer, prepare_model(asset, "timing", capabilities_of(rig.renderer)), &report, &upload_seconds);
    // Informational, not asserted: the 8x8 fixture is tiny, and a null device has no transfer to wait for. The numbers exist so a host can see decode/mips cost apart from
    // submit/readiness cost on its own assets and device.
    std::printf("  prepare %.3f ms (decode and mips %.3f ms), upload %.3f ms on the null device, %llu CPU bytes\n", prepared.prepare_seconds * 1e3, prepared.decode_seconds * 1e3,
                upload_seconds * 1e3, static_cast<unsigned long long>(prepared.cpu_bytes()));
}

TEST_CASE("Prepared: prepare then upload equals stage_model, for every material mode, and the payload is not consumed")
{
    for (const render::ModelShading shading : {render::ModelShading::Legacy, render::ModelShading::Extended, render::ModelShading::MetallicRoughness}) {
        GltfAsset asset = load_fixture();
        asset.materials[0].emissive_texture.texture = 0;
        asset.materials[0].occlusion_texture.texture = 0;
        Rig staged(shading), split(shading);
        StageReport staged_report, split_report;
        const render::GpuModel a = stage_model(staged.renderer, asset, "same", {}, &staged_report);
        const PreparedModel prepared = prepare_model(asset, "same", capabilities_of(split.renderer));
        const uint64_t cpu_before = prepared.cpu_bytes();
        double upload_seconds = -1.0;
        const render::GpuModel b = upload_model(split.renderer, prepared, &split_report, &upload_seconds);
        CHECK(fingerprint(staged, a) == fingerprint(split, b));
        CHECK(staged_report.warnings == split_report.warnings);
        CHECK(staged_report.textures == split_report.textures);
        CHECK(staged_report.texture_bytes == split_report.texture_bytes);
        CHECK(staged_report.vertex_bytes == split_report.vertex_bytes);
        CHECK(staged_report.index_bytes == split_report.index_bytes);
        CHECK(upload_seconds >= 0.0 && split_report.upload_seconds == upload_seconds);
        CHECK(split_report.prepare_seconds == prepared.prepare_seconds);
        CHECK(prepared.cpu_bytes() == cpu_before);  // upload_model takes the payload by const reference and leaves it alone
        CHECK(shading == render::ModelShading::Legacy ? split_report.textures == 1 : split_report.textures == 2);  // sRGB and linear uploads of the one image
        CHECK(split.device.validation_error_count() == 0);
    }
}

TEST_CASE("Prepared: one payload uploads twice into independent models, on one renderer or two, and outlives the renderer it was prepared for")
{
    const GltfAsset asset = load_fixture();
    PreparedModel prepared;
    {
        Rig first;
        prepared = prepare_model(asset, "shared", capabilities_of(first.renderer));
    }  // the renderer the capabilities were read from is gone: the payload does not refer to it
    Rig rig;
    (void)rig.renderer.model_renderer();
    const render::ResourceStats baseline = rig.renderer.registry().total_stats();
    {
        const render::GpuModel one = upload_model(rig.renderer, prepared);
        const render::ResourceStats after_one = rig.renderer.registry().total_stats();
        const render::GpuModel two = upload_model(rig.renderer, prepared);
        const render::ResourceStats after_two = rig.renderer.registry().total_stats();
        CHECK(after_one.live > baseline.live);
        CHECK(after_two.live - after_one.live == after_one.live - baseline.live);  // the second upload made exactly as many objects as the first
        CHECK(one.meshes[0][0].vertices.handle() != two.meshes[0][0].vertices.handle());
        CHECK(one.meshes[0][0].vertices.bytes() == two.meshes[0][0].vertices.bytes());
    }
    rig.device.wait_idle();
    CHECK(rig.renderer.registry().total_stats().live == baseline.live);  // both released; the payload is untouched
    Rig other;
    const render::GpuModel again = upload_model(other.renderer, prepared);  // a different renderer with the same capabilities
    CHECK(again.meshes.size() == 1);
    CHECK(other.device.validation_error_count() == 0);
}

TEST_CASE("Prepared: a renderer with other capabilities refuses the payload before creating anything")
{
    const GltfAsset asset = load_fixture();
    Rig legacy;
    const PreparedModel prepared = prepare_model(asset, "legacy", capabilities_of(legacy.renderer));
    const auto refused = [&](Rig& rig) {
        (void)rig.renderer.model_renderer();
        const uint32_t before = rig.renderer.registry().total_stats().live;
        bool threw = false;
        try {
            (void)upload_model(rig.renderer, prepared);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        return threw && rig.renderer.registry().total_stats().live == before;
    };
    Rig metallic(render::ModelShading::MetallicRoughness);
    Rig extended(render::ModelShading::Extended);
    Rig blending(render::ModelShading::Legacy, /*alpha_blending=*/true);
    Rig skinning(render::ModelShading::Legacy, false, /*skinning=*/true);
    CHECK(refused(metallic));
    CHECK(refused(extended));
    CHECK(refused(blending));
    CHECK(refused(skinning));
    CHECK(!refused(legacy));  // the matching renderer accepts it (refused() reports a refusal)
    // Preparing per mode gives per-mode payloads of different content: the other slots are decoded only when the mode reads them.
    GltfAsset slots = load_fixture();
    slots.materials[0].emissive_texture.texture = 0;    // sRGB: shares the base color's image
    slots.materials[0].occlusion_texture.texture = 0;   // linear: a second decode of the same image
    CHECK(prepare_model(slots, "m", capabilities_of(legacy.renderer)).images.size() == 1);
    CHECK(prepare_model(slots, "m", capabilities_of(metallic.renderer)).images.size() == 2);
}

TEST_CASE("Prepared: asset errors fail the preparation with the same messages and no device effect; an upload failure at any creation releases everything and keeps the payload usable")
{
    {
        GltfAsset asset = load_fixture();
        asset.images[0].bytes.resize(20);
        std::string message;
        try {
            (void)prepare_model(asset, "broken", StageCapabilities{});
        } catch (const std::runtime_error& e) {
            message = e.what();
        }
        CHECK(message.find("broken/image[0]") != std::string::npos);
        asset = load_fixture();
        asset.meshes[0].primitives[0].indices[0] = 9999;
        message.clear();
        try {
            (void)prepare_model(asset, "bad-index", StageCapabilities{});
        } catch (const std::runtime_error& e) {
            message = e.what();
        }
        CHECK(message.find("bad-index/mesh[0]") != std::string::npos);
        CHECK(message.find("past the last vertex") != std::string::npos);
    }
    Rig rig(render::ModelShading::MetallicRoughness);
    GltfAsset asset = load_fixture();
    asset.materials[0].emissive_texture.texture = 0;
    asset.materials[0].occlusion_texture.texture = 0;
    const PreparedModel prepared = prepare_model(asset, "swept", capabilities_of(rig.renderer));
    rig.device.wait_idle();
    const uint32_t baseline = rig.device.live_resource_count();
    uint32_t failures = 0;
    for (uint32_t n = 1; n <= 80; ++n) {
        rig.device.fail_creation_after(n);
        bool threw = false;
        try {
            const render::GpuModel m = upload_model(rig.renderer, prepared);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        rig.device.fail_creation_after(0);
        rig.device.wait_idle();
        if (!threw) break;
        ++failures;
        CHECK(rig.device.live_resource_count() == baseline);
    }
    std::printf("  %u failing creation points swept\n", failures);
    CHECK(failures >= 5);  // textures, a sampler, materials and their groups, vertex and index buffers
    const render::GpuModel fine = upload_model(rig.renderer, prepared);  // the payload survived every failed attempt
    CHECK(fine.meshes.size() == 1);
    CHECK(rig.device.validation_error_count() == 0);
}

TEST_CASE("Prepared: preparation is a pure function of its inputs (several threads at once give identical payloads), and skinning payloads match what staging produced")
{
    const GltfAsset asset = load_fixture();
    const StageCapabilities caps{render::ModelShading::MetallicRoughness, false, false, false};
    const PreparedModel reference = prepare_model(asset, "threads", caps);
    std::vector<PreparedModel> results(4);
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < results.size(); ++i) {
        workers.emplace_back([&, i] { results[i] = prepare_model(asset, "threads", caps); });
    }
    for (std::thread& w : workers) w.join();
    for (const PreparedModel& r : results) {
        REQUIRE(r.images.size() == reference.images.size());
        for (std::size_t i = 0; i < r.images.size(); ++i) CHECK(r.images[i].data == reference.images[i].data);
        CHECK(r.meshes[0][0].vertices == reference.meshes[0][0].vertices);
        CHECK(r.meshes[0][0].indices == reference.meshes[0][0].indices);
        CHECK(r.report.warnings == reference.report.warnings);
    }
    // A skinned asset prepared for a skinning renderer packs skinned vertices, with the same joint limit, joint count and identity placement as stage_model's.
    Rig rig(render::ModelShading::Legacy, false, /*skinning=*/true);
    const GltfAsset skinned = skinned_fixture(8);
    const PreparedModel p = prepare_model(skinned, "skinned", capabilities_of(rig.renderer));
    REQUIRE(p.meshes.size() == 1);
    CHECK(p.meshes[0][0].skinned);
    CHECK(p.meshes[0][0].joint_limit == 3);
    CHECK(p.meshes[0][0].vertices.size() == 24 * sizeof(render::SkinnedModelVertex));
    CHECK(p.skin_joint_count == 8);
    CHECK(p.report.skinned_primitives == 1);
    const render::GpuModel model = upload_model(rig.renderer, p);
    CHECK(model.skin_joint_count == 8);
    CHECK(model.meshes[0][0].skinned);
}

namespace {
bool has_flag(rhi::TextureUsage u, rhi::TextureUsage f) { return (static_cast<uint32_t>(u) & static_cast<uint32_t>(f)) != 0; }
}  // namespace

TEST_CASE("Prepared: the uploaded textures have the color space of their slot, every slot's sampler reaches the device, and the bounds are the box center and farthest vertex")
{
    Rig rig(render::ModelShading::MetallicRoughness);
    GltfAsset asset = load_fixture();
    Sampler nearest;
    nearest.mag_filter = 9728;
    nearest.min_filter = 9728;
    Sampler clamped;
    clamped.wrap_s = 33071;
    clamped.wrap_t = 33071;
    asset.samplers.assign({nearest, clamped});
    asset.textures[0].sampler = 0;
    Texture second = asset.textures[0];
    second.sampler = 1;
    asset.textures.push_back(second);
    Material& m = asset.materials[0];
    m.base_color_texture.texture = 0;  // sRGB, the nearest sampler
    m.occlusion_texture.texture = 1;   // linear, the clamping sampler
    const PreparedModel prepared = prepare_model(asset, "slots", capabilities_of(rig.renderer));
    REQUIRE(prepared.images.size() == 2);
    CHECK(prepared.images[0].srgb);    // the base color is decoded first, as sRGB
    CHECK(!prepared.images[1].srgb);
    const PreparedMaterial& pm = prepared.materials[0];
    CHECK(pm.slots[static_cast<uint32_t>(render::MaterialSlot::BaseColor)].image == 0);
    CHECK(pm.slots[static_cast<uint32_t>(render::MaterialSlot::BaseColor)].sampler.min_filter == rhi::Filter::Nearest);
    CHECK(pm.slots[static_cast<uint32_t>(render::MaterialSlot::Occlusion)].image == 1);
    CHECK(pm.slots[static_cast<uint32_t>(render::MaterialSlot::Occlusion)].sampler.address_u == rhi::AddressMode::ClampToEdge);

    (void)rig.renderer.model_renderer();
    const render::ResourceStats samplers_before = rig.renderer.registry().stats(render::ResourceKind::Sampler);
    const render::GpuModel model = upload_model(rig.renderer, prepared);
    CHECK(rig.renderer.registry().stats(render::ResourceKind::Sampler).created - samplers_before.created == 2);  // both non-default descriptions
    // Which of the model's textures is sRGB and which linear: one of each, by the device's own description.
    uint32_t srgb = 0, linear = 0;
    for (const render::ResourceInfo& r : rig.renderer.registry().live_resources()) {
        if (r.kind != render::ResourceKind::Texture || r.name.find("slots/image[0]") != 0) continue;
        if (r.name.find("(sRGB)") != std::string::npos) ++srgb;
        if (r.name.find("(linear)") != std::string::npos) ++linear;
    }
    CHECK(srgb == 1);
    CHECK(linear == 1);
    // The GPU description each prepared image is uploaded with: the color space decides the format, and the mip chain is the whole chain.
    const rhi::TextureDesc d_srgb = texture_desc_of(prepared.images[0]), d_linear = texture_desc_of(prepared.images[1]);
    CHECK(d_srgb.format == rhi::Format::RGBA8_SRGB);
    CHECK(d_linear.format == rhi::Format::RGBA8_UNORM);
    CHECK(d_srgb.width == 8 && d_srgb.height == 8 && d_srgb.mip_levels == 4);
    CHECK(has_flag(d_srgb.usage, rhi::TextureUsage::Sampled) && has_flag(d_srgb.usage, rhi::TextureUsage::TransferDst));

    // Bounds: the center of the position box and the farthest vertex from it, checked against the asset's own positions.
    const Primitive& p = asset.meshes[0].primitives[0];
    float lo[3] = {p.positions[0], p.positions[1], p.positions[2]}, hi[3] = {lo[0], lo[1], lo[2]};
    for (std::size_t v = 1; v < p.vertex_count(); ++v) {
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], p.positions[v * 3 + static_cast<std::size_t>(c)]);
            hi[c] = std::max(hi[c], p.positions[v * 3 + static_cast<std::size_t>(c)]);
        }
    }
    const float center[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float far2 = 0.0f;
    for (std::size_t v = 0; v < p.vertex_count(); ++v) {
        float d2 = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float d = p.positions[v * 3 + static_cast<std::size_t>(c)] - center[c];
            d2 += d * d;
        }
        far2 = std::max(far2, d2);
    }
    const render::GpuPrimitive& gp = model.meshes[0][0];
    CHECK(std::fabs(gp.bounds_center.x - center[0]) < 1e-6f && std::fabs(gp.bounds_center.y - center[1]) < 1e-6f && std::fabs(gp.bounds_center.z - center[2]) < 1e-6f);
    CHECK(std::fabs(gp.bounds_radius - std::sqrt(far2)) < 1e-5f);
    CHECK(gp.bounds_radius > 0.5f);  // a real sphere, not a zero default
}
