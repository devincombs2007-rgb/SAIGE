// The humanoid ragdoll (include/saige/ragdoll.hpp) on the dynamics backend: anatomical limits, momentum continuity from an animation pose, falls and impacts, momentum transfer from a thrown crate, recovery blending, the
// motor-driven pose tracking and the ankle balance strategy. Exit 77 (skipped) when no backend is compiled in.
#include "saige/ragdoll.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

constexpr double g = 9.81;

DVec sub(DVec a, DVec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dotv(DVec a, DVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double lenv(DVec a) { return std::sqrt(dotv(a, a)); }
DVec crossv(DVec a, DVec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
DVec rot(DQuat q, DVec v) {
    const DVec u{q.x, q.y, q.z};
    const DVec t = crossv(u, v);
    const DVec t2{2 * t.x, 2 * t.y, 2 * t.z};
    const DVec c = crossv(u, t2);
    return {v.x + q.w * t2.x + c.x, v.y + q.w * t2.y + c.y, v.z + q.w * t2.z + c.z};
}
DQuat qmul(DQuat a, DQuat b) { return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z}; }
DQuat axis_angle(DVec axis, double angle) {
    const double l = lenv(axis), s = std::sin(angle / 2) / l;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2)};
}
// The rotation angle (radians, 0..pi) taking a to b.
double angle_between(DQuat a, DQuat b) {
    const double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0 * std::acos(std::min(1.0, d));
}

std::unique_ptr<DynamicsWorld> world_with_floor(double gravity = 9.81) {
    DynamicsConfig cfg;
    cfg.gravity = gravity;
    cfg.collision_steps = 4;
    cfg.velocity_iterations = 20;
    cfg.position_iterations = 10;
    cfg.allow_sleeping = false;  // driven ragdolls are never left to sleep short of their target
    auto w = make_dynamics_world(DynamicsBackend::Jolt, cfg);
    require(w != nullptr, "the Jolt backend is compiled in");
    w->set_static_geometry(StaticCollision(std::vector<Aabb>{{{-100, -100, -2}, {100, 100, 0}}}, {}), nullptr);
    return w;
}

// The whole rest pose, rotated by `angle` about `axis` through `pivot` and moved by `shift`.
RagdollPose moved_pose(const RagdollPose& p, DVec pivot, DVec axis, double angle, DVec shift) {
    RagdollPose out = p;
    const DQuat q = axis_angle(axis, angle);
    for (std::size_t i = 0; i < kBoneCount; ++i) {
        const DVec d = rot(q, sub(p[i].position, pivot));
        out[i].position = {pivot.x + d.x + shift.x, pivot.y + d.y + shift.y, pivot.z + d.z + shift.z};
        out[i].rotation = qmul(q, p[i].rotation);
    }
    return out;
}

// The signed bend of a hinge joint: the angle from the parent's bone direction to the child's about the left axis (positive: the lower bone swinging back).
double bend(const RagdollPose& p, Bone parent, Bone child) {
    const DVec up{0, 0, 1};
    (void)up;
    const DVec a = rot(p[static_cast<std::size_t>(parent)].rotation, {0, 0, 1}), b = rot(p[static_cast<std::size_t>(child)].rotation, {0, 0, 1});
    // Bone directions are the bodies' local Z; both start pointing the same way (down the limb), so the sign of the swing about +Y (left) is the sign of a x b . left.
    const double s = dotv(crossv(a, b), {0, 1, 0});
    const double c = dotv(a, b);
    return std::atan2(s, c);  // R_y(+t) takes a down-pointing bone toward -X (the foot going back): a x b is along +Y for a positive bend about +Y
}

double lowest_point(const RagdollPose& p) {
    double low = 1e9;
    for (const BoneTransform& b : p) low = std::min(low, b.position.z);
    return low;
}

