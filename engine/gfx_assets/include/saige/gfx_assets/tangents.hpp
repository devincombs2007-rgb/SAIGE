// Tangent generation for normal mapping.
//
// A primitive that has a normal texture but no TANGENT attribute needs per-vertex tangents (xyz plus a handedness sign in
// w) so the renderer can build the tangent frame. This is a deterministic CPU step in the asset pipeline: the shader never
// derives tangents. glTF's convention is followed: the tangent points along increasing U, the bitangent is
// cross(normal, tangent) * w and points toward increasing *up-in-the-image* V, and the stored UVs have their origin at the
// top left (like Vulkan), so the bitangent is measured against -V.
//
// Algorithm: per-triangle tangent and bitangent from position and UV edges, accumulated onto each corner weighted by the
// corner's angle, then Gram-Schmidt against the vertex normal; w is the sign of dot(cross(normal, tangent), bitangent). This is
// the widely used Lengyel-style method. It is NOT bit-identical to MikkTSpace, which the glTF specification names as the
// reference: results agree on smooth, consistently mapped surfaces and can differ at UV seams and on degenerate mappings. Adopting
// MikkTSpace itself means pinning a third-party dependency (zlib licensed) and is a separate decision.
#pragma once

#include "saige/gfx_assets/gltf.hpp"

#include <cstdint>
#include <string>

namespace saige::gfx {

enum class TangentResult : uint8_t {
    Generated,       ///< tangents were computed and stored in `Primitive::tangents`
    AlreadyPresent,  ///< the primitive has tangents: left untouched
    NoUvs,           ///< no TEXCOORD_0, so no tangent space exists; nothing was stored
    Invalid,         ///< inconsistent arrays (sizes, indices out of range): nothing was stored
};

struct TangentReport {
    TangentResult result = TangentResult::Invalid;
    uint32_t degenerate_triangles = 0;  ///< zero-area or zero-UV-area triangles, which contribute nothing
    uint32_t fallback_vertices = 0;     ///< vertices with no usable tangent, given an arbitrary frame perpendicular to the normal
    uint32_t split_vertices = 0;        ///< vertices added because triangles of mirrored UV orientation shared one (see below)
    uint32_t mixed_handedness_triangles = 0;  ///< triangles whose corners still disagree on handedness (normals contradicting the winding)
};

/// Fills `primitive.tangents` (xyzw per vertex) if it is empty and the primitive has UVs. An absent index buffer is a plain triangle list
/// over the vertices. Triangles are grouped by the orientation of their UV mapping: where triangles of opposite orientation (a mirrored
/// UV island) share a vertex, that vertex has no single tangent frame, so it is **split**: a copy (position, normal, uv and colors) is
/// appended for the second orientation and that orientation's triangles are remapped to it (indexed input only; `split_vertices` counts
/// them). That is the only case in which vertex arrays and indices change. The change is all or nothing: the full result is built first
/// and published with non-throwing moves, so an allocation failure leaves the primitive exactly as it was. `Invalid` and the other
/// non-results change nothing.
TangentReport generate_tangents(Primitive& primitive);

[[nodiscard]] std::string to_string(TangentResult result);

} // namespace saige::gfx
