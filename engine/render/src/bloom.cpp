#include "saige/render/bloom.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::render {

void validate_bloom(const BloomSettings& s)
{
    const auto finite_in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!finite_in(s.intensity, 0.0f, 16.0f)) throw std::invalid_argument("BloomSettings: the intensity must be finite and in 0 to 16");
    if (!finite_in(s.threshold, 0.0f, 65504.0f)) throw std::invalid_argument("BloomSettings: the threshold must be finite and in 0 to 65504");
    if (!finite_in(s.knee, 0.0f, 1.0f)) throw std::invalid_argument("BloomSettings: the knee must be finite and in 0 to 1");
    if (!finite_in(s.scatter, 0.0f, 1.0f)) throw std::invalid_argument("BloomSettings: the scatter must be finite and in 0 to 1");
    if (s.levels < 1 || s.levels > kMaxBloomLevels) throw std::invalid_argument("BloomSettings: the level count must be in 1 to 8");
}

uint32_t bloom_level_extent(uint32_t source_extent, uint32_t index) noexcept
{
    const uint32_t shift = index + 1;
    return shift >= 32 ? 1u : std::max(1u, source_extent >> shift);
}

uint32_t bloom_level_count(uint32_t source_width, uint32_t source_height, uint32_t requested) noexcept
{
    if (source_width == 0 || source_height == 0 || requested == 0) return 0;
    uint32_t n = 1;
    while (n < requested && n < kMaxBloomLevels) {
        // Level n is made from level n - 1, which must be at least 2 x 2 to have something to reduce.
        if (std::min(bloom_level_extent(source_width, n - 1), bloom_level_extent(source_height, n - 1)) < 2) break;
        ++n;
    }
    return n;
}

Float3 bloom_clean(Float3 c) noexcept
{
    const auto one = [](float x) { return std::isnan(x) ? 0.0f : std::min(std::max(x, 0.0f), 65504.0f); };
    return {one(c.x), one(c.y), one(c.z)};
}

Float3 bloom_threshold(Float3 c, float threshold, float knee_width) noexcept
{
    const float b = std::max(c.x, std::max(c.y, c.z));
    float q = std::min(std::max(b - threshold + knee_width, 0.0f), 2.0f * knee_width);
    q = q * q / (4.0f * knee_width + 1e-5f);
    const float w = std::max(q, b - threshold) / std::max(b, 1e-5f);
    return {c.x * w, c.y * w, c.z * w};
}

Float3 bloom_sample(const BloomImage& image, float u, float v) noexcept
{
    const float fw = static_cast<float>(image.width), fh = static_cast<float>(image.height);
    const float x = u * fw - 0.5f, y = v * fh - 0.5f;
    const float x0f = std::floor(x), y0f = std::floor(y);
    const float tx = x - x0f, ty = y - y0f;
    const auto texel = [&](float i, float n) { return static_cast<uint32_t>(std::min(std::max(i, 0.0f), n - 1.0f)); };
    const uint32_t xa = texel(x0f, fw), xb = texel(x0f + 1.0f, fw), ya = texel(y0f, fh), yb = texel(y0f + 1.0f, fh);
    const Float3 top = image.at(xa, ya) * (1.0f - tx) + image.at(xb, ya) * tx;
    const Float3 bottom = image.at(xa, yb) * (1.0f - tx) + image.at(xb, yb) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

BloomImage bloom_downsample(const BloomImage& source, bool prefilter, float threshold, float knee_width)
{
    if (source.width == 0 || source.height == 0) throw std::invalid_argument("bloom_downsample: empty source");
    const uint32_t dw = std::max(1u, source.width >> 1), dh = std::max(1u, source.height >> 1);
    BloomImage out(dw, dh);
    const float tx = 1.0f / static_cast<float>(source.width), ty = 1.0f / static_cast<float>(source.height);
    for (uint32_t y = 0; y < dh; ++y) {
        for (uint32_t x = 0; x < dw; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dw), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dh);
            const auto tap = [&](float ox, float oy) {
                Float3 c = bloom_clean(bloom_sample(source, u + ox * tx, v + oy * ty));
                if (prefilter) c = bloom_threshold(c, threshold, knee_width);
                return c;
            };
            const Float3 e = tap(0, 0);
            const Float3 corners = tap(-2, 2) + tap(2, 2) + tap(-2, -2) + tap(2, -2);
            const Float3 edges = tap(0, 2) + tap(-2, 0) + tap(2, 0) + tap(0, -2);
            const Float3 diagonals = tap(-1, 1) + tap(1, 1) + tap(-1, -1) + tap(1, -1);
            out.at(x, y) = e * 0.125f + corners * 0.03125f + edges * 0.0625f + diagonals * 0.125f;
        }
    }
    return out;
}

BloomImage bloom_combine(const BloomImage& fine, const BloomImage& coarse, float scatter)
{
    if (fine.width == 0 || fine.height == 0 || coarse.width == 0 || coarse.height == 0) throw std::invalid_argument("bloom_combine: empty image");
    BloomImage out(fine.width, fine.height);
    const float tx = 1.0f / static_cast<float>(coarse.width), ty = 1.0f / static_cast<float>(coarse.height);
    for (uint32_t y = 0; y < fine.height; ++y) {
        for (uint32_t x = 0; x < fine.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(fine.width), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(fine.height);
            const auto tap = [&](float ox, float oy) { return bloom_sample(coarse, u + ox * tx, v + oy * ty); };
            const Float3 corners = tap(-1, -1) + tap(1, -1) + tap(-1, 1) + tap(1, 1);
            const Float3 edges = tap(0, -1) + tap(-1, 0) + tap(1, 0) + tap(0, 1);
            const Float3 tent = (corners + edges * 2.0f + tap(0, 0) * 4.0f) * (1.0f / 16.0f);
            out.at(x, y) = fine.at(x, y) + tent * scatter;
        }
    }
    return out;
}

BloomImage bloom_chain(const BloomImage& scene, const BloomSettings& settings)
{
    validate_bloom(settings);
    const uint32_t levels = bloom_level_count(scene.width, scene.height, settings.levels);
    if (levels == 0) throw std::invalid_argument("bloom_chain: empty scene");
    const float knee_width = settings.threshold * settings.knee;
    std::vector<BloomImage> down;
    down.push_back(bloom_downsample(scene, true, settings.threshold, knee_width));
    for (uint32_t i = 1; i < levels; ++i) down.push_back(bloom_downsample(down.back(), false, 0.0f, 0.0f));
    BloomImage up = down.back();
    for (uint32_t i = levels - 1; i-- > 0;) up = bloom_combine(down[i], up, settings.scatter);
    return up;
}

Float3 bloom_composite(Float3 scene, Float3 bloom, float intensity) noexcept
{
    const float k = std::isfinite(intensity) ? std::min(std::max(intensity, 0.0f), 16.0f) : 0.0f;
    return scene + bloom * k;
}

}  // namespace saige::render