void test_build_and_spec() {
    auto w = world_with_floor();
    const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 0});
    Ragdoll r(*w, spec, 1);
    require(w->body_count() == kBoneCount, "thirteen bodies");
    require(std::fabs(r.total_mass() - 75.0) < 1e-9, "the mass is the spec's");
    double sum = 0.0;
    for (std::size_t i = 0; i < kBoneCount; ++i) sum += w->state(r.body(static_cast<Bone>(i))).position.z >= 0.0 ? 1.0 : 0.0;
    require(sum == static_cast<double>(kBoneCount), "every body starts above the floor");
    const DVec com = r.center_of_mass();
    std::printf("  standard humanoid 1.75 m, 75 kg: center of mass at %.3f m (%.0f%% of the height)\n", com.z, 100.0 * com.z / 1.75);
    require(com.z > 0.5 * 1.75 && com.z < 0.62 * 1.75, "the center of mass is at about 55% of the height (anatomical: 55-57% standing)");
    bool threw = false;
    try {
        Ragdoll bad(*w, spec, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw && w->body_count() == kBoneCount, "group 0 is refused and nothing was left behind");
    {
        Ragdoll second(*w, standard_humanoid(1.6, 60.0, {5, 0, 0}), 2);
        require(w->body_count() == 2 * kBoneCount, "a second ragdoll in another group");
    }
    require(w->body_count() == kBoneCount, "a ragdoll removes its bodies when it goes");
    threw = false;
    try {
        (void)standard_humanoid(0.2, 75.0, {0, 0, 0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a nonsense height is refused");
}

void test_collapse_and_limits() {
    // A limp ragdoll dropped from a stand falls over a crumples; the joints hold (pivot separation), the knees and elbows never bend the wrong way, nothing goes through the floor, and it comes to rest.
    for (const DVec heading : {DVec{1, 0, 0}, DVec{0, 1, 0}}) {
        auto w = world_with_floor();
        const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 0.0}, heading);
        Ragdoll r(*w, spec, 1);
        // The ragdoll is built facing `heading`; the bend measure assumes +X, so measure on the +X one only.
        const RagdollPose rest = r.rest_pose();
        {
            const RagdollPose at = moved_pose(rest, {0, 0, 1}, {0, 1, 0}, 0.0, {0, 0, 0.3});
            r.start(at, at, 1.0 / 60.0);
        }
        double worst_knee_back = 0.0, worst_elbow_back = 0.0, worst_elbow_fwd = 0.0, worst_knee_fwd = 0.0, deepest = 1e9;
        for (int i = 0; i < 60 * 6; ++i) {
            w->step(1.0 / 60.0);
            const RagdollPose p = r.pose();
            deepest = std::min(deepest, lowest_point(p));
            if (heading.x == 1.0) {
                worst_knee_back = std::min(worst_knee_back, std::min(bend(p, Bone::ThighL, Bone::ShinL), bend(p, Bone::ThighR, Bone::ShinR)));
                worst_knee_fwd = std::max(worst_knee_fwd, std::max(bend(p, Bone::ThighL, Bone::ShinL), bend(p, Bone::ThighR, Bone::ShinR)));
                worst_elbow_back = std::max(worst_elbow_back, std::max(bend(p, Bone::UpperArmL, Bone::ForearmL), bend(p, Bone::UpperArmR, Bone::ForearmR)));
                worst_elbow_fwd = std::min(worst_elbow_fwd, std::min(bend(p, Bone::UpperArmL, Bone::ForearmL), bend(p, Bone::UpperArmR, Bone::ForearmR)));
            }
            require(std::isfinite(p[0].position.x + p[0].position.z), "the state stays finite");
        }
        double speed = 0.0;
        for (std::size_t i = 0; i < kBoneCount; ++i) {
            const DBodyState s = w->state(r.body(static_cast<Bone>(i)));
            speed = std::max(speed, lenv(s.linear_velocity));
        }
        const double com_z = r.center_of_mass().z;
        std::printf("  collapse (heading %.0f,%.0f): after 6 s the fastest body moves %.3f m/s, center of mass at %.2f m, lowest point ever %.3f m%s\n", heading.x, heading.y, speed, com_z, deepest,
                    heading.x == 1.0 ? "" : " (no bend measure)");
        if (heading.x == 1.0) std::printf("    knee bend range [%.3f, %.3f] rad (limits [-0.05, 2.4]), elbow bend range [%.3f, %.3f] rad (limits [-2.5, 0.05])\n", worst_knee_back, worst_knee_fwd, worst_elbow_fwd, worst_elbow_back);
        require(deepest > -0.03, "no body goes through the floor (to the contact slop and a bit)");
        require(speed < 0.2 && com_z < 0.45, "the ragdoll has fallen and come (nearly) to rest low to the ground");
        if (heading.x == 1.0) {
            require(worst_knee_back > -0.2, "knees do not bend backwards (hyperextension under 0.2 rad)");
            require(worst_knee_fwd < 2.4 + 0.3, "and not past full flexion by more than 0.3 rad (the limit is a soft constraint under the body weight: up to 0.25 rad over with 4 substeps and 10 position iterations, more with fewer)");
            require(worst_elbow_back < 0.2 && worst_elbow_fwd > -2.5 - 0.3, "elbows do not hyperextend, and flex no more than 0.3 rad past their limit");
        }
    }
}

void test_momentum() {
    // In the air (no contact), a ragdoll started from an animation moving at 4 m/s forward and 1 m/s up keeps its momentum: horizontal constant, vertical -M g t.
    auto w = world_with_floor();
    const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 0});
    Ragdoll r(*w, spec, 1);
    const RagdollPose rest = r.rest_pose();
    const double dt = 1.0 / 60.0;
    const RagdollPose now = moved_pose(rest, {0, 0, 1}, {0, 1, 0}, 0.0, {0, 0, 6.0});
    const RagdollPose before = moved_pose(rest, {0, 0, 1}, {0, 1, 0}, 0.0, {-4.0 * dt, 0, 6.0 - 1.0 * dt});
    r.start(now, before, dt);
    const DVec p0 = r.linear_momentum();
    std::printf("  started at 4 m/s forward, 1 m/s up: momentum (%.1f, %.1f, %.1f) kg m/s (expected 300, 0, 75)\n", p0.x, p0.y, p0.z);
    require(std::fabs(p0.x - 300.0) < 1.5 && std::fabs(p0.z - 75.0) < 1.5 && std::fabs(p0.y) < 1.5, "the ragdoll starts with the animation's momentum");
    {
        // The angular velocity of the animation is inherited too: the whole pose turning 1.2 rad/s about the vertical makes the pelvis spin at that rate.
        const RagdollPose turned_before = moved_pose(now, {0, 0, 6.5}, {0, 0, 1}, -1.2 * dt, {0, 0, 0});
        auto w2 = world_with_floor();
        Ragdoll r2(*w2, spec, 1);
        r2.start(now, turned_before, dt);
        const double spin = w2->state(r2.body(Bone::Pelvis)).angular_velocity.z;
        std::printf("  started turning at 1.2 rad/s: the pelvis spins at %.3f rad/s\n", spin);
        require(std::fabs(spin - 1.2) < 0.03, "the ragdoll starts with the animation's angular velocity");
    }
    for (int i = 0; i < 30; ++i) w->step(dt);
    const DVec p1 = r.linear_momentum();
    const double damp = std::exp(-0.02 * 0.5);
    std::printf("  after 0.5 s of flight: (%.1f, %.1f, %.1f); expected (%.1f, 0, %.1f) with the 2%%/s damping\n", p1.x, p1.y, p1.z, 300.0 * damp, (75.0 - 75.0 * g * 0.5) * damp);
    require(std::fabs(p1.x - 300.0 * damp) < 4.0 && std::fabs(p1.y) < 2.0, "horizontal momentum is conserved in flight");
    require(std::fabs(p1.z - (75.0 - 75.0 * g * 0.5) * damp) < 6.0, "vertical momentum follows gravity");
    require(lenv(sub(r.pose()[0].position, {0, 0, 0})) > 1.0, "control: it moved");
}

