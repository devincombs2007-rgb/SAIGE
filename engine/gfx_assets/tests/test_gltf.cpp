#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_assets/resource_uri.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace saige::gfx;
using saige::render::Float3;
using saige::render::Mat4;
namespace fs = std::filesystem;

namespace {

// ----------------------------------------------------------------------------------------------- builders
struct Blob {
    std::vector<uint8_t> bytes;
    std::size_t f32(float v)
    {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        const std::size_t at = bytes.size();
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
        return at;
    }
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) { bytes.push_back(static_cast<uint8_t>(v & 0xFF)); bytes.push_back(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
    void vec3(float x, float y, float z) { f32(x); f32(y); f32(z); }
    void pad4() { while (bytes.size() % 4 != 0) bytes.push_back(0); }
};

std::string base64(const std::vector<uint8_t>& raw)
{
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        const uint32_t b0 = raw[i];
        const uint32_t b1 = i + 1 < raw.size() ? raw[i + 1] : 0;
        const uint32_t b2 = i + 2 < raw.size() ? raw[i + 2] : 0;
        const uint32_t t = (b0 << 16) | (b1 << 8) | b2;
        out += a[(t >> 18) & 63];
        out += a[(t >> 12) & 63];
        out += i + 1 < raw.size() ? a[(t >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? a[t & 63] : '=';
    }
    return out;
}

std::string data_uri(const Blob& b, const char* mime = "application/octet-stream")
{
    return std::string("data:") + mime + ";base64," + base64(b.bytes);
}

std::span<const uint8_t> bytes_of(const std::string& s)
{
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

GltfLoadResult load_text(const std::string& json, const ResourceReader& reader = {}, const GltfLimits& limits = {})
{
    return load_gltf(bytes_of(json), reader, limits);
}

/// A document with one non-indexed triangle (glTF space): (0,0,0), (1,0,0), (0,1,0), counter-clockwise seen from +Z.
std::string triangle_doc(const std::string& extra_root = "", const std::string& node_extra = "")
{
    Blob b;
    b.vec3(0, 0, 0);
    b.vec3(1, 0, 0);
    b.vec3(0, 1, 0);
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
           R"("nodes":[{"mesh":0)" + node_extra + R"(}],)"
           R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
           R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
           R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
           R"("buffers":[{"byteLength":36,"uri":")" + data_uri(b) + R"("}])" + extra_root + "}";
}

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
bool near(const float* a, const Float3& b) { return near(a[0], b.x) && near(a[1], b.y) && near(a[2], b.z); }

bool fails_with(const GltfLoadResult& r, const std::string& fragment)
{
    if (r.ok() || r.error.find(fragment) == std::string::npos) {
        std::printf("  expected an error containing '%s', got: %s\n", fragment.c_str(),
                    r.ok() ? "(success)" : r.error.c_str());
        return false;
    }
    return true;
}

/// Wraps JSON and an optional BIN payload into a GLB container.
std::vector<uint8_t> make_glb(std::string json, const Blob* bin = nullptr)
{
    while (json.size() % 4 != 0) json.push_back(' ');
    Blob out;
    out.u32(0x46546C67);
    out.u32(2);
    const std::size_t length_at = out.bytes.size();
    out.u32(0);
    out.u32(static_cast<uint32_t>(json.size()));
    out.u32(0x4E4F534A);
    out.bytes.insert(out.bytes.end(), json.begin(), json.end());
    if (bin != nullptr) {
        Blob padded = *bin;
        padded.pad4();
        out.u32(static_cast<uint32_t>(padded.bytes.size()));
        out.u32(0x004E4942);
        out.bytes.insert(out.bytes.end(), padded.bytes.begin(), padded.bytes.end());
    }
    const auto total = static_cast<uint32_t>(out.bytes.size());
    for (int i = 0; i < 4; ++i) out.bytes[length_at + static_cast<std::size_t>(i)] = static_cast<uint8_t>((total >> (8 * i)) & 0xFF);
    return out.bytes;
}

} // namespace

// ================================================================================================= basics
TEST_CASE("glTF: a triangle is converted to engine space with its winding preserved")
{
    const GltfLoadResult r = load_text(triangle_doc());
    REQUIRE(r.ok());
    const GltfAsset& a = *r.asset;
    REQUIRE(a.meshes.size() == 1);
    REQUIRE(a.meshes[0].primitives.size() == 1);
    const Primitive& p = a.meshes[0].primitives[0];
    REQUIRE(p.vertex_count() == 3);
    // glTF (0,0,0),(1,0,0),(0,1,0) -> engine (z,x,y): (0,0,0),(0,1,0),(0,0,1)
    CHECK(near(&p.positions[0], {0, 0, 0}));
    CHECK(near(&p.positions[3], {0, 1, 0}));
    CHECK(near(&p.positions[6], {0, 0, 1}));
    // The file had no normals: flat normals were generated. glTF face normal +Z becomes engine +X (forward).
    CHECK(p.normals_generated);
    for (int v = 0; v < 3; ++v) {
        CHECK(near(&p.normals[static_cast<std::size_t>(v) * 3], {1, 0, 0}));
    }
    // Winding is preserved: the geometric normal of the converted triangle agrees with the converted face normal.
    const Float3 e0{p.positions[0], p.positions[1], p.positions[2]};
    const Float3 e1{p.positions[3], p.positions[4], p.positions[5]};
    const Float3 e2{p.positions[6], p.positions[7], p.positions[8]};
    const Float3 geometric = saige::render::normalize(saige::render::cross(e1 - e0, e2 - e0));
    CHECK(near(geometric.x, 1.0f));
    CHECK(p.indices == std::vector<uint32_t>({0, 1, 2}));
    // One node with a mesh and an identity transform: an identity instance (conjugating identity gives identity).
    REQUIRE(a.instances.size() == 1);
    CHECK(a.instances[0].mesh == 0);
    for (int i = 0; i < 16; ++i) {
        CHECK(near(a.instances[0].world.m[i], Mat4::identity().m[i]));
    }
    CHECK(a.bounds.valid());
    CHECK(near(a.bounds.max.z, 1.0f));
}

TEST_CASE("glTF: the three axes map up -> +Z, front -> +X, left -> +Y")
{
    const Float3 up = gltf_to_engine({0, 1, 0});
    const Float3 front = gltf_to_engine({0, 0, 1});
    const Float3 left = gltf_to_engine({1, 0, 0});
    CHECK(up == Float3{0, 0, 1});
    CHECK(front == Float3{1, 0, 0});
    CHECK(left == Float3{0, 1, 0});
    // A proper rotation: right-handed stays right-handed (forward x left = up in the engine).
    CHECK(saige::render::cross(front, left) == up);
}

TEST_CASE("glTF: indexed geometry keeps shared vertices and honors every index type")
{
    for (const int index_type : {5121, 5123, 5125}) {
        Blob b;
        b.vec3(0, 0, 0); b.vec3(1, 0, 0); b.vec3(1, 1, 0); b.vec3(0, 1, 0);   // positions: 48 bytes
        for (int i = 0; i < 4; ++i) b.vec3(0, 0, 1);                           // normals: 48 bytes
        const std::size_t index_offset = b.bytes.size();
        for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            if (index_type == 5121) b.u8(static_cast<uint8_t>(i));
            else if (index_type == 5123) b.u16(static_cast<uint16_t>(i));
            else b.u32(i);
        }
        const std::size_t index_bytes = b.bytes.size() - index_offset;
        const std::string json =
            R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)"
            R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},)"
            R"({"bufferView":2,"componentType":)" + std::to_string(index_type) + R"(,"count":6,"type":"SCALAR"}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},)"
            R"({"buffer":0,"byteOffset":96,"byteLength":)" + std::to_string(index_bytes) + R"(}],)"
            R"("buffers":[{"byteLength":)" + std::to_string(b.bytes.size()) + R"(,"uri":")" + data_uri(b) + R"("}]})";
        const GltfLoadResult r = load_text(json);
        REQUIRE(r.ok());
        const Primitive& p = r.asset->meshes[0].primitives[0];
        CHECK(p.vertex_count() == 4);              // shared: not duplicated
        CHECK_FALSE(p.normals_generated);
        CHECK(p.indices == std::vector<uint32_t>({0, 1, 2, 0, 2, 3}));
        CHECK(near(&p.normals[0], {1, 0, 0}));     // glTF +Z normal -> engine +X
        // No scene: every root node is instantiated.
        CHECK(r.asset->instances.size() == 1);
    }
}

