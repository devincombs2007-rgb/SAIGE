#include "backend.hpp"

#include <btBulletDynamicsCommon.h>

#include <algorithm>

namespace eval {
namespace {

bool saige_rule_callback(btManifoldPoint& cp, const btCollisionObjectWrapper* a, int, int, const btCollisionObjectWrapper* b, int, int) {
    const float fa = a->getCollisionObject()->getFriction(), fb = b->getCollisionObject()->getFriction();
    cp.m_combinedFriction = std::sqrt(fa * fb);
    cp.m_combinedRestitution = std::max(a->getCollisionObject()->getRestitution(), b->getCollisionObject()->getRestitution());
    return true;
}

btVector3 bv(V3 v) { return btVector3(static_cast<btScalar>(v.x), static_cast<btScalar>(v.y), static_cast<btScalar>(v.z)); }
V3 fromb(const btVector3& v) { return {static_cast<double>(v.x()), static_cast<double>(v.y()), static_cast<double>(v.z())}; }

class Bullet final : public Backend {
public:
    explicit Bullet(int) : disp_(&cfg_), world_(&disp_, &bp_, &solver_, &cfg_) { world_.setGravity(btVector3(0, 0, -9.81f)); }
    ~Bullet() override {
        for (btTypedConstraint* c : constraints_) {
            world_.removeConstraint(c);
            delete c;
        }
        for (btRigidBody* b : bodies_) {
            world_.removeRigidBody(b);
            delete b->getMotionState();
            delete b;
        }
        for (btCollisionShape* s : shapes_) delete s;
    }
    std::string name() const override { return "Bullet 3.24"; }
    int add_box(const BoxDesc& d) override {
        auto* shape = new btBoxShape(bv(d.half));
        return create(shape, d.center, d.rotation, d.mass, d.material, d.ccd, std::min({d.half.x, d.half.y, d.half.z}));
    }
    int add_sphere(V3 c, double r, double mass, Material m, bool ccd) override { return create(new btSphereShape(static_cast<btScalar>(r)), c, Q4{}, mass, m, ccd, r); }
    void add_swing_twist(int a, int b, V3 pivot, V3 axis, double swing, double tmin, double tmax) override {
        btRigidBody& ra = *bodies_[static_cast<size_t>(a)];
        btRigidBody& rb = *bodies_[static_cast<size_t>(b)];
        const btVector3 p = bv(pivot);
        // Frames: the cone-twist constraint's x axis is its twist axis; build a frame whose x is the twist axis, in each body's space.
        const btVector3 ax = bv(axis).normalized();
        btVector3 up = std::fabs(ax.z()) < 0.9 ? btVector3(0, 0, 1) : btVector3(1, 0, 0);
        const btVector3 y = ax.cross(up).normalized(), z = ax.cross(y);
        btMatrix3x3 basis(ax.x(), y.x(), z.x(), ax.y(), y.y(), z.y(), ax.z(), y.z(), z.z());
        btTransform world_frame(basis, p);
        const btTransform fa = ra.getWorldTransform().inverse() * world_frame, fb = rb.getWorldTransform().inverse() * world_frame;
        auto* c = new btConeTwistConstraint(ra, rb, fa, fb);
        c->setLimit(static_cast<btScalar>(swing), static_cast<btScalar>(swing), static_cast<btScalar>(std::max(std::fabs(tmin), std::fabs(tmax))));
        world_.addConstraint(c, true);
        constraints_.push_back(c);
    }
    void add_hinge(int a, int b, V3 pivot, V3 axis, double mn, double mx) override {
        btRigidBody& ra = *bodies_[static_cast<size_t>(a)];
        btRigidBody& rb = *bodies_[static_cast<size_t>(b)];
        const btVector3 p = bv(pivot), ax = bv(axis).normalized();
        auto* c = new btHingeConstraint(ra, rb, ra.getWorldTransform().inverse() * p, rb.getWorldTransform().inverse() * p, ra.getWorldTransform().getBasis().inverse() * ax, rb.getWorldTransform().getBasis().inverse() * ax);
        c->setLimit(static_cast<btScalar>(mn), static_cast<btScalar>(mx));
        world_.addConstraint(c, true);
        constraints_.push_back(c);
    }
    void set_velocity(int id, V3 v) override { bodies_[static_cast<size_t>(id)]->setLinearVelocity(bv(v)); }
    void apply_force_at(int id, V3 f, V3 p) override {
        btRigidBody* b = bodies_[static_cast<size_t>(id)];
        b->activate(true);
        b->applyForce(bv(f), bv(p) - b->getCenterOfMassPosition());
    }
    V3 position(int id) const override { return fromb(bodies_[static_cast<size_t>(id)]->getCenterOfMassPosition()); }
    Q4 rotation(int id) const override {
        const btQuaternion q = bodies_[static_cast<size_t>(id)]->getWorldTransform().getRotation();
        return {static_cast<double>(q.x()), static_cast<double>(q.y()), static_cast<double>(q.z()), static_cast<double>(q.w())};
    }
    V3 velocity(int id) const override { return fromb(bodies_[static_cast<size_t>(id)]->getLinearVelocity()); }
    V3 angular_velocity(int id) const override { return fromb(bodies_[static_cast<size_t>(id)]->getAngularVelocity()); }
    bool active(int id) const override { return bodies_[static_cast<size_t>(id)]->getActivationState() != ISLAND_SLEEPING; }
    int body_count() const override { return static_cast<int>(bodies_.size()); }
    void step(double dt, int substeps) override { world_.stepSimulation(static_cast<btScalar>(dt), substeps, static_cast<btScalar>(dt / substeps)); }
    bool multithreaded() const override { return false; }  // the packaged library is built without BT_THREADSAFE task scheduling
    void set_threads(int) override {}
    void set_solver_iterations(int n) override { world_.getSolverInfo().m_numIterations = n; }
    double raycast_fraction(V3 from, V3 to) const override {
        btCollisionWorld::ClosestRayResultCallback cb(bv(from), bv(to));
        const_cast<btDiscreteDynamicsWorld&>(world_).rayTest(bv(from), bv(to), cb);
        return cb.hasHit() ? static_cast<double>(cb.m_closestHitFraction) : -1.0;
    }
    void use_saige_material_rule() override {
        gContactAddedCallback = saige_rule_callback;
        for (btRigidBody* b : bodies_) b->setCollisionFlags(b->getCollisionFlags() | btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);
        custom_rule_ = true;
    }

private:
    int create(btCollisionShape* shape, V3 c, Q4 q, double mass, Material m, bool ccd, double ccd_radius) {
        shapes_.push_back(shape);
        btVector3 inertia(0, 0, 0);
        if (mass > 0) shape->calculateLocalInertia(static_cast<btScalar>(mass), inertia);
        auto* ms = new btDefaultMotionState(btTransform(btQuaternion(static_cast<btScalar>(q.x), static_cast<btScalar>(q.y), static_cast<btScalar>(q.z), static_cast<btScalar>(q.w)), bv(c)));
        btRigidBody::btRigidBodyConstructionInfo info(static_cast<btScalar>(mass), ms, shape, inertia);
        info.m_friction = m.friction;
        info.m_restitution = m.restitution;
        auto* b = new btRigidBody(info);
        if (ccd && mass > 0) {
            b->setCcdMotionThreshold(static_cast<btScalar>(ccd_radius * 0.5));
            b->setCcdSweptSphereRadius(static_cast<btScalar>(ccd_radius * 0.5));
        }
        if (custom_rule_) b->setCollisionFlags(b->getCollisionFlags() | btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);
        world_.addRigidBody(b);
        bodies_.push_back(b);
        return static_cast<int>(bodies_.size() - 1);
    }

    btDefaultCollisionConfiguration cfg_;
    btCollisionDispatcher disp_;
    btDbvtBroadphase bp_;
    btSequentialImpulseConstraintSolver solver_;
    btDiscreteDynamicsWorld world_;
    std::vector<btCollisionShape*> shapes_;
    std::vector<btRigidBody*> bodies_;
    std::vector<btTypedConstraint*> constraints_;
    bool custom_rule_ = false;
};

} // namespace

std::unique_ptr<Backend> make_bullet(int max_bodies) { return std::make_unique<Bullet>(max_bodies); }

} // namespace eval
