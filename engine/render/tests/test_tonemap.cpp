#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/tonemap.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace saige::render;

namespace {

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

ToneMapSettings settings(ToneMapOperator op, float ev = 0.0f, float white = 4.0f)
{
    ToneMapSettings s;
    s.op = op;
    s.exposure_ev = ev;
    s.white_point = white;
    return s;
}

} // namespace

TEST_CASE("Tone map: the default settings use the ACES fit with no exposure offset")
{
    const ToneMapSettings defaults;
    CHECK(defaults.op == ToneMapOperator::AcesFitted);
    CHECK(defaults.exposure_ev == 0.0f);
    const Float3 mapped = tone_map({1.0f, 1.0f, 1.0f}, defaults);
    CHECK(near(mapped.x, 2.54f / 3.16f, 1e-5f));
}

TEST_CASE("Tone map: Linear is the identity inside [0, 1] and clips above")
{
    const ToneMapSettings s = settings(ToneMapOperator::Linear);
    for (const float v : {0.0f, 0.1f, 0.5f, 0.999f, 1.0f}) {
        const Float3 out = tone_map({v, v * 0.5f, v * 0.25f}, s);
        CHECK(near(out.x, v));
        CHECK(near(out.y, v * 0.5f));
        CHECK(near(out.z, v * 0.25f));
    }
    CHECK(tone_map({3.0f, 100.0f, 1.5f}, s) == Float3(1.0f, 1.0f, 1.0f));
}

TEST_CASE("Tone map: exposure scales the input by 2^EV before the operator")
{
    CHECK(near(exposure_scale(0.0f), 1.0f));
    CHECK(near(exposure_scale(1.0f), 2.0f));
    CHECK(near(exposure_scale(-2.0f), 0.25f));
    const Float3 base = tone_map({0.25f, 0.25f, 0.25f}, settings(ToneMapOperator::Linear, 0.0f));
    const Float3 plus_one = tone_map({0.25f, 0.25f, 0.25f}, settings(ToneMapOperator::Linear, 1.0f));
    CHECK(near(base.x, 0.25f));
    CHECK(near(plus_one.x, 0.5f));
    // +EV on the input equals -EV of headroom for a curve that is not linear: the same input at +1 EV equals twice the input at 0 EV.
    for (const ToneMapOperator op : {ToneMapOperator::Reinhard, ToneMapOperator::AcesFitted}) {
        const Float3 a = tone_map({0.5f, 0.5f, 0.5f}, settings(op, 1.0f));
        const Float3 b = tone_map({1.0f, 1.0f, 1.0f}, settings(op, 0.0f));
        CHECK(near(a.x, b.x));
    }
}

TEST_CASE("Tone map: the Reinhard white point maps to 1 and smaller values stay below it")
{
    for (const float white : {1.0f, 2.0f, 4.0f, 10.0f}) {
        const ToneMapSettings s = settings(ToneMapOperator::Reinhard, 0.0f, white);
        CHECK(near(tone_map({white, white, white}, s).x, 1.0f, 1e-5f));
        CHECK(tone_map({white * 0.5f, 0.0f, 0.0f}, s).x < 1.0f);
        CHECK(near(tone_map({0.0f, 0.0f, 0.0f}, s).x, 0.0f));
    }
    CHECK(tone_map({1000.0f, 1000.0f, 1000.0f}, settings(ToneMapOperator::Reinhard)).x == 1.0f);  // saturated above the white point
}

TEST_CASE("Tone map: the ACES fit has its known values and saturates")
{
    const ToneMapSettings s = settings(ToneMapOperator::AcesFitted);
    CHECK(near(tone_map({0.0f, 0.0f, 0.0f}, s).x, 0.0f));
    CHECK(near(tone_map({1.0f, 1.0f, 1.0f}, s).x, 2.54f / 3.16f, 1e-5f));  // (2.51 + 0.03) / (2.43 + 0.59 + 0.14)
    CHECK(near(tone_map({0.18f, 0.18f, 0.18f}, s).x, (0.18f * (2.51f * 0.18f + 0.03f)) / (0.18f * (2.43f * 0.18f + 0.59f) + 0.14f), 1e-6f));
    CHECK(tone_map({1000.0f, 1000.0f, 1000.0f}, s).x == 1.0f);
}

TEST_CASE("Tone map: every operator is monotonic, bounded to [0, 1] and independent per channel")
{
    for (const ToneMapOperator op : {ToneMapOperator::Linear, ToneMapOperator::Reinhard, ToneMapOperator::AcesFitted}) {
        const ToneMapSettings s = settings(op);
        float previous = -1.0f;
        for (int i = 0; i <= 400; ++i) {
            const float v = static_cast<float>(i) * 0.05f;
            const Float3 out = tone_map({v, 0.3f, 7.0f}, s);
            CHECK(out.x >= previous - 1e-6f);
            CHECK(out.x >= 0.0f);
            CHECK(out.x <= 1.0f);
            previous = out.x;
            // The other channels do not depend on the first.
            const Float3 other = tone_map({0.0f, 0.3f, 7.0f}, s);
            CHECK(out.y == other.y);
            CHECK(out.z == other.z);
        }
    }
}