TEST_CASE("glTF: interleaved (strided) attributes and normalized integer data")
{
    // One bufferView, stride 20: position (12 bytes) + uv as normalized ushort (4 bytes) + color as normalized ubyte RGBA (4).
    Blob b;
    for (int v = 0; v < 3; ++v) {
        b.vec3(static_cast<float>(v), 0, 0);
        b.u16(static_cast<uint16_t>(v == 1 ? 65535 : 0));   // u
        b.u16(static_cast<uint16_t>(v == 2 ? 32768 : 0));   // v
        b.u8(255); b.u8(0); b.u8(static_cast<uint8_t>(v * 100)); b.u8(128);
    }
    const std::string json =
        R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"COLOR_0":2}}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":0,"byteOffset":12,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"},)"
        R"({"bufferView":0,"byteOffset":16,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":60,"byteStride":20}],)"
        R"("buffers":[{"byteLength":60,"uri":")" + data_uri(b) + R"("}]})";
    const GltfLoadResult r = load_text(json);
    REQUIRE(r.ok());
    const Primitive& p = r.asset->meshes[0].primitives[0];
    REQUIRE(p.vertex_count() == 3);
    CHECK(p.normals_generated);  // flat normals => de-indexed, but attributes follow their vertices
    REQUIRE(p.uv0.size() == 6);
    CHECK(near(p.uv0[2], 1.0f));                       // vertex 1 u = 65535/65535
    CHECK(near(p.uv0[5], 32768.0f / 65535.0f, 1e-4f)); // vertex 2 v
    REQUIRE(p.colors.size() == 12);
    CHECK(near(p.colors[0], 1.0f));
    CHECK(near(p.colors[1], 0.0f));
    CHECK(near(p.colors[8 + 2], 200.0f / 255.0f, 1e-4f));
    CHECK(near(p.colors[3], 128.0f / 255.0f, 1e-4f));
}

TEST_CASE("glTF: COLOR_0 as VEC3 gets an opaque alpha")
{
    Blob b;
    b.vec3(0, 0, 0); b.vec3(1, 0, 0); b.vec3(0, 1, 0);
    for (int v = 0; v < 3; ++v) b.vec3(0.5f, 0.25f, 0.75f);
    const std::string json =
        R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1}}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36}],)"
        R"("buffers":[{"byteLength":72,"uri":")" + data_uri(b) + R"("}]})";
    const GltfLoadResult r = load_text(json);
    REQUIRE(r.ok());
    const Primitive& p = r.asset->meshes[0].primitives[0];
    REQUIRE(p.colors.size() == 12);
    CHECK(near(p.colors[0], 0.5f));
    CHECK(near(p.colors[3], 1.0f));
}

// ================================================================================================= nodes
TEST_CASE("glTF: node hierarchies compose in glTF space and are conjugated into engine space")
{
    // Parent: translate (1,2,3), rotate +90 degrees about glTF +Y. Child: translate (0,0,1) in the parent's frame.
    // Rotating (0,0,1) by +90 about Y gives (1,0,0), so the child's world translation is (2,2,3) in glTF = (3,2,2) in engine.
    const double s = std::sqrt(0.5);
    const std::string nodes = R"("nodes":[{"translation":[1,2,3],"rotation":[0,)" + std::to_string(s) + R"(,0,)" +
                              std::to_string(s) + R"(],"children":[1]},{"translation":[0,0,1],"mesh":0}])";
    std::string doc = triangle_doc();
    const std::size_t at = doc.find(R"("nodes":[{"mesh":0}])");
    REQUIRE(at != std::string::npos);
    doc.replace(at, std::string(R"("nodes":[{"mesh":0}])").size(), nodes);
    const GltfLoadResult r = load_text(doc);
    REQUIRE(r.ok());
    REQUIRE(r.asset->instances.size() == 1);
    const Mat4& w = r.asset->instances[0].world;
    CHECK(near(w.at(0, 3), 3.0f, 1e-4f));
    CHECK(near(w.at(1, 3), 2.0f, 1e-4f));
    CHECK(near(w.at(2, 3), 2.0f, 1e-4f));
    CHECK(r.asset->instances[0].node == 1);
}

