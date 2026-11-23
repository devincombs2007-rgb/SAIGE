// Shared helpers for the imported-model tests: hand-built GpuModels (quads and cubes), transforms and color conversion.
#pragma once

#include "saige/render/gpu_resources.hpp"
#include "saige/render/math.hpp"
#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace saige::test {

using namespace saige::render;

inline float srgb_to_linear(uint8_t v)
{
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline bool near3(Float3 a, Float3 b, float tolerance)
{
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

// ------------------------------------------------------------------------------------------ model building
inline GpuPrimitive make_primitive(GpuResourceRegistry& registry, const std::vector<ModelVertex>& vertices,
                            const std::vector<uint32_t>& indices, uint32_t material)
{
    rhi::BufferDesc vb;
    vb.size = vertices.size() * sizeof(ModelVertex);
    vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    vb.debug_name = "test.vertices";
    rhi::BufferDesc ib;
    ib.size = indices.size() * sizeof(uint32_t);
    ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
    ib.debug_name = "test.indices";
    GpuPrimitive p;
    p.vertices = registry.create_buffer(vb, std::as_bytes(std::span(vertices)));
    p.indices = registry.create_buffer(ib, std::as_bytes(std::span(indices)));
    p.index_count = static_cast<uint32_t>(indices.size());
    p.index_type = rhi::IndexType::Uint32;
    p.material = material;
    return p;
}

/// A quad in the plane x = `x`, centered on (y, z) = (0, `zc`), half extent 1, facing -X. uv (0,0) is the corner at screen
/// top-left for a camera looking along +X (screen left is +Y).
/// With `tangents`, each vertex carries glTF's tangent for this layout: u increases toward -Y, so the tangent is -Y, and the
/// bitangent (up in the image, +Z) is cross(normal, tangent), so the handedness is +1.
inline GpuModel make_quad(GpuResourceRegistry& registry, GpuMaterial material, float x, float zc, float half = 1.0f,
                          bool tangents = false)
{
    const Float3 n{-1, 0, 0};
    std::vector<ModelVertex> v = {
        {{x, +half, zc + half}, n, {0, 0}},  // top-left
        {{x, -half, zc + half}, n, {1, 0}},  // top-right
        {{x, +half, zc - half}, n, {0, 1}},  // bottom-left
        {{x, -half, zc - half}, n, {1, 1}},  // bottom-right
    };
    if (tangents) {
        for (ModelVertex& vertex : v) {
            vertex.tangent[1] = -1.0f;
            vertex.tangent[3] = 1.0f;
        }
    }
    GpuModel m;
    m.name = "quad";
    m.materials.push_back(std::move(material));
    m.meshes.push_back({make_primitive(registry, v, {0, 2, 3, 0, 3, 1}, 0)});  // counter-clockwise seen from -X
    m.placements.push_back({0, Mat4::identity()});
    return m;
}

inline GpuModel make_cube(GpuResourceRegistry& registry, GpuMaterial material)
{
    const CubeMesh cube = make_unit_cube();
    std::vector<ModelVertex> v;
    for (const CubeVertex& cv : cube.vertices) {
        v.push_back({cv.position, cv.normal, {0, 0}});
    }
    std::vector<uint32_t> idx(cube.indices, cube.indices + 36);
    GpuModel m;
    m.name = "cube";
    m.materials.push_back(std::move(material));
    m.meshes.push_back({make_primitive(registry, v, idx, 0)});
    m.placements.push_back({0, Mat4::identity()});
    return m;
}

inline Mat4 translation(Float3 t)
{
    Mat4 m = Mat4::identity();
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}

inline Mat4 scale_matrix(Float3 s)
{
    Mat4 m = Mat4::identity();
    m.at(0, 0) = s.x;
    m.at(1, 1) = s.y;
    m.at(2, 2) = s.z;
    return m;
}

inline Mat4 rotation_z(float radians)
{
    Mat4 m = Mat4::identity();
    m.at(0, 0) = std::cos(radians);
    m.at(0, 1) = -std::sin(radians);
    m.at(1, 0) = std::sin(radians);
    m.at(1, 1) = std::cos(radians);
    return m;
}

} // namespace saige::test
