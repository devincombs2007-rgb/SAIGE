#include "gpu_assets.hpp"

#include "saige/gfx_assets/tangents.hpp"
#include "saige/gfx_scene/stage_model.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace saige::horizon {

using render::Float3;
using render::GpuMaterial;
using render::Mat4;
using render::ModelVertex;

namespace {

constexpr float pi_f = 3.14159265358979f;

std::vector<std::uint8_t> read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct SurfaceStyle {
    const char* texture;  // Poly Haven folder under textures/, or null for a plain material
    float color[3];       // linear base color factor
    float metallic, roughness;
    float emissive[3];    // at emissive level 3 (scaled down for the lower levels)
    bool double_sided;
    float normal_scale;
};

constexpr SurfaceStyle surface_style[surface_count] = {
    /* Ground      */ {"leafy_grass", {0.62f, 0.66f, 0.50f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* Road        */ {"asphalt_04", {0.78f, 0.78f, 0.80f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* Shoulder    */ {"gravel_floor", {0.85f, 0.82f, 0.76f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* Sidewalk    */ {"concrete_pavement", {0.90f, 0.90f, 0.90f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* Marking     */ {nullptr, {0.62f, 0.62f, 0.58f}, 0.0f, 0.55f, {0, 0, 0}, false, 1.0f},
    /* WallBrick   */ {"brick_wall_001", {0.95f, 0.90f, 0.88f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* WallPlaster */ {"white_stucco", {0.95f, 0.92f, 0.86f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* RoofTiles   */ {"clay_roof_tiles_03", {0.95f, 0.85f, 0.80f}, 1.0f, 1.0f, {0, 0, 0}, true, 1.0f},
    /* RoofMetal   */ {"corrugated_iron", {0.80f, 0.82f, 0.85f}, 1.0f, 1.0f, {0, 0, 0}, true, 1.0f},
    /* Apron       */ {"concrete_floor", {0.88f, 0.88f, 0.88f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* WindowLit   */ {nullptr, {0.10f, 0.10f, 0.12f}, 0.0f, 0.15f, {0.30f, 0.185f, 0.075f}, true, 1.0f},
    /* WindowDark  */ {nullptr, {0.025f, 0.035f, 0.05f}, 0.6f, 0.08f, {0, 0, 0}, true, 1.0f},
    /* Door        */ {nullptr, {0.18f, 0.045f, 0.035f}, 0.0f, 0.5f, {0, 0, 0}, true, 1.0f},
    /* Bark        */ {"bark_brown_01", {0.85f, 0.80f, 0.75f}, 1.0f, 1.0f, {0, 0, 0}, false, 1.0f},
    /* Foliage     */ {"forest_leaves_02", {0.38f, 0.55f, 0.24f}, 1.0f, 1.0f, {0, 0, 0}, true, 0.6f},
    /* LampGlow    */ {nullptr, {0.10f, 0.10f, 0.10f}, 0.0f, 0.3f, {9.0f, 7.0f, 4.0f}, true, 1.0f},
};

} // namespace

MaterialLibrary::MaterialLibrary(render::SceneRenderer& renderer, const fs::path& root, std::vector<std::string>& warnings)
{
    gfx::GltfAsset lib;
    lib.samplers.push_back({});  // default: repeat
    std::map<std::string, int> texture_index;  // path -> glTF texture index (one image per file)
    const auto texture = [&](const fs::path& file) -> gfx::TextureRef {
        const std::string key = file.string();
        const auto it = texture_index.find(key);
        if (it != texture_index.end()) return {it->second, 0};
        gfx::Image image;
        image.name = file.filename().string();
        image.mime_type = "image/jpeg";
        image.bytes = read_file(file);
        lib.images.push_back(std::move(image));
        lib.textures.push_back({static_cast<int32_t>(lib.images.size() - 1), 0});
        const int t = static_cast<int>(lib.textures.size() - 1);
        texture_index[key] = t;
        return {t, 0};
    };

    const std::uint32_t total = ExtraCount;
    {
        for (std::uint32_t m = 0; m < total; ++m) {
            gfx::Material mat;
            if (m < surface_count) {
                const SurfaceStyle& st = surface_style[m];
                mat.name = "surface" + std::to_string(m);
                std::copy(st.color, st.color + 3, mat.base_color);
                mat.metallic = st.metallic;
                mat.roughness = st.roughness;
                mat.double_sided = st.double_sided;
                mat.normal_scale = st.normal_scale;
                for (int c = 0; c < 3; ++c) mat.emissive[c] = st.emissive[c];
                if (st.texture != nullptr) {
                    const std::string name = st.texture;
                    const fs::path dir = root / "textures" / name;
                    mat.base_color_texture = texture(dir / (name + "_diff_1k.jpg"));
                    mat.metallic_roughness_texture = texture(dir / (name + "_arm_1k.jpg"));
                    mat.occlusion_texture = mat.metallic_roughness_texture;
                    mat.normal_texture = texture(dir / (name + "_nor_gl_1k.jpg"));
                    mat.metallic = 0.0f;  // the ARM texture's blue channel is the metalness
                    mat.roughness = 1.0f;
                    mat.metallic = 1.0f;
                }
            } else if (m >= ExtraDust0 && m <= ExtraDust5) {
                static const float alpha[6] = {0.022f, 0.045f, 0.07f, 0.10f, 0.14f, 0.19f};
                mat.name = "dust" + std::to_string(m - ExtraDust0);
                mat.base_color[0] = 0.66f; mat.base_color[1] = 0.60f; mat.base_color[2] = 0.52f; mat.base_color[3] = alpha[m - ExtraDust0];
                mat.metallic = 0.0f;
                mat.roughness = 1.0f;
                mat.alpha_mode = gfx::AlphaMode::Blend;
                mat.double_sided = false;
            } else if (m == ExtraConcrete) {
                mat.name = "concrete";
                const fs::path dir = root / "textures" / "concrete_floor";
                mat.base_color_texture = texture(dir / "concrete_floor_diff_1k.jpg");
                mat.metallic_roughness_texture = texture(dir / "concrete_floor_arm_1k.jpg");
                mat.normal_texture = texture(dir / "concrete_floor_nor_gl_1k.jpg");
                mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = 0.85f;
            } else if (m == ExtraRubber) {
                mat.name = "rubber";
                mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = 0.025f;
                mat.metallic = 0.0f;
                mat.roughness = 0.8f;
            } else {
                mat.name = "spark";
                mat.base_color[0] = mat.base_color[1] = mat.base_color[2] = 0.05f;
                mat.metallic = 0.0f;
                mat.roughness = 0.5f;
                mat.emissive[0] = 18.0f; mat.emissive[1] = 8.0f; mat.emissive[2] = 2.0f;
            }
            lib.materials.push_back(std::move(mat));
        }
    }
    // One tiny mesh that uses every material, so the staging creates them all.
    gfx::Mesh mesh;
    gfx::Primitive prim;
    prim.positions = {0, 0, 0, 0.001f, 0, 0, 0, 0.001f, 0};
    prim.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    prim.uv0 = {0, 0, 1, 0, 0, 1};
    prim.indices = {0, 1, 2};
    prim.bounds.expand(Float3{0, 0, 0});
    prim.bounds.expand(Float3{0.001f, 0.001f, 0});
    for (std::uint32_t m = 0; m < lib.materials.size(); ++m) {
        gfx::Primitive p = prim;
        p.material = static_cast<int32_t>(m);
        mesh.primitives.push_back(std::move(p));
    }
    mesh.bounds = prim.bounds;
    lib.meshes.push_back(std::move(mesh));
    lib.instances.push_back({0, 0, Mat4::identity(), "library"});
    lib.bounds = prim.bounds;

    gfx::StageReport report;
    gfx::StageSettings ss;
    ss.max_anisotropy = 4.0f;
    const render::GpuModel staged = gfx::stage_model(renderer, lib, "materials", ss, &report);
    for (const std::string& w : report.warnings) warnings.push_back("materials: " + w);
    std::printf("material library: %u textures (%.1f MiB), %zu materials\n", report.textures, static_cast<double>(report.texture_bytes) / 1048576.0, staged.materials.size());
    {
        materials_.assign(staged.materials.begin(), staged.materials.begin() + total);
    }
}

render::GpuModel upload_model(render::SceneRenderer& renderer, const std::string& name, const std::vector<MeshIn>& meshes, const std::vector<GpuMaterial>& materials)
{
    render::GpuModel model;
    model.name = name;
    model.materials = materials;
    std::vector<render::GpuPrimitive> prims;
    render::GpuResourceRegistry& registry = renderer.registry();
    for (const MeshIn& m : meshes) {
        if (m.vertices.empty() || m.indices.empty()) continue;
        render::GpuPrimitive p;
        rhi::BufferDesc vb;
        vb.size = m.vertices.size() * sizeof(ModelVertex);
        vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
        vb.debug_name = "horizon vertices";
        rhi::BufferDesc ib;
        ib.size = m.indices.size() * sizeof(std::uint32_t);
        ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
        ib.debug_name = "horizon indices";
        p.vertices = registry.create_buffer(vb, std::as_bytes(std::span(m.vertices)));
        p.indices = registry.create_buffer(ib, std::as_bytes(std::span(m.indices)));
        p.index_type = rhi::IndexType::Uint32;
        p.index_count = static_cast<std::uint32_t>(m.indices.size());
        p.material = m.material;
        Float3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const ModelVertex& v : m.vertices) {
            lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
            hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
        }
        p.bounds_center = (lo + hi) * 0.5f;
        float r = 0;
        for (const ModelVertex& v : m.vertices) {
            const Float3 d = v.position - p.bounds_center;
            r = std::max(r, std::sqrt(render::dot(d, d)));
        }
        p.bounds_radius = r;
        prims.push_back(std::move(p));
    }
    model.meshes.push_back(std::move(prims));
    model.placements.push_back({0, Mat4::identity()});
    return model;
}

std::vector<MeshIn> cell_meshes(const std::vector<MeshPart>& parts) { return cell_meshes(parts.data(), parts.size()); }

std::vector<MeshIn> cell_meshes(const MeshPart* parts, std::size_t count)
{
    std::vector<MeshIn> out;
    for (std::size_t k = 0; k < count; ++k) {
        const MeshPart& part = parts[k];
        if (part.vertices.empty()) continue;
        gfx::Primitive p;
        const std::size_t n = part.vertices.size();
        p.positions.reserve(n * 3);
        p.normals.reserve(n * 3);
        p.uv0.reserve(n * 2);
        for (const Vertex& v : part.vertices) {
            p.positions.insert(p.positions.end(), {v.px, v.py, v.pz});
            p.normals.insert(p.normals.end(), {v.nx, v.ny, v.nz});
            p.uv0.insert(p.uv0.end(), {v.u, v.v});
        }
        p.indices = part.indices;
        (void)gfx::generate_tangents(p);
        MeshIn m;
        m.material = static_cast<std::uint32_t>(part.surface);
        const std::size_t count_v = p.positions.size() / 3;
        m.vertices.resize(count_v);
        for (std::size_t i = 0; i < count_v; ++i) {
            ModelVertex& v = m.vertices[i];
            v.position = {p.positions[i * 3], p.positions[i * 3 + 1], p.positions[i * 3 + 2]};
            v.normal = {p.normals[i * 3], p.normals[i * 3 + 1], p.normals[i * 3 + 2]};
            v.uv[0] = p.uv0[i * 2];
            v.uv[1] = p.uv0[i * 2 + 1];
            if (p.tangents.size() == count_v * 4) {
                for (int c = 0; c < 4; ++c) v.tangent[c] = p.tangents[i * 4 + static_cast<std::size_t>(c)];
            }
        }
        m.indices = std::move(p.indices);
        out.push_back(std::move(m));
    }
    return out;
}

// ---- Procedural shapes -------------------------------------------------------------------------------------------------------------------------------------------

namespace {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ull + 0x1234567ull) {}
    float next()
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<float>((s >> 11) & 0xFFFFFF) / 16777216.0f;
    }
    float range(float a, float b) { return a + (b - a) * next(); }
};

// Builds a mesh from triangles, computing tangents from the UVs (via the engine's generator).
MeshIn finish(std::vector<Float3>& pos, std::vector<Float3>& nrm, std::vector<std::array<float, 2>>& uv, std::vector<std::uint32_t>& idx, std::uint32_t material)
{
    gfx::Primitive p;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        p.positions.insert(p.positions.end(), {pos[i].x, pos[i].y, pos[i].z});
        p.normals.insert(p.normals.end(), {nrm[i].x, nrm[i].y, nrm[i].z});
        p.uv0.insert(p.uv0.end(), {uv[i][0], uv[i][1]});
    }
    p.indices = idx;
    (void)gfx::generate_tangents(p);
    MeshIn m;
    m.material = material;
    const std::size_t n = p.positions.size() / 3;
    m.vertices.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        ModelVertex& v = m.vertices[i];
        v.position = {p.positions[i * 3], p.positions[i * 3 + 1], p.positions[i * 3 + 2]};
        v.normal = {p.normals[i * 3], p.normals[i * 3 + 1], p.normals[i * 3 + 2]};
        v.uv[0] = p.uv0[i * 2];
        v.uv[1] = p.uv0[i * 2 + 1];
        if (p.tangents.size() == n * 4) {
            for (int c = 0; c < 4; ++c) v.tangent[c] = p.tangents[i * 4 + static_cast<std::size_t>(c)];
        }
    }
    m.indices = std::move(p.indices);
    return m;
}

struct Soup {
    std::vector<Float3> pos, nrm;
    std::vector<std::array<float, 2>> uv;
    std::vector<std::uint32_t> idx;
    void tri(Float3 a, Float3 b, Float3 c, std::array<float, 2> ua, std::array<float, 2> ub, std::array<float, 2> uc, Float3 na, Float3 nb, Float3 nc)
    {
        const std::uint32_t base = static_cast<std::uint32_t>(pos.size());
        pos.insert(pos.end(), {a, b, c});
        nrm.insert(nrm.end(), {na, nb, nc});
        uv.insert(uv.end(), {ua, ub, uc});
        idx.insert(idx.end(), {base, base + 1, base + 2});
    }
};

Float3 cross3(Float3 a, Float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Float3 norm3(Float3 v)
{
    const float l = std::sqrt(render::dot(v, v));
    return l > 1e-12f ? v * (1.0f / l) : Float3{0, 0, 1};
}

// A (truncated) cone around +Z from z0 (radius r0) to z1 (radius r1), smooth normals; uv wraps around by `u_scale` and along by `v_scale` per meter.
void add_cone(Soup& s, float z0, float z1, float r0, float r1, int sides, float u_scale, float v_scale, float wobble, Rng& rng)
{
    std::vector<float> wob(static_cast<std::size_t>(sides));
    for (float& w : wob) w = 1.0f + rng.range(-wobble, wobble);
    const float slope = (r0 - r1) / (z1 - z0);
    for (int i = 0; i < sides; ++i) {
        const int j = (i + 1) % sides;
        const float a0 = 2.0f * pi_f * static_cast<float>(i) / static_cast<float>(sides), a1 = 2.0f * pi_f * static_cast<float>(i + 1) / static_cast<float>(sides);
        const Float3 p00{std::cos(a0) * r0 * wob[static_cast<std::size_t>(i)], std::sin(a0) * r0 * wob[static_cast<std::size_t>(i)], z0};
        const Float3 p10{std::cos(a1) * r0 * wob[static_cast<std::size_t>(j)], std::sin(a1) * r0 * wob[static_cast<std::size_t>(j)], z0};
        const Float3 p01{std::cos(a0) * r1 * wob[static_cast<std::size_t>(i)], std::sin(a0) * r1 * wob[static_cast<std::size_t>(i)], z1};
        const Float3 p11{std::cos(a1) * r1 * wob[static_cast<std::size_t>(j)], std::sin(a1) * r1 * wob[static_cast<std::size_t>(j)], z1};
        const Float3 n0 = norm3({std::cos(a0), std::sin(a0), slope}), n1 = norm3({std::cos(a1), std::sin(a1), slope});
        const float u0 = static_cast<float>(i) / static_cast<float>(sides) * u_scale, u1 = static_cast<float>(i + 1) / static_cast<float>(sides) * u_scale;
        const float v0 = z0 * v_scale, v1 = z1 * v_scale;
        s.tri(p00, p10, p11, {u0, v0}, {u1, v0}, {u1, v1}, n0, n1, n1);
        s.tri(p00, p11, p01, {u0, v0}, {u1, v1}, {u0, v1}, n0, n1, n0);
    }
}

// An icosphere of radius r at c, squashed by (sx, sy, sz), displaced by noise.
void add_blob(Soup& s, Float3 c, float r, Float3 squash, int subdiv, float noise, float uv_scale, Rng& rng)
{
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<Float3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (Float3& p : v) p = norm3(p);
    std::vector<std::array<int, 3>> f = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                         {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int level = 0; level < subdiv; ++level) {
        std::vector<std::array<int, 3>> nf;
        std::map<std::pair<int, int>, int> cache;
        const auto mid = [&](int a, int b) {
            const std::pair<int, int> key{std::min(a, b), std::max(a, b)};
            const auto it = cache.find(key);
            if (it != cache.end()) return it->second;
            v.push_back(norm3((v[static_cast<std::size_t>(a)] + v[static_cast<std::size_t>(b)]) * 0.5f));
            return cache[key] = static_cast<int>(v.size() - 1);
        };
        for (const auto& tri : f) {
            const int a = mid(tri[0], tri[1]), b = mid(tri[1], tri[2]), c2 = mid(tri[2], tri[0]);
            nf.push_back({tri[0], a, c2});
            nf.push_back({tri[1], b, a});
            nf.push_back({tri[2], c2, b});
            nf.push_back({a, b, c2});
        }
        f = std::move(nf);
    }
    std::vector<float> disp(v.size());
    for (float& d : disp) d = 1.0f + rng.range(-noise, noise);
    const auto place = [&](std::size_t i) { return Float3{c.x + v[i].x * r * squash.x * disp[i], c.y + v[i].y * r * squash.y * disp[i], c.z + v[i].z * r * squash.z * disp[i]}; };
    for (const auto& tri : f) {
        const std::size_t a = static_cast<std::size_t>(tri[0]), b = static_cast<std::size_t>(tri[1]), d = static_cast<std::size_t>(tri[2]);
        const Float3 pa = place(a), pb = place(b), pd = place(d);
        const Float3 n = norm3(cross3(pb - pa, pd - pa));
        // Smooth-ish shading: blend the face normal with the sphere normal.
        const auto nn = [&](std::size_t i) { return norm3(n * 0.35f + v[i] * 0.65f); };
        const auto uvf = [&](Float3 p) { return std::array<float, 2>{p.x * uv_scale + p.z * 0.37f * uv_scale, p.y * uv_scale + p.z * 0.61f * uv_scale}; };
        s.tri(pa, pb, pd, uvf(pa), uvf(pb), uvf(pd), nn(a), nn(b), nn(d));
    }
}

} // namespace

std::vector<MeshIn> tree_pine_meshes(unsigned seed)
{
    Rng rng(seed + 11);
    Soup trunk, leaves;
    const float h = rng.range(7.0f, 9.5f);
    add_cone(trunk, 0.0f, h * 0.9f, 0.34f, 0.07f, 8, 1.0f, 0.35f, 0.04f, rng);
    // Stacked skirts of needles.
    const int tiers = 5;
    for (int t = 0; t < tiers; ++t) {
        const float f = static_cast<float>(t) / static_cast<float>(tiers);
        const float z0 = 1.6f + f * (h - 2.6f);
        const float r = (1.0f - f) * 2.1f + 0.55f;
        add_cone(leaves, z0, z0 + (h - 2.6f) / static_cast<float>(tiers) * 2.1f + 0.5f, r, 0.04f, 9, 3.0f, 0.5f, 0.12f, rng);
    }
    std::vector<MeshIn> out;
    out.push_back(finish(trunk.pos, trunk.nrm, trunk.uv, trunk.idx, static_cast<std::uint32_t>(Surface::Bark)));
    out.push_back(finish(leaves.pos, leaves.nrm, leaves.uv, leaves.idx, static_cast<std::uint32_t>(Surface::Foliage)));
    return out;
}

std::vector<MeshIn> tree_round_meshes(unsigned seed)
{
    Rng rng(seed + 29);
    Soup trunk, leaves;
    const float h = rng.range(4.4f, 6.0f);
    add_cone(trunk, 0.0f, h * 0.72f, 0.30f, 0.12f, 8, 1.0f, 0.35f, 0.05f, rng);
    add_blob(leaves, {0, 0, h * 0.82f}, h * 0.36f, {1.0f, 1.0f, 0.82f}, 2, 0.16f, 0.22f, rng);
    for (int i = 0; i < 4; ++i) {
        const float a = rng.range(0, 2 * pi_f);
        const float d = h * 0.2f;
        add_blob(leaves, {std::cos(a) * d, std::sin(a) * d, h * (0.62f + rng.range(0.0f, 0.22f))}, h * rng.range(0.15f, 0.22f), {1, 1, 0.85f}, 2, 0.18f, 0.22f, rng);
    }
    std::vector<MeshIn> out;
    out.push_back(finish(trunk.pos, trunk.nrm, trunk.uv, trunk.idx, static_cast<std::uint32_t>(Surface::Bark)));
    out.push_back(finish(leaves.pos, leaves.nrm, leaves.uv, leaves.idx, static_cast<std::uint32_t>(Surface::Foliage)));
    return out;
}

std::vector<MeshIn> chunk_meshes(unsigned seed)
{
    Rng rng(seed + 5);
    Soup s;
    add_blob(s, {0, 0, 0}, 0.2f, {rng.range(0.8f, 1.3f), rng.range(0.7f, 1.2f), rng.range(0.5f, 0.9f)}, 1, 0.35f, 1.4f, rng);
    return {finish(s.pos, s.nrm, s.uv, s.idx, ExtraConcrete)};
}

std::vector<MeshIn> puff_meshes(std::uint32_t material)
{
    Rng rng(3);
    Soup s;
    add_blob(s, {0, 0, 0}, 1.0f, {1, 1, 1}, 3, 0.05f, 0.5f, rng);
    return {finish(s.pos, s.nrm, s.uv, s.idx, material)};
}

std::vector<MeshIn> box_meshes(float sx, float sy, float sz, std::uint32_t material)
{
    Soup s;
    const float x = sx * 0.5f, y = sy * 0.5f;
    const Float3 c[8] = {{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}, {-x, -y, sz}, {x, -y, sz}, {x, y, sz}, {-x, y, sz}};
    const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (const auto& f : faces) {
        const Float3 n = norm3(cross3(c[f[1]] - c[f[0]], c[f[2]] - c[f[0]]));
        s.tri(c[f[0]], c[f[1]], c[f[2]], {0, 0}, {1, 0}, {1, 1}, n, n, n);
        s.tri(c[f[0]], c[f[2]], c[f[3]], {0, 0}, {1, 1}, {0, 1}, n, n, n);
    }
    return {finish(s.pos, s.nrm, s.uv, s.idx, material)};
}

// ---- Staged glTF models -------------------------------------------------------------------------------------------------------------------------------------------

render::GpuModel load_prop(render::SceneRenderer& renderer, const fs::path& path, std::vector<std::string>& warnings)
{
    gfx::GltfLoadResult loaded = gfx::load_gltf_file(path);
    if (!loaded.ok()) throw std::runtime_error("cannot load " + path.string() + ": " + loaded.error);
    gfx::StageReport report;
    gfx::StageSettings ss;
    ss.max_anisotropy = 4.0f;
    render::GpuModel model = gfx::stage_model(renderer, *loaded.asset, path.stem().string(), ss, &report);
    for (const std::string& w : report.warnings) warnings.push_back(path.stem().string() + ": " + w);
    return model;
}

render::GpuModel filter_placements(const render::GpuModel& model, const std::vector<bool>& keep, const Mat4& pre)
{
    render::GpuModel out = model;
    out.placements.clear();
    for (std::size_t i = 0; i < model.placements.size() && i < keep.size(); ++i) {
        if (!keep[i]) continue;
        render::GpuPlacement p = model.placements[i];
        p.local = pre * p.local;
        out.placements.push_back(p);
    }
    return out;
}

bool placement_bounds(const gfx::GltfAsset& asset, const std::vector<bool>& keep, Float3& lo, Float3& hi)
{
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (std::size_t i = 0; i < asset.instances.size() && i < keep.size(); ++i) {
        if (!keep[i]) continue;
        const gfx::MeshInstance& inst = asset.instances[i];
        if (inst.mesh < 0) continue;
        const gfx::Mesh& mesh = asset.meshes[static_cast<std::size_t>(inst.mesh)];
        for (const gfx::Primitive& p : mesh.primitives) {
            const gfx::Aabb& b = p.bounds;
            if (!b.valid()) continue;
            for (int corner = 0; corner < 8; ++corner) {
                const Float3 q{(corner & 1) ? b.max.x : b.min.x, (corner & 2) ? b.max.y : b.min.y, (corner & 4) ? b.max.z : b.min.z};
                const Float3 w = render::transform_point(inst.world, q);
                lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
                hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
                any = true;
            }
        }
    }
    return any;
}

} // namespace saige::horizon