void test_fall_impact() {
    // Dropped from a standing start 3 m up: the center of mass speed at the moment before contact is that of free fall; the impact is survived without tunnelling.
    auto w = world_with_floor();
    const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 0});
    Ragdoll r(*w, spec, 1);
    const RagdollPose rest = r.rest_pose();
    {
        const RagdollPose at = moved_pose(rest, {0, 0, 1}, {0, 1, 0}, 0.0, {0, 0, 3.0});
        r.start(at, at, 1.0 / 60.0);
    }
    double impact_speed = 0.0, deepest = 1e9;
    bool in_air = true;
    double t = 0.0;
    for (int i = 0; i < 60 * 3; ++i) {
        const RagdollPose p = r.pose();
        if (in_air && lowest_point(p) > 0.12) {
            impact_speed = std::fabs(r.linear_momentum().z) / 75.0;
            t = i / 60.0;
        } else {
            in_air = false;
        }
        w->step(1.0 / 60.0);
        deepest = std::min(deepest, lowest_point(r.pose()));
    }
    const double expected = g * t * std::exp(-0.01 * t);
    std::printf("  3 m fall: speed %.2f m/s %.2f s in, just above the ground (free fall %.2f); deepest point %.3f m\n", impact_speed, t, expected, deepest);
    require(std::fabs(impact_speed - expected) < 0.05 * expected, "the ragdoll falls at free-fall speed (within 5%)");
    require(deepest > -0.04, "the impact does not push anything through the floor");
}

