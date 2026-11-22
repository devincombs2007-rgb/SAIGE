#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/tangents.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace saige;
using namespace saige::gfx;

namespace {

struct Rig {
    rhi::NullDevice device;
    render::SceneRenderer renderer{device, rhi::Format::RGBA8_SRGB};
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
    blend.name = "glass";
    asset.materials.push_back(mask);
    asset.materials.push_back(blend);
    StageReport report;
    const render::GpuModel model = stage_model(rig.renderer, asset, "materials", {}, &report);
    REQUIRE(model.materials.size() == 4);
    CHECK_FALSE(model.materials[0].double_sided);
    CHECK(model.materials[1].double_sided);
    CHECK(has_warning(report, "alpha blending is not implemented"));
    CHECK(report.warnings.size() == 1);  // warned about once, however many materials trigger it
}

TEST_CASE("Stage: every slot gets its own texture upload per color space and the sampler its glTF texture names")
{
    Rig rig;
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
    // An out-of-range slot texture is rejected with the slot named.
    asset.materials[0].emissive_texture.texture = 99;
    bool threw = false;
    try {
        (void)stage_model(rig.renderer, asset, "bad slot");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("emissive texture index is out of range") != std::string::npos;
    }
    CHECK(threw);
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
    Rig rig;
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
