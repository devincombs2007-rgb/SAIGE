#include "saige/capsule_sweep.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using saige::Aabb;
using saige::Capsule;
using saige::Vec3;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

// ---------------------------------------------------------------------------------------------------------------------------
// Independent reference. For a vertical segment and an axis-aligned box the distance separates per axis, so it can be computed
// directly with no Minkowski construction (which is what the implementation under test uses). All in double.

struct Reference {
    double dx, dy, dz;  // signed per-axis offsets from the box to the capsule axis (0 where they overlap)
    double distance() const { return std::sqrt(dx * dx + dy * dy + dz * dz); }
};

double interval_offset(double lo, double hi, double a, double b) {
    // Signed gap from [lo, hi] to [a, b]: positive when [a, b] is above, negative when below, zero when they overlap.
    if (a > hi) return a - hi;
    if (b < lo) return b - lo;
    return 0.0;
}

Reference reference_at(const Capsule& capsule, const Vec3& shift, const Aabb& box) {
    const double r = capsule.radius;
    const double x = static_cast<double>(capsule.feet.x) + shift.x;
    const double y = static_cast<double>(capsule.feet.y) + shift.y;
    const double bottom = static_cast<double>(capsule.feet.z) + shift.z + r;
    const double top = static_cast<double>(capsule.feet.z) + shift.z + capsule.height - r;
    return {interval_offset(box.min.x, box.max.x, x, x), interval_offset(box.min.y, box.max.y, y, y),
            interval_offset(box.min.z, box.max.z, bottom, top)};
}

double reference_gap(const Capsule& capsule, const Vec3& shift, const Aabb& box) {
    return reference_at(capsule, shift, box).distance() - capsule.radius;
}

Vec3 scaled(Vec3 v, double t) {
    return {static_cast<float>(v.x * t), static_cast<float>(v.y * t), static_cast<float>(v.z * t)};
}

// ---------------------------------------------------------------------------------------------------------------------------

struct Random {
    std::uint32_t state;
    explicit Random(std::uint32_t seed) : state(seed) {}
    double next() {  // [0, 1)
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 16777216.0;
    }
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
};

