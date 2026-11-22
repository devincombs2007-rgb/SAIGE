// SAIGE Render — the split-sum environment BRDF table.
//
// For a white uniform environment, a surface with specular reflectance f0 reflects f0 * scale + bias of the light at a view with
// n.v = x and perceptual roughness y. The table stores (scale, bias) on a regular grid, computed on the CPU by integrating the
// same GGX distribution, height-correlated Smith visibility and Schlick Fresnel weight the shaders use, and is uploaded once as
// a small RG16_FLOAT texture. It replaces an analytic fit, which does not match this BRDF's geometry term (up to 0.16 off).
// Prefiltered image-based lighting (M5) reuses it unchanged.
//
// Texel (i, j) holds the value at n.v = (i + 0.5) / size and roughness = (j + 0.5) / size; sampling with clamp-to-edge and
// linear filtering at uv = (n.v, roughness) interpolates between texel centers.
#pragma once

#include <cstdint>
#include <vector>

namespace saige::render {

struct EnvBrdfValue {
    float scale = 0.0f;
    float bias = 0.0f;
};

/// Numeric integral at one point (double precision internally; deterministic). The shaders' roughness clamp applies.
[[nodiscard]] EnvBrdfValue integrate_env_brdf(float n_dot_v, float roughness, int half_vector_steps = 48);

/// IEEE binary16 conversion with round-to-nearest-even (finite values; overflow saturates to infinity).
[[nodiscard]] uint16_t float_to_half(float value) noexcept;
[[nodiscard]] float half_to_float(uint16_t bits) noexcept;

struct EnvBrdfTable {
    uint32_t size = 0;
    std::vector<uint16_t> texels;  ///< size * size texels of (scale, bias) as binary16, row-major, row = roughness

    [[nodiscard]] EnvBrdfValue at(uint32_t n_dot_v_index, uint32_t roughness_index) const noexcept;
};

[[nodiscard]] EnvBrdfTable make_env_brdf_table(uint32_t size = 32);

/// The default 32x32 table, computed on first use and shared by every renderer in the process (the integration takes a few
/// hundred milliseconds unoptimized; the result is deterministic).
[[nodiscard]] const EnvBrdfTable& default_env_brdf_table();

} // namespace saige::render
