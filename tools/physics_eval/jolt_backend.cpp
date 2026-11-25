#include "backend.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <mutex>
#include <thread>

JPH_SUPPRESS_WARNINGS

namespace eval {
namespace {

namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0, MOVING = 1, NUM = 2;
}
namespace BPLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0), MOVING(1);
constexpr unsigned NUM = 2;
}

class BPInterface final : public JPH::BroadPhaseLayerInterface {
public:
    unsigned GetNumBroadPhaseLayers() const override { return BPLayers::NUM; }
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

JPH::Vec3 jv(V3 v) { return JPH::Vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)); }
JPH::RVec3 jr(V3 v) {
#ifdef JPH_DOUBLE_PRECISION
    return JPH::RVec3(v.x, v.y, v.z);
#else
    return JPH::RVec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
#endif
}
V3 fromj(JPH::Vec3Arg v) { return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())}; }

struct JoltInit {
    JoltInit() { init_jolt_once(); }  // a base class: it runs before any member allocates
};

class Jolt final : private JoltInit, public Backend {
public:
    explicit Jolt(int max_bodies) : temp_(64u * 1024u * 1024u), threads_(1) {
        js_ = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 0);  // 0 helper threads: the caller's thread only
        const unsigned cap = static_cast<unsigned>(max_bodies);
        ps_.Init(cap, 0, cap * 4u, cap * 4u, bp_, ovb_, olp_);
        ps_.SetGravity(JPH::Vec3(0, 0, -9.81f));
    }
    ~Jolt() override {
        auto& bi = ps_.GetBodyInterface();
        for (JPH::Constraint* c : constraints_) ps_.RemoveConstraint(c);
        for (const JPH::BodyID id : ids_) {
            bi.RemoveBody(id);
            bi.DestroyBody(id);
        }
    }
    std::string name() const override {
#ifdef JPH_DOUBLE_PRECISION
        return "Jolt 5.3.0 (double)";
#else
        return "Jolt 5.3.0";
#endif
    }
    int add_box(const BoxDesc& d) override {
        JPH::BodyCreationSettings s(new JPH::BoxShape(jv(d.half)), jr(d.center), JPH::Quat(static_cast<float>(d.rotation.x), static_cast<float>(d.rotation.y), static_cast<float>(d.rotation.z), static_cast<float>(d.rotation.w)),
                                    d.mass > 0 ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, d.mass > 0 ? Layers::MOVING : Layers::NON_MOVING);
        finish(s, d.mass, d.material, d.ccd);
        return create(s, d.mass > 0);
    }
    int add_sphere(V3 c, double r, double mass, Material m, bool ccd) override {
        JPH::BodyCreationSettings s(new JPH::SphereShape(static_cast<float>(r)), jr(c), JPH::Quat::sIdentity(), mass > 0 ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, mass > 0 ? Layers::MOVING : Layers::NON_MOVING);
        finish(s, mass, m, ccd);
        return create(s, mass > 0);
    }
    void add_swing_twist(int a, int b, V3 pivot, V3 axis, double swing, double tmin, double tmax) override {
        JPH::SwingTwistConstraintSettings s;
        s.mSpace = JPH::EConstraintSpace::WorldSpace;
        s.mPosition1 = s.mPosition2 = jr(pivot);
        s.mTwistAxis1 = s.mTwistAxis2 = jv(axis).Normalized();
        s.mPlaneAxis1 = s.mPlaneAxis2 = s.mTwistAxis1.GetNormalizedPerpendicular();
        s.mNormalHalfConeAngle = static_cast<float>(swing);
        s.mPlaneHalfConeAngle = static_cast<float>(swing);
        s.mTwistMinAngle = static_cast<float>(tmin);
        s.mTwistMaxAngle = static_cast<float>(tmax);
        JPH::Constraint* c = s.Create(*body(a), *body(b));
        ps_.AddConstraint(c);
        constraints_.push_back(c);
    }
    void add_hinge(int a, int b, V3 pivot, V3 axis, double mn, double mx) override {
        JPH::HingeConstraintSettings s;
        s.mSpace = JPH::EConstraintSpace::WorldSpace;
        s.mPoint1 = s.mPoint2 = jr(pivot);
        s.mHingeAxis1 = s.mHingeAxis2 = jv(axis).Normalized();
        s.mNormalAxis1 = s.mNormalAxis2 = s.mHingeAxis1.GetNormalizedPerpendicular();
        s.mLimitsMin = static_cast<float>(mn);
        s.mLimitsMax = static_cast<float>(mx);
        JPH::Constraint* c = s.Create(*body(a), *body(b));
        ps_.AddConstraint(c);
        constraints_.push_back(c);
    }
    void set_velocity(int id, V3 v) override { ps_.GetBodyInterface().SetLinearVelocity(ids_[static_cast<size_t>(id)], jv(v)); }
    void apply_force_at(int id, V3 f, V3 p) override { ps_.GetBodyInterface().AddForce(ids_[static_cast<size_t>(id)], jv(f), jr(p)); }
    V3 position(int id) const override {
        const JPH::RVec3 p = ps_.GetBodyInterface().GetCenterOfMassPosition(ids_[static_cast<size_t>(id)]);
        return {static_cast<double>(p.GetX()), static_cast<double>(p.GetY()), static_cast<double>(p.GetZ())};
    }
    Q4 rotation(int id) const override {
        const JPH::Quat q = ps_.GetBodyInterface().GetRotation(ids_[static_cast<size_t>(id)]);
        return {static_cast<double>(q.GetX()), static_cast<double>(q.GetY()), static_cast<double>(q.GetZ()), static_cast<double>(q.GetW())};
    }
    V3 velocity(int id) const override { return fromj(ps_.GetBodyInterface().GetLinearVelocity(ids_[static_cast<size_t>(id)])); }
    V3 angular_velocity(int id) const override { return fromj(ps_.GetBodyInterface().GetAngularVelocity(ids_[static_cast<size_t>(id)])); }
    bool active(int id) const override { return ps_.GetBodyInterface().IsActive(ids_[static_cast<size_t>(id)]); }
    int body_count() const override { return static_cast<int>(ids_.size()); }
    void step(double dt, int substeps) override { ps_.Update(static_cast<float>(dt), substeps, &temp_, js_.get()); }
    bool multithreaded() const override { return true; }
    void set_threads(int n) override {
        threads_ = n < 1 ? 1 : n;
        js_ = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads_ - 1);
    }
    void set_solver_iterations(int n) override {
        JPH::PhysicsSettings s = ps_.GetPhysicsSettings();
        s.mNumVelocitySteps = static_cast<unsigned>(n);
        s.mNumPositionSteps = static_cast<unsigned>(std::max(2, n / 4));
        ps_.SetPhysicsSettings(s);
    }
    double raycast_fraction(V3 from, V3 to) const override {
        const V3 d = to - from;
        JPH::RayCastResult hit;
        const JPH::RRayCast ray{jr(from), jv(d)};
        if (ps_.GetNarrowPhaseQuery().CastRay(ray, hit)) return static_cast<double>(hit.mFraction);
        return -1.0;
    }
    void use_saige_material_rule() override {
        // Jolt's defaults are already SAIGE's rule (friction by the geometric mean, restitution by the larger); set them explicitly so the intent is recorded.
        ps_.SetCombineFriction([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) { return std::sqrt(a.GetFriction() * b.GetFriction()); });
        ps_.SetCombineRestitution([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) { return std::max(a.GetRestitution(), b.GetRestitution()); });
    }

