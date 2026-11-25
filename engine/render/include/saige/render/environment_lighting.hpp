// SAIGE Render — image-based lighting data as the renderer consumes it: nine SH coefficients for the diffuse term and a prefiltered specular
// equirect texture with a roughness mip chain, already converted to RGBA16F. This is a plain data type with no dependency on the asset
// importer; saige::gfx::to_environment_lighting (gfx_assets/environment.hpp) builds it from a precomputed Environment. See
// docs/rendering/ibl-environment.md for what the numbers mean and the mapping from directions to texels.
#pragma once

#include "saige/render/math.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace saige::render {

struct EnvironmentLighting {
    std::array<Float3, 9> sh{};        ///< radiance SH coefficients, order 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22 (z up)
    uint32_t width = 0;                ///< level 0 extent of the equirect texture; level i is max(1, width >> i) by max(1, height >> i)
    uint32_t height = 0;
    uint32_t levels = 0;               ///< roughness levels: mip i is the environment prefiltered at roughness i / (levels - 1)
    std::vector<uint16_t> rgba16f;     ///< every level, largest first, RGBA half floats (the layout Device::upload_texture takes)
    float intensity = 1.0f;            ///< scales both the diffuse and the specular environment (an exposure of the sky only; the sun is separate)
};

/// Throws std::invalid_argument if the data cannot be uploaded and sampled as described: a zero or oversized extent, a level count outside
/// 1 .. the length of the full mip chain, a texel count that does not match the levels, a non-finite coefficient, or a negative or
/// non-finite intensity.
void validate_environment_lighting(const EnvironmentLighting& environment);

} // namespace saige::render
