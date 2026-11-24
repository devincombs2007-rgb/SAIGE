#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/fxaa.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace saige::render;

namespace {

bool near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance * (1.0f + std::fabs(b)); }

FxaaImage filled(uint32_t w, uint32_t h, Float3 c)
{
    FxaaImage img(w, h);
    for (Float3& p : img.pixels) p = c;
    return img;
}

/// A black and white image split by the line y = slope * x + offset (a staircase of aliased pixels when the slope is not 0 or infinite).
FxaaImage split(uint32_t w, uint32_t h, float slope, float offset)
{
    FxaaImage img(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const float line = slope * (static_cast<float>(x) + 0.5f) + offset;
            const float v = (static_cast<float>(y) + 0.5f) > line ? 1.0f : 0.0f;
            img.at(x, y) = {v, v, v};
        }
    }
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

float max_diff(const FxaaImage& a, const FxaaImage& b)
{
    float d = 0.0f;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        d = std::max({d, std::fabs(a.pixels[i].x - b.pixels[i].x), std::fabs(a.pixels[i].y - b.pixels[i].y), std::fabs(a.pixels[i].z - b.pixels[i].z)});
    }
    return d;
}

}  // namespace

TEST_CASE("FXAA settings: ranges are enforced")
{
    validate_fxaa(FxaaSettings{});
    const auto bad = [](auto&& edit) {
        FxaaSettings s;
        edit(s);
        return throws_invalid([&] { validate_fxaa(s); });
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(bad([&](FxaaSettings& s) { s.edge_threshold = -0.01f; }));
    CHECK(bad([&](FxaaSettings& s) { s.edge_threshold = 1.01f; }));
    CHECK(bad([&](FxaaSettings& s) { s.edge_threshold = nan; }));
    CHECK(bad([&](FxaaSettings& s) { s.edge_threshold_min = -1.0f; }));
    CHECK(bad([&](FxaaSettings& s) { s.edge_threshold_min = 2.0f; }));
    CHECK(bad([&](FxaaSettings& s) { s.span_max = 0.5f; }));
    CHECK(bad([&](FxaaSettings& s) { s.span_max = 33.0f; }));
    CHECK(bad([&](FxaaSettings& s) { s.span_max = std::numeric_limits<float>::infinity(); }));
    FxaaSettings edge;
    edge.edge_threshold = 0.0f;
    edge.edge_threshold_min = 0.0f;
    edge.span_max = 1.0f;
    validate_fxaa(edge);
    edge.edge_threshold = 1.0f;
    edge.edge_threshold_min = 1.0f;
    edge.span_max = 32.0f;
    validate_fxaa(edge);
    CHECK(throws_invalid([&] { (void)fxaa_apply(FxaaImage{}, FxaaSettings{}, 1.0f); }));
}

TEST_CASE("FXAA luma: perceptual weights, the exponent, and no NaN from negative input")
{
    CHECK(near(fxaa_luma({1, 1, 1}, 1.0f), 1.0f));
    CHECK(near(fxaa_luma({1, 0, 0}, 1.0f), 0.299f));
    CHECK(near(fxaa_luma({0, 1, 0}, 1.0f), 0.587f));
    CHECK(near(fxaa_luma({0, 0, 1}, 1.0f), 0.114f));
    CHECK(near(fxaa_luma({0.25f, 0.25f, 0.25f}, 0.5f), 0.5f));  // linear 0.25 is perceptual 0.5
    CHECK(fxaa_luma({-1, -2, -3}, 0.5f) == 0.0f);
}

TEST_CASE("FXAA: flat regions, axis-aligned edges and low-contrast noise are copied exactly")
{
    const FxaaSettings s;
    const FxaaImage flat = filled(16, 12, {0.3f, 0.5f, 0.9f});
    CHECK(max_diff(fxaa_apply(flat, s, 1.0f), flat) == 0.0f);
    // A straight horizontal or vertical edge: the blend runs along the edge, through pixels of equal value, so nothing changes.
    const FxaaImage horizontal = split(16, 12, 0.0f, 6.0f);
    CHECK(max_diff(fxaa_apply(horizontal, s, 1.0f), horizontal) < 1e-6f);
    FxaaImage vertical(16, 12);
    for (uint32_t y = 0; y < 12; ++y) {
        for (uint32_t x = 0; x < 16; ++x) vertical.at(x, y) = x < 8 ? Float3{1, 1, 1} : Float3{0, 0, 0};
    }
    CHECK(max_diff(fxaa_apply(vertical, s, 1.0f), vertical) < 1e-6f);
    // Noise whose luma range stays below both thresholds: untouched, bit for bit.
    FxaaImage noise(16, 12);
    uint32_t state = 7u;
    for (Float3& p : noise.pixels) {
        state = state * 1664525u + 1013904223u;
        const float v = 0.5f + 0.02f * (static_cast<float>(state >> 8) / 16777216.0f);
        p = {v, v, v};
    }
    CHECK(max_diff(fxaa_apply(noise, s, 1.0f), noise) == 0.0f);
}

TEST_CASE("FXAA: a diagonal staircase edge is smoothed in a narrow band and nothing outside it changes")
{
    const FxaaSettings s;
    const FxaaImage src = split(48, 48, 0.5f, 10.0f);
    const FxaaImage out = fxaa_apply(src, s, 1.0f);
    uint32_t changed = 0, intermediate = 0, far_changed = 0;
    for (uint32_t y = 0; y < 48; ++y) {
        for (uint32_t x = 0; x < 48; ++x) {
            const float line = 0.5f * (static_cast<float>(x) + 0.5f) + 10.0f;
            const float distance = std::fabs((static_cast<float>(y) + 0.5f) - line) / std::sqrt(1.0f + 0.25f);  // pixels from the edge line
            const bool differs = std::fabs(out.at(x, y).x - src.at(x, y).x) > 1e-6f;
            changed += differs ? 1u : 0u;
            if (out.at(x, y).x > 0.02f && out.at(x, y).x < 0.98f) ++intermediate;
            if (differs && distance > 3.0f) ++far_changed;
        }
    }
    CHECK(changed >= 40);        // the staircase's steps (about one per column along the line) are smoothed
    CHECK(intermediate >= 30);   // and become intermediate grays
    CHECK(far_changed == 0);     // everything farther than a few pixels from the edge is untouched
    // Convexity: the result is a blend of source pixels, so it never leaves the source's range.
    for (const Float3& p : out.pixels) CHECK(p.x >= -1e-6f && p.x <= 1.0f + 1e-6f);
    // The same image gives the same result (the function is pure).
    CHECK(max_diff(fxaa_apply(src, s, 1.0f), out) == 0.0f);
}

TEST_CASE("FXAA: the thresholds decide which edges are touched, and the span limits the blend")
{
    // A gray step (not black and white): relative contrast of 0.5 against 0.3 is a luma range of 0.2.
    FxaaImage step(24, 24);
    for (uint32_t y = 0; y < 24; ++y) {
        for (uint32_t x = 0; x < 24; ++x) {
            const float line = 0.5f * (static_cast<float>(x) + 0.5f) + 6.0f;
            const float v = (static_cast<float>(y) + 0.5f) > line ? 0.5f : 0.3f;
            step.at(x, y) = {v, v, v};
        }
    }
    FxaaSettings loose;
    loose.edge_threshold = 0.125f;
    loose.edge_threshold_min = 0.0312f;
    FxaaSettings strict = loose;
    strict.edge_threshold = 0.9f;  // only contrast above 90% of the brightest neighbour counts
    FxaaSettings absolute = loose;
    absolute.edge_threshold_min = 0.5f;  // only an absolute luma range above 0.5 counts
    CHECK(max_diff(fxaa_apply(step, loose, 1.0f), step) > 0.01f);
    CHECK(max_diff(fxaa_apply(step, strict, 1.0f), step) == 0.0f);
    CHECK(max_diff(fxaa_apply(step, absolute, 1.0f), step) == 0.0f);
    // A shorter span blends less far: the smallest span changes the image less than the largest.
    FxaaSettings short_span = loose, long_span = loose;
    short_span.span_max = 1.0f;
    long_span.span_max = 32.0f;
    const FxaaImage a = fxaa_apply(step, short_span, 1.0f), b = fxaa_apply(step, long_span, 1.0f);
    CHECK(max_diff(a, step) <= max_diff(b, step) + 1e-6f);
    CHECK(max_diff(a, b) > 0.0f);
    // The exponent changes what counts as an edge: a dark step of 0.02 in linear light is below the absolute threshold as a linear value, but as a perceptual value (the square
    // root, what a linear-sampled sRGB target is judged by) it is a clear edge.
    FxaaImage dark(24, 24);
    for (uint32_t y = 0; y < 24; ++y) {
        for (uint32_t x = 0; x < 24; ++x) {
            const float line = 0.5f * (static_cast<float>(x) + 0.5f) + 6.0f;
            const float v = (static_cast<float>(y) + 0.5f) > line ? 0.02f : 0.0f;
            dark.at(x, y) = {v, v, v};
        }
    }
    CHECK(max_diff(fxaa_apply(dark, loose, 1.0f), dark) == 0.0f);
    CHECK(max_diff(fxaa_apply(dark, loose, 0.5f), dark) > 0.002f);
}