TEST_CASE("glTF: node matrix and non-uniform scale land on the right engine axes")
{
    // Column-major matrix with translation (5,6,7) in glTF space => engine (7,5,6).
    std::string doc = triangle_doc("", R"(,"matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1])");
    GltfLoadResult r = load_text(doc);
    REQUIRE(r.ok());
    const Mat4& w = r.asset->instances[0].world;
    CHECK(near(w.at(0, 3), 7.0f));
    CHECK(near(w.at(1, 3), 5.0f));
    CHECK(near(w.at(2, 3), 6.0f));

    // Scale (2,3,4) on glTF (x,y,z) scales engine (y,z,x) by the same factors: engine x scales by 4, y by 2, z by 3.
    doc = triangle_doc("", R"(,"scale":[2,3,4])");
    r = load_text(doc);
    REQUIRE(r.ok());
    const Mat4& s = r.asset->instances[0].world;
    CHECK(near(s.at(0, 0), 4.0f));
    CHECK(near(s.at(1, 1), 2.0f));
    CHECK(near(s.at(2, 2), 3.0f));
    CHECK(near(s.at(0, 1), 0.0f));
}

TEST_CASE("glTF: scene selection and instance bounds")
{
    Blob b;
    b.vec3(-1, 0, -1); b.vec3(1, 0, -1); b.vec3(0, 2, 1);
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":1,"scenes":[{"nodes":[0]},{"nodes":[1]}],)"
        R"("nodes":[{"mesh":0},{"mesh":0,"translation":[10,0,0]}],)"
        R"("meshes":[{"name":"m","primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
        R"("buffers":[{"byteLength":36,"uri":")" + data_uri(b) + R"("}]})";
    const GltfLoadResult r = load_text(json);
    REQUIRE(r.ok());
    REQUIRE(r.asset->instances.size() == 1);      // only the default scene (index 1) is instantiated
    CHECK(r.asset->instances[0].node == 1);
    // glTF x in [-1,1] shifted by +10 => engine y in [9, 11]; glTF y in [0,2] => engine z; glTF z in [-1,1] => engine x
    CHECK(near(r.asset->bounds.min.y, 9.0f));
    CHECK(near(r.asset->bounds.max.y, 11.0f));
    CHECK(near(r.asset->bounds.max.z, 2.0f));
    CHECK(near(r.asset->bounds.min.x, -1.0f));
}

// ================================================================================================= materials
TEST_CASE("glTF: material defaults follow the specification and values are parsed")
{
    Blob img;
    img.u8(0x89); img.u8('P'); img.u8('N'); img.u8('G');
    const std::string json =
        R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_materials_emissive_strength"],)"
        R"("materials":[{"name":"defaults"},)"
        R"({"name":"custom","pbrMetallicRoughness":{"baseColorFactor":[0.1,0.2,0.3,0.4],"metallicFactor":0.25,"roughnessFactor":0.75,)"
        R"("baseColorTexture":{"index":0}},"normalTexture":{"index":0,"scale":2.0},"occlusionTexture":{"index":0,"strength":0.5},)"
        R"("emissiveFactor":[0.5,0.25,1.0],"extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":4.0}},)"
        R"("alphaMode":"MASK","alphaCutoff":0.3,"doubleSided":true}],)"
        R"("textures":[{"source":0,"sampler":0}],"samplers":[{"magFilter":9728,"wrapS":33071}],)"
        R"("images":[{"uri":")" + data_uri(img, "image/png") + R"("}]})";
    const GltfLoadResult r = load_text(json);
    REQUIRE(r.ok());
    const GltfAsset& a = *r.asset;
    REQUIRE(a.materials.size() == 2);
    const Material& d = a.materials[0];
    CHECK(d.base_color[0] == 1.0f);
    CHECK(d.base_color[3] == 1.0f);
    CHECK(d.metallic == 1.0f);   // glTF's default metallicFactor is 1
    CHECK(d.roughness == 1.0f);  // and roughnessFactor is 1
    CHECK(d.alpha_mode == AlphaMode::Opaque);
    CHECK(d.alpha_cutoff == 0.5f);
    CHECK_FALSE(d.double_sided);
    CHECK_FALSE(d.base_color_texture.valid());

    const Material& c = a.materials[1];
    CHECK(near(c.base_color[2], 0.3f));
    CHECK(near(c.metallic, 0.25f));
    CHECK(near(c.roughness, 0.75f));
    CHECK(c.base_color_texture.texture == 0);
    CHECK(c.normal_texture.valid());
    CHECK(near(c.normal_scale, 2.0f));
    CHECK(near(c.occlusion_strength, 0.5f));
    // emissive = factor * strength, not clamped again after scaling (HDR emissive is the point of the extension)
    CHECK(near(c.emissive[0], 2.0f));
    CHECK(near(c.emissive[2], 4.0f));
    CHECK(c.alpha_mode == AlphaMode::Mask);
    CHECK(near(c.alpha_cutoff, 0.3f));
    CHECK(c.double_sided);

    REQUIRE(a.images.size() == 1);
    CHECK(a.images[0].mime_type == "image/png");
    CHECK(a.images[0].bytes.size() == 4);          // kept encoded
    REQUIRE(a.samplers.size() == 1);
    CHECK(a.samplers[0].mag_filter == 9728);
    CHECK(a.samplers[0].wrap_s == 33071);
    CHECK(a.samplers[0].wrap_t == 10497);          // default
    CHECK(a.textures[0].sampler == 0);
}

