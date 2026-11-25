// The Jolt Physics backend of the SAIGE dynamics interface (include/saige/dynamics.hpp). Compiled only with -DSAIGE_PHYSICS_JOLT=ON. Everything that names Jolt is in this file.
#include "saige/dynamics.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace saige {
namespace {

namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0, MOVING = 1;
}
namespace BPLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0), MOVING(1);
}

class BPInterface final : public JPH::BroadPhaseLayerInterface {
public:
    unsigned GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return l == Layers::NON_MOVING ? BPLayers::NON_MOVING : BPLayers::MOVING; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "layer"; }
#endif
};
class ObjVsBP final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l1, JPH::BroadPhaseLayer l2) const override { return l1 == Layers::NON_MOVING ? l2 == BPLayers::MOVING : true; }
};
class ObjPair final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == Layers::MOVING || b == Layers::MOVING; }
};

void init_jolt_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

bool fin(double x) { return std::isfinite(x); }
bool fin(const DVec& v) { return fin(v.x) && fin(v.y) && fin(v.z); }

JPH::Vec3 jv(const DVec& v) { return JPH::Vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)); }
JPH::RVec3 jr(const DVec& v) { return JPH::RVec3(static_cast<JPH::Real>(v.x), static_cast<JPH::Real>(v.y), static_cast<JPH::Real>(v.z)); }
JPH::Quat jq(const DQuat& q) { return JPH::Quat(static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)).Normalized(); }
DVec fromv(JPH::Vec3Arg v) { return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())}; }
DVec fromr(JPH::RVec3Arg v) { return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())}; }

struct JoltInit {
    JoltInit() { init_jolt_once(); }  // a base class: it runs before any member allocates
};

