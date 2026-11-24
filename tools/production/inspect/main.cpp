// saige_asset_inspect: loads a glTF/GLB with the engine's own importer (gfx_assets) and prints a JSON verdict of whether SAIGE can use it as is: the importer's errors and warnings, geometry
// and material cost, texture dimensions and memory, skins and clips against the skinning limit, and the bounds in engine space (+X forward, +Y left, +Z up, meters). This is the runtime
// check of the asset pipeline: a file the importer rejects is not game-ready however good it looks.
//
//   saige_asset_inspect FILE.glb [--max-triangles N] [--max-texture-bytes N]
#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_assets/image.hpp"
#include "saige/render/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace saige;

static std::string esc(const std::string& s)
{
    std::string r;
    for (const char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
    }
    return r;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: saige_asset_inspect FILE [--max-triangles N] [--max-texture-bytes N]\n");
        return 2;
    }
    const std::string path = argv[1];
    std::uint64_t max_tris = 250000, max_tex_bytes = 256ull << 20;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--max-triangles") max_tris = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--max-texture-bytes") max_tex_bytes = std::strtoull(argv[i + 1], nullptr, 10);
    }
    const gfx::GltfLoadResult r = gfx::load_gltf_file(path);
    std::printf("{\n \"file\": \"%s\",\n \"importer\": \"saige gfx_assets\",\n", esc(path).c_str());
    if (!r.ok()) {
        std::printf(" \"loads\": false,\n \"error\": \"%s\",\n \"runtime_ready\": false,\n \"reasons\": [\"the engine importer rejects the file\"]\n}\n", esc(r.error).c_str());
        return 1;
    }
    const gfx::GltfAsset& a = *r.asset;
    std::uint64_t tris = 0, verts = 0;
    std::size_t prims = 0, no_uv = 0, no_tangent = 0, generated_normals = 0, skinned = 0;
    for (const gfx::Mesh& m : a.meshes) {
        for (const gfx::Primitive& p : m.primitives) {
            ++prims;
            tris += p.indices.size() / 3;
            verts += p.vertex_count();
            if (p.uv0.empty()) ++no_uv;
            if (p.tangents.empty()) ++no_tangent;
            if (p.normals_generated) ++generated_normals;
            if (!p.joints.empty()) ++skinned;
        }
    }
    // Texture dimensions and GPU memory (RGBA8 with a full mip chain), by decoding each image with the engine's decoder.
    std::uint64_t tex_bytes = 0;
    std::uint32_t max_dim = 0;
    std::size_t undecodable = 0;
    std::string tex_json;
    for (std::size_t i = 0; i < a.images.size(); ++i) {
        const gfx::ImageDecodeResult d = gfx::decode_image(a.images[i].bytes);
        if (!d.ok()) {
            ++undecodable;
            tex_json += std::string(i ? "," : "") + "{\"image\":" + std::to_string(i) + ",\"error\":\"" + esc(d.error) + "\"}";
            continue;
        }
        const std::uint64_t b = static_cast<std::uint64_t>(d.image->width) * d.image->height * 4 * 4 / 3;
        tex_bytes += b;
        max_dim = std::max({max_dim, d.image->width, d.image->height});
        tex_json += std::string(i ? "," : "") + "{\"image\":" + std::to_string(i) + ",\"width\":" + std::to_string(d.image->width) + ",\"height\":" + std::to_string(d.image->height) + ",\"alpha\":" + (d.image->has_alpha ? "true" : "false") + "}";
    }
    std::string reasons;
    std::string advisories;
    const auto add = [&](const std::string& s) { reasons += std::string(reasons.empty() ? "" : ",") + "\"" + esc(s) + "\""; };
    const auto advise = [&](const std::string& s) { advisories += std::string(advisories.empty() ? "" : ",") + "\"" + esc(s) + "\""; };
    if (tris > max_tris) add("triangle count " + std::to_string(tris) + " exceeds the budget " + std::to_string(max_tris) + ": simplify or build LODs");
    if (tex_bytes > max_tex_bytes) add("texture memory exceeds the budget: resize the textures");
    if (undecodable > 0) add(std::to_string(undecodable) + " image(s) cannot be decoded (only PNG and JPEG are supported)");
    for (const gfx::Skin& s : a.skins) {
        if (s.joint_count() > render::kMaxSkinJoints) add("a skin has more joints than the 256 the skinning palette holds");
    }
    if (a.skins.size() > 1) add("more than one skin: one staged model supports one skin, split the model");
    if (no_tangent > 0) {
        bool needs = false;
        for (const gfx::Material& m : a.materials) needs = needs || m.normal_texture.valid();
        if (needs && no_uv > 0) advise("a normal-mapped primitive has no UVs: its normal map is ignored (not blocking)");
    }
    gfx::Aabb b = a.bounds;
    std::printf(" \"loads\": true,\n \"meshes\": %zu, \"primitives\": %zu, \"triangles\": %llu, \"vertices\": %llu,\n \"materials\": %zu, \"images\": %zu, \"samplers\": %zu, \"instances\": %zu,\n", a.meshes.size(), prims,
                static_cast<unsigned long long>(tris), static_cast<unsigned long long>(verts), a.materials.size(), a.images.size(), a.samplers.size(), a.instances.size());
    std::printf(" \"textures\": [%s],\n \"texture_gpu_bytes\": %llu, \"max_texture_dimension\": %u,\n", tex_json.c_str(), static_cast<unsigned long long>(tex_bytes), max_dim);
    std::printf(" \"primitives_without_uv\": %zu, \"primitives_without_tangents\": %zu, \"primitives_with_generated_normals\": %zu, \"skinned_primitives\": %zu,\n", no_uv, no_tangent, generated_normals, skinned);
    std::printf(" \"skins\": [");
    for (std::size_t i = 0; i < a.skins.size(); ++i) std::printf("%s{\"name\":\"%s\",\"joints\":%zu}", i ? "," : "", esc(a.skins[i].name).c_str(), a.skins[i].joint_count());
    std::printf("],\n \"animations\": [");
    for (std::size_t i = 0; i < a.animations.size(); ++i) {
        std::printf("%s{\"name\":\"%s\",\"duration_s\":%.3f,\"channels\":%zu}", i ? "," : "", esc(a.animations[i].name).c_str(), static_cast<double>(a.animations[i].duration), a.animations[i].channels.size());
    }
    std::printf("],\n");
    if (b.valid()) {
        std::printf(" \"bounds_min\": [%.4f, %.4f, %.4f], \"bounds_max\": [%.4f, %.4f, %.4f], \"size_m\": [%.3f, %.3f, %.3f],\n", static_cast<double>(b.min.x), static_cast<double>(b.min.y), static_cast<double>(b.min.z),
                    static_cast<double>(b.max.x), static_cast<double>(b.max.y), static_cast<double>(b.max.z), static_cast<double>(b.max.x - b.min.x), static_cast<double>(b.max.y - b.min.y), static_cast<double>(b.max.z - b.min.z));
    }
    std::printf(" \"warnings\": [");
    for (std::size_t i = 0; i < a.warnings.size(); ++i) std::printf("%s\"%s\"", i ? "," : "", esc(a.warnings[i]).c_str());
    std::printf("],\n \"runtime_ready\": %s,\n \"reasons\": [%s],\n \"advisories\": [%s]\n}\n", reasons.empty() ? "true" : "false", reasons.c_str(), advisories.c_str());
    return reasons.empty() ? 0 : 1;
}
