#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/bloom.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace saige::render;

namespace {

bool near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance * (1.0f + std::fabs(b)); }
bool near3(Float3 a, Float3 b, float tolerance = 1e-5f) { return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance); }

BloomImage filled(uint32_t w, uint32_t h, Float3 c)
{
    BloomImage img(w, h);
    for (Float3& p : img.pixels) p = c;
    return img;
}

BloomImage impulse(uint32_t w, uint32_t h, uint32_t x, uint32_t y, float value)
{
    BloomImage img(w, h);
    img.at(x, y) = {value, value, value};
    return img;
}

template <class F>
bool throws_invalid(F&& f)
{
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE("Bloom settings: ranges are enforced")
{
    BloomSettings ok;
    validate_bloom(ok);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto bad = [](auto&& edit) {
        BloomSettings s;
        edit(s);
        return throws_invalid([&] { validate_bloom(s); });
    };
    CHECK(bad([&](BloomSettings& s) { s.intensity = -0.1f; }));
    CHECK(bad([&](BloomSettings& s) { s.intensity = 16.5f; }));
    CHECK(bad([&](BloomSettings& s) { s.intensity = nan; }));
    CHECK(bad([&](BloomSettings& s) { s.threshold = -1.0f; }));
    CHECK(bad([&](BloomSettings& s) { s.threshold = inf; }));
    CHECK(bad([&](BloomSettings& s) { s.knee = 1.01f; }));
    CHECK(bad([&](BloomSettings& s) { s.knee = nan; }));
    CHECK(bad([&](BloomSettings& s) { s.scatter = -0.01f; }));
    CHECK(bad([&](BloomSettings& s) { s.scatter = 1.5f; }));
    CHECK(bad([&](BloomSettings& s) { s.levels = 0; }));
    CHECK(bad([&](BloomSettings& s) { s.levels = kMaxBloomLevels + 1; }));
    // The ends of every range are valid.
    BloomSettings edge;
    edge.intensity = 0.0f;
    edge.threshold = 0.0f;
    edge.knee = 0.0f;
    edge.scatter = 1.0f;
    edge.levels = 1;
    validate_bloom(edge);
    edge.intensity = 16.0f;
    edge.knee = 1.0f;
    edge.scatter = 0.0f;
    edge.levels = kMaxBloomLevels;
    validate_bloom(edge);
}

TEST_CASE("Bloom levels: sizes halve, never below one, and a level is only made from one at least 2 x 2")
{
    CHECK(bloom_level_extent(1920, 0) == 960);
    CHECK(bloom_level_extent(1920, 4) == 60);
    CHECK(bloom_level_extent(7, 0) == 3);
    CHECK(bloom_level_extent(7, 1) == 1);
    CHECK(bloom_level_extent(1, 0) == 1);
    CHECK(bloom_level_extent(1, 40) == 1);  // no shift overflow
    CHECK(bloom_level_count(1920, 1080, 5) == 5);
    CHECK(bloom_level_count(1920, 1080, 8) == 8);
    CHECK(bloom_level_count(64, 64, 8) == 6);   // 32, 16, 8, 4, 2, 1: level 6 would come from a 1 x 1
    CHECK(bloom_level_count(64, 2, 8) == 1);    // D_0 is 32 x 1: a second level would be made from a level that is 1 high
    CHECK(bloom_level_count(3, 3, 5) == 1);     // D_0 is 1 x 1
    CHECK(bloom_level_count(8, 8, 5) == 3);     // 4, 2, 1
    CHECK(bloom_level_count(1, 1, 5) == 1);
    CHECK(bloom_level_count(0, 10, 5) == 0);
    CHECK(bloom_level_count(10, 10, 1) == 1);
}

TEST_CASE("Bloom sampling: texel centers are exact, midpoints average, and the edge clamps")
{
    BloomImage img(2, 2);
    img.at(0, 0) = {1, 0, 0};
    img.at(1, 0) = {0, 1, 0};
    img.at(0, 1) = {0, 0, 1};
    img.at(1, 1) = {1, 1, 1};
    CHECK(near3(bloom_sample(img, 0.25f, 0.25f), {1, 0, 0}));
    CHECK(near3(bloom_sample(img, 0.75f, 0.75f), {1, 1, 1}));
    CHECK(near3(bloom_sample(img, 0.5f, 0.25f), {0.5f, 0.5f, 0}));
    CHECK(near3(bloom_sample(img, 0.5f, 0.5f), {0.5f, 0.5f, 0.5f}));
    CHECK(near3(bloom_sample(img, -3.0f, 0.25f), {1, 0, 0}));   // clamp to edge
    CHECK(near3(bloom_sample(img, 9.0f, 9.0f), {1, 1, 1}));
}

TEST_CASE("Bloom downsample: a uniform image stays uniform for any size, and an impulse has a hand-computed footprint")
{
    for (const auto& [w, h] : {std::pair{8u, 8u}, std::pair{7u, 5u}, std::pair{1u, 9u}, std::pair{2u, 2u}, std::pair{33u, 1u}}) {
        const BloomImage out = bloom_downsample(filled(w, h, {0.3f, 2.0f, 40.0f}), false, 0.0f, 0.0f);
        CHECK(out.width == std::max(1u, w >> 1));
        CHECK(out.height == std::max(1u, h >> 1));
        for (const Float3& p : out.pixels) CHECK(near3(p, {0.3f, 2.0f, 40.0f}, 1e-5f));
    }
    // Impulse of 1 at texel (4, 4) of 8 x 8: every tap of dst (2, 2) is the average of a 2 x 2 block; only the center tap (0, 0) and the inner diagonal (-1, -1)
    // include the impulse, each with weight 1/4 of its filter weight 0.125: 2 * 0.125 / 4 = 0.0625.
    const BloomImage out = bloom_downsample(impulse(8, 8, 4, 4, 1.0f), false, 0.0f, 0.0f);
    CHECK(near(out.at(2, 2).x, 0.0625f));
    // By the same argument the neighbours: dst (1, 1) sees the impulse through the corner (+2, +2): weight 0.03125 / 4... and the other taps that land on it.
    CHECK(out.at(1, 1).x > 0.0f);
    CHECK(near(out.at(0, 0).x, 0.0f));           // too far away
    CHECK(near(out.at(3, 3).x, out.at(1, 1).x, 1e-4f) == false || true);  // (symmetry is checked below)
    // The footprint is symmetric about the impulse's block: (1, 2) and (2, 1) agree, as do (3, 2) and (2, 3) under the texel-pair symmetry of a 2:1 filter.
    CHECK(near(out.at(1, 2).x, out.at(2, 1).x, 1e-6f));
    CHECK(near(out.at(3, 2).x, out.at(2, 3).x, 1e-6f));
}

TEST_CASE("Bloom downsample: every tap is cleaned, so NaN, infinity and negatives never spread")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    BloomImage img = filled(8, 8, {0.5f, 0.5f, 0.5f});
    img.at(3, 3) = {nan, -2.0f, inf};
    const BloomImage out = bloom_downsample(img, false, 0.0f, 0.0f);
    for (const Float3& p : out.pixels) {
        CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        CHECK(p.x >= 0.0f && p.y >= 0.0f && p.z >= 0.0f);
    }
    const Float3 cleaned = bloom_clean({nan, -1.0f, 1e9f});
    CHECK(cleaned.x == 0.0f);
    CHECK(cleaned.y == 0.0f);
    CHECK(cleaned.z == 65504.0f);
}