void test_randomized_against_reference() {
    Random random(2024);
    constexpr int scenarios = 6000;
    constexpr int samples = 3000;
    int hits = 0, misses = 0;
    std::array<int, 4> by_region{};  // index = number of axes on which the contact is off the box: 1 face, 2 edge, 3 corner

    for (int n = 0; n < scenarios; ++n) {
        const Vec3 box_min{static_cast<float>(random.range(-3, 3)), static_cast<float>(random.range(-3, 3)),
                           static_cast<float>(random.range(-1, 1))};
        const Aabb box{box_min, {box_min.x + static_cast<float>(random.range(0.2, 4)), box_min.y + static_cast<float>(random.range(0.2, 4)),
                                 box_min.z + static_cast<float>(random.range(0.2, 4))}};
        const float radius = static_cast<float>(random.range(0.1, 0.6));
        Capsule capsule{{static_cast<float>(random.range(-8, 8)), static_cast<float>(random.range(-8, 8)),
                         static_cast<float>(random.range(-4, 4))},
                        radius, 2.0f * radius + static_cast<float>(random.range(0, 2))};
        if (reference_gap(capsule, {}, box) < 0.02) {  // start clear of the box so the first contact is a real transition
            --n;
            continue;
        }
        // Aim at the box center, a random corner, or nowhere in particular, then overshoot or stop short.
        const Vec3 center{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, (box.min.z + box.max.z) * 0.5f};
        Vec3 target = center;
        const int mode = n % 3;
        if (mode == 1) {
            target = {random.next() < 0.5 ? box.min.x : box.max.x, random.next() < 0.5 ? box.min.y : box.max.y,
                      random.next() < 0.5 ? box.min.z : box.max.z};
        } else if (mode == 2) {
            target = {static_cast<float>(random.range(-6, 6)), static_cast<float>(random.range(-6, 6)), static_cast<float>(random.range(-3, 3))};
        }
        const double length = random.range(0.4, 1.6);
        const Vec3 displacement{static_cast<float>((target.x - capsule.feet.x) * length), static_cast<float>((target.y - capsule.feet.y) * length),
                                static_cast<float>((target.z - capsule.feet.z) * length)};

        // Reference: first sampled time at which the capsule overlaps the box (gap <= 0).
        int first = -1;
        double minimum_gap = std::numeric_limits<double>::infinity();
        for (int i = 0; i <= samples; ++i) {
            const double g = reference_gap(capsule, scaled(displacement, static_cast<double>(i) / samples), box);
            minimum_gap = std::min(minimum_gap, g);
            if (first < 0 && g <= 0.0) first = i;
        }

        const auto hit = saige::sweep_capsule_box(capsule, displacement, box);
        const std::string label = "scenario " + std::to_string(n);
        if (!hit) {
            ++misses;
            require(minimum_gap > -2e-3, label + ": reported a clear path but the capsule overlaps the box by " + std::to_string(-minimum_gap));
            continue;
        }
        ++hits;
        const double t = hit->t;
        require(t >= 0.0 && t <= 1.0 && !hit->started_overlapping, label + ": t out of range or flagged as overlapping");
        // Contact: the capsule at t touches the box (to float rounding) ...
        require(near(reference_gap(capsule, scaled(displacement, t), box), 0.0, 3e-4), label + ": the capsule does not touch the box at the reported t");
        // ... no earlier sample penetrates ...
        for (int i = 0; i <= samples; ++i) {
            const double ti = static_cast<double>(i) / samples;
            if (ti < t - 1e-3) {
                require(reference_gap(capsule, scaled(displacement, ti), box) > -3e-4, label + ": the capsule penetrated before the reported t");
            }
        }
        // ... and where the dense search first sees overlap, t is within one sample of it.
        require(first >= 0, label + ": a hit was reported although the dense search never saw overlap (only grazes may do this)" +
                                 (minimum_gap < 5e-3 ? std::string() : std::string(" and the minimum gap was large")));
        require(t >= static_cast<double>(first - 1) / samples - 1e-6 && t <= static_cast<double>(first) / samples + 1e-6,
                label + ": t is not within one sample of the first overlap");

        // Normal: the reference gradient at the contact (the direction from the box to the capsule axis).
        const Reference ref = reference_at(capsule, scaled(displacement, t), box);
        const double length_ref = ref.distance();
        require(length_ref > 1e-6, label + ": degenerate contact");
        const double nx = ref.dx / length_ref, ny = ref.dy / length_ref, nz = ref.dz / length_ref;
        require(near(hit->normal.x, nx, 2e-3) && near(hit->normal.y, ny, 2e-3) && near(hit->normal.z, nz, 2e-3),
                label + ": the contact normal does not match the reference gradient");
        require(near(hit->normal.x * hit->normal.x + hit->normal.y * hit->normal.y + hit->normal.z * hit->normal.z, 1.0, 1e-5),
                label + ": the normal is not unit length");
        require(hit->normal.x * displacement.x + hit->normal.y * displacement.y + hit->normal.z * displacement.z < 0.0,
                label + ": the capsule is not moving into the surface it hit");
        const int off_axes = (std::abs(ref.dx) > 1e-9) + (std::abs(ref.dy) > 1e-9) + (std::abs(ref.dz) > 1e-9);
        ++by_region[static_cast<std::size_t>(off_axes)];
    }
    // The test must exercise every kind of contact or it proves little.
    require(hits >= 2000 && misses >= 1000, "too few hits or misses for a meaningful sample: " + std::to_string(hits) + "/" + std::to_string(misses));
    require(by_region[1] >= 300 && by_region[2] >= 100 && by_region[3] >= 30,
            "too few face/edge/corner contacts: " + std::to_string(by_region[1]) + "/" + std::to_string(by_region[2]) + "/" + std::to_string(by_region[3]));
}

void test_exact_cases() {
    const Aabb wall{{0, -5, 0}, {1, 5, 3}};
    const Capsule c{{-2, 0, 0}, 0.5f, 1.8f};

    // Straight at a face: center x = -2 reaches the grown face at x = -0.5, i.e. t = 1.5 / 4.
    auto hit = saige::sweep_capsule_box(c, {4, 0, 0}, wall);
    require(hit && near(hit->t, 0.375, 1e-6), "face hit time");
    require(hit->normal == Vec3{-1, 0, 0} && !hit->started_overlapping, "face normal points out of the wall");
    require(near(hit->point.x, 0.0, 1e-6) && near(hit->point.y, 0.0, 1e-6), "contact point lies on the wall face");

    // Stopping short, moving away and a parallel path all miss.
    require(!saige::sweep_capsule_box(c, {1.4f, 0, 0}, wall), "stops 0.1 short of contact");
    require(!saige::sweep_capsule_box(c, {-4, 0, 0}, wall), "moving away");
    require(!saige::sweep_capsule_box(c, {0, 8, 0}, wall), "sliding parallel to the wall in the clear");
    require(!saige::sweep_capsule_box(c, {0, 0, 0}, wall), "zero displacement from a clear start");

    // Landing on top: capsule above the box, falling. Feet reach the top face at z = 3.
    const Capsule above{{0.5f, 0, 6}, 0.3f, 1.7f};
    hit = saige::sweep_capsule_box(above, {0, 0, -10}, wall);
    require(hit && near(hit->t, 0.3, 1e-6) && hit->normal == Vec3{0, 0, 1}, "landing on a top face");
    require(near(hit->point.z, 3.0, 1e-6), "landing contact point is on the top face");

    // Head against an overhang: rising capsule below the box.
    const Aabb ceiling{{-2, -2, 3}, {2, 2, 4}};
    const Capsule below{{0, 0, 0}, 0.3f, 1.7f};  // top of the capsule at z = 1.7
    hit = saige::sweep_capsule_box(below, {0, 0, 3}, ceiling);
    require(hit && near(hit->t, (3.0 - 1.7) / 3.0, 1e-6) && hit->normal == Vec3{0, 0, -1}, "head against a ceiling");

    // A capsule is a rounded shape: passing the box corner closer than the radius in x and y but outside the rounding misses.
    const Aabb pillar{{0, 0, 0}, {1, 1, 3}};
    const Capsule diagonal{{2.0f, 2.0f, 0}, 0.5f, 1.8f};
    require(!saige::sweep_capsule_box(diagonal, {-0.6f, 0.0f, 0}, pillar), "passes diagonally outside the rounded edge");
    hit = saige::sweep_capsule_box(diagonal, {-3.0f, -3.0f, 0}, pillar);
    require(hit && near(hit->normal.x, std::sqrt(0.5), 1e-4) && near(hit->normal.y, std::sqrt(0.5), 1e-4) && near(hit->normal.z, 0, 1e-6),
            "a vertical edge gives a diagonal normal");
    // The corner of the grown box is at distance sqrt(2) * 0.5 from the edge line; the rounded edge makes contact later than a
    // box of half-width 0.5 would: centers meet at distance 0.5 from the edge (1, 1), i.e. 0.5 * sqrt(2) along the diagonal.
    require(near(hit->t, (std::sqrt(2.0) - 0.5) / std::sqrt(18.0), 1e-5), "rounded edge contact time");

    // A sphere (height = 2 * radius) is a legal capsule.
    const Capsule ball{{-2, 0, 1.0f}, 0.5f, 1.0f};
    hit = saige::sweep_capsule_box(ball, {4, 0, 0}, wall);
    require(hit && near(hit->t, 0.375, 1e-6), "a sphere sweeps like a capsule");
}