class JoltDynamics final : private JoltInit, public DynamicsWorld {
public:
    explicit JoltDynamics(const DynamicsConfig& c) : config_(c), temp_(32u * 1024u * 1024u) {
        if (c.max_bodies < 16 || c.max_bodies > 1000000 || !fin(c.gravity) || c.threads < 1 || c.threads > 64 || c.collision_steps < 1 || c.collision_steps > 16 || c.velocity_iterations < 1 || c.position_iterations < 1) {
            throw std::invalid_argument("DynamicsConfig: a value is out of range");
        }
        js_ = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(c.threads) - 1);
        const unsigned cap = static_cast<unsigned>(c.max_bodies);
        ps_.Init(cap, 0, cap * 4u, cap * 4u, bp_, ovb_, olp_);
        ps_.SetGravity(JPH::Vec3(0, 0, static_cast<float>(-c.gravity)));
        JPH::PhysicsSettings s = ps_.GetPhysicsSettings();
        s.mNumVelocitySteps = static_cast<unsigned>(c.velocity_iterations);
        s.mNumPositionSteps = static_cast<unsigned>(c.position_iterations);
        s.mAllowSleeping = c.allow_sleeping;
        ps_.SetPhysicsSettings(s);
        // SAIGE's contact rule, stated rather than inherited: friction by the geometric mean, restitution by the larger.
        ps_.SetCombineFriction([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) { return std::sqrt(a.GetFriction() * b.GetFriction()); });
        ps_.SetCombineRestitution([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) { return std::max(a.GetRestitution(), b.GetRestitution()); });
    }
    ~JoltDynamics() override {
        auto& bi = ps_.GetBodyInterface();
        for (CSlot& c : cslots_) {
            if (c.alive) ps_.RemoveConstraint(c.c);
        }
        cslots_.clear();
        for (const Slot& s : slots_) {
            if (s.alive) {
                bi.RemoveBody(s.id);
                bi.DestroyBody(s.id);
            }
        }
        clear_static();
    }

    DynamicsInfo info() const override {
        DynamicsInfo i;
        i.backend = "jolt";
        i.version = std::to_string(JPH_VERSION_MAJOR) + "." + std::to_string(JPH_VERSION_MINOR) + "." + std::to_string(JPH_VERSION_PATCH);
#ifdef JPH_DOUBLE_PRECISION
        i.double_precision = true;
#endif
        i.multithreaded = true;
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
        i.deterministic_across_threads = true;
#else
        i.deterministic_across_threads = false;
#endif
        i.continuous_collision = true;
        i.max_bodies = config_.max_bodies;
        return i;
    }

    void set_static_geometry(const StaticCollision& collision, const SurfaceMap* surfaces) override {
        clear_static();
        auto& bi = ps_.GetBodyInterface();
        std::size_t index = 0;
        for (const Aabb& b : collision.boxes()) {
            const DVec c{(static_cast<double>(b.min.x) + static_cast<double>(b.max.x)) * 0.5, (static_cast<double>(b.min.y) + static_cast<double>(b.max.y)) * 0.5, (static_cast<double>(b.min.z) + static_cast<double>(b.max.z)) * 0.5};
            const JPH::Vec3 half(static_cast<float>(b.max.x - b.min.x) * 0.5f, static_cast<float>(b.max.y - b.min.y) * 0.5f, static_cast<float>(b.max.z - b.min.z) * 0.5f);
            JPH::BodyCreationSettings s(new JPH::BoxShape(half, 0.0f), jr(c), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING);
            const PhysicsMaterial& m = surfaces != nullptr ? surfaces->material_at({StaticPrimitive::Kind::Box, index}) : physics_material(Surface::Default);
            s.mFriction = m.friction;
            s.mRestitution = m.restitution;
            static_ids_.push_back(bi.CreateAndAddBody(s, JPH::EActivation::DontActivate));
            ++index;
        }
        // Triangles, grouped by surface: one mesh body per surface so each has its own material.
        std::map<int, JPH::TriangleList> groups;
        index = 0;
        for (const Triangle& t : collision.triangles()) {
            const Surface surf = surfaces != nullptr ? surfaces->at({StaticPrimitive::Kind::Triangle, index}) : Surface::Default;
            // SAIGE triangles are two-sided; Jolt's simulation ignores back faces, so each triangle is added in both windings.
            groups[static_cast<int>(surf)].push_back(JPH::Triangle(JPH::Float3(t.a.x, t.a.y, t.a.z), JPH::Float3(t.b.x, t.b.y, t.b.z), JPH::Float3(t.c.x, t.c.y, t.c.z)));
            groups[static_cast<int>(surf)].push_back(JPH::Triangle(JPH::Float3(t.a.x, t.a.y, t.a.z), JPH::Float3(t.c.x, t.c.y, t.c.z), JPH::Float3(t.b.x, t.b.y, t.b.z)));
            ++index;
        }
        for (auto& [surf, list] : groups) {
            JPH::MeshShapeSettings ms(list);
            JPH::ShapeSettings::ShapeResult r = ms.Create();
            if (r.HasError()) throw std::invalid_argument("set_static_geometry: the triangles could not be made into a shape");
            JPH::BodyCreationSettings s(r.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING);
            const PhysicsMaterial& m = physics_material(static_cast<Surface>(surf));
            s.mFriction = m.friction;
            s.mRestitution = m.restitution;
            static_ids_.push_back(bi.CreateAndAddBody(s, JPH::EActivation::DontActivate));
        }
        ps_.OptimizeBroadPhase();
    }

    BodyId add_body(const DBodyDesc& d) override {
        if (!fin(d.position) || !fin(d.linear_velocity) || !fin(d.angular_velocity) || !fin(d.half_extents) || !fin(d.rotation.x) || !fin(d.rotation.y) || !fin(d.rotation.z) || !fin(d.rotation.w) || !fin(d.mass) || !(d.mass >= 0.0) ||
            !(d.half_extents.x > 0.0) || (d.shape == DShapeKind::Box && (!(d.half_extents.y > 0.0) || !(d.half_extents.z > 0.0))) || (d.shape == DShapeKind::Capsule && !(d.half_extents.y > 0.0)) || !(d.linear_damping >= 0.0f) ||
            !(d.angular_damping >= 0.0f) || !std::isfinite(d.linear_damping) || !std::isfinite(d.angular_damping)) {
            throw std::invalid_argument("DBodyDesc: a value is not finite, a size is not positive, or a mass or damping is negative");
        }
        std::size_t alive = 0;
        for (const Slot& s : slots_) alive += s.alive ? 1u : 0u;
        if (alive + static_ids_.size() + 8 >= config_.max_bodies) throw std::invalid_argument("add_body: the world is full");
        JPH::RefConst<JPH::Shape> shape;
        double volume = 0.0;
        switch (d.shape) {
        case DShapeKind::Box:
            shape = new JPH::BoxShape(jv(d.half_extents), 0.0f);
            volume = 8.0 * d.half_extents.x * d.half_extents.y * d.half_extents.z;
            break;
        case DShapeKind::Sphere:
            shape = new JPH::SphereShape(static_cast<float>(d.half_extents.x));
            volume = 4.0 / 3.0 * 3.14159265358979 * d.half_extents.x * d.half_extents.x * d.half_extents.x;
            break;
        case DShapeKind::Capsule:
            shape = new JPH::CapsuleShape(static_cast<float>(d.half_extents.y), static_cast<float>(d.half_extents.x));
            volume = 3.14159265358979 * d.half_extents.x * d.half_extents.x * (2.0 * d.half_extents.y + 4.0 / 3.0 * d.half_extents.x);
            break;
        }
        const bool dynamic = d.motion == DMotion::Dynamic;
        const JPH::EMotionType motion = d.motion == DMotion::Static ? JPH::EMotionType::Static : (d.motion == DMotion::Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic);
        JPH::BodyCreationSettings s(shape, jr(d.position), jq(d.rotation), motion, d.motion == DMotion::Static ? Layers::NON_MOVING : Layers::MOVING);
        const PhysicsMaterial& m = physics_material(d.surface);
        s.mFriction = d.friction >= 0.0f ? d.friction : m.friction;
        s.mRestitution = d.restitution >= 0.0f ? d.restitution : m.restitution;
        s.mLinearDamping = d.linear_damping;
        s.mAngularDamping = d.angular_damping;
        if (d.self_collision_group != 0) {
            Group& g = groups_[d.self_collision_group];
            if (g.count >= 64) throw std::invalid_argument("add_body: a self-collision group holds at most 64 bodies");
            if (g.filter == nullptr) {
                g.filter = new JPH::GroupFilterTable(64);
                for (JPH::CollisionGroup::SubGroupID a = 0; a < 64; ++a) {
                    for (JPH::CollisionGroup::SubGroupID b = a + 1; b < 64; ++b) g.filter->DisableCollision(a, b);
                }
            }
            s.mCollisionGroup = JPH::CollisionGroup(g.filter, d.self_collision_group, g.count++);
        }
        if (dynamic) {
            const double mass = d.mass > 0.0 ? d.mass : static_cast<double>(m.density) * volume;
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            s.mMassPropertiesOverride.mMass = static_cast<float>(mass);
            if (d.continuous) s.mMotionQuality = JPH::EMotionQuality::LinearCast;
            s.mLinearVelocity = jv(d.linear_velocity);
            s.mAngularVelocity = jv(d.angular_velocity);
        }
        const JPH::BodyID id = ps_.GetBodyInterface().CreateAndAddBody(s, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].alive) {
                slots_[i].id = id;
                slots_[i].alive = true;
                return {static_cast<std::uint32_t>(i), slots_[i].generation};
            }
        }
        slots_.push_back({id, 0, true});
        return {static_cast<std::uint32_t>(slots_.size() - 1), 0};
    }

    void remove_body(BodyId id) override {
        Slot& s = slot(id);
        for (CSlot& cs : cslots_) {  // a constraint on a removed body goes with it
            if (cs.alive && uses(static_cast<JPH::TwoBodyConstraint*>(cs.c), s.id)) drop(cs);
        }
        ps_.GetBodyInterface().RemoveBody(s.id);
        ps_.GetBodyInterface().DestroyBody(s.id);
        s.alive = false;
        ++s.generation;
    }

    ConstraintId add_constraint(const DConstraintDesc& d) override {
        if (!fin(d.pivot) || !fin(d.axis) || !fin(d.pivot_b) || !fin(d.min_angle) || !fin(d.max_angle) || !fin(d.swing_half_angle) || !fin(d.min_distance) || !fin(d.max_distance) || !(d.min_angle <= d.max_angle) ||
            !(d.min_distance >= 0.0 && d.max_distance >= d.min_distance)) {
            throw std::invalid_argument("DConstraintDesc: a value is not finite or a range is reversed");
        }
        JPH::Body* a = &ps_.GetBodyLockInterfaceNoLock().TryGetBody(slot(d.a).id)[0];
        JPH::Body* b = d.b.valid() ? &ps_.GetBodyLockInterfaceNoLock().TryGetBody(slot(d.b).id)[0] : &JPH::Body::sFixedToWorld;
        // Angles are those of the second body relative to the first. With no second body the world is the first, so a hinge's angle is that of `a` itself about the axis (right-hand rule).
        const bool world_first = !d.b.valid();
        if (world_first) std::swap(a, b);
        JPH::Vec3 axis = jv(d.axis);
        if (axis.LengthSq() < 1e-12f) throw std::invalid_argument("DConstraintDesc: the axis is zero");
        axis = axis.Normalized();
        JPH::Constraint* c = nullptr;
        switch (d.kind) {
        case DConstraintKind::Hinge: {
            JPH::HingeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = jr(d.pivot);
            s.mHingeAxis1 = s.mHingeAxis2 = axis;
            s.mNormalAxis1 = s.mNormalAxis2 = axis.GetNormalizedPerpendicular();
            s.mLimitsMin = static_cast<float>(d.min_angle);
            s.mLimitsMax = static_cast<float>(d.max_angle);
            c = s.Create(*a, *b);
            break;
        }
        case DConstraintKind::SwingTwist: {
            JPH::SwingTwistConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPosition1 = s.mPosition2 = jr(d.pivot);
            s.mTwistAxis1 = s.mTwistAxis2 = axis;
            s.mPlaneAxis1 = s.mPlaneAxis2 = axis.GetNormalizedPerpendicular();
            s.mNormalHalfConeAngle = static_cast<float>(d.swing_half_angle);
            s.mPlaneHalfConeAngle = static_cast<float>(d.swing_half_angle);
            s.mTwistMinAngle = static_cast<float>(d.min_angle);
            s.mTwistMaxAngle = static_cast<float>(d.max_angle);
            c = s.Create(*a, *b);
            break;
        }
        case DConstraintKind::Fixed: {
            JPH::FixedConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mAutoDetectPoint = true;
            c = s.Create(*a, *b);
            break;
        }
        case DConstraintKind::Distance: {
            JPH::DistanceConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = jr(world_first ? (d.use_pivot_b ? d.pivot_b : d.pivot) : d.pivot);
            s.mPoint2 = jr(world_first ? d.pivot : (d.use_pivot_b ? d.pivot_b : d.pivot));
            s.mMinDistance = static_cast<float>(d.min_distance);
            s.mMaxDistance = static_cast<float>(d.max_distance);
            c = s.Create(*a, *b);
            break;
        }
        }
        std::size_t alive = 0;
        for (const CSlot& cs : cslots_) alive += cs.alive ? 1u : 0u;
        if (alive >= config_.max_constraints) {
            c->AddRef();
            c->Release();
            throw std::invalid_argument("add_constraint: too many constraints");
        }
        ps_.AddConstraint(c);
        for (std::size_t i = 0; i < cslots_.size(); ++i) {
            if (!cslots_[i].alive) {
                cslots_[i].c = c;
                cslots_[i].kind = d.kind;
                cslots_[i].alive = true;
                return {static_cast<std::uint32_t>(i), cslots_[i].generation};
            }
        }
        cslots_.push_back({c, d.kind, 0, true});
        return {static_cast<std::uint32_t>(cslots_.size() - 1), 0};
    }

    void set_motor(ConstraintId id, DQuat target, const DMotor& m) override {
        CSlot& cs = cslot(id);
        if (!fin(target.x) || !fin(target.y) || !fin(target.z) || !fin(target.w) || !(m.frequency > 0.0) || !fin(m.frequency) || !(m.damping >= 0.0) || !fin(m.damping) || !(m.max_torque >= 0.0) || !fin(m.max_torque)) {
            throw std::invalid_argument("set_motor: a value is not finite or the frequency is not positive");
        }
        const float limit = m.max_torque > 0.0 ? static_cast<float>(m.max_torque) : 1.0e7f;  // "unlimited" is a very large finite torque (FLT_MAX would overflow the solver)
        const auto configure = [&](JPH::MotorSettings& ms) {
            ms.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, static_cast<float>(m.frequency), static_cast<float>(m.damping));
            ms.SetTorqueLimit(limit);
        };
        if (cs.kind == DConstraintKind::Hinge) {
            auto* h = static_cast<JPH::HingeConstraint*>(cs.c);
            configure(h->GetMotorSettings());
            h->SetMotorState(JPH::EMotorState::Position);
            h->SetTargetOrientationBS(jq(target));
            wake_both(cs);
        } else if (cs.kind == DConstraintKind::SwingTwist) {
            auto* t = static_cast<JPH::SwingTwistConstraint*>(cs.c);
            configure(t->GetSwingMotorSettings());
            configure(t->GetTwistMotorSettings());
            t->SetSwingMotorState(JPH::EMotorState::Position);
            t->SetTwistMotorState(JPH::EMotorState::Position);
            t->SetTargetOrientationBS(jq(target));
            wake_both(cs);
        } else {
            throw std::invalid_argument("set_motor: only hinge and swing-twist constraints have motors");
        }
    }

    void clear_motor(ConstraintId id) override {
        CSlot& cs = cslot(id);
        if (cs.kind == DConstraintKind::Hinge) {
            static_cast<JPH::HingeConstraint*>(cs.c)->SetMotorState(JPH::EMotorState::Off);
            wake_both(cs);
        } else if (cs.kind == DConstraintKind::SwingTwist) {
            auto* t = static_cast<JPH::SwingTwistConstraint*>(cs.c);
            t->SetSwingMotorState(JPH::EMotorState::Off);
            t->SetTwistMotorState(JPH::EMotorState::Off);
            wake_both(cs);
        } else {
            throw std::invalid_argument("clear_motor: only hinge and swing-twist constraints have motors");
        }
    }

    void remove_constraint(ConstraintId id) override {
        CSlot& cs = cslot(id);
        wake_both(cs);  // what it was holding up must fall
        drop(cs);
    }

    DBodyState state(BodyId id) const override {
        const Slot& s = slot_const(id);
        const auto& bi = ps_.GetBodyInterface();
        DBodyState out;
        out.position = fromr(bi.GetCenterOfMassPosition(s.id));
        const JPH::Quat q = bi.GetRotation(s.id);
        out.rotation = {static_cast<double>(q.GetX()), static_cast<double>(q.GetY()), static_cast<double>(q.GetZ()), static_cast<double>(q.GetW())};
        out.linear_velocity = fromv(bi.GetLinearVelocity(s.id));
        out.angular_velocity = fromv(bi.GetAngularVelocity(s.id));
        out.active = bi.IsActive(s.id);
        return out;
    }
    void set_velocity(BodyId id, DVec linear, DVec angular) override {
        if (!fin(linear) || !fin(angular)) throw std::invalid_argument("set_velocity: not finite");
        auto& bi = ps_.GetBodyInterface();
        bi.SetLinearAndAngularVelocity(slot(id).id, jv(linear), jv(angular));
    }
    void add_impulse(BodyId id, DVec impulse, DVec world_point) override {
        if (!fin(impulse) || !fin(world_point)) throw std::invalid_argument("add_impulse: not finite");
        ps_.GetBodyInterface().AddImpulse(slot(id).id, jv(impulse), jr(world_point));
    }
    void add_angular_impulse(BodyId id, DVec impulse) override {
        if (!fin(impulse)) throw std::invalid_argument("add_angular_impulse: not finite");
        ps_.GetBodyInterface().AddAngularImpulse(slot(id).id, jv(impulse));
    }
    void wake(BodyId id) override { ps_.GetBodyInterface().ActivateBody(slot(id).id); }
    void set_transform(BodyId id, DVec position, DQuat rotation) override {
        if (!fin(position) || !fin(rotation.x) || !fin(rotation.y) || !fin(rotation.z) || !fin(rotation.w)) throw std::invalid_argument("set_transform: not finite");
        ps_.GetBodyInterface().SetPositionAndRotation(slot(id).id, jr(position), jq(rotation), JPH::EActivation::Activate);
    }
    void set_kinematic_target(BodyId id, DVec position, DQuat rotation, double dt) override {
        if (!fin(position) || !(dt > 0.0) || !fin(dt)) throw std::invalid_argument("set_kinematic_target: not finite");
        ps_.GetBodyInterface().MoveKinematic(slot(id).id, jr(position), jq(rotation), static_cast<float>(dt));
    }
    std::size_t body_count() const override {
        std::size_t n = 0;
        for (const Slot& s : slots_) n += s.alive ? 1u : 0u;
        return n;
    }
    void step(double dt) override {
        if (!(dt > 0.0) || !fin(dt) || dt > 0.1) throw std::invalid_argument("step: dt must be finite, positive and at most 0.1 s");
        ps_.Update(static_cast<float>(dt), config_.collision_steps, &temp_, js_.get());
    }
    void shift_origin(DVec shift) override {
        if (!fin(shift)) throw std::invalid_argument("shift_origin: not finite");
        auto& bi = ps_.GetBodyInterface();
        for (const Slot& s : slots_) {
            if (!s.alive) continue;
            bi.SetPosition(s.id, bi.GetPosition(s.id) + jr(shift), JPH::EActivation::DontActivate);
        }
        clear_static();  // the caller sets the geometry again in the new frame
    }
    DRayHit cast_ray(DVec from, DVec to) const override {
        DRayHit out;
        const DVec d{to.x - from.x, to.y - from.y, to.z - from.z};
        JPH::RayCastResult hit;
        const JPH::RRayCast ray{jr(from), jv(d)};
        if (!ps_.GetNarrowPhaseQuery().CastRay(ray, hit)) return out;
        out.hit = true;
        out.fraction = static_cast<double>(hit.mFraction);
        const JPH::RVec3 p = ray.GetPointOnRay(hit.mFraction);
        out.point = fromr(p);
        JPH::BodyLockRead lock(ps_.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            out.normal = fromv(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p));
            if (out.normal.x * d.x + out.normal.y * d.y + out.normal.z * d.z > 0.0) out.normal = {-out.normal.x, -out.normal.y, -out.normal.z};  // faces the ray's origin, as StaticCollision's do
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].alive && slots_[i].id == hit.mBodyID) out.body = {static_cast<std::uint32_t>(i), slots_[i].generation};
            }
        }
        return out;
    }