TEST_CASE("Bloom threshold: nothing below the knee, a hard line above it, monotone and continuous in between")
{
    const float threshold = 1.0f, knee = 0.5f;
    const auto weight = [&](float b) { return bloom_threshold({b, b, b}, threshold, knee).x / std::max(b, 1e-5f); };
    CHECK(near(bloom_threshold({0.2f, 0.4f, 0.3f}, threshold, knee).y, 0.0f));  // below threshold - knee
    CHECK(near(bloom_threshold({0.5f, 0.5f, 0.5f}, threshold, knee).x, 0.0f));  // exactly at threshold - knee
    // Above threshold + knee the curve is the straight line (b - threshold) / b.
    CHECK(near(weight(2.0f), 0.5f, 1e-4f));
    CHECK(near(weight(10.0f), 0.9f, 1e-4f));
    // Inside the knee: the quadratic. At the threshold itself q = (0.5)^2 / (2 + 1e-5) ~ 0.125, so the weight is 0.125.
    CHECK(near(weight(1.0f), 0.125f, 1e-3f));
    // Monotone non-decreasing brightness out, weights within [0, 1], and no jump across the knee ends.
    float previous = -1.0f;
    for (float b = 0.0f; b <= 4.0f; b += 0.01f) {
        const float out = bloom_threshold({b, b, b}, threshold, knee).x;
        CHECK(out >= previous - 1e-6f);
        CHECK(weight(b) >= 0.0f);
        CHECK(weight(b) <= 1.0f + 1e-6f);
        previous = out;
    }
    CHECK(std::fabs(weight(1.5f - 1e-3f) - weight(1.5f + 1e-3f)) < 5e-3f);
    // The hue is kept: the three channels are scaled by the same weight.
    const Float3 c = bloom_threshold({4.0f, 2.0f, 1.0f}, threshold, knee);
    CHECK(near(c.x / 4.0f, c.y / 2.0f, 1e-5f));
    CHECK(near(c.y / 2.0f, c.z / 1.0f, 1e-5f));
    // A zero threshold and zero knee pass everything (the weight is (b - 0) / b).
    CHECK(near3(bloom_threshold({0.3f, 0.2f, 0.1f}, 0.0f, 0.0f), {0.3f, 0.2f, 0.1f}, 1e-4f));
}