void test_touching_and_overlap() {
    const Aabb floor{{-10, -10, -1}, {10, 10, 0}};
    const Capsule standing{{0, 0, 0}, 0.3f, 1.7f};  // feet exactly on the floor top
    require(near(saige::capsule_box_gap(standing, floor), 0.0, 1e-6), "standing on the floor has no gap");

    // Resting contact: moving along the floor or away does not hit; pushing down is blocked at t = 0.
    require(!saige::sweep_capsule_box(standing, {3, 0, 0}, floor), "walking along the floor");
    require(!saige::sweep_capsule_box(standing, {0, 0, 2}, floor), "jumping off the floor");
    auto hit = saige::sweep_capsule_box(standing, {0, 0, -1}, floor);
    require(hit && hit->t == 0.0f && hit->normal == Vec3{0, 0, 1} && !hit->started_overlapping, "pushing into the floor is blocked at once");

    // Touching a wall: sliding along it passes, pressing into it is blocked.
    const Aabb wall{{1, -5, 0}, {2, 5, 3}};
    const Capsule at_wall{{0.7f, 0, 0}, 0.3f, 1.7f};  // surface touches x = 1
    require(!saige::sweep_capsule_box(at_wall, {0, 4, 0}, wall), "sliding along a touched wall");
    hit = saige::sweep_capsule_box(at_wall, {0.5f, 0, 0}, wall);
    require(hit && hit->t == 0.0f && hit->normal == Vec3{-1, 0, 0}, "pressing into a touched wall");

    // Started overlapping: the push-out is along the nearest face, and the move is stopped at t = 0.
    const Capsule sunk{{0.0f, 0, -0.1f}, 0.3f, 1.7f};
    hit = saige::sweep_capsule_box(sunk, {1, 0, 0}, floor);
    require(hit && hit->started_overlapping && hit->t == 0.0f, "overlap is reported");
    require(hit->normal == Vec3{0, 0, 1}, "a capsule sunk into a floor is pushed up");
    require(saige::capsule_box_gap(sunk, floor) < 0.0f, "negative gap while overlapping");
}

Capsule moved(Capsule c, Vec3 by) {
    c.feet = {c.feet.x + by.x, c.feet.y + by.y, c.feet.z + by.z};
    return c;
}

