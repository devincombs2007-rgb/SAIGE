#include "saige/ragdoll.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige {
namespace {

using V = DVec;
using Q = DQuat;

V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(V a) { return std::sqrt(dot(a, a)); }
V norm(V a) {
    const double l = len(a);
    if (!(l > 1e-12)) throw std::invalid_argument("ragdoll: a zero-length direction");
    return a * (1.0 / l);
}
bool finite(V a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

Q mul(Q a, Q b) { return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z}; }
Q conj(Q a) { return {-a.x, -a.y, -a.z, a.w}; }
Q normalized(Q a) {
    const double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    return l > 1e-12 ? Q{a.x / l, a.y / l, a.z / l, a.w / l} : Q{};
}
V rotate(Q q, V v) {
    const V u{q.x, q.y, q.z};
    const V t = cross(u, v) * 2.0;
    return v + t * q.w + cross(u, t);
}
// The rotation with columns x, y, z (a right-handed orthonormal basis).
Q from_basis(V x, V y, V z) {
    const double m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
    Q q;
    const double tr = m00 + m11 + m22;
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q = {0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q = {(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s};
    }
    return normalized(q);
}
// The rotation vector (axis times angle, angle in [0, pi]) of q.
V rotation_vector(Q q) {
    q = normalized(q);
    if (q.w < 0.0) q = {-q.x, -q.y, -q.z, -q.w};
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-12) return {q.x * 2.0, q.y * 2.0, q.z * 2.0};
    const double angle = 2.0 * std::atan2(s, q.w);
    return V{q.x, q.y, q.z} * (angle / s);
}
Q slerp(Q a, Q b, double t) {
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995) return normalized({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    const double th = std::acos(std::min(1.0, d)), s = std::sin(th);
    const double wa = std::sin((1.0 - t) * th) / s, wb = std::sin(t * th) / s;
    return normalized({a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

constexpr std::size_t B(Bone b) { return static_cast<std::size_t>(b); }

// De Leva-like mass fractions (sum 1): the torso carries the hands' and the arms' attachments, the head 6.9%.
constexpr double kMassFraction[kBoneCount] = {0.142, 0.370, 0.069, 0.027, 0.027, 0.022, 0.022, 0.100, 0.100, 0.046, 0.046, 0.0145, 0.0145};

struct Geometry {
    DShapeKind shape;
    V half;
    V center;
    Q rotation;
    double length;  // the long dimension, for the inertia estimate
};

Geometry capsule_between(V a, V b, double radius, V forward) {
    const V d = b - a;
    const double l = len(d);
    if (!(l > 2.0 * radius * 0.5 + 1e-6)) throw std::invalid_argument("ragdoll: a limb shorter than its radius");
    const V z = d * (1.0 / l);
    V x = forward - z * dot(forward, z);
    if (len(x) < 1e-6) x = cross(z, V{0, 1, 0});
    x = norm(x);
    const V y = cross(z, x);
    return {DShapeKind::Capsule, {radius, std::max(0.01, 0.5 * l - radius), 0.0}, (a + b) * 0.5, from_basis(x, y, z), l};
}

} // namespace

HumanoidSpec standard_humanoid(double height, double mass, DVec feet, DVec forward) {
    if (!(height > 0.5 && height < 3.0) || !(mass > 5.0 && mass < 500.0) || !finite(feet) || !finite(forward)) throw std::invalid_argument("standard_humanoid: height 0.5 to 3 m, mass 5 to 500 kg, finite position and heading");
    forward.z = 0.0;
    const V f = norm(forward), up{0, 0, 1}, left = cross(up, f);
    const double H = height;
    const auto at = [&](double a, double b, double c) { return feet + f * (a * H) + left * (b * H) + up * (c * H); };
    HumanoidSpec s;
    s.forward = f;
    s.up = up;
    s.mass = mass;
    s.height = height;
    s.pelvis = at(0, 0, 0.54);
    s.chest = at(0, 0, 0.72);
    s.neck = at(0, 0, 0.85);
    s.head = at(0, 0, 0.93);
    for (int i = 0; i < 2; ++i) {
        const double side = i == 0 ? 1.0 : -1.0;
        s.shoulder[i] = at(0, side * 0.12, 0.82);
        s.elbow[i] = at(0, side * 0.14, 0.634);
        s.wrist[i] = at(0, side * 0.14, 0.485);
        s.hip[i] = at(0, side * 0.055, 0.52);
        s.knee[i] = at(0, side * 0.055, 0.285);
        s.ankle[i] = at(0, side * 0.055, 0.039);
        s.toe[i] = at(0.12, side * 0.055, 0.0);
    }
    s.radius_arm = 0.022 * H;
    s.radius_forearm = 0.019 * H;
    s.radius_thigh = 0.043 * H;
    s.radius_shin = 0.031 * H;
    s.radius_head = 0.062 * H;
    return s;
}

namespace {

struct JointDef {
    Bone parent, child;
    DConstraintKind kind;
    V pivot, axis;
    double lo, hi, swing;
};

Geometry geometry_of(const HumanoidSpec& s, Bone b) {
    const double H = s.height;
    const V f = s.forward, up = s.up, left = cross(up, f);
    const Q frame = from_basis(f, left, up);
    const auto side = [](Bone bone) { return (static_cast<std::size_t>(bone) % 2 == 1) ? 0 : 1; };  // L bones are the odd indices from UpperArmL (3): L = 3,5,7,9,11
    switch (b) {
    case Bone::Pelvis: return {DShapeKind::Box, {0.075 * H, 0.09 * H, 0.055 * H}, s.pelvis, frame, 0.18 * H};
    case Bone::Torso: return {DShapeKind::Box, {0.065 * H, 0.10 * H, 0.12 * H}, s.chest, frame, 0.24 * H};
    case Bone::Head: return {DShapeKind::Sphere, {s.radius_head, 0, 0}, s.head, frame, 2.0 * s.radius_head};
    case Bone::UpperArmL:
    case Bone::UpperArmR: return capsule_between(s.shoulder[side(b)], s.elbow[side(b)], s.radius_arm, f);
    case Bone::ForearmL:
    case Bone::ForearmR: return capsule_between(s.elbow[side(b)], s.wrist[side(b)], s.radius_forearm, f);
    case Bone::ThighL:
    case Bone::ThighR: return capsule_between(s.hip[side(b)], s.knee[side(b)], s.radius_thigh, f);
    case Bone::ShinL:
    case Bone::ShinR: return capsule_between(s.knee[side(b)], s.ankle[side(b)], s.radius_shin, f);
    case Bone::FootL:
    case Bone::FootR: {
        // From 4 cm behind the ankle to the toe, from the sole to just above the ankle joint.
        const int i = side(b);
        const V ankle = s.ankle[i], toe = s.toe[i];
        const V reach = V{toe.x - ankle.x, toe.y - ankle.y, 0.0};
        const double l = len(reach) + 0.04 * H, th = 0.045 * H;
        const V c = V{ankle.x, ankle.y, toe.z + 0.5 * th} + f * (0.5 * (len(reach) - 0.04 * H));
        return {DShapeKind::Box, {0.5 * l, 0.03 * H, 0.5 * th}, c, frame, l};
    }
    case Bone::Count: break;
    }
    throw std::invalid_argument("ragdoll: unknown bone");
}

} // namespace

Ragdoll::Ragdoll(DynamicsWorld& world, const HumanoidSpec& spec, std::uint32_t group, const RagdollLimits& lim) : world_(&world), spec_(spec) {
    if (group == 0) throw std::invalid_argument("Ragdoll: the self-collision group must be nonzero");
    if (!(spec.mass > 5.0) || !finite(spec.forward) || !finite(spec.up) || !(spec.height > 0.5)) throw std::invalid_argument("Ragdoll: a bad spec");
    const V f = norm(spec.forward), up = norm(spec.up), left = cross(up, f);
    spec_.forward = f;
    spec_.up = up;
    std::array<Geometry, kBoneCount> geo;
    for (std::size_t i = 0; i < kBoneCount; ++i) geo[i] = geometry_of(spec_, static_cast<Bone>(i));
    try {
        for (std::size_t i = 0; i < kBoneCount; ++i) {
            DBodyDesc d;
            d.shape = geo[i].shape;
            d.half_extents = geo[i].half;
            d.position = geo[i].center;
            d.rotation = geo[i].rotation;
            d.mass = spec_.mass * kMassFraction[i];
            d.surface = Surface::Default;
            d.self_collision_group = group;
            d.linear_damping = 0.02f;
            d.angular_damping = 0.1f;
            bodies_[i] = world.add_body(d);
            mass_[i] = d.mass;

            rest_[i] = {geo[i].center, geo[i].rotation};
        }
        const auto sw = [&](Bone p, Bone c, V pivot, V axis, double swing, double twist) { return JointDef{p, c, DConstraintKind::SwingTwist, pivot, axis, -twist, twist, swing}; };
        const auto hinge = [&](Bone p, Bone c, V pivot, double lo, double hi) { return JointDef{p, c, DConstraintKind::Hinge, pivot, left, lo, hi, 0.0}; };
        std::vector<JointDef> joints;
        joints.push_back(sw(Bone::Pelvis, Bone::Torso, (spec_.pelvis + spec_.chest) * 0.5, up, lim.spine_swing, lim.spine_twist));
        joints.push_back(sw(Bone::Torso, Bone::Head, spec_.neck, up, lim.neck_swing, lim.neck_twist));
        for (int i = 0; i < 2; ++i) {
            const Bone ua = i == 0 ? Bone::UpperArmL : Bone::UpperArmR, fa = i == 0 ? Bone::ForearmL : Bone::ForearmR;
            const Bone th = i == 0 ? Bone::ThighL : Bone::ThighR, sh = i == 0 ? Bone::ShinL : Bone::ShinR, ft = i == 0 ? Bone::FootL : Bone::FootR;
            joints.push_back(sw(Bone::Torso, ua, spec_.shoulder[i], norm(spec_.elbow[i] - spec_.shoulder[i]), lim.shoulder_swing, lim.shoulder_twist));
            joints.push_back(hinge(ua, fa, spec_.elbow[i], -lim.elbow_flexion, 0.05));  // flexion toward the front is a negative angle about the left axis
            joints.push_back(sw(Bone::Pelvis, th, spec_.hip[i], norm(spec_.knee[i] - spec_.hip[i]), lim.hip_swing, lim.hip_twist));
            joints.push_back(hinge(th, sh, spec_.knee[i], -0.05, lim.knee_flexion));  // flexion (the foot going back) is a positive angle about the left axis
            joints.push_back(sw(sh, ft, spec_.ankle[i], norm(spec_.ankle[i] - spec_.knee[i]), lim.ankle_swing, lim.ankle_twist));
        }
        for (const JointDef& j : joints) {
            DConstraintDesc c;
            c.kind = j.kind;
            c.a = bodies_[B(j.parent)];
            c.b = bodies_[B(j.child)];
            c.pivot = j.pivot;
            c.axis = j.axis;
            c.min_angle = j.lo;
            c.max_angle = j.hi;
            c.swing_half_angle = j.swing;
            joints_.push_back({world.add_constraint(c), j.parent, j.child});
        }
    } catch (...) {
        for (const BodyId id : bodies_) {
            if (id.valid()) world.remove_body(id);
        }
        throw;
    }
}

Ragdoll::Ragdoll(Ragdoll&& o) noexcept : world_(o.world_), spec_(o.spec_), rest_(o.rest_), bodies_(o.bodies_), mass_(o.mass_), joints_(std::move(o.joints_)) { o.world_ = nullptr; }

Ragdoll& Ragdoll::operator=(Ragdoll&& o) noexcept {
    if (this != &o) {
        if (world_ != nullptr) {
            for (const BodyId id : bodies_) {
                try {
                    world_->remove_body(id);
                } catch (...) {
                }
            }
        }
        world_ = o.world_;
        spec_ = o.spec_;
        rest_ = o.rest_;
        bodies_ = o.bodies_;
        mass_ = o.mass_;
        joints_ = std::move(o.joints_);
        o.world_ = nullptr;
    }
    return *this;
}

Ragdoll::~Ragdoll() {
    if (world_ == nullptr) return;
    for (const BodyId id : bodies_) {
        try {
            world_->remove_body(id);
        } catch (...) {
        }
    }
}

void Ragdoll::start(const RagdollPose& now, const RagdollPose& before, double dt) {
    if (!(dt > 0.0) || !std::isfinite(dt)) throw std::invalid_argument("Ragdoll::start: dt must be positive and finite");
    for (std::size_t i = 0; i < kBoneCount; ++i) {
        if (!finite(now[i].position) || !finite(before[i].position)) throw std::invalid_argument("Ragdoll::start: a pose is not finite");
    }
    for (std::size_t i = 0; i < kBoneCount; ++i) {
        const V v = (now[i].position - before[i].position) * (1.0 / dt);
        const V w = rotation_vector(mul(normalized(now[i].rotation), conj(normalized(before[i].rotation)))) * (1.0 / dt);
        world_->set_transform(bodies_[i], now[i].position, normalized(now[i].rotation));
        world_->set_velocity(bodies_[i], v, w);
    }
}

RagdollPose Ragdoll::pose() const {
    RagdollPose p;
    for (std::size_t i = 0; i < kBoneCount; ++i) {
        const DBodyState s = world_->state(bodies_[i]);
        p[i] = {s.position, s.rotation};
    }
    return p;
}

DVec Ragdoll::center_of_mass() const {
    V c{};
    for (std::size_t i = 0; i < kBoneCount; ++i) c = c + world_->state(bodies_[i]).position * mass_[i];
    return c * (1.0 / spec_.mass);
}

DVec Ragdoll::linear_momentum() const {
    V p{};
    for (std::size_t i = 0; i < kBoneCount; ++i) p = p + world_->state(bodies_[i]).linear_velocity * mass_[i];
    return p;
}

bool Ragdoll::lying_face_up() const {
    const DBodyState t = world_->state(bodies_[B(Bone::Torso)]);
    return dot(rotate(normalized(t.rotation), V{1, 0, 0}), spec_.up) > 0.0;
}

namespace {
bool load_bearing(Bone child) {
    return child == Bone::ThighL || child == Bone::ThighR || child == Bone::ShinL || child == Bone::ShinR || child == Bone::FootL || child == Bone::FootR;
}
} // namespace

void Ragdoll::drive_to_pose(const RagdollPose& target, const DMotor& motor, double support_scale) {
    if (!(support_scale >= 1.0) || !std::isfinite(support_scale)) throw std::invalid_argument("Ragdoll::drive_to_pose: support_scale must be at least 1");
    for (const BoneTransform& t : target) {
        if (!finite(t.position) || !std::isfinite(t.rotation.x + t.rotation.y + t.rotation.z + t.rotation.w)) throw std::invalid_argument("Ragdoll::drive_to_pose: the target pose is not finite");
    }
    for (const BodyId id : bodies_) world_->wake(id);  // a motor does not wake a sleeping body
    for (const Joint& j : joints_) {
        const Q rel = mul(conj(normalized(target[B(j.parent)].rotation)), normalized(target[B(j.child)].rotation));
        world_->set_motor(j.id, rel, load_bearing(j.child) ? DMotor{motor.frequency * support_scale, motor.damping, motor.max_torque} : motor);
    }
}

void Ragdoll::release() {
    for (const Joint& j : joints_) world_->clear_motor(j.id);
}

void Ragdoll::balance(const RagdollPose& target, const DMotor& motor, double gain, double damping, double support_scale) {
    if (!(gain >= 0.0) || !(damping >= 0.0) || !std::isfinite(gain + damping)) throw std::invalid_argument("Ragdoll::balance: the gains must be finite and not negative");
    const V f = spec_.forward, up = spec_.up, left = cross(up, f);
    V com{}, vel{};
    for (std::size_t i = 0; i < kBoneCount; ++i) {
        const DBodyState s = world_->state(bodies_[i]);
        com = com + s.position * mass_[i];
        vel = vel + s.linear_velocity * mass_[i];
    }
    com = com * (1.0 / spec_.mass);
    vel = vel * (1.0 / spec_.mass);
    const V support = (world_->state(bodies_[B(Bone::FootL)]).position + world_->state(bodies_[B(Bone::FootR)]).position) * 0.5;
    for (const BodyId id : bodies_) world_->wake(id);
    const double pitch = std::clamp(gain * dot(com - support, f) + damping * dot(vel, f), -0.35, 0.35);
    const double roll = std::clamp(-(gain * dot(com - support, left) + damping * dot(vel, left)), -0.35, 0.35);
    // A tilt of the foot target relative to the shin: toe down (positive about the left axis) moves the shin back relative to a foot flat on the ground, which pushes the center of mass back.
    const Q tilt = mul(Q{left.x * std::sin(pitch / 2), left.y * std::sin(pitch / 2), left.z * std::sin(pitch / 2), std::cos(pitch / 2)}, Q{f.x * std::sin(roll / 2), f.y * std::sin(roll / 2), f.z * std::sin(roll / 2), std::cos(roll / 2)});
    for (const Joint& j : joints_) {
        if (j.child != Bone::FootL && j.child != Bone::FootR) continue;
        const Q tilted_foot = mul(tilt, normalized(target[B(j.child)].rotation));
        const Q rel = mul(conj(normalized(target[B(j.parent)].rotation)), tilted_foot);
        world_->set_motor(j.id, rel, DMotor{motor.frequency * support_scale, motor.damping, motor.max_torque});
    }
}

RagdollRecovery::RagdollRecovery(double duration) : duration_(duration) {
    if (!(duration > 0.0) || !std::isfinite(duration)) throw std::invalid_argument("RagdollRecovery: the duration must be positive and finite");
}

void RagdollRecovery::advance(double dt) noexcept {
    if (dt > 0.0 && std::isfinite(dt)) time_ = std::min(duration_, time_ + dt);
}

double RagdollRecovery::weight() const noexcept {
    const double u = std::clamp(time_ / duration_, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

RagdollPose RagdollRecovery::blend(const RagdollPose& physical, const RagdollPose& animated) const {
    const double w = weight();
    RagdollPose out;
    for (std::size_t i = 0; i < kBoneCount; ++i) {
        out[i].position = physical[i].position * (1.0 - w) + animated[i].position * w;
        out[i].rotation = slerp(normalized(physical[i].rotation), normalized(animated[i].rotation), w);
    }
    return out;
}

} // namespace saige