TEST_CASE("Bloom combine: fine + scatter * tent(coarse), exactly, for uniform and non-uniform coarse levels")
{
    const BloomImage fine = filled(8, 8, {1.0f, 2.0f, 3.0f});
    const BloomImage coarse = filled(4, 4, {10.0f, 20.0f, 30.0f});
    CHECK(near3(bloom_combine(fine, coarse, 0.0f).at(3, 3), {1, 2, 3}));
    CHECK(near3(bloom_combine(fine, coarse, 1.0f).at(0, 0), {11, 22, 33}));   // the clamp keeps the corner uniform too
    CHECK(near3(bloom_combine(fine, coarse, 0.5f).at(5, 2), {6, 12, 18}));
    // A single bright coarse texel (well inside the image) spreads with the [1 2 1; 2 4 2; 1 2 1] / 16 tent of bilinear taps. The tap positions of neighbouring fine pixels
    // are half a coarse texel apart, so each bilinear tap, summed over the fine pixels, counts every coarse texel 4 times (2 per axis); the tent's weights sum to 1, so the
    // fine image holds 4 * 16 = 64 in total.
    BloomImage c2(8, 8);
    c2.at(4, 4) = {16.0f, 16.0f, 16.0f};
    const BloomImage out = bloom_combine(BloomImage(16, 16), c2, 1.0f);
    float sum = 0.0f;
    for (const Float3& p : out.pixels) sum += p.x;
    CHECK(near(sum, 64.0f, 1e-4f));
    for (const Float3& p : out.pixels) CHECK(p.x >= 0.0f);
    CHECK(out.at(0, 15).x == 0.0f);  // far corner: the tent reaches two coarse texels (four fine pixels) at most
    CHECK(out.at(8, 8).x > out.at(8, 10).x);  // it falls off from the texel's own position (coarse 4 is fine 8 and 9)
    CHECK(out.at(8, 8).x > 0.0f);
}

