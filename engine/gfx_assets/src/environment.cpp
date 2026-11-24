#include "saige/gfx_assets/environment.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace saige::gfx {

using render::Float3;

namespace {

constexpr double pi_d = 3.14159265358979323846;

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<float> rgb;
};

struct View {
    uint32_t w, h;
    const float* rgb;
};

Float3 texel(const View& im, int x, int y) noexcept
{
    const std::size_t i = (static_cast<std::size_t>(y) * im.w + static_cast<std::size_t>(x)) * 3;
    return {im.rgb[i], im.rgb[i + 1], im.rgb[i + 2]};
}

/// Bilinear lookup at (u, v) with u wrapping and v clamped; texel centers at (x + 0.5) / w.
Float3 bilinear(const View& im, float u, float v) noexcept
{
    const float fx = u * static_cast<float>(im.w) - 0.5f, fy = v * static_cast<float>(im.h) - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const auto wrap = [&](int x) {
        const int w = static_cast<int>(im.w);
        return ((x % w) + w) % w;
    };
    const auto clampy = [&](int y) { return std::clamp(y, 0, static_cast<int>(im.h) - 1); };
    const int x0 = wrap(static_cast<int>(x0f)), x1 = wrap(static_cast<int>(x0f) + 1);
    const int y0 = clampy(static_cast<int>(y0f)), y1 = clampy(static_cast<int>(y0f) + 1);
    const Float3 a = texel(im, x0, y0), b = texel(im, x1, y0), c = texel(im, x0, y1), d = texel(im, x1, y1);
    const auto mix = [&](float p, float q, float r, float s) { return (p * (1 - tx) + q * tx) * (1 - ty) + (r * (1 - tx) + s * tx) * ty; };
    return {mix(a.x, b.x, c.x, d.x), mix(a.y, b.y, c.y, d.y), mix(a.z, b.z, c.z, d.z)};
}

Image box_down(const Image& s)
{
    Image d;
    d.w = std::max(1u, s.w / 2);
    d.h = std::max(1u, s.h / 2);
    d.rgb.assign(static_cast<std::size_t>(d.w) * d.h * 3, 0.0f);
    for (uint32_t y = 0; y < d.h; ++y) {
        for (uint32_t x = 0; x < d.w; ++x) {
            const uint32_t x0 = std::min(x * 2, s.w - 1), x1 = std::min(x * 2 + 1, s.w - 1), y0 = std::min(y * 2, s.h - 1), y1 = std::min(y * 2 + 1, s.h - 1);
            for (int c = 0; c < 3; ++c) {
                const auto at = [&](uint32_t xx, uint32_t yy) { return s.rgb[(static_cast<std::size_t>(yy) * s.w + xx) * 3 + static_cast<std::size_t>(c)]; };
                d.rgb[(static_cast<std::size_t>(y) * d.w + x) * 3 + static_cast<std::size_t>(c)] = 0.25f * (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1));
            }
        }
    }
    return d;
}

double radical_inverse(uint32_t bits) noexcept
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

std::array<double, 9> sh_basis(Float3 d) noexcept
{
    const double x = d.x, y = d.y, z = d.z;
    return {0.282094791773878,        0.488602511902920 * y,      0.488602511902920 * z,      0.488602511902920 * x,      1.092548430592079 * x * y,
            1.092548430592079 * y * z, 0.315391565252520 * (3.0 * z * z - 1.0), 1.092548430592079 * x * z, 0.546274215296040 * (x * x - y * y)};
}

} // namespace

Float3 equirect_direction(float u, float v) noexcept
{
    const double phi = (static_cast<double>(u) - 0.5) * 2.0 * pi_d, theta = static_cast<double>(v) * pi_d;
    const double s = std::sin(theta);
    return {static_cast<float>(s * std::cos(phi)), static_cast<float>(s * std::sin(phi)), static_cast<float>(std::cos(theta))};
}

void equirect_uv(Float3 d, float& u, float& v) noexcept
{
    u = static_cast<float>(std::atan2(static_cast<double>(d.y), static_cast<double>(d.x)) / (2.0 * pi_d) + 0.5);
    v = static_cast<float>(std::acos(std::clamp(static_cast<double>(d.z), -1.0, 1.0)) / pi_d);
}

Float3 sh_diffuse(const Sh9& sh, Float3 n) noexcept
{
    const std::array<double, 9> b = sh_basis(n);
    static constexpr double band[9] = {1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25};
    double r = 0.0, g = 0.0, bl = 0.0;
    for (std::size_t k = 0; k < 9; ++k) {
        r += static_cast<double>(sh.c[k].x) * b[k] * band[k];
        g += static_cast<double>(sh.c[k].y) * b[k] * band[k];
        bl += static_cast<double>(sh.c[k].z) * b[k] * band[k];
    }
    return {static_cast<float>(r), static_cast<float>(g), static_cast<float>(bl)};
}

uint32_t Environment::level_width(uint32_t level) const noexcept { return std::max(1u, width >> level); }
uint32_t Environment::level_height(uint32_t level) const noexcept { return std::max(1u, height >> level); }

