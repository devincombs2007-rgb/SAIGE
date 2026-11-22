// Compares SAIGE's tangent generator with reference MikkTSpace on synthetic meshes with seams, poles, mirrored and stretched UVs.
#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_assets/tangents.hpp"
extern "C" {
#include "mikktspace.h"
}
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using saige::gfx::Primitive;

struct Mesh { std::string name; Primitive p; };
static const float PI = 3.14159265358979f;

static Mesh sphere(const char* name, int lon, int lat, float u_scale, bool mirror_half) {
    Mesh m; m.name = name; Primitive& p = m.p;
    for (int j = 0; j <= lat; ++j) for (int i = 0; i <= lon; ++i) {   // seam: i == lon duplicates i == 0 with u = 1
        float th = PI * j / lat, ph = 2 * PI * i / lon;
        float x = std::sin(th) * std::cos(ph), y = std::sin(th) * std::sin(ph), z = std::cos(th);
        p.positions.insert(p.positions.end(), {x, y, z}); p.normals.insert(p.normals.end(), {x, y, z});
        float u = (float)i / lon * u_scale, v = (float)j / lat;
        if (mirror_half && j > lat / 2) u = u_scale - u;                     // the lower half is a mirror image in UV space
        p.uv0.insert(p.uv0.end(), {u, v});
    }
    for (int j = 0; j < lat; ++j) for (int i = 0; i < lon; ++i) {
        uint32_t a = j * (lon + 1) + i, b = a + 1, c = a + lon + 1, d = c + 1;
        if (j != 0) p.indices.insert(p.indices.end(), {a, c, b});          // poles: the degenerate triangle is dropped, as exporters do
        if (j != lat - 1) p.indices.insert(p.indices.end(), {b, c, d});
    }
    return m;
}
static Mesh cube() {
    Mesh m; m.name = "cube, per-face UVs (hard edges)"; Primitive& p = m.p;
    const float n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float tx = ny != 0 || nz != 0 ? (nz != 0 ? 1.f : 0.f) : 0.f, ty = nz == 0 && ny == 0 ? 1.f : 0.f, tz = ny != 0 ? 1.f : 0.f; (void)tx; (void)ty; (void)tz;
        float ax[3], bx[3];  // two tangents spanning the face
        if (nx != 0) { ax[0]=0;ax[1]=1;ax[2]=0; bx[0]=0;bx[1]=0;bx[2]=1; } else if (ny != 0) { ax[0]=1;ax[1]=0;ax[2]=0; bx[0]=0;bx[1]=0;bx[2]=1; } else { ax[0]=1;ax[1]=0;ax[2]=0; bx[0]=0;bx[1]=1;bx[2]=0; }
        uint32_t base = (uint32_t)p.positions.size() / 3;
        const int su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            for (int c = 0; c < 3; ++c) p.positions.push_back(n[f][c] + su[k] * ax[c] + sv[k] * bx[c]);
            p.normals.insert(p.normals.end(), {nx, ny, nz});
            p.uv0.insert(p.uv0.end(), {su[k] * 0.5f + 0.5f, sv[k] * 0.5f + 0.5f});
        }
        bool flip = (nx + ny + nz) < 0; (void)flip;
        // orient triangles so the geometric normal agrees with the face normal
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            const float* pa = &p.positions[a*3]; const float* pb = &p.positions[b*3]; const float* pc = &p.positions[c*3];
            float e1[3] = {pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2]}, e2[3] = {pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2]};
            float g[3] = {e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
            if (g[0]*nx + g[1]*ny + g[2]*nz < 0) std::swap(b, c);
            p.indices.insert(p.indices.end(), {a, b, c});
        };
        tri(base, base+1, base+2); tri(base, base+2, base+3);
    }
    return m;
}
static Mesh heightfield() {
    Mesh m; m.name = "curved heightfield, shared vertices, stretched UVs"; Primitive& p = m.p; const int n = 40;
    auto h = [](float x, float y) { return 0.35f * std::sin(3.1f * x) * std::cos(2.3f * y) + 0.1f * std::sin(9.f * x + y); };
    for (int j = 0; j <= n; ++j) for (int i = 0; i <= n; ++i) {
        float x = -1 + 2.f * i / n, y = -1 + 2.f * j / n, e = 1e-3f;
        float z = h(x, y), dzdx = (h(x + e, y) - h(x - e, y)) / (2 * e), dzdy = (h(x, y + e) - h(x, y - e)) / (2 * e);
        float nx = -dzdx, ny = -dzdy, nz = 1, l = std::sqrt(nx*nx + ny*ny + nz*nz);
        p.positions.insert(p.positions.end(), {x, y, z}); p.normals.insert(p.normals.end(), {nx/l, ny/l, nz/l});
        // shear and non-uniform scale: u runs 4x faster than v, rotated 25 degrees
        float u = (0.9f * x - 0.42f * y) * 4.f, v = (0.42f * x + 0.9f * y);
        p.uv0.insert(p.uv0.end(), {u, v});
    }
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
        uint32_t a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
        p.indices.insert(p.indices.end(), {a, b, c, b, d, c});
    }
    return m;
}