TEST_CASE("glTF: a non-zero texCoord is kept but warned about")
{
    Blob img;
    img.u8(1);
    const std::string json =
        R"({"asset":{"version":"2.0"},"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":1}}}],)"
        R"("textures":[{"source":0}],"images":[{"uri":")" + data_uri(img, "image/png") + R"("}]})";
    const GltfLoadResult r = load_text(json);
    REQUIRE(r.ok());
    CHECK(r.asset->materials[0].base_color_texture.tex_coord == 1);
    bool warned = false;
    for (const std::string& w : r.asset->warnings) warned |= w.find("texCoord 1") != std::string::npos;
    CHECK(warned);
}

// ================================================================================================= extensions
TEST_CASE("glTF: required extensions must be supported; optional unsupported ones only warn")
{
    GltfLoadResult r = load_text(triangle_doc(R"(,"extensionsRequired":["KHR_draco_mesh_compression"])"));
    CHECK(fails_with(r, "required extension 'KHR_draco_mesh_compression' is not supported"));

    r = load_text(triangle_doc(R"(,"extensionsRequired":["KHR_materials_emissive_strength"])"));
    CHECK(r.ok());

    r = load_text(triangle_doc(R"(,"extensionsUsed":["KHR_texture_transform","KHR_lights_punctual"])"));
    REQUIRE(r.ok());
    int warnings = 0;
    for (const std::string& w : r.asset->warnings) warnings += w.find("is not supported and was ignored") != std::string::npos ? 1 : 0;
    CHECK(warnings == 2);
}

TEST_CASE("glTF: things that are ignored produce one deduplicated warning each")
{
    // Two nodes with cameras, a skin and weights; two meshes would each warn about TEXCOORD_1; warnings dedupe.
    Blob b;
    b.vec3(0, 0, 0); b.vec3(1, 0, 0); b.vec3(0, 1, 0);
    b.f32(0); b.f32(0); b.f32(1); b.f32(0); b.f32(0); b.f32(1);
    const std::string json =
        R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0,"camera":0,"skin":0},{"mesh":0,"camera":0,"weights":[0.5]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_1":1,"JOINTS_0":1}}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],)"
        R"("buffers":[{"byteLength":60,"uri":")" + data_uri(b) + R"("}]})";
    const GltfLoadResult r = load_text(json);
    REQUIRE(r.ok());
    int camera = 0;
    for (const std::string& w : r.asset->warnings) camera += w.find("cameras") != std::string::npos ? 1 : 0;
    CHECK(camera == 1);   // two cameras, one warning
    CHECK(r.asset->warnings.size() == 5);  // cameras, skins, morph weights, TEXCOORD_1, JOINTS_0
    CHECK(r.asset->instances.size() == 2);
}

TEST_CASE("glTF: non-triangle primitives are skipped with a warning, not an error")
{
    std::string doc = triangle_doc();
    const std::string key = R"({"attributes":{"POSITION":0}})";
    const std::size_t at = doc.find(key);
    REQUIRE(at != std::string::npos);
    doc.replace(at, key.size(), R"({"attributes":{"POSITION":0},"mode":1})");
    const GltfLoadResult r = load_text(doc);
    REQUIRE(r.ok());
    CHECK(r.asset->meshes[0].primitives.empty());
    REQUIRE_FALSE(r.asset->warnings.empty());
    CHECK(r.asset->warnings[0].find("only TRIANGLES") != std::string::npos);
}

// ================================================================================================= containers
TEST_CASE("GLB: the BIN chunk backs buffer 0 and gives the same result as a .gltf")
{
    Blob bin;
    bin.vec3(0, 0, 0); bin.vec3(1, 0, 0); bin.vec3(0, 1, 0);
    const std::string json =
        R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36}]})";
    const std::vector<uint8_t> glb = make_glb(json, &bin);
    const GltfLoadResult r = load_gltf(glb, {});
    REQUIRE(r.ok());
    const Primitive& p = r.asset->meshes[0].primitives[0];
    CHECK(p.vertex_count() == 3);
    CHECK(near(&p.positions[3], {0, 1, 0}));
}

TEST_CASE("GLB: malformed containers are rejected with specific errors")
{
    Blob bin;
    bin.vec3(0, 0, 0); bin.vec3(1, 0, 0); bin.vec3(0, 1, 0);
    const std::string json =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}]})";
    const std::vector<uint8_t> good = make_glb(json, &bin);
    REQUIRE(load_gltf(good, {}).ok());

    auto patched = [&](std::size_t offset, uint32_t value) {
        std::vector<uint8_t> g = good;
        for (int i = 0; i < 4; ++i) g[offset + static_cast<std::size_t>(i)] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
        return g;
    };
    CHECK(fails_with(load_gltf(patched(4, 1), {}), "unsupported container version 1"));
    CHECK(fails_with(load_gltf(patched(8, static_cast<uint32_t>(good.size()) + 100), {}), "header says"));
    CHECK(fails_with(load_gltf(patched(8, 4), {}), "header says"));
    CHECK(fails_with(load_gltf(patched(12, 0x7FFFFFF0), {}), "extends past the end"));
    CHECK(fails_with(load_gltf(patched(16, 0x004E4942), {}), "first chunk must be JSON"));
    CHECK(fails_with(load_gltf(patched(12, static_cast<uint32_t>(good[12]) + 1), {}), "multiple of 4"));
    CHECK(fails_with(load_gltf(std::span<const uint8_t>(good.data(), 10), {}), "truncated header"));
    // A GLB without a BIN chunk cannot satisfy a uri-less buffer.
    CHECK(fails_with(load_gltf(make_glb(json, nullptr), {}), "no uri"));
}