std::size_t Environment::level_offset(uint32_t level) const noexcept
{
    std::size_t offset = 0;
    for (uint32_t i = 0; i < level; ++i) offset += static_cast<std::size_t>(level_width(i)) * level_height(i) * 3;
    return offset;
}

Float3 Environment::sample(Float3 direction, float roughness) const noexcept
{
    if (levels == 0) return {};
    float u, v;
    equirect_uv(direction, u, v);
    const float lod = std::clamp(roughness, 0.0f, 1.0f) * static_cast<float>(levels - 1);
    const uint32_t lo = static_cast<uint32_t>(std::floor(lod)), hi = std::min(lo + 1, levels - 1);
    const float t = lod - static_cast<float>(lo);
    const auto level = [&](uint32_t i) { return bilinear(View{level_width(i), level_height(i), texels.data() + level_offset(i)}, u, v); };
    const Float3 a = level(lo);
    if (hi == lo || t == 0.0f) return a;
    const Float3 b = level(hi);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

uint16_t float_to_half_saturate(float value) noexcept
{
    uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    if (std::isnan(value)) return static_cast<uint16_t>(sign);  // NaN is mapped to zero: callers pass finite data, and a NaN texel is the worst outcome
    const float magnitude = std::fabs(value);
    if (magnitude >= 65520.0f) return static_cast<uint16_t>(sign | 0x7BFFu);  // saturate at 65504 (including infinity)
    bits &= 0x7FFFFFFFu;
    if (magnitude < 6.103515625e-05f) {  // subnormal half (or zero): scale into the 10-bit mantissa, round to nearest even
        const float scaled = magnitude * 16777216.0f;  // 2^24: the subnormal step is 2^-24
        const uint32_t m = static_cast<uint32_t>(std::nearbyint(scaled));
        return static_cast<uint16_t>(sign | m);  // m can reach 0x400, which is the smallest normal: still correct
    }
    const uint32_t exponent = (bits >> 23) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    uint32_t half = (exponent << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u) != 0)) ++half;  // round to nearest even (a carry into the exponent is correct)
    return static_cast<uint16_t>(sign | half);
}

std::vector<uint16_t> Environment::to_rgba16f() const
{
    std::vector<uint16_t> out;
    out.reserve(texels.size() / 3 * 4);
    for (std::size_t i = 0; i + 2 < texels.size(); i += 3) {
        out.push_back(float_to_half_saturate(texels[i]));
        out.push_back(float_to_half_saturate(texels[i + 1]));
        out.push_back(float_to_half_saturate(texels[i + 2]));
        out.push_back(0x3C00u);  // 1.0
    }
    return out;
}

render::EnvironmentLighting to_environment_lighting(const Environment& e, float intensity)
{
    render::EnvironmentLighting out;
    out.sh = e.diffuse.c;
    out.width = e.width;
    out.height = e.height;
    out.levels = e.levels;
    out.rgba16f = e.to_rgba16f();
    out.intensity = intensity;
    return out;
}

