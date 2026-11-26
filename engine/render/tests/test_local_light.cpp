// The host light set: the 64-byte record, validation, capacity and overflow. The GPU side (the shader reading these bytes) is checked in rhi_vulkan/tests/test_local_lights.cpp, which builds its lights through this class.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/local_light.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace saige::render;

namespace {

template <typename Fn>
bool throws(Fn&& fn)
{
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

} // namespace

TEST_CASE("LocalLightSet: the record is 64 bytes and the fields sit where the shader reads them")
{
    CHECK(sizeof(LocalLightGpu) == 64);
    LocalLightSet set(8);
    PointLightDesc p;
    p.position = {1, 2, 3};
    p.color = {0.25f, 0.5f, 0.75f};
    p.intensity = 900.0f;
    p.radius = 12.0f;
    CHECK(set.add_point(p) == 0);
    const LocalLightGpu& g = set.lights()[0];
    CHECK(g.position_radius[0] == 1 && g.position_radius[1] == 2 && g.position_radius[2] == 3 && g.position_radius[3] == 12.0f);
    CHECK(g.color_intensity[0] == 0.25f && g.color_intensity[1] == 0.5f && g.color_intensity[2] == 0.75f && g.color_intensity[3] == 900.0f);
    CHECK(g.direction_outer[3] == -2.0f);        // a point light has no cone
    CHECK(g.cone[0] == 1.0f && g.cone[1] == 1.0f);
    SpotLightDesc s;
    s.position = {-4, 5, 6};
    s.direction = {0, 0, -1};
    s.intensity = 5000.0f;
    s.radius = 30.0f;
    s.inner_angle = 0.2f;
    s.outer_angle = 0.6f;
    CHECK(set.add_spot(s) == 1);
    const LocalLightGpu& h = set.lights()[1];
    CHECK(h.direction_outer[2] == -1.0f && std::fabs(h.direction_outer[3] - std::cos(0.6f)) < 1e-6f);
    CHECK(std::fabs(h.cone[0] - std::cos(0.2f)) < 1e-6f);
    CHECK(std::fabs(h.cone[1] - 1.0f / (std::cos(0.2f) - std::cos(0.6f))) < 1e-3f);
    // The bytes to upload are exactly the records.
    CHECK(set.bytes().size() == 2 * 64);
    CHECK(std::memcmp(set.bytes().data(), set.lights().data(), 128) == 0);
}

TEST_CASE("LocalLightSet: zero lights are valid and upload zero bytes")
{
    LocalLightSet set(4);
    CHECK(set.size() == 0 && set.bytes().empty() && set.lights().empty() && set.rejected() == 0);
    PointLightDesc p;
    set.add_point(p);
    set.clear();
    CHECK(set.size() == 0 && set.bytes().empty());
}

TEST_CASE("LocalLightSet: a light that cannot be lit correctly is refused, and nothing is stored for it")
{
    LocalLightSet set(8);
    const auto bad_point = [&](auto edit) {
        PointLightDesc p;
        edit(p);
        return throws([&] { set.add_point(p); });
    };
    CHECK(bad_point([](PointLightDesc& p) { p.position[0] = kNan; }));
    CHECK(bad_point([](PointLightDesc& p) { p.position[1] = kInf; }));
    CHECK(bad_point([](PointLightDesc& p) { p.position[2] = 2e7f; }));
    CHECK(bad_point([](PointLightDesc& p) { p.color[0] = -0.1f; }));
    CHECK(bad_point([](PointLightDesc& p) { p.color[2] = kNan; }));
    CHECK(bad_point([](PointLightDesc& p) { p.intensity = -1.0f; }));
    CHECK(bad_point([](PointLightDesc& p) { p.intensity = kInf; }));
    CHECK(bad_point([](PointLightDesc& p) { p.intensity = 2e9f; }));
    CHECK(bad_point([](PointLightDesc& p) { p.radius = 0.0f; }));
    CHECK(bad_point([](PointLightDesc& p) { p.radius = -3.0f; }));
    CHECK(bad_point([](PointLightDesc& p) { p.radius = kNan; }));
    CHECK(bad_point([](PointLightDesc& p) { p.radius = 2e5f; }));
    const auto bad_spot = [&](auto edit) {
        SpotLightDesc s;
        edit(s);
        return throws([&] { set.add_spot(s); });
    };
    CHECK(bad_spot([](SpotLightDesc& s) { s.direction = {0, 0, -2}; }));          // not a unit vector
    CHECK(bad_spot([](SpotLightDesc& s) { s.direction = {0, 0, 0}; }));
    CHECK(bad_spot([](SpotLightDesc& s) { s.direction = {kNan, 0, -1}; }));
    CHECK(bad_spot([](SpotLightDesc& s) { s.outer_angle = 0.0f; }));
    CHECK(bad_spot([](SpotLightDesc& s) { s.outer_angle = 3.2f; }));              // above pi
    CHECK(bad_spot([](SpotLightDesc& s) { s.inner_angle = -0.1f; }));
    CHECK(bad_spot([](SpotLightDesc& s) { s.inner_angle = 0.7f; s.outer_angle = 0.5f; }));
    CHECK(bad_spot([](SpotLightDesc& s) { s.outer_angle = kNan; }));
    CHECK(bad_spot([](SpotLightDesc& s) { s.radius = 0.0f; }));
    CHECK(set.size() == 0 && set.rejected() == 0);    // refusing is not rejecting for capacity: nothing was stored and nothing was counted
    // The edges that are valid: a spot whose outer angle is pi, one whose inner equals its outer.
    SpotLightDesc wide;
    wide.inner_angle = 3.0f;
    wide.outer_angle = 3.14159265f;
    CHECK(set.add_spot(wide) == 0);
    SpotLightDesc hard;
    hard.inner_angle = hard.outer_angle = 0.4f;
    CHECK(set.add_spot(hard) == 1);
    CHECK(std::fabs(set.lights()[1].cone[1] - 1.0f / 1e-4f) < 1.0f);   // a hard edge: the scale is capped
}

TEST_CASE("LocalLightSet: the capacity is a bound, not a crash: lights beyond it are not stored and are counted")
{
    LocalLightSet set(3);
    PointLightDesc p;
    for (int i = 0; i < 3; ++i) CHECK(set.add_point(p) == i);
    CHECK(set.add_point(p) == -1 && set.add_point(p) == -1);
    SpotLightDesc s;
    CHECK(set.add_spot(s) == -1);
    CHECK(set.size() == 3 && set.rejected() == 3 && set.bytes().size() == 3 * 64);
    set.clear();
    CHECK(set.size() == 0 && set.rejected() == 0 && set.add_point(p) == 0);
    CHECK(throws([] { LocalLightSet bad(0); }));
    CHECK(throws([] { LocalLightSet bad(65537); }));
    LocalLightSet exact(65536);   // the largest capacity is accepted
    CHECK(exact.capacity() == 65536);
}