TEST_CASE("glTF: external resources go through the reader with a normalized path, and only then")
{
    Blob bin;
    bin.vec3(0, 0, 0); bin.vec3(1, 0, 0); bin.vec3(0, 1, 0);
    auto doc = [](const std::string& uri) {
        return R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
               R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
               R"("bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36,"uri":")" + uri + R"("}]})";
    };
    std::vector<std::string> requested;
    const ResourceReader reader = [&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        requested.push_back(path);
        if (path == "meshes/tri.bin") return bin.bytes;
        return std::nullopt;
    };
    GltfLoadResult r = load_text(doc("./meshes/./tri.bin"), reader);
    REQUIRE(r.ok());
    REQUIRE(requested.size() == 1);
    CHECK(requested[0] == "meshes/tri.bin");   // normalized before the reader ever sees it

    // Policy violations never reach the reader.
    requested.clear();
    for (const char* bad : {"../tri.bin", "/etc/passwd", "C:/x.bin", "a/../../b.bin", "file:///x", "a%2F..%2F..%2Fb.bin", "..\\\\x.bin"}) {
        r = load_text(doc(bad), reader);
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("rejected") != std::string::npos);
    }
    CHECK(requested.empty());

    CHECK(fails_with(load_text(doc("missing.bin"), reader), "cannot read resource 'missing.bin'"));
    CHECK(fails_with(load_text(doc("tri.bin"), {}), "no resource reader"));

    // A reader that returns too little data is caught by the declared byteLength.
    const ResourceReader short_reader = [](const std::string&) -> std::optional<std::vector<uint8_t>> {
        return std::vector<uint8_t>(10, 0);
    };
    CHECK(fails_with(load_text(doc("tri.bin"), short_reader), "byteLength says 36"));
}

// ================================================================================================= validation
TEST_CASE("glTF: structural errors are reported precisely")
{
    auto mutate = [](const std::string& from, const std::string& to) {
        std::string d = triangle_doc();
        const std::size_t at = d.find(from);
        REQUIRE(at != std::string::npos);
        d.replace(at, from.size(), to);
        return d;
    };
    CHECK(fails_with(load_text("{"), "invalid JSON"));
    CHECK(fails_with(load_text("[]"), "root must be a JSON object"));
    CHECK(fails_with(load_text("{}"), "missing required 'asset'"));
    CHECK(fails_with(load_text(R"({"asset":{}})"), "asset.version is missing"));
    CHECK(fails_with(load_text(R"({"asset":{"version":"1.0"}})"), "unsupported glTF version '1.0'"));
    CHECK(fails_with(load_text(R"({"asset":{"version":"2.0","minVersion":"2.1"}})"), "minVersion"));
    CHECK(load_text(R"({"asset":{"version":"2.0"}})").ok());   // an empty asset is valid

    CHECK(fails_with(load_text(mutate(R"("type":"VEC3")", R"("type":"VEC7")")), "invalid type"));
    CHECK(fails_with(load_text(mutate(R"("componentType":5126)", R"("componentType":5124)")), "invalid componentType"));
    CHECK(fails_with(load_text(mutate(R"("count":3,"type")", R"("count":0,"type")")), "count must be a positive integer"));
    CHECK(fails_with(load_text(mutate(R"("count":3,"type")", R"("count":4,"type")")), "reads past the end of its bufferView"));
    CHECK(fails_with(load_text(mutate(R"("byteLength":36}],"buffers")", R"("byteLength":72}],"buffers")")), "extends past the end of its buffer"));
    CHECK(fails_with(load_text(mutate(R"("bufferView":0,"componentType")", R"("bufferView":5,"componentType")")), "invalid bufferView index"));
    CHECK(fails_with(load_text(mutate(R"("bufferView":0,"componentType":5126,"count":3,"type":"VEC3")",
                                      R"("bufferView":0,"componentType":5126,"count":3,"type":"VEC3","sparse":{})")),
                     "sparse accessors are not supported"));
    CHECK(fails_with(load_text(mutate(R"("bufferView":0,"componentType":5126,"count":3,"type":"VEC3")",
                                      R"("componentType":5126,"count":3,"type":"VEC3")")),
                     "without a bufferView"));
    CHECK(fails_with(load_text(mutate(R"("bufferViews":[{"buffer":0,"byteLength":36}])", R"("bufferViews":[{"buffer":0,"byteLength":36,"byteStride":6}])")),
                     "byteStride must be a multiple of 4"));
    CHECK(fails_with(load_text(mutate(R"("bufferViews":[{"buffer":0,"byteLength":36}])", R"("bufferViews":[{"buffer":0,"byteLength":36,"byteStride":8}])")),
                     "smaller than one element"));
    CHECK(fails_with(load_text(mutate(R"("componentType":5126)", R"("componentType":5123)")), "must be FLOAT"));  // POSITION not float
    CHECK(fails_with(load_text(mutate(R"({"mesh":0})", R"({"mesh":5})")), "invalid mesh index"));
}

TEST_CASE("glTF: node graph errors (cycles, shared children, bad indices, depth)")
{
    auto with_nodes = [](const std::string& nodes, const std::string& scenes) {
        std::string d = triangle_doc();
        const std::string old_nodes = R"("nodes":[{"mesh":0}])";
        d.replace(d.find(old_nodes), old_nodes.size(), nodes);
        const std::string old_scenes = R"("scenes":[{"nodes":[0]}])";
        d.replace(d.find(old_scenes), old_scenes.size(), scenes);
        return d;
    };
    CHECK(fails_with(load_text(with_nodes(R"("nodes":[{"children":[1]},{"children":[0]}])", R"("scenes":[{"nodes":[0]}])")),
                     "reachable more than once"));
    CHECK(fails_with(load_text(with_nodes(R"("nodes":[{"children":[0]}])", R"("scenes":[{"nodes":[0]}])")), "invalid child index"));
    CHECK(fails_with(load_text(with_nodes(R"("nodes":[{"children":[1,1]},{"mesh":0}])", R"("scenes":[{"nodes":[0]}])")),
                     "reachable more than once"));
    CHECK(fails_with(load_text(with_nodes(R"("nodes":[{"children":[1]},{"mesh":0}])", R"("scenes":[{"nodes":[0,1]}])")),
                     "reachable more than once"));  // node 1 is both a root and a child
    CHECK(fails_with(load_text(with_nodes(R"("nodes":[{"mesh":0}])", R"("scenes":[{"nodes":[7]}])")), "invalid node index"));
    CHECK(fails_with(load_text(triangle_doc(R"(,"scene":4)")), "invalid default scene index"));
    CHECK(fails_with(load_text(with_nodes(R"("nodes":[{"matrix":[1,2,3]}])", R"("scenes":[{"nodes":[0]}])")), "expected an array of 16 numbers"));
    CHECK(fails_with(load_text(with_nodes(R"("nodes":[{"translation":["a",0,0]}])", R"("scenes":[{"nodes":[0]}])")), "non-numeric"));

    // A chain deeper than the limit.
    GltfLimits limits;
    limits.max_node_depth = 8;
    std::string chain = R"("nodes":[)";
    for (int i = 0; i < 20; ++i) {
        chain += std::string(i != 0 ? "," : "") + R"({"children":[)" + (i + 1 < 20 ? std::to_string(i + 1) : "") + "]}";
    }
    chain += "]";
    CHECK(fails_with(load_text(with_nodes(chain, R"("scenes":[{"nodes":[0]}])"), {}, limits), "deeper than 8"));
}