void test_push_out() {
    Random random(77);
    int overlapping = 0, clear = 0;
    for (int n = 0; n < 3000; ++n) {
        const Vec3 box_min{static_cast<float>(random.range(-2, 2)), static_cast<float>(random.range(-2, 2)), static_cast<float>(random.range(-1, 1))};
        const Aabb box{box_min, {box_min.x + static_cast<float>(random.range(0.3, 3)), box_min.y + static_cast<float>(random.range(0.3, 3)),
                                 box_min.z + static_cast<float>(random.range(0.3, 3))}};
        const float radius = static_cast<float>(random.range(0.1, 0.5));
        const float height = 2.0f * radius + static_cast<float>(random.range(0, 1.5));
        // Close to the box on every side, so roughly half the scenes overlap.
        const Capsule capsule{{static_cast<float>(random.range(box.min.x - 1.5, box.max.x + 1.5)),
                               static_cast<float>(random.range(box.min.y - 1.5, box.max.y + 1.5)),
                               static_cast<float>(random.range(box.min.z - height - 0.8, box.max.z + 0.8))},
                              radius, height};
        const double gap = reference_gap(capsule, {}, box);
        const auto push = saige::capsule_box_push_out(capsule, box);
        if (gap >= -1e-5) {
            ++clear;
            require(!push, "a capsule that is not overlapping needs no push");
            continue;
        }
        // The reference gap saturates once the capsule axis is inside the box, so only judge depth when it is outside.
        ++overlapping;
        require(push.has_value(), "an overlapping capsule needs a push");
        const double length = std::sqrt(static_cast<double>(push->x) * push->x + static_cast<double>(push->y) * push->y +
                                        static_cast<double>(push->z) * push->z);
        // Applying the push leaves the capsule touching (not overlapping, and not floating away).
        const double after = reference_gap(moved(capsule, *push), {}, box);
        require(after > -2e-4 && after < 2e-4, "after the push the capsule touches the box");
        // Minimal: no axis-aligned push of the six that clears the box is shorter than the reported one.
        double best_axis = std::numeric_limits<double>::infinity();
        for (int axis = 0; axis < 3; ++axis) {
            for (const double sign : {-1.0, 1.0}) {
                for (double d = 0.0; d < 8.0; d += 0.001) {
                    Vec3 by{0, 0, 0};
                    (axis == 0 ? by.x : axis == 1 ? by.y : by.z) = static_cast<float>(sign * d);
                    if (reference_gap(moved(capsule, by), {}, box) >= -1e-9) {
                        best_axis = std::min(best_axis, d);
                        break;
                    }
                }
            }
        }
        require(length <= best_axis + 2e-3, "the push is no longer than the best axis-aligned alternative");
    }
    require(overlapping >= 400 && clear >= 400, "push-out test needs both overlapping and clear scenes: " + std::to_string(overlapping) + "/" + std::to_string(clear));

    // Exact cases: sunk into a floor, and into a wall.
    const Aabb floor{{-10, -10, -1}, {10, 10, 0}};
    auto push = saige::capsule_box_push_out({{0, 0, -0.1f}, 0.3f, 1.7f}, floor);
    require(push && push->x == 0.0f && push->y == 0.0f && near(push->z, 0.1, 1e-6), "sunk 0.1 into a floor: push up 0.1");
    const Aabb wall{{1, -5, 0}, {2, 5, 3}};
    push = saige::capsule_box_push_out({{0.9f, 0, 0}, 0.3f, 1.7f}, wall);  // surface reaches x = 1.2, 0.2 into the wall
    require(push && near(push->x, -0.2, 1e-6) && push->y == 0.0f && push->z == 0.0f, "into a wall: push back out along x");
    require(!saige::capsule_box_push_out({{0.7f, 0, 0}, 0.3f, 1.7f}, wall), "touching is not overlapping");
    // A capsule whose axis is inside the box leaves through the nearest face, deterministically.
    push = saige::capsule_box_push_out({{1.5f, 0, 0.4f}, 0.3f, 1.7f}, wall);
    require(push.has_value(), "axis inside the box");
    require(!saige::capsule_box_push_out({{50, 0, 0}, 0.3f, 1.7f}, wall), "far away");
}

void test_multiple_boxes_and_ties() {
    const std::vector<Aabb> boxes{{{5, -1, 0}, {6, 1, 2}}, {{2, -1, 0}, {3, 1, 2}}, {{2, -1, 0}, {3, 1, 2}}};
    const Capsule c{{0, 0, 0}, 0.3f, 1.7f};
    const auto hit = saige::sweep_capsule_boxes(c, {10, 0, 0}, boxes);
    require(hit && hit->index == 1 && near(hit->t, 0.17, 1e-6), "the nearest box wins and ties go to the lowest index");
    require(!saige::sweep_capsule_boxes(c, {10, 0, 0}, std::span<const Aabb>{}), "no boxes, no hit");
    require(!saige::sweep_capsule_boxes(c, {1, 0, 0}, boxes), "none reached");
}