void test_crate_transfer() {
    // In the air, a 20 kg crate thrown at 12 m/s into the ragdoll's chest: horizontal momentum of crate plus ragdoll is conserved through the hit and the ragdoll is carried forward.
    auto w = world_with_floor();
    const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 0});
    Ragdoll r(*w, spec, 1);
    const RagdollPose rest = r.rest_pose();
    {
        const RagdollPose at = moved_pose(rest, {0, 0, 1}, {0, 1, 0}, 0.0, {0, 0, 8.0});
        r.start(at, at, 1.0 / 60.0);
    }
    DBodyDesc crate;
    crate.shape = DShapeKind::Box;
    crate.half_extents = {0.2, 0.2, 0.2};
    crate.mass = 20.0;
    crate.position = {-2.0, 0.0, 8.0 + 1.26};  // at the chest
    crate.linear_velocity = {12.0, 0.0, 0.0};
    const BodyId c = w->add_body(crate);
    const double before = 20.0 * 12.0;
    double peak_ragdoll = 0.0;
    for (int i = 0; i < 60 * 1; ++i) {
        w->step(1.0 / 60.0);
        peak_ragdoll = std::max(peak_ragdoll, r.linear_momentum().x / 75.0);
    }
    const double total = r.linear_momentum().x + 20.0 * w->state(c).linear_velocity.x;
    std::printf("  crate (20 kg, 12 m/s) into a ragdoll in the air: horizontal momentum before %.1f, after %.1f kg m/s; ragdoll reached %.2f m/s\n", before, total, peak_ragdoll);
    require(std::fabs(total - before) < 0.05 * before, "momentum is conserved through the impact (within 5%: the damping of both is 2%/s)");
    require(peak_ragdoll > 0.8, "the ragdoll is carried forward by the hit");
}

void test_recovery() {
    RagdollRecovery rec(0.5);
    require(rec.weight() == 0.0 && !rec.finished(), "starts physical");
    double last = 0.0;
    for (int i = 0; i < 60; ++i) {
        rec.advance(1.0 / 120.0);
        require(rec.weight() >= last - 1e-12, "the weight never goes down");
        last = rec.weight();
    }
    require(rec.finished() && rec.weight() == 1.0, "ends animated after the duration");
    RagdollRecovery quarter(1.0);
    quarter.advance(0.25);
    require(std::fabs(quarter.weight() - 0.15625) < 1e-12, "smoothstep: a quarter of the way is 5/32 (eased, not linear)");
    RagdollRecovery mid(1.0);
    mid.advance(0.5);
    require(std::fabs(mid.weight() - 0.5) < 1e-12, "smoothstep: half way is a half");
    mid.advance(-1.0);
    mid.advance(std::nan(""));
    require(std::fabs(mid.weight() - 0.5) < 1e-12, "a negative or NaN step is ignored");
    RagdollPose a{}, b{};
    for (std::size_t i = 0; i < kBoneCount; ++i) {
        a[i] = {{double(i), 0, 0}, {0, 0, 0, 1}};
        b[i] = {{double(i), 2.0, 0}, axis_angle({0, 0, 1}, 1.0)};
    }
    RagdollRecovery start(1.0);
    RagdollPose o = start.blend(a, b);
    require(o[3].position.y == 0.0 && angle_between(o[3].rotation, a[3].rotation) < 1e-9, "weight 0 is the physical pose");
    RagdollRecovery half(1.0);
    half.advance(0.5);
    o = half.blend(a, b);
    require(std::fabs(o[3].position.y - 1.0) < 1e-12 && std::fabs(angle_between(o[3].rotation, a[3].rotation) - 0.5) < 1e-9, "half way: the middle position and half the rotation");
    half.advance(1.0);
    o = half.blend(a, b);
    require(o[3].position.y == 2.0 && angle_between(o[3].rotation, b[3].rotation) < 1e-9, "weight 1 is the animated pose");
    bool threw = false;
    try {
        RagdollRecovery bad(0.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a zero duration is refused");
}

void test_face_up() {
    for (const double angle : {-1.5707963, 1.5707963}) {  // about the left axis: -90 degrees tips the front up, +90 down
        auto w = world_with_floor();
        const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 0});
        Ragdoll r(*w, spec, 1);
        const RagdollPose rest = r.rest_pose();
        {
            const RagdollPose at = moved_pose(rest, {0, 0, 0.9}, {0, 1, 0}, angle, {0, 0, 0.35});
            r.start(at, at, 1.0 / 60.0);
        }
        for (int i = 0; i < 60 * 4; ++i) w->step(1.0 / 60.0);
        const bool up = r.lying_face_up();
        std::printf("  laid down with a %+.0f degree tip about the left axis: face up = %d\n", angle * 57.29578, up ? 1 : 0);
        require(up == (angle < 0.0), "face up exactly when the front was tipped upward");
    }
}