TEST_CASE("glTF: primitive errors (indices, missing and mismatched attributes)")
{
    Blob b;
    b.vec3(0, 0, 0); b.vec3(1, 0, 0); b.vec3(0, 1, 0);
    b.u16(0); b.u16(1); b.u16(9);   // index 9 is out of range for 3 vertices
    auto doc = [&](const std::string& prim_extra, const std::string& index_accessor, const std::string& attrs = R"("POSITION":0)") {
        return R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],)"
               R"("meshes":[{"primitives":[{"attributes":{)" + attrs + "}" + prim_extra + R"(}]}],)"
               R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"})" + index_accessor + R"(],)"
               R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
               R"("buffers":[{"byteLength":42,"uri":")" + data_uri(b) + R"("}]})";
    };
    const std::string idx3 = R"(,{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"})";
    CHECK(fails_with(load_text(doc(R"(,"indices":1)", idx3)), "out of range for 3 vertices"));
    const std::string idx2 = R"(,{"bufferView":1,"componentType":5123,"count":2,"type":"SCALAR"})";
    CHECK(fails_with(load_text(doc(R"(,"indices":1)", idx2)), "not a multiple of 3"));
    const std::string idx_float = R"(,{"bufferView":1,"componentType":5126,"count":1,"type":"SCALAR"})";
    CHECK(fails_with(load_text(doc(R"(,"indices":1)", idx_float)), "indices must be UNSIGNED"));
    CHECK(fails_with(load_text(doc("", "", R"("NORMAL":0)")), "POSITION is required"));
    CHECK(fails_with(load_text(doc("", "", R"("POSITION":0,"NORMAL":3)")), "invalid accessor index"));
    CHECK(fails_with(load_text(doc(R"(,"material":0)", "")), "invalid material index"));
    // Mismatched counts between attributes.
    const std::string two_normals = R"(,{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3"})";
    Blob tiny;
    for (int i = 0; i < 12; ++i) tiny.f32(0.0f);
    (void)tiny;
    // (reuse accessor 1 declared as 1 element of VEC3 against POSITION's 3)
    CHECK(fails_with(load_text(doc("", two_normals, R"("POSITION":0,"NORMAL":1)")), "has 1 elements but POSITION has 3"));
    // A non-indexed primitive with a vertex count that is not a multiple of 3.
    std::string two_vertices = doc("", "");
    const std::size_t at = two_vertices.find(R"("count":3,"type":"VEC3")");
    two_vertices.replace(at, std::string(R"("count":3,"type":"VEC3")").size(), R"("count":2,"type":"VEC3")");
    CHECK(fails_with(load_text(two_vertices), "multiple of 3"));
}

TEST_CASE("glTF: non-finite floats in buffer data are rejected")
{
    Blob b;
    b.vec3(0, 0, 0);
    b.vec3(1, 0, 0);
    b.f32(std::nanf("")); b.f32(1); b.f32(0);
    const std::string json =
        R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36,"uri":")" + data_uri(b) + R"("}]})";
    CHECK(fails_with(load_text(json), "non-finite"));
}

TEST_CASE("glTF: limits are enforced from declared sizes, before anything large is allocated")
{
    GltfLimits tiny;
    tiny.max_buffer_bytes = 16;
    CHECK(fails_with(load_text(triangle_doc(), {}, tiny), "over the limit"));
    tiny = {};
    tiny.max_accessor_count = 2;
    CHECK(fails_with(load_text(triangle_doc(), {}, tiny), "over the limit of 2"));
    tiny = {};
    tiny.max_vertices_per_primitive = 2;
    CHECK(fails_with(load_text(triangle_doc(), {}, tiny), "vertices is over the limit"));
    tiny = {};
    tiny.max_file_bytes = 20;
    CHECK(fails_with(load_text(triangle_doc(), {}, tiny), "over the limit"));
    tiny = {};
    tiny.max_instances = 0;
    CHECK(fails_with(load_text(triangle_doc(), {}, tiny), "more than 0 mesh instances"));
    // A hostile document that declares a gigantic accessor and a huge buffer must fail without allocating them.
    const std::string huge =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":2147483647,"uri":"data:;base64,AAAA"}]})";
    CHECK(fails_with(load_text(huge), "over the limit"));
    const std::string huge_count =
        R"({"asset":{"version":"2.0"},"accessors":[{"bufferView":0,"componentType":5126,"count":2147483647,"type":"VEC3"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":4}],"buffers":[{"byteLength":4,"uri":"data:;base64,AAAAAA=="}]})";
    CHECK(fails_with(load_text(huge_count), "over the limit"));
}

