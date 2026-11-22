// SAIGE Render — the split-sum environment BRDF table.
//
// For a white uniform environment, a surface with specular reflectance f0 reflects f0 * scale + bias of the light at a view with
// n.v = x and perceptual roughness y. The table stores (scale, bias) on a regular grid, computed on the CPU by integrating the
// same GGX distribution, height-correlated Smith visibility and Schlick Fresnel weight the shaders use, and is uploaded once as
// a small RG16_FLOAT texture. It replaces an analytic fit, which does not match this BRDF's geometry term (up to 0.16 off).
// Prefiltered image-based lighting (M5) reuses it unchanged.
//
// The shipped table is a precomputed, versioned asset embedded in the library (src/generated/env_brdf_table.inc, written by
// `saige_envbrdf_gen generate`, checked by `saige_envbrdf_gen verify` in CTest). Integrating it at startup cost over 100 ms and
// limited accuracy; the embedded 64x64 table integrated with 512 steps has a worst interpolation error of about 0.005 (the former
// runtime 32x32 table: 0.02) and costs nothing to create. Changing the BRDF terms, the roughness clamp or the parametrization
// requires regenerating the table and bumping `kEnvBrdfTableVersion`; the verify test fails until that is done.
//
// Texel (i, j) holds the value at n.v = (i + 0.5) / size and roughness = (j + 0.5) / size; sampling with clamp-to-edge and
// linear filtering at uv = (n.v, roughness) interpolates between texel centers.
#pragma once

#include <cstddef>
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

/// Integrates a whole table (what the generator runs, and what tests use as a reference). `steps` per half-vector axis.
[[nodiscard]] EnvBrdfTable make_env_brdf_table(uint32_t size, int steps);

inline constexpr uint32_t kEnvBrdfTableVersion = 1;  ///< bump with any change to the integrand, clamp or parametrization
inline constexpr uint32_t kEnvBrdfTableSize = 64;
inline constexpr int kEnvBrdfTableSteps = 512;

/// The embedded asset: metadata and the texels (`size * size` pairs of binary16 (scale, bias), row = roughness).
struct EnvBrdfAsset {
    uint32_t version = 0;
    uint32_t size = 0;
    int steps = 0;
    uint64_t checksum = 0;  ///< FNV-1a (64-bit) over the texels' little-endian bytes: detects hand edits and corruption
    const uint16_t* texels = nullptr;
    std::size_t texel_count = 0;  ///< `size * size * 2` values

    [[nodiscard]] EnvBrdfValue at(uint32_t n_dot_v_index, uint32_t roughness_index) const noexcept;
};
[[nodiscard]] const EnvBrdfAsset& precomputed_env_brdf();
[[nodiscard]] uint64_t env_brdf_checksum(const uint16_t* texels, std::size_t count) noexcept;

} // namespace saige::render