struct Ctx { const Primitive* p; std::vector<float> tangent; std::vector<float> sign; };
static int numFaces(const SMikkTSpaceContext* c) { return (int)((Ctx*)c->m_pUserData)->p->indices.size() / 3; }
static int nVerts(const SMikkTSpaceContext*, int) { return 3; }
static void pos(const SMikkTSpaceContext* c, float o[], int f, int v) { auto* x = (Ctx*)c->m_pUserData; uint32_t i = x->p->indices[f*3+v]; for (int k = 0; k < 3; ++k) o[k] = x->p->positions[i*3+k]; }
static void nor(const SMikkTSpaceContext* c, float o[], int f, int v) { auto* x = (Ctx*)c->m_pUserData; uint32_t i = x->p->indices[f*3+v]; for (int k = 0; k < 3; ++k) o[k] = x->p->normals[i*3+k]; }
static void uv(const SMikkTSpaceContext* c, float o[], int f, int v) { auto* x = (Ctx*)c->m_pUserData; uint32_t i = x->p->indices[f*3+v]; o[0] = x->p->uv0[i*2]; o[1] = 1.0f - x->p->uv0[i*2+1]; }  // glTF's v points down, MikkTSpace's up
static void setT(const SMikkTSpaceContext* c, const float t[], float s, int f, int v) { auto* x = (Ctx*)c->m_pUserData; for (int k = 0; k < 3; ++k) x->tangent[(f*3+v)*3+k] = t[k]; x->sign[f*3+v] = s; }

int main() {
    std::vector<Mesh> meshes;
    meshes.push_back(sphere("UV sphere 64x32: seam, poles", 64, 32, 1.f, false));
    meshes.push_back(sphere("UV sphere, u stretched 4x", 64, 32, 4.f, false));
    meshes.push_back(sphere("UV sphere, lower half mirrored in UV", 64, 32, 1.f, true));
    meshes.push_back(cube());
    meshes.push_back(heightfield());
    for (Mesh& m : meshes) {
        Primitive ours = m.p;
        auto r = saige::gfx::generate_tangents(ours);
        Ctx ctx{&m.p, std::vector<float>(m.p.indices.size() * 3), std::vector<float>(m.p.indices.size())};
        SMikkTSpaceInterface itf{}; itf.m_getNumFaces = numFaces; itf.m_getNumVerticesOfFace = nVerts; itf.m_getPosition = pos; itf.m_getNormal = nor; itf.m_getTexCoord = uv; itf.m_setTSpaceBasic = setT;
        SMikkTSpaceContext sc{&itf, &ctx}; int ok = genTangSpaceDefault(&sc);
        std::vector<double> angles; size_t signs_differ = 0, compared = 0, ours_missing = 0;
        for (size_t k = 0; k < m.p.indices.size(); ++k) {
            uint32_t i = m.p.indices[k];
            const float* a = &ours.tangents[i * 4]; const float* b = &ctx.tangent[k * 3];
            double la = std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]), lb = std::sqrt(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
            if (la < 1e-9 || lb < 1e-9) { ++ours_missing; continue; }
            double d = (a[0]*b[0]+a[1]*b[1]+a[2]*b[2]) / (la * lb);
            angles.push_back(std::acos(std::clamp(d, -1.0, 1.0)) * 180.0 / 3.14159265358979);
            if ((a[3] > 0) != (ctx.sign[k] > 0)) ++signs_differ;
            ++compared;
        }
        std::sort(angles.begin(), angles.end());
        auto pct = [&](double q) { return angles.empty() ? 0.0 : angles[std::min(angles.size() - 1, (size_t)(q * angles.size()))]; };
        size_t over1 = 0, over5 = 0; for (double a : angles) { over1 += a > 1; over5 += a > 5; }
        std::printf("%-52s corners %6zu  ours:%d mikk:%d | angle deg: median %.4f p99 %.3f max %.3f | >1deg %.2f%%  >5deg %.2f%% | handedness differs %zu\n",
                    m.name.c_str(), compared, (int)r.result, ok, pct(.5), pct(.99), angles.empty() ? 0.0 : angles.back(),
                    100.0 * over1 / std::max<size_t>(1, angles.size()), 100.0 * over5 / std::max<size_t>(1, angles.size()), signs_differ);
        if (ours_missing) std::printf("   (%zu corners skipped: a zero tangent in one result)\n", ours_missing);
    }
}