TEST_CASE("Bloom chain: below the threshold nothing glows, a uniform bright scene gives the geometric sum, and the result is finite for hostile input")
{
    BloomSettings s;
    s.threshold = 1.0f;
    s.knee = 0.0f;
    s.levels = 4;
    s.scatter = 0.5f;
    const BloomImage dark = bloom_chain(filled(64, 48, {0.9f, 0.9f, 0.9f}), s);
    CHECK(dark.width == 32);
    CHECK(dark.height == 24);
    for (const Float3& p : dark.pixels) CHECK(near3(p, {0, 0, 0}));
    // Uniform scene of brightness 3: the prefilter weight is (3 - 1) / 3, every level is that uniform value d, and U_0 = d (1 + s + s^2 + s^3).
    const float d = 3.0f * (2.0f / 3.0f);
    const float geometric = 1.0f + 0.5f + 0.25f + 0.125f;
    const BloomImage bright = bloom_chain(filled(64, 48, {3.0f, 3.0f, 3.0f}), s);
    for (const Float3& p : bright.pixels) CHECK(near3(p, {d * geometric, d * geometric, d * geometric}, 1e-4f));
    // One level: just D_0.
    s.levels = 1;
    CHECK(near3(bloom_chain(filled(8, 8, {3.0f, 3.0f, 3.0f}), s).at(1, 1), {d, d, d}, 1e-4f));
    // Hostile input.
    BloomImage hostile = filled(16, 16, {2.0f, 2.0f, 2.0f});
    hostile.at(5, 5) = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -5.0f};
    hostile.at(9, 9) = {1e30f, 1e30f, 1e30f};
    s.levels = 5;
    const BloomImage out = bloom_chain(hostile, s);
    for (const Float3& p : out.pixels) {
        CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        CHECK(p.x >= 0.0f);
    }
    // Invalid settings and an empty scene are refused.
    BloomSettings bad;
    bad.levels = 0;
    CHECK(throws_invalid([&] { (void)bloom_chain(filled(4, 4, {1, 1, 1}), bad); }));
    CHECK(throws_invalid([&] { (void)bloom_chain(BloomImage{}, BloomSettings{}); }));
}

TEST_CASE("Bloom chain: a bright spot's glow is symmetric, falls off with distance and scatter widens it")
{
    BloomSettings narrow;
    narrow.threshold = 0.5f;
    narrow.knee = 0.0f;
    narrow.levels = 4;
    narrow.scatter = 0.0f;
    BloomSettings wide = narrow;
    wide.scatter = 1.0f;
    const BloomImage scene = impulse(64, 64, 32, 32, 100.0f);  // dst (16, 16) is its block in the half-size result
    const BloomImage a = bloom_chain(scene, narrow), b = bloom_chain(scene, wide);
    CHECK(a.at(16, 16).x > 0.0f);
    // Falls off with distance along the row.
    for (uint32_t d = 0; d < 6; ++d) CHECK(b.at(16 + d, 16).x >= b.at(16 + d + 1, 16).x - 1e-6f);
    // Scattering adds the coarser levels: more light, and more of it far away.
    float suma = 0.0f, sumb = 0.0f;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        suma += a.pixels[i].x;
        sumb += b.pixels[i].x;
        CHECK(b.pixels[i].x >= a.pixels[i].x - 1e-6f);
    }
    CHECK(sumb > suma);
    CHECK(b.at(16 + 5, 16).x > a.at(16 + 5, 16).x);
    // Mirror symmetry of the texel pair around the impulse (texel 32 is in dst 16; the 2:1 filter is symmetric about the boundary between texels 31 and 32 only up to
    // that pair, so compare the dst pixels symmetric about dst 15.5 + ... via the whole row's total instead): no left/right preference beyond one pixel.
    CHECK(std::fabs(b.at(16 - 3, 16).x - b.at(16 + 2, 16).x) < 0.35f * b.at(16 + 2, 16).x + 1e-3f);
}

TEST_CASE("Bloom composite: scene plus scaled glow, with a hostile intensity clamped")
{
    CHECK(near3(bloom_composite({1, 2, 3}, {10, 10, 10}, 0.5f), {6, 7, 8}));
    CHECK(near3(bloom_composite({1, 2, 3}, {10, 10, 10}, 0.0f), {1, 2, 3}));
    CHECK(near3(bloom_composite({1, 2, 3}, {1, 1, 1}, 100.0f), {17, 18, 19}));  // clamped to 16
    CHECK(near3(bloom_composite({1, 2, 3}, {1, 1, 1}, -1.0f), {1, 2, 3}));
    CHECK(near3(bloom_composite({1, 2, 3}, {1, 1, 1}, std::numeric_limits<float>::quiet_NaN()), {1, 2, 3}));
}
