// Temporal anti-aliasing reference: the jitter sequence, convergence on a static edge, tracking of a moving edge, ghost rejection, and the properties the shader port must keep.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/render_reference/taa.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>

using namespace saige::render_reference;

namespace {

template <class F>
bool throws(F&& f)
{
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

using Scene = std::function<float(double, double)>;  // grey level at a continuous pixel position

Image render(unsigned w, unsigned h, const Scene& scene, double jx, double jy)
{
    Image img(w, h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const float v = scene(static_cast<double>(x) + 0.5 + jx, static_cast<double>(y) + 0.5 + jy);
            img.set(x, y, {v, v, v});
        }
    return img;
}

/// The exact-ish pixel coverage: a 16 x 16 box average of the scene over each pixel.
Image truth(unsigned w, unsigned h, const Scene& scene)
{
    Image img(w, h);
    constexpr int n = 16;
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            double sum = 0.0;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) sum += static_cast<double>(scene(static_cast<double>(x) + (i + 0.5) / n, static_cast<double>(y) + (j + 0.5) / n));
            const float v = static_cast<float>(sum / (n * n));
            img.set(x, y, {v, v, v});
        }
    return img;
}

double rms(const Image& a, const Image& b)
{
    double s = 0.0;
    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        const double d = static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i]);
        s += d * d;
    }
    return std::sqrt(s / static_cast<double>(a.rgb.size()));
}

std::uint32_t hash32(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float rnd(std::uint32_t seed) { return static_cast<float>(hash32(seed) >> 8) / 16777216.0f; }

} // namespace

TEST_CASE("taa: the Halton sequence, the jitter set and the projection offsets") {
    CHECK(std::abs(halton(1, 2) - 0.5) < 1e-15);
    CHECK(std::abs(halton(2, 2) - 0.25) < 1e-15);
    CHECK(std::abs(halton(3, 2) - 0.75) < 1e-15);
    CHECK(std::abs(halton(4, 2) - 0.125) < 1e-15);
    CHECK(std::abs(halton(1, 3) - 1.0 / 3.0) < 1e-15);
    CHECK(std::abs(halton(2, 3) - 2.0 / 3.0) < 1e-15);
    CHECK(std::abs(halton(3, 3) - 1.0 / 9.0) < 1e-15);
    for (const unsigned length : {8u, 16u}) {
        double mx = 0.0, my = 0.0;
        for (unsigned f = 0; f < length; ++f) {
            const auto j = jitter_offset(f, length);
            REQUIRE(j[0] >= -0.5);
            REQUIRE(j[0] < 0.5);
            REQUIRE(j[1] >= -0.5);
            REQUIRE(j[1] < 0.5);
            mx += j[0];
            my += j[1];
            for (unsigned g = f + 1; g < length; ++g) REQUIRE((jitter_offset(g, length) != j));
        }
        CHECK(std::abs(mx / length) < 0.07);  // an unbiased set: the image does not drift
        CHECK(std::abs(my / length) < 0.07);
    }
    CHECK(jitter_offset(3, 8) == jitter_offset(11, 8));  // periodic
    const auto n = jitter_ndc({0.25, -0.25}, 1920, 1080);
    CHECK(std::abs(n[0] - 0.5 / 1920.0) < 1e-15);
    CHECK(std::abs(n[1] - 0.5 / 1080.0) < 1e-15);  // pixel y down is NDC y up
    CHECK(std::abs(texture_lod_bias(960, 1920) + 1.0) < 1e-12);
    CHECK(texture_lod_bias(1920, 1920) == 0.0);
    CHECK(throws([] { (void)halton(0, 2); }));
    CHECK(throws([] { (void)halton(1, 1); }));
    CHECK(throws([] { (void)jitter_offset(0, 0); }));
    CHECK(throws([] { (void)jitter_ndc({0, 0}, 0, 10); }));
}

TEST_CASE("taa: YCoCg round-trips and is linear") {
    for (std::uint32_t i = 0; i < 100; ++i) {
        const std::array<float, 3> c{rnd(i * 3), rnd(i * 3 + 1), rnd(i * 3 + 2)};
        const auto r = from_ycocg(to_ycocg(c));
        for (std::size_t k = 0; k < 3; ++k) REQUIRE(std::abs(r[k] - c[k]) < 1e-6f);
    }
    const auto g = to_ycocg({0.5f, 0.5f, 0.5f});  // grey has no chroma
    CHECK(std::abs(g[0] - 0.5f) < 1e-7f);
    CHECK(std::abs(g[1]) < 1e-7f);
    CHECK(std::abs(g[2]) < 1e-7f);
}