// ================================================================================================= hostile input
TEST_CASE("glTF: every truncation and every single-byte corruption of a GLB fails cleanly or loads (sanitizers watch)")
{
    Blob bin;
    bin.vec3(0, 0, 0); bin.vec3(1, 0, 0); bin.vec3(1, 1, 0); bin.vec3(0, 1, 0);
    for (int i = 0; i < 4; ++i) bin.vec3(0, 0, 1);
    for (const unsigned i : {0u, 1u, 2u, 0u, 2u, 3u}) bin.u16(static_cast<uint16_t>(i));
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"translation":[1,2,3]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},)"
        R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},{"buffer":0,"byteOffset":96,"byteLength":12}],)"
        R"("buffers":[{"byteLength":108}]})";
    const std::vector<uint8_t> glb = make_glb(json, &bin);
    REQUIRE(load_gltf(glb, {}).ok());

    std::size_t truncated_ok = 0;
    for (std::size_t n = 0; n < glb.size(); ++n) {
        const GltfLoadResult r = load_gltf(std::span<const uint8_t>(glb.data(), n), {});
        truncated_ok += r.ok() ? 1u : 0u;
        CHECK((r.ok() || !r.error.empty()));
    }
    CHECK(truncated_ok == 0);  // a proper prefix of a well-formed GLB is never complete

    std::size_t survived = 0;
    for (std::size_t i = 0; i < glb.size(); ++i) {
        for (const unsigned flip : {0x01u, 0x80u, 0xFFu}) {
            std::vector<uint8_t> corrupt = glb;
            corrupt[i] = static_cast<uint8_t>(corrupt[i] ^ static_cast<uint8_t>(flip));
            const GltfLoadResult r = load_gltf(corrupt, {});
            CHECK((r.ok() || !r.error.empty()));
            survived += r.ok() ? 1u : 0u;
        }
    }
    std::printf("  %zu truncations and %zu corruptions processed; %zu corrupted files still loaded (e.g. data bytes)\n",
                glb.size(), glb.size() * 3, survived);
}

TEST_CASE("glTF: truncated JSON text never loads")
{
    const std::string doc = triangle_doc();
    for (std::size_t n = 0; n + 1 < doc.size(); n += 7) {
        CHECK_FALSE(load_gltf(bytes_of(doc.substr(0, n)), {}).ok());
    }
}

// ================================================================================================= files
TEST_CASE("glTF files: the obelisk fixture loads, in engine space, with correct winding and normals")
{
    const GltfLoadResult r = load_gltf_file(fs::path(SAIGE_GLTF_FIXTURE_DIR) / "obelisk.gltf");
    REQUIRE(r.ok());
    const GltfAsset& a = *r.asset;
    CHECK(a.warnings.empty());
    REQUIRE(a.meshes.size() == 1);
    CHECK(a.meshes[0].name == "ObeliskMesh");
    REQUIRE(a.meshes[0].primitives.size() == 1);
    const Primitive& p = a.meshes[0].primitives[0];
    CHECK(p.vertex_count() == 32);
    CHECK(p.indices.size() == 42);
    CHECK_FALSE(p.normals_generated);
    CHECK(p.uv0.size() == 64);

    // The fixture is 0.5 m square, 2.5 m tall, standing on glTF y = 0. In engine space: x and y in [-0.25, 0.25], z in [0, 2.5].
    CHECK(near(p.bounds.min.x, -0.25f));
    CHECK(near(p.bounds.max.x, 0.25f));
    CHECK(near(p.bounds.min.y, -0.25f));
    CHECK(near(p.bounds.max.y, 0.25f));
    CHECK(near(p.bounds.min.z, 0.0f));
    CHECK(near(p.bounds.max.z, 2.5f));

    // Every triangle's geometric normal (from its counter-clockwise winding) must agree with the stored vertex normals,
    // after conversion. That proves winding and normals were converted consistently, on a real mesh.
    for (std::size_t t = 0; t < p.indices.size() / 3; ++t) {
        Float3 v[3];
        for (std::size_t k = 0; k < 3; ++k) {
            const std::size_t i = p.indices[t * 3 + k];
            v[k] = {p.positions[i * 3], p.positions[i * 3 + 1], p.positions[i * 3 + 2]};
        }
        const Float3 geometric = saige::render::normalize(saige::render::cross(v[1] - v[0], v[2] - v[0]));
        const std::size_t i0 = p.indices[t * 3];
        const Float3 stored{p.normals[i0 * 3], p.normals[i0 * 3 + 1], p.normals[i0 * 3 + 2]};
        CHECK(saige::render::dot(geometric, stored) > 0.999f);
        CHECK(near(saige::render::length(stored), 1.0f, 1e-4f));
    }
    // Outward: every side-face normal points away from the pillar's axis (the engine z axis).
    uint32_t outward_sides = 0;
    for (std::size_t i = 0; i < 16; ++i) {  // the first 16 vertices are the four pillar sides
        const Float3 n{p.normals[i * 3], p.normals[i * 3 + 1], p.normals[i * 3 + 2]};
        const Float3 pos{p.positions[i * 3], p.positions[i * 3 + 1], 0.0f};
        outward_sides += saige::render::dot(n, pos) > 0.0f ? 1u : 0u;
    }
    CHECK(outward_sides == 16);

    REQUIRE(a.materials.size() == 1);
    CHECK(a.materials[0].name == "Stone");
    CHECK(near(a.materials[0].roughness, 0.8f));
    CHECK(near(a.materials[0].metallic, 0.0f));
    CHECK(a.instances.size() == 1);
    CHECK(near(a.bounds.max.z, 2.5f));
}