private:
    void finish(JPH::BodyCreationSettings& s, double mass, Material m, bool ccd) {
        s.mFriction = m.friction;
        s.mRestitution = m.restitution;
        s.mLinearDamping = 0.0f;   // Jolt's default is 0.05 per second (Bullet's is 0): zeroed so the two are compared on equal terms
        s.mAngularDamping = 0.0f;
        if (mass > 0) {
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            s.mMassPropertiesOverride.mMass = static_cast<float>(mass);
            if (ccd) s.mMotionQuality = JPH::EMotionQuality::LinearCast;
        }
    }
    int create(const JPH::BodyCreationSettings& s, bool dynamic) {
        auto& bi = ps_.GetBodyInterface();
        const JPH::BodyID id = bi.CreateAndAddBody(s, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        ids_.push_back(id);
        return static_cast<int>(ids_.size() - 1);
    }
    JPH::Body* body(int id) {
        // Constraints need the body objects; the lock interface's no-lock path is fine in this single-threaded setup.
        return const_cast<JPH::Body*>(&ps_.GetBodyLockInterfaceNoLock().TryGetBody(ids_[static_cast<size_t>(id)]) [0]);
    }

    BPInterface bp_;
    ObjVsBP ovb_;
    ObjPair olp_;
    JPH::PhysicsSystem ps_;
    JPH::TempAllocatorImpl temp_;
    std::unique_ptr<JPH::JobSystemThreadPool> js_;
    int threads_;
    std::vector<JPH::BodyID> ids_;
    std::vector<JPH::Constraint*> constraints_;
};

} // namespace

std::unique_ptr<Backend> make_jolt(int max_bodies) { return std::make_unique<Jolt>(max_bodies); }

} // namespace eval
