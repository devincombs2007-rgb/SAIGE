// Two-bone IK and foot placement on uneven ground.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"

using namespace saige;
using namespace saige::anim;
using render::Float3;

namespace {

float dist(Float3 a, Float3 b) { return render::length(a - b); }

} // namespace

TEST_CASE("IK: a two-bone chain reaches any point in range, keeps its bone lengths and bends toward the pole") {
    const fixtures::Biped b = fixtures::make_biped();
    const std::size_t up = static_cast<std::size_t>(b.map[Bone::LeftUpperLeg]), lo = static_cast<std::size_t>(b.map[Bone::LeftLowerLeg]), ft = static_cast<std::size_t>(b.map[Bone::LeftFoot]);
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const std::vector<JointWorld> w0 = joint_world(b.skin, rest);
    const float len_a = dist(w0[up].position, w0[lo].position), len_b = dist(w0[lo].position, w0[ft].position);
    const Float3 hip = w0[up].position;
    int count = 0;
    for (const Float3 offset : {Float3{0.3f, 0.1f, -0.6f}, Float3{-0.4f, 0.0f, -0.5f}, Float3{0.0f, 0.3f, -0.7f}, Float3{0.5f, -0.1f, -0.4f}, Float3{0.1f, 0.0f, -0.8f}, Float3{0.45f, 0.2f, -0.3f}}) {
        gfx::Pose pose = rest;
        const Float3 target = hip + offset;
        const TwoBoneResult r = solve_two_bone(b.skin, pose, up, lo, ft, target, Float3{1, 0, 0});
        REQUIRE(r.solved);
        const std::vector<JointWorld> w = joint_world(b.skin, pose);
        CHECK(dist(w[ft].position, target) < 2e-3f);
        CHECK(std::fabs(dist(w[up].position, w[lo].position) - len_a) < 1e-4f);
        CHECK(std::fabs(dist(w[lo].position, w[ft].position) - len_b) < 1e-4f);
        CHECK(r.reached);
        // The knee is in front of the line from hip to foot (bends toward +X, the pole).
        const Float3 axis = render::normalize(w[ft].position - w[up].position);
        const Float3 knee_off = (w[lo].position - w[up].position) - axis * render::dot(w[lo].position - w[up].position, axis);
        CHECK(knee_off.x > 0.0f);
        ++count;
    }
    CHECK(count == 6);
    // Out of reach: the chain extends straight toward the target and reports it.
    gfx::Pose pose = rest;
    const Float3 far = hip + Float3{0.0f, 0.0f, -3.0f};
    const TwoBoneResult r = solve_two_bone(b.skin, pose, up, lo, ft, far, Float3{1, 0, 0});
    CHECK(r.solved && !r.reached);
    const std::vector<JointWorld> w = joint_world(b.skin, pose);
    CHECK(std::fabs(dist(w[up].position, w[ft].position) - (len_a + len_b)) < 2e-3f);
    // Weight 0 leaves the pose, weight 1 solves it, 0.5 is between.
    gfx::Pose p0 = rest, p5 = rest, p1 = rest;
    const Float3 t = hip + Float3{0.3f, 0.0f, -0.6f};
    solve_two_bone(b.skin, p0, up, lo, ft, t, Float3{1, 0, 0}, 0.0f);
    solve_two_bone(b.skin, p5, up, lo, ft, t, Float3{1, 0, 0}, 0.5f);
    solve_two_bone(b.skin, p1, up, lo, ft, t, Float3{1, 0, 0}, 1.0f);
    CHECK(pose_distance(p0, rest) < 1e-6f);
    CHECK(pose_distance(p5, rest) > 0.0f && pose_distance(p5, rest) < pose_distance(p1, rest));
    // Bad inputs.
    bool threw = false;
    try { solve_two_bone(b.skin, pose, up, ft, lo, t, Float3{1, 0, 0}); } catch (const std::invalid_argument&) { threw = true; }  // not a chain
    CHECK(threw);
    threw = false;
    try { solve_two_bone(b.skin, pose, up, lo, ft, Float3{std::nanf(""), 0, 0}, Float3{1, 0, 0}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

namespace {

struct Ramp {
    float slope = 0.0f;        // dz/dx
    float offset = 0.0f;       // base height
    bool operator()(float x, float, float, GroundSample& g) const {
        g.height = offset + slope * x;
        const float n = 1.0f / std::sqrt(1.0f + slope * slope);
        g.normal = {-slope * n, 0.0f, n};
        return true;
    }
};

// Plants the feet of one gait pose on `ground` and returns the lowest and highest height of a planted contact point above the ground.
struct Planted {
    float worst_gap = 0.0f;  // largest |contact height above the ground| among the planted feet
    float pelvis = 0.0f;
    int feet = 0;
};

Planted plant(const fixtures::Biped& b, const gfx::AnimationClip& clip, float t, const GroundQuery& ground, FootIkState& state, int ticks = 1) {
    gfx::Pose pose = gfx::rest_pose(b.skin);
    gfx::evaluate_clip(clip, gfx::bind_clip(clip, b.skin), t, true, pose);
    FootIkConfig cfg;
    cfg.pelvis_smoothing = 0.0f;
    for (int i = 0; i < ticks; ++i) adapt_feet_to_ground(b.skin, b.map, b.rig, pose, {{0, 0, 0}, {0, 0, 0, 1}}, ground, cfg, state, 1.0f / 60.0f);
    const std::vector<JointWorld> w = joint_world(b.skin, pose);
    Planted out;
    out.pelvis = state.pelvis_offset;
    for (const Bone toe : {Bone::LeftToes, Bone::RightToes}) {
        const Float3 p = w[static_cast<std::size_t>(b.map[toe])].position;
        GroundSample g;
        ground(p.x, p.y, 0.0f, g);
        const float h = p.z - g.height;
        // Planted by the animation's intent: the contact point was on the floor.
        gfx::Pose raw = gfx::rest_pose(b.skin);
        gfx::evaluate_clip(clip, gfx::bind_clip(clip, b.skin), t, true, raw);
        const Float3 q = joint_world(b.skin, raw)[static_cast<std::size_t>(b.map[toe])].position;
        if (q.z < 0.005f) {  // fully planted by the animation (a foot just lifting is only partly corrected)
            out.worst_gap = std::max(out.worst_gap, std::fabs(h));
            ++out.feet;
        }
    }
    return out;
}

} // namespace

TEST_CASE("Foot IK: planted feet sit on slopes and steps, the pelvis follows, and a flying foot is left alone") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::AnimationClip gait = fixtures::make_gait(b, 1.4f, 1.1f, 0.6f, 0.10f);
    for (const float slope : {0.0f, 0.15f, 0.27f, -0.2f}) {
        for (const float t : {0.0f, 0.2f, 0.45f, 0.7f, 0.95f}) {
            FootIkState state;
            const Planted p = plant(b, gait, t, Ramp{slope, 0.0f}, state);
            CHECK(p.feet >= 1);
            CHECK(p.worst_gap < 0.012f);  // within 12 mm of the surface
        }
    }
    // A step down of 0.2 m under both feet: the pelvis drops by that much.
    FootIkState down;
    const Planted d = plant(b, gait, 0.1f, Ramp{0.0f, -0.2f}, down);
    CHECK(std::fabs(d.pelvis + 0.2f) < 0.01f);
    CHECK(d.worst_gap < 0.012f);
    // A flat floor changes nothing: the pose stays the animation's.
    gfx::Pose raw = gfx::rest_pose(b.skin), adapted = gfx::rest_pose(b.skin);
    const gfx::ClipBinding bind = gfx::bind_clip(gait, b.skin);
    gfx::evaluate_clip(gait, bind, 0.3f, true, raw);
    adapted = raw;
    FootIkState flat;
    FootIkConfig cfg;
    cfg.pelvis_smoothing = 0.0f;
    adapt_feet_to_ground(b.skin, b.map, b.rig, adapted, {{0, 0, 0}, {0, 0, 0, 1}}, Ramp{}, cfg, flat, 1.0f / 60.0f);
    CHECK(pose_distance(raw, adapted) < 0.02f);
    // Where there is no ground the pose is untouched.
    gfx::Pose none = raw;
    FootIkState s2;
    adapt_feet_to_ground(b.skin, b.map, b.rig, none, {{0, 0, 0}, {0, 0, 0, 1}}, [](float, float, float, GroundSample&) { return false; }, cfg, s2, 1.0f / 60.0f);
    CHECK(pose_distance(raw, none) < 1e-6f);
    // A foot in the swing (well above the floor) is not pulled down onto a ramp.
    {
        FootIkState s3;
        gfx::Pose swing = gfx::rest_pose(b.skin);
        gfx::evaluate_clip(gait, bind, 0.8f, true, swing);  // left foot swings (phase 0.73): high point of the arc
        const Float3 before = joint_world(b.skin, swing)[static_cast<std::size_t>(b.map[Bone::LeftToes])].position;
        CHECK(before.z > 0.05f);
        adapt_feet_to_ground(b.skin, b.map, b.rig, swing, {{0, 0, 0}, {0, 0, 0, 1}}, Ramp{0.0f, 0.04f}, cfg, s3, 1.0f / 60.0f);
        // The pelvis rose for the planted foot, so everything moved up by about 0.04; the swing foot kept its animation relative to the pelvis, i.e. it was not forced to the floor.
        const Float3 after = joint_world(b.skin, swing)[static_cast<std::size_t>(b.map[Bone::LeftToes])].position;
        CHECK(after.z > 0.05f);
    }
}

TEST_CASE("Foot IK: the pelvis offset is smoothed and bounded") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::AnimationClip gait = fixtures::make_gait(b, 1.4f, 1.1f);
    gfx::Pose pose = gfx::rest_pose(b.skin);
    gfx::evaluate_clip(gait, gfx::bind_clip(gait, b.skin), 0.1f, true, pose);
    FootIkState state;
    FootIkConfig cfg;  // default smoothing 0.08 s
    float prev = 0.0f, step_max = 0.0f;
    for (int i = 0; i < 90; ++i) {
        gfx::Pose p = pose;
        adapt_feet_to_ground(b.skin, b.map, b.rig, p, {{0, 0, 0}, {0, 0, 0, 1}}, Ramp{0.0f, -0.5f}, cfg, state, 1.0f / 60.0f);  // a big drop: bounded by max_pelvis_drop
        step_max = std::max(step_max, std::fabs(state.pelvis_offset - prev));
        prev = state.pelvis_offset;
    }
    CHECK(std::fabs(state.pelvis_offset + cfg.max_pelvis_drop) < 0.01f);
    CHECK(step_max < 0.12f);  // it arrives over many ticks, not in one
}

namespace {

// Moves a character at `speed` while a single cycle plays at its authored rate (so its feet move at the cycle's own speed, not the character's), optionally locking the planted feet, and returns the
// mean horizontal world speed of the planted contact points (planted by the animation's own intent, three ticks running).
double planted_world_speed(const fixtures::Biped& b, const gfx::AnimationClip& gait, float speed, bool lock, bool ground_flat_y = true) {
    (void)ground_flat_y;
    LocomotionSet set(b.skin, b.map);
    REQUIRE(set.add_cycle(gait));
    LocomotionConfig cfg;
    cfg.stride_matching = false;  // authored rate, whatever the speed
    LocomotionController c(set, cfg);
    FootLockState state;
    const float dt = 1.0f / 60.0f;
    const Float3 fwd = b.rig.forward;
    float x = 0.0f;
    Float3 prev[2];
    // Per tick and foot: the animation's own contact height and the world speed of the (locked) contact point. A tick counts as planted when the animation had the foot on the floor from two ticks before
    // to six ticks after it, which leaves out the landing and the toe-off, where the foot is still arriving or already leaving.
    std::vector<float> raw_height[2];
    std::vector<double> speed_of[2];
    bool have = false;
    for (int i = 0; i < 60 * 8; ++i) {
        c.update(dt, {speed, true, 0.0f});
        x += speed * dt;
        gfx::Pose pose = c.pose();
        const render::Transform root{fwd * x, {0, 0, 0, 1}, {1, 1, 1}};
        const std::vector<JointWorld> raw = joint_world(b.skin, pose);
        if (lock) lock_planted_feet(b.skin, b.map, b.rig, pose, root, {}, state, dt);
        const std::vector<JointWorld> w = joint_world(b.skin, pose);
        Float3 now[2];
        float raw_z[2];
        for (int f = 0; f < 2; ++f) {
            const std::size_t j = static_cast<std::size_t>(b.map[f == 0 ? Bone::LeftToes : Bone::RightToes]);
            now[f] = root.position + w[j].position;
            raw_z[f] = raw[j].position.z;
        }
        for (int f = 0; f < 2; ++f) {
            raw_height[f].push_back(raw_z[f]);
            speed_of[f].push_back(have ? std::hypot(static_cast<double>(now[f].x - prev[f].x), static_cast<double>(now[f].y - prev[f].y)) / static_cast<double>(dt) : 0.0);
            prev[f] = now[f];
        }
        have = true;
    }
    double sum = 0.0;
    int count = 0;
    for (std::size_t i = 100; i + 6 < raw_height[0].size(); ++i) {
        for (int f = 0; f < 2; ++f) {
            bool planted = true;
            for (std::size_t k = i - 2; k <= i + 6 && planted; ++k) planted = raw_height[f][k] < 0.003f;
            if (planted) {
                sum += speed_of[f][i];
                ++count;
            }
        }
    }
    return count > 0 ? sum / count : 0.0;
}

} // namespace

TEST_CASE("Foot locking: a planted foot stays where it landed when the stride does not match the speed") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::AnimationClip gait = fixtures::make_gait(b, 1.2f, 1.10f, 0.60f, 0.08f);
    // The character is 10% faster than the cycle's own speed: the feet skate at 0.12 m/s without a lock.
    const double free_10 = planted_world_speed(b, gait, 1.32f, false), locked_10 = planted_world_speed(b, gait, 1.32f, true);
    CHECK(free_10 > 0.09);
    CHECK(locked_10 < 0.03);
    // 50% faster: the drift outgrows the lock's reach (0.15 m), so it gives way and the foot is free for the rest of the stance: it is then no worse than without a lock.
    const double free_50 = planted_world_speed(b, gait, 1.8f, false), locked_50 = planted_world_speed(b, gait, 1.8f, true);
    CHECK(free_50 > 0.4);
    CHECK(locked_50 <= free_50 * 1.05);
    // At the cycle's own speed there is nothing to correct and the lock changes nothing noticeable.
    CHECK(planted_world_speed(b, gait, 1.2f, true) < 0.03);
    // Bad dt throws.
    gfx::Pose pose = gfx::rest_pose(b.skin);
    FootLockState state;
    bool threw = false;
    try { lock_planted_feet(b.skin, b.map, b.rig, pose, {{0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}, {}, state, 0.0f); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