TEST_CASE("taa: a static slanted edge converges to the supersampled coverage and beats a single aliased frame") {
    constexpr unsigned W = 32, H = 32;
    const Scene edge = [](double x, double y) { return 0.8 * x + 0.6 * y > 24.3 ? 1.0f : 0.0f; };
    const Image reference = truth(W, H, edge);
    const Image aliased = render(W, H, edge, 0.0, 0.0);
    Image history = aliased;
    const MotionField still(W, H);
    for (unsigned frame = 0; frame < 96; ++frame) {
        const auto j = jitter_offset(frame, 8);
        history = taa_resolve(render(W, H, edge, j[0], j[1]), history, still, TaaParams{}, frame > 0);
    }
    const double aliased_error = rms(aliased, reference), taa_error = rms(history, reference);
    CHECK(aliased_error > 0.04);
    CHECK(taa_error < 0.5 * aliased_error);
}

TEST_CASE("taa: a moving edge is tracked with correct motion vectors and ghosts without them unless the history is clipped") {
    constexpr unsigned W = 48, H = 8;
    constexpr double speed = 0.75;
    auto edge_at = [&](double t) { return [=](double x, double) { return x < 8.0 + speed * t ? 1.0f : 0.0f; }; };  // bright behind, dark ahead; the edge moves +x
    Image good = render(W, H, edge_at(0), 0, 0), stale = good, unclipped = good;
    const MotionField correct(W, H, static_cast<float>(-speed), 0.0f), none(W, H);
    TaaParams off;
    off.clip_history = false;
    const unsigned frames = 20;
    for (unsigned f = 1; f <= frames; ++f) {
        const auto j = jitter_offset(f, 8);
        const Image cur = render(W, H, edge_at(f), j[0], j[1]);
        good = taa_resolve(cur, good, correct, TaaParams{});
        stale = taa_resolve(cur, stale, none, TaaParams{});
        unclipped = taa_resolve(cur, unclipped, none, off);
    }
    const Image ref = truth(W, H, edge_at(frames));
    CHECK(rms(good, ref) < rms(stale, ref));
    // Three pixels behind the edge the surface is solidly bright now and was dark a few frames ago. The clipped resolve shows the current value; without clipping the history ghosts.
    const unsigned px = static_cast<unsigned>(8.0 + speed * frames) - 3;
    CHECK(std::abs(stale.at(px, 4)[0] - 1.0f) < 1e-5f);
    CHECK(std::abs(unclipped.at(px, 4)[0] - 1.0f) > 0.01f);
    // The bright pixel behind the edge that just changed ghosts the most without the clip.
    double worst_clipped = 0.0, worst_free = 0.0;
    for (unsigned x = 0; x < W; ++x) {
        worst_clipped = std::max(worst_clipped, static_cast<double>(std::abs(stale.at(x, 4)[0] - ref.at(x, 4)[0])));
        worst_free = std::max(worst_free, static_cast<double>(std::abs(unclipped.at(x, 4)[0] - ref.at(x, 4)[0])));
    }
    CHECK(worst_clipped < worst_free);
}