TEST_CASE("Tone map: NaN, negative and infinite input never produce a non-finite result")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const ToneMapOperator op : {ToneMapOperator::Linear, ToneMapOperator::Reinhard, ToneMapOperator::AcesFitted}) {
        const Float3 out = tone_map({nan, -5.0f, inf}, settings(op));
        CHECK(out.x == 0.0f);  // NaN becomes 0
        CHECK(out.y == 0.0f);  // negatives become 0
        CHECK(out.z == 1.0f);  // +inf is clamped to the largest half float, which every operator saturates
        const Float3 neg_inf = tone_map({-inf, 0.5f, 0.5f}, settings(op));
        CHECK(neg_inf.x == 0.0f);
    }
    CHECK(std::isfinite(tone_map({nan, nan, nan}, settings(ToneMapOperator::Reinhard, 30.0f)).x));  // huge exposure stays finite
}

// ----------------------------------------------------------------------------------------------- review findings on #48

TEST_CASE("Tone map: any white point or exposure, including zero, negative, NaN, infinite or underflowing, still gives finite output in [0, 1]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float inputs[] = {0.0f, 1e-30f, 1e-6f, 0.5f, 1.0f, 100.0f, 65504.0f, 1e30f, inf, nan, -5.0f, -inf};
    for (const float white : {0.0f, -3.0f, nan, inf, -inf, 1e-30f, 1e-20f, 1e-3f, 1e30f}) {
        for (const float ev : {0.0f, -2.0f, 100.0f, -100.0f, nan, inf, -inf}) {
            ToneMapSettings s = settings(ToneMapOperator::Reinhard, ev);
            s.white_point = white;
            for (const float x : inputs) {
                const Float3 out = tone_map({x, x * 0.5f, 0.0f}, s);
                for (const float c : {out.x, out.y, out.z}) {
                    CHECK(std::isfinite(c));
                    CHECK(c >= 0.0f);
                    CHECK(c <= 1.0f);
                }
            }
        }
    }
    // Black stays black for a zero white point: 0 * (1 + 0 / 0) used to be NaN.
    ToneMapSettings zero = settings(ToneMapOperator::Reinhard);
    zero.white_point = 0.0f;
    CHECK(tone_map({0.0f, 0.0f, 0.0f}, zero).x == 0.0f);
}

TEST_CASE("Tone map: sanitize clamps invalid settings, keeps valid ones, and is what the reference applies")
{
    const ToneMapSettings defaults;
    const ToneMapSettings same = sanitize(defaults);
    CHECK(same.op == defaults.op);
    CHECK(same.exposure_ev == defaults.exposure_ev);
    CHECK(same.white_point == defaults.white_point);
    ToneMapSettings s = settings(ToneMapOperator::Reinhard, 3.0f);
    s.white_point = 8.0f;
    CHECK(sanitize(s).white_point == 8.0f);
    CHECK(sanitize(s).exposure_ev == 3.0f);
    s.white_point = 0.0f;
    CHECK(sanitize(s).white_point == kMinWhitePoint);
    s.white_point = 1e30f;
    CHECK(sanitize(s).white_point == kMaxWhitePoint);
    s.white_point = std::numeric_limits<float>::quiet_NaN();
    CHECK(sanitize(s).white_point == defaults.white_point);
    s.exposure_ev = std::numeric_limits<float>::quiet_NaN();
    CHECK(sanitize(s).exposure_ev == 0.0f);
    s.exposure_ev = -1000.0f;
    CHECK(sanitize(s).exposure_ev == -kMaxExposureEv);
    // The reference gives identical output for a setting and its sanitized form.
    ToneMapSettings bad = settings(ToneMapOperator::Reinhard, 1.0f);
    bad.white_point = 0.0f;
    const Float3 a = tone_map({2.0f, 0.3f, 7.0f}, bad);
    const Float3 b = tone_map({2.0f, 0.3f, 7.0f}, sanitize(bad));
    CHECK(a.x == b.x);
    CHECK(a.y == b.y);
    CHECK(a.z == b.z);
}

TEST_CASE("ToneMapper: a failure at any acquisition releases everything acquired before it")
{
    // A tone mapper acquires five device objects in a row (layout, two shaders, pipeline, sampler). A throw from the constructor means the
    // destructor never runs, so each earlier object must be released by the constructor itself.
    for (uint32_t failing = 1; failing <= 5; ++failing) {
        saige::rhi::NullDevice device;
        const uint32_t before = device.live_resource_count();
        device.fail_creation_after(failing);
        bool threw = false;
        try {
            ToneMapper mapper(device, saige::rhi::Format::RGBA8_SRGB);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        device.fail_creation_after(0);
        device.wait_idle();  // destruction is deferred until frames retire
        CHECK(device.live_resource_count() == before);
        CHECK(device.validation_error_count() == 0);
    }
    // And with no failure it constructs and releases cleanly.
    saige::rhi::NullDevice device;
    const uint32_t before = device.live_resource_count();
    {
        ToneMapper mapper(device, saige::rhi::Format::RGBA8_SRGB);
        CHECK(device.live_resource_count() == before + 5);
    }
    device.wait_idle();
    CHECK(device.live_resource_count() == before);
}