private:
    struct CSlot {
        JPH::Constraint* c = nullptr;
        DConstraintKind kind = DConstraintKind::Fixed;
        std::uint32_t generation = 0;
        bool alive = false;
    };
    CSlot& cslot(ConstraintId id) {
        if (id.index >= cslots_.size() || !cslots_[id.index].alive || cslots_[id.index].generation != id.generation) throw std::invalid_argument("a stale or invalid constraint id");
        return cslots_[id.index];
    }
    void drop(CSlot& cs) {
        ps_.RemoveConstraint(cs.c);
        cs.alive = false;
        cs.c = nullptr;
        ++cs.generation;
    }
    void wake_both(const CSlot& cs) {  // a motor, or the lack of one, changes what a resting body should do: it must not sleep through that
        const auto* tb = static_cast<const JPH::TwoBodyConstraint*>(cs.c);
        for (const JPH::Body* b : {tb->GetBody1(), tb->GetBody2()}) {
            if (b != &JPH::Body::sFixedToWorld && b->IsDynamic()) ps_.GetBodyInterface().ActivateBody(b->GetID());
        }
    }

    std::vector<CSlot> cslots_;
    struct Group {
        JPH::Ref<JPH::GroupFilterTable> filter;
        std::uint32_t count = 0;
    };
    std::map<std::uint32_t, Group> groups_;
    struct Slot {
        JPH::BodyID id;
        std::uint32_t generation = 0;
        bool alive = false;
    };
    Slot& slot(BodyId id) {
        if (id.index >= slots_.size() || !slots_[id.index].alive || slots_[id.index].generation != id.generation) throw std::invalid_argument("a stale or invalid body id");
        return slots_[id.index];
    }
    const Slot& slot_const(BodyId id) const {
        if (id.index >= slots_.size() || !slots_[id.index].alive || slots_[id.index].generation != id.generation) throw std::invalid_argument("a stale or invalid body id");
        return slots_[id.index];
    }
    static bool uses(JPH::TwoBodyConstraint* c, JPH::BodyID id) { return c->GetBody1()->GetID() == id || c->GetBody2()->GetID() == id; }
    void clear_static() {
        auto& bi = ps_.GetBodyInterface();
        for (const JPH::BodyID id : static_ids_) {
            bi.RemoveBody(id);
            bi.DestroyBody(id);
        }
        static_ids_.clear();
    }

    DynamicsConfig config_;
    BPInterface bp_;
    ObjVsBP ovb_;
    ObjPair olp_;
    JPH::PhysicsSystem ps_;
    JPH::TempAllocatorImpl temp_;
    std::unique_ptr<JPH::JobSystemThreadPool> js_;
    std::vector<Slot> slots_;
    std::vector<JPH::BodyID> static_ids_;
};

} // namespace

std::unique_ptr<DynamicsWorld> make_jolt_dynamics_world(const DynamicsConfig& config) { return std::make_unique<JoltDynamics>(config); }

} // namespace saige