// Joint-space tracking: how far the joints are from the target, the largest relative rotation error over the child bones (radians).
double joint_error(const RagdollPose& p, const RagdollPose& target) {
    static constexpr Bone parent[kBoneCount] = {Bone::Count, Bone::Pelvis, Bone::Torso, Bone::Torso, Bone::Torso, Bone::UpperArmL, Bone::UpperArmR, Bone::Pelvis, Bone::Pelvis, Bone::ThighL, Bone::ThighR, Bone::ShinL, Bone::ShinR};
    double worst = 0.0;
    for (std::size_t i = 1; i < kBoneCount; ++i) {
        const std::size_t q = static_cast<std::size_t>(parent[i]);
        const DQuat rel = qmul({-p[q].rotation.x, -p[q].rotation.y, -p[q].rotation.z, p[q].rotation.w}, p[i].rotation);
        const DQuat relt = qmul({-target[q].rotation.x, -target[q].rotation.y, -target[q].rotation.z, target[q].rotation.w}, target[i].rotation);
        worst = std::max(worst, angle_between(rel, relt));
    }
    return worst;
}

RagdollPose raised_arms_and_bent_knees(const Ragdoll& r, const HumanoidSpec& spec) {
    // The rest pose with each upper arm swung 60 degrees outward-forward and each knee bent 40 degrees: built by rotating the child bones about their joints (the parents stay).
    RagdollPose p = r.rest_pose();
    const DVec left{0, 1, 0}, fwd{1, 0, 0};
    const auto turn = [&](Bone b, DVec pivot, DVec axis, double angle) {
        const DQuat q = axis_angle(axis, angle);
        BoneTransform& t = p[static_cast<std::size_t>(b)];
        const DVec d = rot(q, sub(t.position, pivot));
        t.position = {pivot.x + d.x, pivot.y + d.y, pivot.z + d.z};
        t.rotation = qmul(q, t.rotation);
    };
    // Raise the arms to the front: forearms follow their upper arms.
    for (int i = 0; i < 2; ++i) {
        const Bone ua = i == 0 ? Bone::UpperArmL : Bone::UpperArmR, fa = i == 0 ? Bone::ForearmL : Bone::ForearmR;
        const DVec sh = spec.shoulder[i];
        turn(ua, sh, left, -1.0472);
        turn(fa, sh, left, -1.0472);
    }
    (void)fwd;
    for (int i = 0; i < 2; ++i) {
        const Bone sh = i == 0 ? Bone::ShinL : Bone::ShinR, ft = i == 0 ? Bone::FootL : Bone::FootR;
        turn(sh, spec.knee[i], left, 0.7);
        turn(ft, spec.knee[i], left, 0.7);
    }
    return p;
}