TEST_CASE("taa: a cut, an off-screen history, a flash and a constant image behave as defined") {
    constexpr unsigned W = 16, H = 16;
    Image black(W, H), white(W, H);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) white.set(x, y, {1, 1, 1});
    const MotionField still(W, H);
    // A cut returns the current frame exactly.
    CHECK(taa_resolve(white, black, still, TaaParams{}, false).rgb == white.rgb);
    // A flat neighbourhood that disagrees with the history takes the current value at once (the clip's box is a point), so a flash does not fade in.
    const Image flash = taa_resolve(white, black, still, TaaParams{});
    for (const float v : flash.rgb) REQUIRE(std::abs(v - 1.0f) < 1e-6f);
    TaaParams off;
    off.clip_history = false;
    CHECK(std::abs(taa_resolve(white, black, still, off).at(8, 8)[0] - 0.1f) < 1e-5f);  // the unclipped blend is slow: 10% of the new value
    // History from outside the frame is unusable: the current value comes back.
    const MotionField away(W, H, 100.0f, 0.0f);
    const Image oob = taa_resolve(white, black, away, TaaParams{});
    CHECK(oob.rgb == white.rgb);
    // ... by the bounds check itself, not by the clip: with the clip off the unusable history must still be refused (it would otherwise blend in as 90% black).
    CHECK(taa_resolve(white, black, away, off).rgb == white.rgb);
    // A small motion that carries only the left column off-screen (the blend weight stays near its resting value, so only the bounds check can return the current value there).
    const MotionField edge_motion(W, H, -0.6f, 0.0f);
    const Image edge_out = taa_resolve(white, black, edge_motion, off);
    CHECK(edge_out.at(0, 8)[0] == 1.0f);                                   // history from x = -0.1: refused
    CHECK(std::abs(edge_out.at(8, 8)[0] - 0.13f) < 1e-5f);                // elsewhere 0.1 + 0.05 * 0.6 of the new value
    // The blend weight follows the motion: 0.1 at rest, 0.1 + 0.05 * |mv| in motion.
    CHECK(std::abs(taa_resolve(white, black, MotionField(W, H, 0.0f, -2.0f), off).at(8, 8)[0] - 0.2f) < 1e-5f);
    // A constant image stays constant under any motion (no brightness drift, no edge handling error).
    Image grey(W, H);
    for (unsigned y = 0; y < H; ++y)
        for (unsigned x = 0; x < W; ++x) grey.set(x, y, {0.3f, 0.5f, 0.2f});
    const MotionField mv(W, H, 0.37f, -1.6f);
    const Image kept = taa_resolve(grey, grey, mv, TaaParams{});
    for (std::size_t i = 0; i < kept.rgb.size(); ++i) REQUIRE(std::abs(kept.rgb[i] - grey.rgb[i]) < 1e-6f);
}

TEST_CASE("taa: the resolved colour never leaves the current neighbourhood's range, for any history and motion") {
    constexpr unsigned W = 24, H = 20;
    for (std::uint32_t trial = 0; trial < 6; ++trial) {
        Image cur(W, H), hist(W, H);
        MotionField mv(W, H);
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x) {
                const std::uint32_t s = trial * 100000u + y * W + x;
                cur.set(x, y, {rnd(s * 7), rnd(s * 7 + 1), rnd(s * 7 + 2)});
                hist.set(x, y, {rnd(s * 11 + 5), rnd(s * 11 + 6), rnd(s * 11 + 7)});
                mv.set(x, y, (rnd(s * 13) - 0.5f) * 6.0f, (rnd(s * 13 + 1) - 0.5f) * 6.0f);
            }
        const Image out = taa_resolve(cur, hist, mv, TaaParams{});
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x) {
                std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const unsigned nx = static_cast<unsigned>(std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(W) - 1));
                        const unsigned ny = static_cast<unsigned>(std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(H) - 1));
                        const auto s = to_ycocg(cur.at(nx, ny));
                        for (std::size_t k = 0; k < 3; ++k) {
                            lo[k] = std::min(lo[k], s[k]);
                            hi[k] = std::max(hi[k], s[k]);
                        }
                    }
                const auto o = to_ycocg(out.at(x, y));
                for (std::size_t k = 0; k < 3; ++k) {
                    REQUIRE(o[k] >= lo[k] - 1e-4f);
                    REQUIRE(o[k] <= hi[k] + 1e-4f);
                }
            }
    }
}

TEST_CASE("taa: invalid input is rejected") {
    Image a(4, 4), b(4, 5), empty;
    const MotionField m(4, 4), bad_m(5, 4);
    CHECK(throws([&] { (void)taa_resolve(a, b, m, TaaParams{}); }));
    CHECK(throws([&] { (void)taa_resolve(a, a, bad_m, TaaParams{}); }));
    CHECK(throws([&] { (void)taa_resolve(empty, empty, MotionField{}, TaaParams{}); }));
    TaaParams p;
    p.blend = 0.0f;
    CHECK(throws([&] { (void)taa_resolve(a, a, m, p); }));
    p = TaaParams{};
    p.gamma = -1.0f;
    CHECK(throws([&] { (void)taa_resolve(a, a, m, p); }));
}
