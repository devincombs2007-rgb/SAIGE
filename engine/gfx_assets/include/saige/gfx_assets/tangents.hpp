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
// reference: results agree on smooth, consistently mapped surfaces and can differ at UV seams and on degenerate mappings.
// `generate_tangents_mikktspace` (below, with SAIGE_GFX_MIKKTSPACE) runs the pinned reference MikkTSpace. The importer still uses
// this Lengyel-style generator by default; switching is a separate step after review (docs/rendering/mikktspace-evaluation.md).
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

/// Fills `primitive.tangents` (xyzw per vertex) if it is empty and the primitive has UVs. Never changes other arrays.
TangentReport generate_tangents(Primitive& primitive);

#if defined(SAIGE_HAS_MIKKTSPACE)
/// The same job with the reference MikkTSpace algorithm (zlib, pinned: docs/third-party/mikktspace.md), the one the glTF
/// specification names. MikkTSpace yields a tangent per triangle *corner*, so where the corners of one indexed vertex need
/// different tangents (mirrored or sheared UV islands that share vertices, or any seam the file did not split) the vertex is
/// duplicated (position, normal, uv and colors copied; counted in `split_vertices`) and the indices are remapped. Corner tangents
/// within 1 degree of each other with the same handedness share a vertex. A primitive without indices is treated as a plain
/// triangle list and comes out indexed. Fills `Primitive::tangents` only when it is empty and the primitive has UVs; never
/// changes bounds. Deterministic. Same result codes as `generate_tangents`; additionally Invalid for more than 50 million
/// triangles (MikkTSpace sizes are `int`).
///
/// All or nothing: the result is built in locals and every appended capacity is reserved before the primitive is first changed, so
/// an allocation failure leaves it exactly as it was (the guarantee is exercised by an allocation-failure sweep in the tests). Every
/// output tangent is finite, unit length and perpendicular to its vertex normal: a MikkTSpace result that is not (non-finite, zero,
/// parallel to the normal) is replaced by a deterministic perpendicular frame and counted in `fallback_vertices`, and a vertex no
/// triangle uses gets such a frame too.
TangentReport generate_tangents_mikktspace(Primitive& primitive);
#endif

[[nodiscard]] std::string to_string(TangentResult result);

} // namespace saige::gfx