void test_pose_tracking() {
    // No gravity (the motors work against nothing but the bodies' own inertia): they bring the limbs to a raised-arms, bent-knees target and hold it.
    auto w = world_with_floor(0.0);
    const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 1.0});
    Ragdoll r(*w, spec, 1);
    const RagdollPose target = raised_arms_and_bent_knees(r, spec);
    const double initial = joint_error(r.rest_pose(), target);
    double last = initial, tail = 0.0, at_1s = 0.0;
    r.drive_to_pose(target, {15.0, 2.0, 0.0});
    for (int i = 0; i < 60 * 8; ++i) {
        w->step(1.0 / 60.0);
        if (i == 59) at_1s = joint_error(r.pose(), target);
        if (i >= 60 * 5) tail = std::max(tail, joint_error(r.pose(), target));
        if (i == 60 * 8 - 1) last = joint_error(r.pose(), target);
    }
    std::printf("  motor tracking in zero gravity (15 Hz, damping 2): joint error %.3f rad at the start, %.3f after 1 s, %.3f after 8 s, at most %.3f between 5 s and 8 s\n", initial, at_1s, last, tail);
    require(initial > 0.6, "control: the target is far from the rest pose");
    require(at_1s < 0.5 * initial, "the motors close more than half the distance in a second");
    require(last < 0.05 && tail < 0.06, "and reach the target pose and hold it to within 3 degrees");
    // Control: without the motors the joints stay where they were (no gravity, no motion); and released motors let go.
    auto w2 = world_with_floor(0.0);
    Ragdoll r2(*w2, spec, 1);
    for (int i = 0; i < 60 * 3; ++i) w2->step(1.0 / 60.0);
    require(joint_error(r2.pose(), target) > 0.6, "control: with no motors the pose is not reached");
    r.release();
    const RagdollPose held = r.pose();
    for (int i = 0; i < 60 * 2; ++i) w->step(1.0 / 60.0);
    require(joint_error(r.pose(), held) < 0.05, "after release the limbs stay where they were (no gravity, nothing pushes them)");
    bool threw = false;
    try {
        r.drive_to_pose(target, {0.0, 1.0, 0.0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a motor with no frequency is refused");
}

double standing_height(const Ragdoll& r) { return r.center_of_mass().z; }

void test_balance() {
    // A ragdoll stood on its feet with every joint driven to the standing pose. Joint motors alone cannot hold a body up on ankles whose spring is set by a foot's mass (it stands for a moment and then sways over like a
    // plank); the ankle strategy (balance) re-aims the feet's motors against the center of mass' offset and velocity and keeps it upright, recovering from a push, within what a foot allows.
    struct Result {
        double com_after, x_after, y_after;
        bool standing;
    };
    const DMotor motor{15.0, 2.0, 0.0};
    const double support = 3.0;
    const auto run = [&](bool balance, DVec push) {
        auto w = world_with_floor();
        const HumanoidSpec spec = standard_humanoid(1.75, 75.0, {0, 0, 0.0});
        Ragdoll r(*w, spec, 1);
        const RagdollPose stand = r.rest_pose();
        r.drive_to_pose(stand, motor, support);
        for (int i = 0; i < 60 * 8; ++i) {
            if (balance) r.balance(stand, motor, 3.0, 0.6, support);
            if (i == 60 * 2) w->add_impulse(r.body(Bone::Torso), push, w->state(r.body(Bone::Torso)).position);
            w->step(1.0 / 60.0);
        }
        const DVec c = r.center_of_mass();
        return Result{c.z, c.x, c.y, c.z > 0.8};
    };
    const Result plain = run(false, {0, 0, 0});
    const Result active = run(true, {0, 0, 0});
    std::printf("  stood for 8 s without a push: plain motors end at %.2f m (%s); with the ankle strategy %.2f m (%s), leaning %.3f m ahead of the feet' center\n", plain.com_after, plain.standing ? "standing" : "fell", active.com_after,
                active.standing ? "standing" : "fell", active.x_after);
    require(!plain.standing, "control: stiff joints alone do not keep it up (the ankle spring is a foot's)");
    require(active.standing && active.x_after < 0.2, "the ankle strategy stands it");
    struct Case {
        const char* name;
        DVec push;
        bool survives;
    };
    for (const Case& c : {Case{"from behind, 15 N s", {15, 0, 0}, true}, Case{"from behind, 22 N s", {22, 0, 0}, true}, Case{"from the front, 15 N s", {-15, 0, 0}, true}, Case{"from the right, 15 N s", {0, 15, 0}, true},
                           Case{"from the left, 15 N s", {0, -15, 0}, true}, Case{"from behind, 60 N s", {60, 0, 0}, false}}) {
        const Result res = run(true, c.push);
        std::printf("  push %s (%.2f m/s on the body): %s, center of mass %.2f m high, %.2f m ahead, %.2f m to the left\n", c.name, std::sqrt(c.push.x * c.push.x + c.push.y * c.push.y) / 75.0, res.standing ? "stays up" : "falls", res.com_after,
                    res.x_after, res.y_after);
        require(res.standing == c.survives, std::string("balance recovery ") + c.name);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (available_dynamics_backends().empty()) {
        std::cout << "no dynamics backend is compiled in (configure with -DSAIGE_PHYSICS_JOLT=ON): skipped\n";
        return 77;
    }
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"build", test_build_and_spec}, {"collapse", test_collapse_and_limits}, {"momentum", test_momentum}, {"fall", test_fall_impact}, {"crate", test_crate_transfer},
                           {"recovery", test_recovery},    {"faceup", test_face_up},              {"tracking", test_pose_tracking}, {"balance", test_balance}};
    int failures = 0;
    for (const Named& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "FAIL (" << t.name << "): " << e.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) std::cout << "ragdoll tests passed\n";
    return failures == 0 ? 0 : 1;
}