void test_slopes() {
    const auto tilted = [](double degrees) {
        const double r = degrees * 3.14159265358979323846 / 180.0;
        return Vec3{static_cast<float>(std::sin(r)), 0.0f, static_cast<float>(std::cos(r))};
    };
    constexpr float limit = 45.0f * 3.14159265f / 180.0f;
    require(saige::is_walkable({0, 0, 1}, limit), "flat ground");
    require(saige::is_walkable(tilted(30), limit), "30 degrees");
    require(saige::is_walkable(tilted(44.9), limit), "just under the limit");
    require(!saige::is_walkable(tilted(45.5), limit), "just over the limit");
    require(!saige::is_walkable(tilted(80), limit) && !saige::is_walkable({1, 0, 0}, limit) && !saige::is_walkable({0, 0, -1}, limit),
            "steep walls, and a ceiling, are not ground");
    require(saige::is_walkable(tilted(45.0), limit), "exactly at the limit counts (within rounding)");
    // 45 degrees is the one limit where sine and cosine agree, so also check limits on either side of it.
    constexpr float steep_limit = 60.0f * 3.14159265f / 180.0f;
    constexpr float gentle_limit = 30.0f * 3.14159265f / 180.0f;
    require(saige::is_walkable(tilted(50), steep_limit) && !saige::is_walkable(tilted(70), steep_limit), "a 60 degree limit");
    require(saige::is_walkable(tilted(20), gentle_limit) && !saige::is_walkable(tilted(40), gentle_limit), "a 30 degree limit");
    require(saige::is_walkable({0, 0, 1}, 0.0f) && !saige::is_walkable(tilted(1), 0.0f), "a zero limit allows only level ground");
    require(!saige::is_walkable({0, 0, 1}, -0.1f), "a negative limit is invalid");
    // The defined range of a slope limit is [0, pi/2): outside it nothing is ground, and a ceiling never is.
    constexpr float pi = 3.14159265f;
    const saige::Vec3 ceiling{0, 0, -1}, wall{1, 0, 0}, nearly_flat{0, 0, 1};
    for (const float bad_limit : {pi / 2.0f, 1.6f, pi, 4.0f, 1e6f, std::numeric_limits<float>::infinity()}) {
        require(!saige::is_walkable(ceiling, bad_limit) && !saige::is_walkable(wall, bad_limit) && !saige::is_walkable(nearly_flat, bad_limit),
                "a limit of " + std::to_string(bad_limit) + " (outside [0, pi/2)) makes nothing walkable");
    }
    for (const float ok_limit : {0.0f, 0.3f, 0.87266463f, 1.5f, 1.5707f}) {
        require(!saige::is_walkable(ceiling, ok_limit), "a ceiling is never ground (limit " + std::to_string(ok_limit) + ")");
        require(!saige::is_walkable(wall, ok_limit), "a vertical wall is never ground (limit " + std::to_string(ok_limit) + ")");
        require(saige::is_walkable(nearly_flat, ok_limit), "level ground is ground under any valid limit (" + std::to_string(ok_limit) + ")");
    }
    require(!saige::is_walkable({0, 0, 0.5f}, limit), "a non-unit normal is rejected");
    require(!saige::is_walkable({std::nanf(""), 0, 1}, limit), "NaN is rejected");
}

