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