TEST_CASE("glTF files: external resources are confined to the asset's directory")
{
    const fs::path dir = fs::temp_directory_path() / "saige_gltf_files";
    fs::remove_all(dir);
    fs::create_directories(dir / "assets" / "sub");
    fs::create_directories(dir / "outside");

    Blob bin;
    bin.vec3(0, 0, 0); bin.vec3(1, 0, 0); bin.vec3(0, 1, 0);
    auto write_bytes = [](const fs::path& p, const std::vector<uint8_t>& bytes) {
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    write_bytes(dir / "assets" / "sub" / "tri.bin", bin.bytes);
    write_bytes(dir / "outside" / "secret.bin", bin.bytes);
    auto doc = [](const std::string& uri) {
        return R"({"asset":{"version":"2.0"},"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
               R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
               R"("bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36,"uri":")" + uri + R"("}]})";
    };
    auto write_doc = [&](const std::string& name, const std::string& uri) {
        std::ofstream(dir / "assets" / name) << doc(uri);
        return dir / "assets" / name;
    };

    CHECK(load_gltf_file(write_doc("ok.gltf", "sub/tri.bin")).ok());
    CHECK(fails_with(load_gltf_file(write_doc("up.gltf", "../outside/secret.bin")), "rejected"));
    CHECK(fails_with(load_gltf_file(write_doc("abs.gltf", (dir / "outside" / "secret.bin").generic_string())), "rejected"));
    CHECK(fails_with(load_gltf_file(write_doc("missing.gltf", "sub/none.bin")), "cannot read resource"));
    CHECK(fails_with(load_gltf_file(dir / "assets" / "does_not_exist.gltf"), "cannot open"));

    // A symlink inside the directory that points outside must not be followed.
    std::error_code ec;
    fs::create_symlink(dir / "outside" / "secret.bin", dir / "assets" / "link.bin", ec);
    if (ec) {
        std::printf("  (symlink creation not permitted here: %s; skipping the symlink escape check)\n", ec.message().c_str());
    } else {
        CHECK(fails_with(load_gltf_file(write_doc("link.gltf", "link.bin")), "cannot read resource"));
    }
    fs::remove_all(dir);
}

// ================================================================================================= tangents
namespace {

/// A unit quad (glTF space: facing +Z) with UVs, optionally TANGENT, and a material that does or does not use a normal texture.
std::string quad_doc(bool uvs, bool tangent, bool normal_texture)
{
    Blob b;
    const float pos[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    for (float v : pos) b.f32(v);                                         // POSITION at 0 (48 bytes)
    for (int i = 0; i < 4; ++i) b.vec3(0, 0, 1);                          // NORMAL at 48
    const float uv[8] = {0, 1, 1, 1, 1, 0, 0, 0};                         // top-left origin: +Y world is up the image
    for (float v : uv) b.f32(v);                                          // TEXCOORD_0 at 96 (32 bytes)
    for (int i = 0; i < 4; ++i) { b.f32(0); b.f32(1); b.f32(0); b.f32(1); }  // TANGENT at 128: +Y, w = +1 (deliberately unlike the generated one)
    b.u16(0); b.u16(1); b.u16(2); b.u16(0); b.u16(2); b.u16(3);          // indices at 192 (12 bytes)
    Blob img;
    img.u8(1);
    std::string attributes = R"("POSITION":0,"NORMAL":1)";
    if (uvs) attributes += R"(,"TEXCOORD_0":2)";
    if (tangent) attributes += R"(,"TANGENT":3)";
    const std::string material = normal_texture ? R"("materials":[{"normalTexture":{"index":0}}],)" : R"("materials":[{}],)";
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
           R"("meshes":[{"primitives":[{"attributes":{)" + attributes + R"(},"indices":4,"material":0}]}],)" + material +
           R"("textures":[{"source":0}],"images":[{"uri":")" + data_uri(img, "image/png") + R"("}],)"
           R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)"
           R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},)"
           R"({"bufferView":2,"componentType":5126,"count":4,"type":"VEC2"},)"
           R"({"bufferView":3,"componentType":5126,"count":4,"type":"VEC4"},)"
           R"({"bufferView":4,"componentType":5123,"count":6,"type":"SCALAR"}],)"
           R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},)"
           R"({"buffer":0,"byteOffset":96,"byteLength":32},{"buffer":0,"byteOffset":128,"byteLength":64},)"
           R"({"buffer":0,"byteOffset":192,"byteLength":12}],)"
           R"("buffers":[{"byteLength":204,"uri":")" + data_uri(b) + R"("}]})";
}

bool has_warning(const GltfAsset& a, const std::string& fragment)
{
    for (const std::string& w : a.warnings) {
        if (w.find(fragment) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("glTF tangents: a normal-mapped primitive without TANGENT gets generated tangents, reported once")
{
    const GltfLoadResult r = load_text(quad_doc(true, false, true));
    REQUIRE(r.ok());
    const Primitive& p = r.asset->meshes[0].primitives[0];
    REQUIRE(p.tangents.size() == 16);
    // glTF +X is engine +Y (front/left axes are converted once at import); the tangent follows U, which runs along glTF +X.
    for (uint32_t v = 0; v < 4; ++v) {
        const float len = std::sqrt(p.tangents[v * 4] * p.tangents[v * 4] + p.tangents[v * 4 + 1] * p.tangents[v * 4 + 1] +
                                    p.tangents[v * 4 + 2] * p.tangents[v * 4 + 2]);
        CHECK(near(len, 1.0f));
        CHECK(near(p.tangents[v * 4 + 1], 1.0f));  // glTF +X (the U direction) is engine +Y
        CHECK(near(p.tangents[v * 4 + 3], 1.0f));  // handedness
    }
    CHECK(has_warning(*r.asset, "tangents were generated"));
}

TEST_CASE("glTF tangents: the file's own TANGENT is kept untouched, and unused tangents are never generated")
{
    const GltfLoadResult given = load_text(quad_doc(true, true, true));
    REQUIRE(given.ok());
    const Primitive& p = given.asset->meshes[0].primitives[0];
    REQUIRE(p.tangents.size() == 16);
    CHECK(near(p.tangents[2], 1.0f));  // the file's glTF +Y tangent, converted to engine +Z: not the generated engine +Y one
    CHECK(near(p.tangents[1], 0.0f));
    CHECK_FALSE(has_warning(*given.asset, "tangents were generated"));

    const GltfLoadResult no_normal_map = load_text(quad_doc(true, false, false));
    REQUIRE(no_normal_map.ok());
    CHECK(no_normal_map.asset->meshes[0].primitives[0].tangents.empty());  // nothing would use them
    CHECK_FALSE(has_warning(*no_normal_map.asset, "tangents were generated"));
}

TEST_CASE("glTF tangents: a normal texture without UVs is warned about and generates nothing")
{
    const GltfLoadResult r = load_text(quad_doc(false, false, true));
    REQUIRE(r.ok());
    CHECK(r.asset->meshes[0].primitives[0].tangents.empty());
    CHECK(has_warning(*r.asset, "no TEXCOORD_0"));
}
