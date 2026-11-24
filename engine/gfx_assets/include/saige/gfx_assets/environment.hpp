// SAIGE gfx_assets — image-based lighting data precomputed on the CPU from an HDR environment map (milestone M5).
//
// From one equirectangular HDR image this produces what a PBR shader needs, as a deterministic, versioned asset:
//  * diffuse: nine spherical-harmonics coefficients per channel of the environment (the cosine-convolved diffuse term is a nine-term dot
//    product in the shader). SH9 is exact for smooth skies and unreliable for a bright sun disc, so `diffuse_radiance_limit` can clamp the
//    radiance that goes into the SH (the sun is a directional light, not part of the ambient);
//  * specular: the GGX-prefiltered environment as ONE equirectangular 2D texture with a mip chain, where mip `i` holds the environment
//    prefiltered at perceptual roughness `i / (levels - 1)` (level 0 is the unfiltered mirror). A shader samples it with
//    lod = roughness * (levels - 1): ordinary trilinear filtering then interpolates between roughness levels, and no cube texture,
//    render-to-mip or compute support is needed. Prefiltering is split-sum (N = V = R), by filtered importance sampling of GGX with
//    Hammersley points, so it is deterministic.
//
// Convention (engine space, +Z up, +X forward, +Y left): a direction d maps to u = atan2(d.y, d.x) / (2 pi) + 0.5 and v = acos(d.z) / pi,
// with v = 0 at the top (+Z). u = 0.5 is the +X direction. Texel centers are at (x + 0.5) / width.
//
// "Diffuse radiance factor" is irradiance / pi, what a white Lambertian surface with that normal reflects: a uniform environment of radiance c
// gives c, the unit the renderer's ambient term already uses.
#pragma once

#include "saige/gfx_assets/hdr_image.hpp"
#include "saige/render/environment_lighting.hpp"
#include "saige/render/math.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace saige::gfx {

inline constexpr uint32_t kEnvironmentVersion = 1;

struct Sh9 {
    std::array<render::Float3, 9> c{};  ///< radiance coefficients L_lm, in the order 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22 (z up)
};

/// The diffuse radiance factor (irradiance / pi) for unit normal `n` from SH coefficients: the radiance coefficients scaled by the clamped
/// cosine's band weights 1, 2/3, 1/4 (Ramamoorthi and Hanrahan). May be slightly negative for extreme environments (SH ringing): clamp at the use.
[[nodiscard]] render::Float3 sh_diffuse(const Sh9& sh, render::Float3 n) noexcept;

struct EnvironmentSettings {
    uint32_t width = 256;                  ///< level 0 width; the height is width / 2. Even, 8 to 4096
    uint32_t levels = 5;                   ///< roughness levels 0, 1/(levels-1), ..., 1; 1 to 12 (one level is just the mirror). Keep the last level at least 16 texels wide: an equirect map
                                           ///< resolves the poles poorly below that (the nearest texel centers are 45 degrees from the pole)
    uint32_t samples = 128;                ///< GGX importance samples per texel for levels above 0; 1 to 4096
    float diffuse_radiance_limit = 0.0f;   ///< > 0 clamps each source pixel's largest channel before the SH projection (the sun); 0 leaves it
};

struct Environment {
    uint32_t version = kEnvironmentVersion;
    Sh9 diffuse;
    uint32_t width = 0;                    ///< level 0 extent; level i is max(1, width >> i) by max(1, height >> i)
    uint32_t height = 0;
    uint32_t levels = 0;
    std::vector<float> texels;             ///< all levels, largest first, row-major, RGB floats

    [[nodiscard]] uint32_t level_width(uint32_t level) const noexcept;
    [[nodiscard]] uint32_t level_height(uint32_t level) const noexcept;
    [[nodiscard]] std::size_t level_offset(uint32_t level) const noexcept;  ///< in floats
    /// The CPU reference of the shader's lookup: lod = roughness * (levels - 1), bilinear in the two nearest levels (u wraps, v clamps), blended.
    [[nodiscard]] render::Float3 sample(render::Float3 direction, float perceptual_roughness) const noexcept;
    /// RGBA16F texels (alpha 1) for every level, ready for Device::upload_texture (largest level first). Values above 65504 saturate.
    [[nodiscard]] std::vector<uint16_t> to_rgba16f() const;
};

struct EnvironmentResult {
    std::optional<Environment> environment;
    std::string error;  ///< set when environment is empty
    [[nodiscard]] bool ok() const noexcept { return environment.has_value(); }
};

/// Precomputes the environment from an equirectangular HDR image (width at least 2, any aspect; the image is resampled to the settings' size).
/// Returns an error for invalid settings, a malformed image, or non-finite or negative radiance. Deterministic: the same input gives the same bits in the same build.
[[nodiscard]] EnvironmentResult precompute_environment(const HdrImage& source, const EnvironmentSettings& settings = {});

/// The renderer's form of an environment: the SH coefficients and the RGBA16F texels of every level, with the sky's intensity scale.
[[nodiscard]] render::EnvironmentLighting to_environment_lighting(const Environment& environment, float intensity = 1.0f);

/// The equirectangular mapping (see the header comment).
[[nodiscard]] render::Float3 equirect_direction(float u, float v) noexcept;
void equirect_uv(render::Float3 direction, float& u, float& v) noexcept;

/// float -> IEEE half, round to nearest even, saturating at the largest finite half; used by to_rgba16f.
[[nodiscard]] uint16_t float_to_half_saturate(float value) noexcept;

} // namespace saige::gfx
