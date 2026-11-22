#include "saige/render/env_brdf.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace saige::render {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinRoughness = 0.045;  // the shaders' clamp (pbr.hlsli MIN_ROUGHNESS)

double smith_visibility(double n_dot_l, double n_dot_v, double alpha)
{
    const double a2 = alpha * alpha;
    const double gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const double gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    const double sum = gv + gl;
    return sum > 0.0 ? 0.5 / sum : 0.0;
}
} // namespace

EnvBrdfValue integrate_env_brdf(float n_dot_v, float roughness, int steps)
{
    const double nv = std::clamp(static_cast<double>(n_dot_v), 1e-4, 1.0);
    const double r = std::clamp(static_cast<double>(roughness), kMinRoughness, 1.0);
    const double alpha = r * r;
    const double vx = std::sqrt(1.0 - nv * nv);
    const int nu = std::max(steps, 8);
    const int nphi = std::max(steps, 8);
    // The azimuth samples are the same for every u: take the trigonometry out of the inner loop.
    std::vector<double> cos_phi(static_cast<std::size_t>(nphi));
    for (int j = 0; j < nphi; ++j) {
        cos_phi[static_cast<std::size_t>(j)] = std::cos((j + 0.5) / nphi * 2.0 * kPi);
    }
    double scale = 0.0;
    double bias = 0.0;
    for (int i = 0; i < nu; ++i) {
        // GGX half-vector CDF: tan^2(theta_h) = alpha^2 u / (1 - u), so samples are uniform in the distribution's mass.
        const double u = (i + 0.5) / nu;
        const double tan_h = alpha * std::sqrt(u / (1.0 - u));
        const double cos_h = 1.0 / std::sqrt(1.0 + tan_h * tan_h);
        const double sin_h = tan_h * cos_h;
        for (int j = 0; j < nphi; ++j) {
            const double hx = sin_h * cos_phi[static_cast<std::size_t>(j)];
            const double hz = cos_h;
            const double v_dot_h = vx * hx + nv * hz;
            if (v_dot_h <= 0.0) {
                continue;
            }
            const double n_dot_l = 2.0 * v_dot_h * hz - nv;  // l = 2 (v.h) h - v
            if (n_dot_l <= 0.0) {
                continue;
            }
            // dω_l = 4 (v.h) dω_h and D cosθ_h dω_h = du dφ / 2π under that sampling, so D cancels from D * V * n.l.
            const double lobe = smith_visibility(n_dot_l, nv, alpha) * n_dot_l * 4.0 * v_dot_h / hz / (static_cast<double>(nu) * nphi);
            const double m = 1.0 - v_dot_h;
            const double w = m * m * m * m * m;
            scale += lobe * (1.0 - w);
            bias += lobe * w;
        }
    }
    return {static_cast<float>(scale), static_cast<float>(bias)};
}

uint16_t float_to_half(float value) noexcept
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu) {  // inf or NaN
        return static_cast<uint16_t>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
    }
    const int32_t e = static_cast<int32_t>(exponent) - 127 + 15;
    if (e >= 0x1F) {
        return static_cast<uint16_t>(sign | 0x7C00u);  // overflow
    }
    if (e <= 0) {
        if (e < -10) {
            return static_cast<uint16_t>(sign);  // underflow to zero
        }
        mantissa |= 0x800000u;  // subnormal
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t half = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half & 1u) != 0)) {
            ++half;
        }
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(e) << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0)) {
        ++half;  // a carry into the exponent is correct (rounds up to the next binade or to infinity)
    }
    return static_cast<uint16_t>(sign | half);
}

float half_to_float(uint16_t bits) noexcept
{
    const uint32_t sign = (static_cast<uint32_t>(bits) & 0x8000u) << 16;
    const uint32_t exponent = (bits >> 10) & 0x1Fu;
    const uint32_t mantissa = bits & 0x3FFu;
    uint32_t out;
    if (exponent == 0) {
        if (mantissa == 0) {
            out = sign;
        } else {  // subnormal half: normalise
            int e = -1;
            uint32_t m = mantissa;
            do {
                ++e;
                m <<= 1;
            } while ((m & 0x400u) == 0);
            out = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exponent == 0x1Fu) {
        out = sign | 0x7F800000u | (mantissa << 13);
    } else {
        out = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(out);
}

EnvBrdfValue EnvBrdfTable::at(uint32_t n_dot_v_index, uint32_t roughness_index) const noexcept
{
    const std::size_t i = (static_cast<std::size_t>(roughness_index) * size + n_dot_v_index) * 2;
    return {half_to_float(texels[i]), half_to_float(texels[i + 1])};
}

EnvBrdfTable make_env_brdf_table(uint32_t size, int steps)
{
    EnvBrdfTable table;
    table.size = size;
    table.texels.resize(static_cast<std::size_t>(size) * size * 2);
    for (uint32_t j = 0; j < size; ++j) {
        const float roughness = (static_cast<float>(j) + 0.5f) / static_cast<float>(size);
        for (uint32_t i = 0; i < size; ++i) {
            const float n_dot_v = (static_cast<float>(i) + 0.5f) / static_cast<float>(size);
            const EnvBrdfValue v = integrate_env_brdf(n_dot_v, roughness, steps);
            const std::size_t k = (static_cast<std::size_t>(j) * size + i) * 2;
            table.texels[k] = float_to_half(v.scale);
            table.texels[k + 1] = float_to_half(v.bias);
        }
    }
    return table;
}

EnvBrdfValue EnvBrdfAsset::at(uint32_t n_dot_v_index, uint32_t roughness_index) const noexcept
{
    const std::size_t i = (static_cast<std::size_t>(roughness_index) * size + n_dot_v_index) * 2;
    return {half_to_float(texels[i]), half_to_float(texels[i + 1])};
}

uint64_t env_brdf_checksum(const uint16_t* texels, std::size_t count) noexcept
{
    uint64_t hash = 0xcbf29ce484222325ull;  // FNV-1a, 64-bit
    for (std::size_t i = 0; i < count; ++i) {
        hash = (hash ^ (texels[i] & 0xFFu)) * 0x100000001b3ull;
        hash = (hash ^ (texels[i] >> 8)) * 0x100000001b3ull;
    }
    return hash;
}

} // namespace saige::render