void test_query_is_validated_even_for_an_empty_set() {
    const Capsule good{{-2, 0, 0}, 0.3f, 1.7f};
    const auto throws = [](auto&& call) {
        try {
            call();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::span<const Aabb> none{};
    require(!saige::sweep_capsule_boxes(good, {1, 0, 0}, none).has_value(), "a valid query against no boxes is simply clear");
    require(throws([&] { (void)saige::sweep_capsule_boxes({{0, 0, 0}, 0.0f, 1.0f}, {1, 0, 0}, none); }), "an invalid capsule throws against an empty set");
    require(throws([&] { (void)saige::sweep_capsule_boxes({{std::nanf(""), 0, 0}, 0.3f, 1.7f}, {1, 0, 0}, none); }), "a NaN capsule throws against an empty set");
    require(throws([&] { (void)saige::sweep_capsule_boxes({{0, 0, 0}, 0.3f, 0.4f}, {1, 0, 0}, none); }), "a capsule shorter than its diameter throws against an empty set");
    require(throws([&] { (void)saige::sweep_capsule_boxes(good, {std::nanf(""), 0, 0}, none); }), "a NaN displacement throws against an empty set");
    require(throws([&] { (void)saige::sweep_capsule_boxes(good, {0, INFINITY, 0}, none); }), "an infinite displacement throws against an empty set");
    const std::vector<Aabb> far_box{{{50, 50, 50}, {51, 51, 51}}};
    require(throws([&] { (void)saige::sweep_capsule_boxes(good, {std::nanf(""), 0, 0}, far_box); }), "and against a set whose boxes are nowhere near");
}

void test_invalid_input_is_rejected() {
    const Aabb box{{0, 0, 0}, {1, 1, 1}};
    const Capsule good{{-2, 0, 0}, 0.3f, 1.7f};
    const auto throws = [](auto&& call) {
        try {
            call();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    require(throws([&] { (void)saige::sweep_capsule_box({{nan, 0, 0}, 0.3f, 1.7f}, {1, 0, 0}, box); }), "NaN position");
    require(throws([&] { (void)saige::sweep_capsule_box({{0, 0, 0}, 0.0f, 1.7f}, {1, 0, 0}, box); }), "zero radius");
    require(throws([&] { (void)saige::sweep_capsule_box({{0, 0, 0}, 0.3f, 0.5f}, {1, 0, 0}, box); }), "height below two radii");
    require(throws([&] { (void)saige::sweep_capsule_box(good, {inf, 0, 0}, box); }), "infinite displacement");
    require(throws([&] { (void)saige::sweep_capsule_box(good, {1, 0, 0}, {{1, 0, 0}, {0, 1, 1}}); }), "inverted box");
    require(throws([&] { (void)saige::capsule_box_gap({{0, 0, 0}, -1.0f, 1.7f}, box); }), "gap validates too");
}

// ---------------------------------------------------------------------------------------------------------------------------
// Supported envelope and the conservative stop (issue #43 decision: chunk-local meters, +-256 m, 64 m per sweep).

bool throws_invalid(const Capsule& capsule, Vec3 displacement, const Aabb& box) {
    try {
        (void)saige::sweep_capsule_box(capsule, displacement, box);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// The independent double oracle's signed gap with the capsule's feet moved to `feet`.
double gap_at(Capsule capsule, Vec3 feet, const Aabb& box) {
    capsule.feet = feet;
    return reference_gap(capsule, {0, 0, 0}, box);
}

void test_envelope_is_enforced() {
    const Capsule c{{-1e8f, 0.0f, 0.0f}, 0.30f, 1.70f};
    const Aabb wall{{0, -2, -2}, {1, 2, 4}};
    // Core's reproducer (a 2e8 m sweep that used to report t = 0.5 and a 0.30 m penetration) is rejected, not answered at low precision.
    require(throws_invalid(c, {2e8f, 0, 0}, wall), "a sweep from -1e8 m is outside the envelope");
    require(throws_invalid({{-1000.0f, 0, 0}, 0.30f, 1.70f}, {2000.0f, 0, 0}, wall), "a 2000 m sweep is outside the envelope");

    const Capsule edge{{-255.5f, 0, 0}, 0.30f, 1.70f};  // bounding box reaches -255.8
    const Aabb far_wall{{-200, -2, -2}, {-199, 2, 4}};
    require(!throws_invalid(edge, {64.0f, 0, 0}, far_wall), "a 64 m sweep at the extent limit is supported");
    require(throws_invalid(edge, {64.01f, 0, 0}, far_wall), "a sweep just over 64 m is rejected");
    require(throws_invalid(edge, {50.0f, 50.0f, 0}, far_wall), "the limit is on the length, not per axis");
    require(!throws_invalid(edge, {40.0f, 40.0f, 0}, far_wall), "a 56.6 m diagonal is supported");
    require(throws_invalid({{-255.8f, 0, 0}, 0.30f, 1.70f}, {1.0f, 0, 0}, far_wall), "a capsule whose radius crosses -256 is rejected");
    require(throws_invalid({{255.8f, 0, 0}, 0.30f, 1.70f}, {-1.0f, 0, 0}, far_wall), "a capsule whose radius crosses +256 is rejected");
    require(throws_invalid({{0, 0, 254.4f}, 0.30f, 1.70f}, {0, 0, -1.0f}, far_wall), "a capsule whose top crosses +256 is rejected");
    require(throws_invalid({{0, 0, -256.5f}, 0.30f, 1.70f}, {0, 0, 1.0f}, far_wall), "a capsule whose feet are below -256 is rejected");
    require(throws_invalid({{0, 0, 0}, 0.30f, 1.70f}, {300.0f, 0, 0}, far_wall), "a displacement that ends outside is rejected even if short enough");
    require(throws_invalid({{250.0f, 0, 0}, 0.30f, 1.70f}, {10.0f, 0, 0}, far_wall), "the end of the sweep must be inside as well");

    // The same rule applies to the set query (also with an empty set) and to push-out.
    bool empty_throws = false;
    try {
        (void)saige::sweep_capsule_boxes(c, {2e8f, 0, 0}, {});
    } catch (const std::invalid_argument&) {
        empty_throws = true;
    }
    require(empty_throws, "an out-of-range query throws against an empty set");
    bool push_throws = false;
    try {
        (void)saige::capsule_box_push_out({{300.0f, 0, 0}, 0.30f, 1.70f}, wall);
    } catch (const std::invalid_argument&) {
        push_throws = true;
    }
    require(push_throws, "push-out outside the envelope throws");

    // Obstacles may extend beyond the envelope: only the capsule's path is bounded.
    const Aabb ground{{-5000, -5000, -10}, {5000, 5000, 0}};
    const auto hit = saige::sweep_capsule_box({{0, 0, 10.0f}, 0.30f, 1.70f}, {0, 0, -20.0f}, ground);
    require(hit && gap_at({{0, 0, 10.0f}, 0.30f, 1.70f}, hit->stop, ground) >= 0.0, "a huge ground box is supported");
}

void test_long_sweeps_at_the_limits_stop_clear() {
    const Capsule base{{0, 0, 0}, 0.30f, 1.70f};
    struct Case {
        Vec3 feet;
        Vec3 move;
        Aabb box;
    };
    const Case cases[] = {
        {{-255.5f, 0.0f, 0.0f}, {64.0f, 0, 0}, {{-200, -2, -2}, {-199, 2, 4}}},                  // +x from the -256 limit
        {{255.5f, 0.0f, 0.0f}, {-64.0f, 0, 0}, {{199, -2, -2}, {200, 2, 4}}},                    // -x from the +256 limit
        {{0.0f, -255.5f, 0.0f}, {0, 64.0f, 0}, {{-2, -200, -2}, {2, -199, 4}}},                  // +y
        {{0.0f, 255.5f, 0.0f}, {0, -64.0f, 0}, {{-2, 199, -2}, {2, 200, 4}}},                    // -y
        {{0.0f, 0.0f, 200.0f}, {0, 0, -64.0f}, {{-5, -5, 130}, {5, 5, 140}}},                    // fall onto a floor
        {{0.0f, 0.0f, -255.0f}, {0, 0, 64.0f}, {{-5, -5, -195}, {5, 5, -190}}},                  // rise into a ceiling
        {{-250.0f, -250.0f, 0.0f}, {44.0f, 44.0f, 0.0f}, {{-210, -210, -2}, {-200, -200, 4}}},   // oblique at the corner of the range
        {{249.7f, 250.1f, 3.0f}, {-30.0f, -40.0f, -10.0f}, {{210, 210, -50}, {220, 220, 50}}},   // oblique and descending
    };
    for (const Case& c : cases) {
        Capsule capsule = base;
        capsule.feet = c.feet;
        const auto hit = saige::sweep_capsule_box(capsule, c.move, c.box);
        require(hit.has_value(), "the long sweep reaches the box");
        const double gap = gap_at(capsule, hit->stop, c.box);
        require(gap >= 0.0, "the applied stop is clear: gap " + std::to_string(gap));
        require(gap < 1e-4, "and close to the contact: gap " + std::to_string(gap));
        require(near(hit->t * std::sqrt(static_cast<double>(c.move.x) * c.move.x + static_cast<double>(c.move.y) * c.move.y +
                                         static_cast<double>(c.move.z) * c.move.z),
                     std::sqrt(std::pow(static_cast<double>(hit->stop.x) - c.feet.x, 2) + std::pow(static_cast<double>(hit->stop.y) - c.feet.y, 2) +
                               std::pow(static_cast<double>(hit->stop.z) - c.feet.z, 2)),
                     2e-4),
                "t is the fraction at the stop");
    }
}

void test_stop_is_clear_for_random_hits_across_the_range() {
    Random random(20260101);
    int hits = 0;
    int skipped = 0;
    for (int i = 0; i < 1500; ++i) {
        Capsule capsule{{0, 0, 0}, static_cast<float>(random.range(0.2, 0.6)), 0.0f};
        capsule.height = capsule.radius * 2.0f + static_cast<float>(random.range(0.0, 1.5));
        const Vec3 center{static_cast<float>(random.range(-190, 190)), static_cast<float>(random.range(-190, 190)), static_cast<float>(random.range(-100, 100))};
        const Vec3 half{static_cast<float>(random.range(0.25, 3.0)), static_cast<float>(random.range(0.25, 3.0)), static_cast<float>(random.range(0.25, 3.0))};
        std::vector<Aabb> boxes;
        boxes.push_back({{center.x - half.x, center.y - half.y, center.z - half.z}, {center.x + half.x, center.y + half.y, center.z + half.z}});
        for (int extra = 0; extra < 2; ++extra) {  // neighbours around the target, so stops are judged against several boxes
            const Vec3 c2{center.x + static_cast<float>(random.range(-6, 6)), center.y + static_cast<float>(random.range(-6, 6)), center.z + static_cast<float>(random.range(-6, 6))};
            const float h2 = static_cast<float>(random.range(0.25, 2.0));
            boxes.push_back({{c2.x - h2, c2.y - h2, c2.z - h2}, {c2.x + h2, c2.y + h2, c2.z + h2}});
        }
        const double distance = random.range(1.0, 60.0);
        const double azimuth = random.range(0.0, 6.283185307179586);
        const double elevation = random.range(-0.6, 0.6);
        const Vec3 direction{static_cast<float>(std::cos(azimuth) * std::cos(elevation)), static_cast<float>(std::sin(azimuth) * std::cos(elevation)),
                             static_cast<float>(std::sin(elevation))};
        capsule.feet = {center.x + direction.x * static_cast<float>(distance), center.y + direction.y * static_cast<float>(distance),
                        center.z + direction.z * static_cast<float>(distance)};
        const double length = std::min(distance + 1.0, 64.0);
        const Vec3 move{static_cast<float>(-direction.x * length), static_cast<float>(-direction.y * length), static_cast<float>(-direction.z * length)};
        bool start_clear = true;
        for (const Aabb& box : boxes) start_clear = start_clear && gap_at(capsule, capsule.feet, box) > 0.001;
        if (!start_clear) {
            ++skipped;
            continue;
        }
        const auto hit = saige::sweep_capsule_boxes(capsule, move, boxes);
        if (!hit) continue;
        ++hits;
        // The oracle is independent of the implementation and uses the stop exactly as the caller applies it: no arithmetic in float.
        double hit_gap = gap_at(capsule, hit->stop, boxes[hit->index]);
        require(hit_gap >= 0.0, "scenario " + std::to_string(i) + ": the hit box is cleared, gap " + std::to_string(hit_gap));
        require(hit_gap < 1e-4, "scenario " + std::to_string(i) + ": the stop is within a few coordinate steps of the contact, gap " + std::to_string(hit_gap));
        for (const Aabb& box : boxes) {
            require(gap_at(capsule, hit->stop, box) >= -0.5e-5, "scenario " + std::to_string(i) + ": every box is cleared within the contact tolerance");
        }
        require(hit->t >= 0.0f && hit->t <= 1.0f, "t stays in [0, 1]");
    }
    require(hits >= 700, "too few hits for a meaningful sample: " + std::to_string(hits) + " (skipped " + std::to_string(skipped) + ")");
}

void test_stop_is_clear_of_every_box_at_an_inner_corner() {
    // A path into an inner corner reaches two walls at almost the same time, and the two faces sit at unrelated float coordinates so
    // the rounding error differs between the axes. Whichever wall is reported, the rounded stop must not enter the other: that is what
    // judging the stop against every box (not only the hit one) is for.
    Random random(7);
    int hits = 0;
    for (int i = 0; i < 3000; ++i) {
        const float fx = static_cast<float>(random.range(130, 200)) * (random.next() < 0.5 ? -1.0f : 1.0f);
        const float fy = static_cast<float>(random.range(30, 200)) * (random.next() < 0.5 ? -1.0f : 1.0f);
        const float osx = fx < 0 ? 1.0f : -1.0f;  // the side of each wall facing the capsule (toward the origin)
        const float osy = fy < 0 ? 1.0f : -1.0f;
        const auto interval = [](float a, float b) { return std::pair<float, float>{std::min(a, b), std::max(a, b)}; };
        const auto ax = interval(fx, fx - osx * 4.0f), ay = interval(fy - 10.0f, fy + 10.0f);  // wall A: face at x = fx
        const auto bx = interval(fx - 10.0f, fx + 10.0f), by = interval(fy, fy - osy * 4.0f);  // wall B: face at y = fy
        const std::vector<Aabb> boxes{{{ax.first, ay.first, -2}, {ax.second, ay.second, 6}}, {{bx.first, by.first, -2}, {bx.second, by.second, 6}}};
        // The capsule's feet are the contact point's projection, so both faces are reached together at (fx + osx r, fy + osy r).
        const float radius = static_cast<float>(random.range(0.2, 0.6));  // varies where the contact falls between two float coordinates
        const double a = random.range(3.0, 30.0), b = random.range(3.0, 30.0);
        const Capsule c{{static_cast<float>(fx + osx * radius + osx * a), static_cast<float>(fy + osy * radius + osy * b), 0.0f}, radius, 1.70f};
        bool start_clear = true;
        for (const Aabb& box : boxes) start_clear = start_clear && gap_at(c, c.feet, box) > 0.001;
        if (!start_clear) continue;
        const Vec3 move{static_cast<float>(-osx * a * 1.2), static_cast<float>(-osy * b * 1.2), 0.0f};
        const auto hit = saige::sweep_capsule_boxes(c, move, boxes);
        if (!hit) continue;
        ++hits;
        for (const Aabb& box : boxes) {
            const double gap = gap_at(c, hit->stop, box);
            require(gap >= -0.5e-5, "scenario " + std::to_string(i) + ": the stop is clear of both walls, gap " + std::to_string(gap));
        }
        require(gap_at(c, hit->stop, boxes[hit->index]) >= 0.0, "and the wall that was hit is cleared exactly");
    }
    require(hits >= 1000, "too few corner hits: " + std::to_string(hits));
}

void test_results_are_reproducible() {
    const Aabb box{{0, 0, 0}, {1, 1, 3}};
    const Capsule c{{2.0f, 1.7f, 0.2f}, 0.4f, 1.9f};
    const auto a = saige::sweep_capsule_box(c, {-3.1f, -1.3f, 0.2f}, box);
    const auto b = saige::sweep_capsule_box(c, {-3.1f, -1.3f, 0.2f}, box);
    require(a && b && a->t == b->t && a->normal == b->normal && a->point.x == b->point.x, "identical inputs give bit-identical results");
}

} // namespace

int main() {
    try {
        test_exact_cases();
        test_touching_and_overlap();
        test_push_out();
        test_multiple_boxes_and_ties();
        test_slopes();
        test_query_is_validated_even_for_an_empty_set();
        test_invalid_input_is_rejected();
        test_results_are_reproducible();
        test_randomized_against_reference();
        test_envelope_is_enforced();
        test_long_sweeps_at_the_limits_stop_clear();
        test_stop_is_clear_for_random_hits_across_the_range();
        test_stop_is_clear_of_every_box_at_an_inner_corner();
    } catch (const std::exception& error) {
        std::cerr << "capsule sweep test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "capsule sweep tests passed\n";
}