EnvironmentResult precompute_environment(const HdrImage& source, const EnvironmentSettings& s)
{
    EnvironmentResult result;
    const auto fail = [&](std::string message) {
        result.error = "environment: " + std::move(message);
        return result;
    };
    if (s.width < 8 || s.width > 4096 || (s.width % 2) != 0) return fail("the width must be even and between 8 and 4096");
    if (s.levels < 1 || s.levels > 12) return fail("the level count must be between 1 and 12");
    if (s.samples < 1 || s.samples > 4096) return fail("the sample count must be between 1 and 4096");
    if (!std::isfinite(s.diffuse_radiance_limit) || s.diffuse_radiance_limit < 0.0f) return fail("the diffuse radiance limit must be finite and not negative");
    if (source.width < 2 || source.height < 1 || source.width > 16384 || source.height > 16384) return fail("the source image must be at least 2x1 and at most 16384 per side");
    if (source.rgb.size() != static_cast<std::size_t>(source.width) * source.height * 3) return fail("the source image's pixel count does not match its size");
    for (const float v : source.rgb) {
        if (!std::isfinite(v) || v < 0.0f) return fail("the source image holds a non-finite or negative radiance value");
    }

    // ---- diffuse: SH9 by quadrature over the source pixels (solid angle weighted), optionally with the radiance clamped.
    {
        double acc[9][3] = {};
        const double dphi = 2.0 * pi_d / source.width, dtheta = pi_d / source.height;
        for (uint32_t y = 0; y < source.height; ++y) {
            const double theta = (y + 0.5) * dtheta;
            const double weight = std::sin(theta) * dtheta * dphi;
            for (uint32_t x = 0; x < source.width; ++x) {
                const Float3 d = equirect_direction((static_cast<float>(x) + 0.5f) / static_cast<float>(source.width), (static_cast<float>(y) + 0.5f) / static_cast<float>(source.height));
                const float* p = source.rgb.data() + (static_cast<std::size_t>(y) * source.width + x) * 3;
                double r = p[0], g = p[1], b = p[2];
                if (s.diffuse_radiance_limit > 0.0f) {
                    const double peak = std::max({r, g, b});
                    const double limit = static_cast<double>(s.diffuse_radiance_limit);
                    if (peak > limit) {
                        const double k = limit / peak;
                        r *= k;
                        g *= k;
                        b *= k;
                    }
                }
                const std::array<double, 9> basis = sh_basis(d);
                for (std::size_t k = 0; k < 9; ++k) {
                    acc[k][0] += r * basis[k] * weight;
                    acc[k][1] += g * basis[k] * weight;
                    acc[k][2] += b * basis[k] * weight;
                }
            }
        }
        Environment e;
        for (std::size_t k = 0; k < 9; ++k) e.diffuse.c[k] = {static_cast<float>(acc[k][0]), static_cast<float>(acc[k][1]), static_cast<float>(acc[k][2])};
        result.environment = std::move(e);
    }
    Environment& env = *result.environment;
    env.width = s.width;
    env.height = s.width / 2;
    env.levels = s.levels;

    // ---- source pyramid for filtered importance sampling.
    std::vector<Image> pyramid;
    pyramid.push_back({source.width, source.height, source.rgb});
    while (pyramid.back().w > 1 || pyramid.back().h > 1) pyramid.push_back(box_down(pyramid.back()));
    const double base_texel_solid_angle = 4.0 * pi_d / (static_cast<double>(source.width) * source.height);
    const float max_lod = static_cast<float>(pyramid.size() - 1);
    const auto sample_lod = [&](float u, float v, float lod) {
        const std::size_t level = static_cast<std::size_t>(std::clamp(std::lround(lod), 0l, static_cast<long>(pyramid.size() - 1)));
        return bilinear(View{pyramid[level].w, pyramid[level].h, pyramid[level].rgb.data()}, u, v);
    };

    // ---- specular: level 0 is the mirror (the source resampled), level i the GGX prefilter at roughness i / (levels - 1).
    for (uint32_t level = 0; level < s.levels; ++level) {
        const uint32_t w = env.level_width(level), h = env.level_height(level);
        const double roughness = s.levels == 1 ? 0.0 : static_cast<double>(level) / static_cast<double>(s.levels - 1);
        const double alpha = std::max(roughness, 0.045) * std::max(roughness, 0.045);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
                Float3 out;
                if (level == 0) {
                    // Mirror: the source at this texel's footprint (a pyramid level when the source is larger than the base).
                    const float lod = std::log2(std::max(1.0f, static_cast<float>(source.width) / static_cast<float>(w)));
                    out = sample_lod(u, v, lod);
                } else {
                    const Float3 r = equirect_direction(u, v);
                    const Float3 helper = std::fabs(r.z) < 0.999f ? Float3{0.0f, 0.0f, 1.0f} : Float3{1.0f, 0.0f, 0.0f};
                    const Float3 tx = render::normalize(render::cross(helper, r));
                    const Float3 ty = render::cross(r, tx);
                    double sr = 0.0, sg = 0.0, sb = 0.0, total = 0.0;
                    for (uint32_t k = 0; k < s.samples; ++k) {
                        const double u1 = (static_cast<double>(k) + 0.5) / static_cast<double>(s.samples), u2 = radical_inverse(k);
                        const double phi = 2.0 * pi_d * u2;
                        const double cos_theta = std::sqrt((1.0 - u1) / (1.0 + (alpha * alpha - 1.0) * u1));
                        const double sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));
                        const Float3 hv = tx * static_cast<float>(sin_theta * std::cos(phi)) + ty * static_cast<float>(sin_theta * std::sin(phi)) + r * static_cast<float>(cos_theta);
                        const Float3 l = hv * (2.0f * render::dot(r, hv)) - r;
                        const double n_dot_l = static_cast<double>(render::dot(r, l));
                        if (n_dot_l <= 0.0) continue;
                        // Filtered importance sampling: read the source at the footprint of this sample's solid angle (N = V = R, so pdf = D / 4).
                        const double denom = cos_theta * cos_theta * (alpha * alpha - 1.0) + 1.0;
                        const double d_ggx = alpha * alpha / (pi_d * denom * denom);
                        const double pdf = std::max(d_ggx / 4.0, 1e-12);
                        const double sample_solid_angle = 1.0 / (static_cast<double>(s.samples) * pdf);
                        const float lod = std::clamp(static_cast<float>(0.5 * std::log2(sample_solid_angle / base_texel_solid_angle) + 1.0), 0.0f, max_lod);
                        float lu, lv;
                        equirect_uv(l, lu, lv);
                        const Float3 radiance = sample_lod(lu, lv, lod);
                        sr += static_cast<double>(radiance.x) * n_dot_l;
                        sg += static_cast<double>(radiance.y) * n_dot_l;
                        sb += static_cast<double>(radiance.z) * n_dot_l;
                        total += n_dot_l;
                    }
                    if (total > 0.0) out = {static_cast<float>(sr / total), static_cast<float>(sg / total), static_cast<float>(sb / total)};
                }
                env.texels.push_back(out.x);
                env.texels.push_back(out.y);
                env.texels.push_back(out.z);
            }
        }
    }
    return result;
}

} // namespace saige::gfx
