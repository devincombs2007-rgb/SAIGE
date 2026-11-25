#include "sim.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace saige::horizon {

namespace {

constexpr double pi = std::numbers::pi;
double wrap_pi(double a) {
    while (a > pi) a -= 2 * pi;
    while (a < -pi) a += 2 * pi;
    return a;
}
double clampd(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

CapsuleCharacterConfig character_config() {
    CapsuleCharacterConfig c;
    c.radius = 0.30f;
    c.height = 1.75f;
    c.walk_speed = 3.4f;
    c.sprint_speed = 6.2f;
    c.max_slope_radians = 0.9f;
    c.step_height = 0.4f;
    return c;
}

// The physics of the showcase car, matched to the licensed CarConcept model (Khronos, CC-BY-4.0): its body spans x -1.94..2.40 (4.34 m), the wheel centers sit at
// x 1.486 and -1.314, y +-0.98 and 0.384 m above the road, the tire radius is 0.384 m. The chassis box is centered on the body (model x 0.23, z 0.62), so the wheel
// mounts are given relative to it; the center of mass sits low and between the axles.
VehicleConfig vehicle_config() {
    VehicleConfig c;
    c.mass = 1450.0f;
    c.half_extents = {2.17f, 1.0f, 0.53f};
    c.center_of_mass = {-0.14f, 0.0f, -0.30f};
    c.wheel_radius = 0.384f;
    c.suspension_travel = 0.40f;
    c.spring = 36000.0f;
    c.damper = 4300.0f;
    c.wheels = {{
        {{1.256f, 0.98f, 0.065f}, true, false},
        {{1.256f, -0.98f, 0.065f}, true, false},
        {{-1.544f, 0.98f, 0.065f}, false, true},
        {{-1.544f, -0.98f, 0.065f}, false, true},
    }};
    return c;
}

} // namespace

const char* phase_name(Phase p) {
    static const char* names[] = {"establish", "idle", "walk", "run", "sprint", "jump", "approach", "enter_car", "drive", "crash", "aftermath", "exit_car", "walk_away", "overlook", "done"};
    return names[static_cast<std::size_t>(p)];
}

struct Sim::Cache {
    const World& world;
    std::map<CellKey, std::shared_future<std::shared_ptr<const CellData>>> futures;
    std::map<CellKey, std::shared_ptr<const CellData>> ready;
    explicit Cache(const World& w) : world(w) {}
    void prefetch(const CellKey& k) {
        if (ready.count(k) || futures.count(k)) return;
        const World* wp = &world;
        futures[k] = std::async(std::launch::async, [wp, k] { return std::make_shared<const CellData>(wp->build_cell(k.first, k.second)); }).share();
    }
    std::shared_ptr<const CellData> get(const CellKey& k, StreamStats& stats) {
        if (const auto it = ready.find(k); it != ready.end()) return it->second;
        const auto fit = futures.find(k);
        std::shared_ptr<const CellData> data;
        if (fit != futures.end()) {
            if (fit->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) ++stats.prefetch_hits;
            else ++stats.blocking_waits;
            data = fit->second.get();
            futures.erase(fit);
        } else {
            ++stats.blocking_waits;
            data = std::make_shared<const CellData>(world.build_cell(k.first, k.second));
        }
        ++stats.cells_prepared;
        ready[k] = data;
        return data;
    }
    void trim(const CellKey& center, int radius) {
        for (auto it = ready.begin(); it != ready.end();) {
            if (std::abs(it->first.first - center.first) > radius || std::abs(it->first.second - center.second) > radius) it = ready.erase(it);
            else ++it;
        }
    }
};

Sim::Sim(const World& world) : world_(world), cache_(std::make_unique<Cache>(world)) {
    // The script's geometry, in world meters. The character starts on the south pavement of the main street; the car stands on the gas station's apron.
    timeline_.walk = 19.0;
    timeline_.run = 26.0;
    timeline_.sprint = 30.0;
    timeline_.end = 142.0;
    const D2 start{96.0, -5.0};
    const double car_x = 165.0, car_y = 7.5;
    // The focus cell and origin first.
    const CellKey center{static_cast<int>(std::floor(start.x / cell_size)), static_cast<int>(std::floor(start.y / cell_size))};
    origin_ = {center.first * cell_size, center.second * cell_size};
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) resident_.insert({center.first + dx, center.second + dy});
    rebuild_collision(origin_);
    ++stats_.residency_changes;
    const double sz = world.height(start.x, start.y) + 0.15;  // the sidewalk top
    character_.emplace(Vec3{static_cast<float>(start.x - origin_.x), static_cast<float>(start.y - origin_.y), static_cast<float>(sz + 0.02)}, collision_.boxes(), collision_.triangles(), character_config());
    char_yaw_ = 0.0f;
    // The car, level, at ride height.
    const double cz = world.height(car_x, car_y) + 0.03;
    VehicleConfig vc = vehicle_config();
    const double ride = 0.62;
    vehicle_.emplace(vc, Vec3{static_cast<float>(car_x - origin_.x), static_cast<float>(car_y - origin_.y), static_cast<float>(cz + ride + 0.15)}, 0.0f);
    vehicle_record_ = {true, car_x, car_y, cz, {}, 0.0f, true};
    // Physical props: barrels on the outer shoulder in the car's path before the barrier.
    for (int i = 0; i < 6; ++i) {
        PropBody b;
        b.type = PropType::Barrel;
        b.x = 712.0 + (i % 3) * 1.1 + (i / 3) * 0.4;
        b.y = -80.0 - (i / 3) * 1.8 - (i % 3) * 0.3;
        b.z = world.height(b.x, b.y);
        b.radius = 0.33;
        b.height = 0.88;
        b.scale = 1.0f;
        b.yaw = static_cast<float>(i * 0.9);
        b.settled = true;
        bodies_.push_back(b);
    }
    cache_->trim(center, 3);
    record({});
    trace_.clear();
    trace_.reserve(12000);
}

Sim::~Sim() = default;

std::shared_ptr<const CellData> Sim::cell(const CellKey& k) const {
    const auto it = cache_->ready.find(k);
    return it == cache_->ready.end() ? nullptr : it->second;
}

Vec3 Sim::character_feet() const noexcept { return character_ ? character_->state().feet : Vec3{}; }
bool Sim::character_grounded() const noexcept { return character_ && character_->state().grounded; }

void Sim::rebuild_collision(D2 new_origin) {
    std::vector<Aabb> boxes;
    std::vector<Triangle> tris;
    std::uint64_t vertex_tris = 0;
    for (const CellKey& k : resident_) {
        const auto data = cache_->get(k, stats_);
        const float ox = static_cast<float>(k.first * cell_size - new_origin.x), oy = static_cast<float>(k.second * cell_size - new_origin.y);
        for (const Aabb& b : data->boxes) boxes.push_back({{b.min.x + ox, b.min.y + oy, b.min.z}, {b.max.x + ox, b.max.y + oy, b.max.z}});
        for (const Triangle& t : data->triangles) tris.push_back({{t.a.x + ox, t.a.y + oy, t.a.z}, {t.b.x + ox, t.b.y + oy, t.b.z}, {t.c.x + ox, t.c.y + oy, t.c.z}});
        vertex_tris += data->triangle_count();
    }
    collision_ = StaticCollision(boxes, tris);
    stats_.resident_cells = resident_.size();
    stats_.resident_triangles = vertex_tris;
    stats_.resident_collision_triangles = tris.size();
    const D2 old = origin_;
    origin_ = new_origin;
    if (character_ || vehicle_) {
        const Vec3 shift{static_cast<float>(old.x - new_origin.x), static_cast<float>(old.y - new_origin.y), 0.0f};
        if (shift.x != 0.0f || shift.y != 0.0f) {
            if (character_ && !character_->migrate_origin(shift, collision_.boxes(), collision_.triangles())) throw std::runtime_error("origin migration was refused by the character");
            if (vehicle_) vehicle_->migrate_origin(shift);
            ++stats_.origin_migrations;
            ++origin_epoch_;
        }
    }
}

void Sim::update_residency() {
    // The focus entity: the vehicle while driving, else the character.
    D2 focus{};
    if (driving_ && vehicle_) {
        focus = to_world(static_cast<double>(vehicle_->state().position.x), static_cast<double>(vehicle_->state().position.y));
    } else if (character_) {
        focus = to_world(static_cast<double>(character_->state().feet.x), static_cast<double>(character_->state().feet.y));
    }
    const CellKey center{static_cast<int>(std::floor(focus.x / cell_size)), static_cast<int>(std::floor(focus.y / cell_size))};
    const CellKey current{static_cast<int>(std::floor(origin_.x / cell_size + 0.5)), static_cast<int>(std::floor(origin_.y / cell_size + 0.5))};
    // Prefetch the next ring ahead of need (a neighbour cell once the focus is in the outer third of its cell).
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) cache_->prefetch({center.first + dx, center.second + dy});
    if (center == current) return;
    std::set<CellKey> wanted;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) wanted.insert({center.first + dx, center.second + dy});
    // The vehicle's cell must stay resident or the vehicle parks (its record keeps it).
    if (vehicle_ && !driving_) {
        const D2 vw = to_world(static_cast<double>(vehicle_->state().position.x), static_cast<double>(vehicle_->state().position.y));
        const CellKey vc{static_cast<int>(std::floor(vw.x / cell_size)), static_cast<int>(std::floor(vw.y / cell_size))};
        if (!wanted.count(vc)) {
            const auto& st = vehicle_->state();
            vehicle_record_ = {true, vw.x, vw.y, static_cast<double>(st.position.z), st.orientation, st.yaw, true};
            vehicle_.reset();
        }
    }
    resident_ = wanted;
    ++stats_.residency_changes;
    rebuild_collision({center.first * cell_size, center.second * cell_size});
    if (!vehicle_ && vehicle_record_.exists && !driving_) {
        const CellKey vc{static_cast<int>(std::floor(vehicle_record_.x / cell_size)), static_cast<int>(std::floor(vehicle_record_.y / cell_size))};
        if (resident_.count(vc)) {
            vehicle_.emplace(vehicle_config(), Vec3{static_cast<float>(vehicle_record_.x - origin_.x), static_cast<float>(vehicle_record_.y - origin_.y), static_cast<float>(vehicle_record_.z + 0.06)}, vehicle_record_.yaw);
        }
    }
    cache_->trim(center, 3);
}

void Sim::step_character(double dt) {
    if (!character_ || driving_) return;
    // Hand the controller the primitives near the feet, in the local frame.
    const Vec3 f = character_->state().feet;
    const float r = 4.0f;
    std::vector<StaticPrimitive> ids;
    collision_.query_region({{f.x - r, f.y - r, f.z - 3.0f}, {f.x + r, f.y + r, f.z + 3.0f}}, ids);
    std::vector<Aabb> boxes;
    std::vector<Triangle> tris;
    for (const StaticPrimitive& p : ids) {
        if (p.kind == StaticPrimitive::Kind::Box) boxes.push_back(collision_.boxes()[p.index]);
        else tris.push_back(collision_.triangles()[p.index]);
    }
    (void)character_->replace_static_geometry(boxes, tris);
    character_->step(char_input_, std::chrono::nanoseconds(static_cast<long long>(dt * 1e9)));
    const CapsuleCharacterState& s = character_->state();
    const float speed = std::hypot(s.velocity.x, s.velocity.y);
    char_speed_ += (speed - char_speed_) * 0.35f;
    const float prev_yaw = char_yaw_;
    if (speed > 0.35f) {
        const float target = std::atan2(s.velocity.y, s.velocity.x);
        float d = static_cast<float>(wrap_pi(static_cast<double>(target - char_yaw_)));
        char_yaw_ += d * 0.18f;
    }
    char_turn_ += (static_cast<float>(wrap_pi(static_cast<double>(char_yaw_ - prev_yaw))) / static_cast<float>(dt) - char_turn_) * 0.2f;
    // The walk cycle advances with the distance covered (about one cycle per 1.4 m of stride at a walk, longer at a sprint).
    gait_ += speed * static_cast<float>(dt) / 1.5f;
    gait_ = std::fmod(gait_, 1.0f);
}

void Sim::step_props(double dt) {
    const float g = 9.81f;
    const OrientedBox* car_box = nullptr;
    OrientedBox box;
    Vec3 car_v{};
    if (vehicle_) {
        box = vehicle_->chassis();
        car_box = &box;
        car_v = vehicle_->state().velocity;
    }
    for (PropBody& b : bodies_) {
        b.age += static_cast<float>(dt);
        const double lx = b.x - origin_.x, ly = b.y - origin_.y;
        // Hit by the car?
        if (car_box && std::fabs(lx - static_cast<double>(box.center.x)) < 6.0 && std::fabs(ly - static_cast<double>(box.center.y)) < 6.0) {
            const Vec3 d{static_cast<float>(lx) - box.center.x, static_cast<float>(ly) - box.center.y, static_cast<float>(b.z) + static_cast<float>(b.height) * 0.5f - box.center.z};
            const float ax = d.x * box.axis_x.x + d.y * box.axis_x.y + d.z * box.axis_x.z, ay = d.x * box.axis_y.x + d.y * box.axis_y.y + d.z * box.axis_y.z, az = d.x * box.axis_z.x + d.y * box.axis_z.y + d.z * box.axis_z.z;
            const float cx = std::clamp(ax, -box.half_extents.x, box.half_extents.x), cy = std::clamp(ay, -box.half_extents.y, box.half_extents.y), cz = std::clamp(az, -box.half_extents.z, box.half_extents.z);
            const Vec3 closest{box.center.x + box.axis_x.x * cx + box.axis_y.x * cy + box.axis_z.x * cz, box.center.y + box.axis_x.y * cx + box.axis_y.y * cy + box.axis_z.y * cz,
                               box.center.z + box.axis_x.z * cx + box.axis_y.z * cy + box.axis_z.z * cz};
            Vec3 n{static_cast<float>(lx) - closest.x, static_cast<float>(ly) - closest.y, 0.0f};
            const float dist = std::hypot(n.x, n.y);
            if (dist < static_cast<float>(b.radius) + 0.02f) {
                if (dist > 1e-4f) n = {n.x / dist, n.y / dist, 0.0f};
                else n = {box.axis_x.x, box.axis_x.y, 0.0f};
                const float rel = (car_v.x - b.vx) * n.x + (car_v.y - b.vy) * n.y;
                if (rel > 0.0f) {
                    const float j = (1.0f + 0.25f) * rel * 0.8f;  // the barrel is light compared with the car
                    b.vx += n.x * j;
                    b.vy += n.y * j;
                    b.vz += std::min(3.5f, j * 0.18f);
                    b.wy += (static_cast<float>(lx) - box.center.x > 0 ? 1.0f : -1.0f) * j * 0.6f;
                    b.settled = false;
                }
                b.x += static_cast<double>(n.x) * (static_cast<double>(b.radius) + 0.03 - static_cast<double>(dist));
                b.y += static_cast<double>(n.y) * (static_cast<double>(b.radius) + 0.03 - static_cast<double>(dist));
            }
        }
        if (b.settled) continue;
        b.vz -= g * static_cast<float>(dt);
        b.x += static_cast<double>(b.vx) * dt;
        b.y += static_cast<double>(b.vy) * dt;
        b.z += static_cast<double>(b.vz) * dt;
        b.yaw += b.wy * static_cast<float>(dt);
        b.pitch += b.wp * static_cast<float>(dt);
        b.roll += b.wr * static_cast<float>(dt);
        // The ground under the body: a ray from above.
        const Vec3 o{static_cast<float>(b.x - origin_.x), static_cast<float>(b.y - origin_.y), static_cast<float>(b.z) + 1.5f};
        if (std::fabs(o.x) < 250.0f && std::fabs(o.y) < 250.0f) {
            if (const auto hit = collision_.raycast(o, {0, 0, -1}, 4.0f)) {
                const double ground = static_cast<double>(o.z) - static_cast<double>(hit->distance);
                if (b.z < ground) {
                    b.z = ground;
                    if (b.vz < -0.6f) {
                        b.vz = -b.vz * (b.debris ? 0.32f : 0.18f);
                        b.vx *= 0.72f;
                        b.vy *= 0.72f;
                        b.wp *= 0.6f;
                        b.wr *= 0.6f;
                    } else {
                        b.vz = 0.0f;
                        const float sp = std::hypot(b.vx, b.vy);
                        const float fr = std::min(sp, (b.debris ? 3.5f : 1.8f) * static_cast<float>(dt));
                        if (sp > 1e-4f) {
                            b.vx -= b.vx / sp * fr;
                            b.vy -= b.vy / sp * fr;
                        }
                        b.wy *= 0.96f;
                        b.wp *= 0.9f;
                        b.wr *= 0.9f;
                        if (sp < 0.05f && std::fabs(b.wy) < 0.1f) {
                            b.settled = true;
                            b.vx = b.vy = b.wy = b.wp = b.wr = 0.0f;
                        }
                    }
                }
            }
        }
    }
    for (DustPuff& d : dust_) {
        d.age += static_cast<float>(dt);
        d.x += static_cast<double>(d.vx) * dt;
        d.y += static_cast<double>(d.vy) * dt;
        d.z += static_cast<double>(d.vz) * dt;
        d.vx *= 0.985f;
        d.vy *= 0.985f;
        d.vz = d.vz * 0.985f + 0.15f * static_cast<float>(dt);
    }
    std::erase_if(dust_, [](const DustPuff& d) { return d.age > d.life; });
}

void Sim::spawn_debris(const ImpactEvent& e) {
    const std::uint64_t h0 = hash64(static_cast<std::uint64_t>(impacts_.size() + 1), 4242);
    const int n = 20;
    for (int i = 0; i < n; ++i) {
        const std::uint64_t h = hash64(h0, static_cast<std::uint64_t>(i));
        PropBody b;
        b.type = PropType::RockSmall;  // a concrete chunk (drawn with a dedicated chunk model)
        b.debris = true;
        b.x = e.x + (unit(hash64(h, 1)) - 0.5) * 0.8;
        b.y = e.y + (unit(hash64(h, 2)) - 0.5) * 0.8;
        b.z = e.z + 0.2 + unit(hash64(h, 3)) * 0.8;
        const float sp = 0.25f * e.speed * (0.4f + static_cast<float>(unit(hash64(h, 4))));
        const double a = std::atan2(static_cast<double>(e.ny), static_cast<double>(e.nx)) + (unit(hash64(h, 5)) - 0.5) * 2.2;
        b.vx = static_cast<float>(std::cos(a)) * sp;
        b.vy = static_cast<float>(std::sin(a)) * sp;
        b.vz = 1.0f + 0.12f * e.speed * static_cast<float>(unit(hash64(h, 6)));
        b.wy = static_cast<float>((unit(hash64(h, 7)) - 0.5) * 14.0);
        b.wp = static_cast<float>((unit(hash64(h, 8)) - 0.5) * 14.0);
        b.wr = static_cast<float>((unit(hash64(h, 9)) - 0.5) * 14.0);
        b.scale = static_cast<float>(0.5 + unit(hash64(h, 10)) * 1.1);
        b.radius = 0.12f * b.scale;
        b.height = 0.2f * b.scale;
        bodies_.push_back(b);
    }
    for (int i = 0; i < 46; ++i) {
        const std::uint64_t h = hash64(h0, 5000 + static_cast<std::uint64_t>(i));
        DustPuff d;
        d.x = e.x + (unit(hash64(h, 1)) - 0.5) * 2.2;
        d.y = e.y + (unit(hash64(h, 2)) - 0.5) * 2.2;
        d.z = e.z + 0.1 + unit(hash64(h, 3)) * 0.9;
        const double a = unit(hash64(h, 4)) * 2 * pi;
        const float sp = 0.4f + 0.25f * e.speed * static_cast<float>(unit(hash64(h, 5)));
        d.vx = static_cast<float>(std::cos(a)) * sp * 0.5f + e.nx * sp * 0.5f;
        d.vy = static_cast<float>(std::sin(a)) * sp * 0.5f + e.ny * sp * 0.5f;
        d.vz = 0.3f + static_cast<float>(unit(hash64(h, 6))) * 1.4f;
        d.life = 2.0f + static_cast<float>(unit(hash64(h, 7))) * 2.5f;
        d.size = 0.7f + static_cast<float>(unit(hash64(h, 8))) * 1.3f;
        dust_.push_back(d);
    }
}

void Sim::handle_impacts() {
    if (!vehicle_) return;
    for (const VehicleImpact& vi : vehicle_->take_impacts()) {
        ImpactEvent e;
        e.time = time_;
        e.speed = vi.speed;
        const D2 w = to_world(static_cast<double>(vi.point.x), static_cast<double>(vi.point.y));
        e.x = w.x;
        e.y = w.y;
        e.z = static_cast<double>(vi.point.z);
        e.nx = vi.normal.x;
        e.ny = vi.normal.y;
        e.nz = vi.normal.z;
        e.energy = vi.energy;
        e.barrier = vi.primitive.kind == StaticPrimitive::Kind::Triangle && vi.speed > 4.0f && std::fabs(vi.normal.z) < 0.6f;
        // Merge the closely spaced events of one collision.
        if (!impacts_.empty() && time_ - impacts_.back().time < 0.35) {
            ImpactEvent& last = impacts_.back();
            last.speed = std::max(last.speed, e.speed);
            last.energy += e.energy;
            last.barrier = last.barrier || e.barrier;
            continue;
        }
        impacts_.push_back(e);
        if (e.speed > 4.0f) {
            damage_ = std::min(1.0f, damage_ + e.speed / 22.0f);
            // The dent: where, in the car's frame.
            const auto ax = vehicle_->axes();
            const Vec3 c = vehicle_->state().position;
            const Vec3 d{vi.point.x - c.x, vi.point.y - c.y, vi.point.z - c.z};
            damage_point_ = {d.x * ax[0].x + d.y * ax[0].y + d.z * ax[0].z, d.x * ax[1].x + d.y * ax[1].y + d.z * ax[1].z, d.x * ax[2].x + d.y * ax[2].y + d.z * ax[2].z};
            damage_dir_ = {vi.normal.x * ax[0].x + vi.normal.y * ax[0].y + vi.normal.z * ax[0].z, vi.normal.x * ax[1].x + vi.normal.y * ax[1].y + vi.normal.z * ax[1].z,
                           vi.normal.x * ax[2].x + vi.normal.y * ax[2].y + vi.normal.z * ax[2].z};
            spawn_debris(e);
        }
    }
}

VehicleInput Sim::drive_inputs() {
    VehicleInput in;
    if (!vehicle_) return in;
    const VehicleState& st = vehicle_->state();
    const D2 pos = to_world(static_cast<double>(st.position.x), static_cast<double>(st.position.y));
    const double v = static_cast<double>(st.forward_speed);
    const double heading = static_cast<double>(st.yaw);
    // The path: the driveway leg first, then the route in the right-hand lane.
    static const D2 leg[] = {{169.5, 8.0}, {172.0, 5.0}, {172.6, 1.6}, {176.0, -1.1}, {184.0, -1.8}};
    D2 target{};
    double v_target = 4.0;
    const Road& route = world_.route();
    if (waypoint_ < 5) {
        target = leg[waypoint_];
        if (std::hypot(target.x - pos.x, target.y - pos.y) < 3.2) ++waypoint_;
        if (waypoint_ < 5) target = leg[waypoint_];
        v_target = 4.2;
    }
    if (waypoint_ >= 5) {
        // Find the progress along the route near the car and look ahead.
        const RoadSample q = world_.nearest_road(pos.x, pos.y, 0);
        route_s_ = std::max(route_s_, q.s);
        const double look = 6.0 + 0.9 * std::max(v, 0.0);
        double hd = 0.0;
        const D2 ahead = world_.route_point(route_s_ + look, &hd);
        target = {ahead.x + std::sin(hd) * 1.8, ahead.y - std::cos(hd) * 1.8};  // the right-hand lane
        // Speed: slower in town, faster in the open, limited by the curvature ahead.
        double h1 = 0.0, h2 = 0.0;
        (void)world_.route_point(route_s_ + 8.0, &h1);
        (void)world_.route_point(route_s_ + 38.0, &h2);
        const double kappa = std::fabs(wrap_pi(h2 - h1)) / 30.0;
        const double town = route_s_ < 350.0 ? 9.5 : 24.0;
        v_target = std::min(town, kappa > 1e-4 ? std::sqrt(3.4 / kappa) : town);
        v_target = std::max(v_target, 7.0);
    }
    const double desired = std::atan2(target.y - pos.y, target.x - pos.x);
    double err = wrap_pi(desired - heading);
    in.steer = static_cast<float>(clampd(err * 1.6, -1.0, 1.0));
    const double thr = clampd((v_target - v) * 0.35, -1.0, 1.0);
    in.throttle = static_cast<float>(std::max(0.0, thr));
    in.brake = static_cast<float>(thr < -0.15 ? clampd(-thr * 0.8, 0.0, 1.0) : 0.0);
    // The crash script: the driver stops steering for the bend (distracted) while keeping the power on, a little before the crash bend.
    if (waypoint_ >= 5 && route_s_ > world_.landmarks().crash_s - 48.0) {
        in.steer = -0.10f;  // wide of the bend, a little the wrong way
        in.throttle = static_cast<float>(clampd((22.0 - v) * 0.4, 0.0, 1.0));
        in.brake = 0.0f;
    }
    return in;
}

void Sim::step_vehicle(double dt) {
    if (!vehicle_) return;
    VehicleInput in{};
    if (driving_ && phase_ != Phase::Aftermath) in = drive_inputs();
    if (!driving_ || crashed_) in = VehicleInput{0.0f, rest_time_ > 0.0 || crashed_ ? 0.6f : 0.0f, 0.0f};
    vehicle_input_ = in;
    vehicle_->step(in, static_cast<float>(dt), collision_, scratch_);
    handle_impacts();
}

void Sim::script(double dt) {
    phase_time_ += dt;
    const D2 feet_w = character_ ? to_world(static_cast<double>(character_->state().feet.x), static_cast<double>(character_->state().feet.y)) : D2{};
    const auto go = [&](D2 target, double magnitude, bool sprint) {
        const double dx = target.x - feet_w.x, dy = target.y - feet_w.y;
        const double d = std::hypot(dx, dy);
        char_input_ = {static_cast<float>(dx / std::max(d, 1e-6) * magnitude), static_cast<float>(dy / std::max(d, 1e-6) * magnitude), false, sprint};
        return d;
    };
    const auto enter = [&](Phase p) {
        if (p == Phase::Jump) timeline_.jump = time_;
        if (p == Phase::Approach) timeline_.approach = time_;
        phase_ = p;
        phase_time_ = 0.0;
        waypoint_ = 0;
    };
    char_input_ = {};
    switch (phase_) {
    case Phase::Establish:
        if (time_ >= 2.0) enter(Phase::Idle);
        break;
    case Phase::Idle:
        if (time_ >= timeline_.walk) enter(Phase::Walk);
        break;
    case Phase::Walk:
        go({108.0, -5.0}, 0.5, false);
        if (time_ >= timeline_.run) enter(Phase::Run);
        break;
    case Phase::Run:
        go({126.0, -5.0}, 1.0, false);
        if (time_ >= timeline_.sprint) enter(Phase::Sprint);
        break;
    case Phase::Sprint: {
        const double d = go({150.0, -5.0}, 1.0, true);
        if (feet_w.x > 140.0 && !jumped_ && character_ && character_->state().grounded) {
            char_input_.jump = true;
            jumped_ = true;
            enter(Phase::Jump);
        }
        (void)d;
        break;
    }
    case Phase::Jump: {
        go({150.0, -5.0}, 1.0, true);
        if (phase_time_ > 0.9 && character_ && character_->state().grounded) enter(Phase::Approach);
        break;
    }
    case Phase::Approach: {
        static const D2 path[] = {{153.0, 2.5}, {158.0, 9.6}, {164.0, 9.7}};
        const double d = go(path[waypoint_], waypoint_ == 2 ? 0.55 : 0.85, false);
        if (d < (waypoint_ == 2 ? 0.35 : 1.0)) {
            if (waypoint_ == 2) {
                timeline_.enter = time_;
                enter(Phase::EnterCar);
                enter_progress_ = 0.0;
                enter_from_ = character_ ? character_->state().feet : Vec3{};
            } else {
                ++waypoint_;
            }
        }
        break;
    }
    case Phase::EnterCar: {
        enter_progress_ += dt;
        const double t = enter_progress_;
        door_.angle = static_cast<float>(t < 1.0 ? t * 1.1 : (t < 2.4 ? 1.1 : std::max(0.0, 1.1 - (t - 2.4) * 1.1)));
        if (t > 0.9 && t < 2.2) {
            // The rider steps in through the door opening toward the seat (a presentation move: the controller is idle while the interaction runs).
            const double u = (t - 0.9) / 1.3;
            if (vehicle_) {
                const auto ax = vehicle_->axes();
                const Vec3 c = vehicle_->state().position;
                const Vec3 seat{c.x + ax[0].x * 0.15f + ax[1].x * 0.42f, c.y + ax[0].y * 0.15f + ax[1].y * 0.42f, c.z - 0.35f};
                (void)seat;
                (void)u;
            }
            char_visible_ = u < 0.85;
        }
        if (t >= 3.5) {
            driving_ = true;
            char_visible_ = false;
            character_.reset();  // the rider is in the car: there is no capsule while seated (it is recreated where the rider steps out)
            timeline_.drive = time_;
            enter(Phase::Drive);
            waypoint_ = 0;
            route_s_ = 0.0;
        }
        break;
    }
    case Phase::Drive:
        if (crashed_) {
            timeline_.crash = time_;
            enter(Phase::Crash);
        }
        break;
    case Phase::Crash:
        // The car is a body at rest against the barrier: wait for it to stop.
        if (vehicle_ && std::fabs(static_cast<double>(vehicle_->state().forward_speed)) < 0.25) rest_time_ += dt;
        else rest_time_ = 0.0;
        if (rest_time_ > 1.5) {
            timeline_.aftermath = time_;
            enter(Phase::Aftermath);
        }
        break;
    case Phase::Aftermath:
        if (phase_time_ > 2.5) {
            timeline_.exit = time_;
            enter(Phase::ExitCar);
            enter_progress_ = 0.0;
        }
        break;
    case Phase::ExitCar: {
        enter_progress_ += dt;
        const double t = enter_progress_;
        door_.angle = static_cast<float>(t < 1.0 ? t * 1.1 : 1.1);
        if (t > 0.9 && !char_visible_ && vehicle_ && !character_) {
            // Step out on the driver's side, on the nearest clear ground, and the car is parked: the capsule is created at that spot.
            const auto ax = vehicle_->axes();
            const Vec3 c = vehicle_->state().position;
            const Vec3 spot{c.x + ax[1].x * 1.9f, c.y + ax[1].y * 1.9f, c.z - 0.9f};
            Vec3 feet = spot;
            if (const auto hit = collision_.raycast({spot.x, spot.y, spot.z + 3.0f}, {0, 0, -1}, 8.0f)) feet.z = spot.z + 3.0f - static_cast<float>(hit->distance) + 0.03f;
            const auto subset = [&](Vec3 f, std::vector<Aabb>& boxes, std::vector<Triangle>& tris) {
                std::vector<StaticPrimitive> ids;
                collision_.query_region({{f.x - 6.0f, f.y - 6.0f, f.z - 4.0f}, {f.x + 6.0f, f.y + 6.0f, f.z + 4.0f}}, ids);
                for (const StaticPrimitive& p : ids) {
                    if (p.kind == StaticPrimitive::Kind::Box) boxes.push_back(collision_.boxes()[p.index]);
                    else tris.push_back(collision_.triangles()[p.index]);
                }
            };
            std::vector<Aabb> boxes;
            std::vector<Triangle> tris;
            subset(feet, boxes, tris);
            character_.emplace(feet, boxes, tris, character_config());
            char_visible_ = true;
            driving_ = false;
            vehicle_->park();
            char_yaw_ = static_cast<float>(std::atan2(static_cast<double>(ax[1].y), static_cast<double>(ax[1].x)));
        }
        if (t > 2.6) {
            door_.angle = static_cast<float>(std::max(0.0, 1.1 - (t - 2.6) * 1.1));
        }
        if (t > 3.8 && char_visible_) {
            timeline_.walk_away = time_;
            enter(Phase::WalkAway);
        }
        break;
    }
    case Phase::WalkAway: {
        // Toward the overlook: the highest ground 28 to 48 m from the crash site on the outer side of the road, found once.
        static D2 overlook{};
        static bool found = false;
        if (!found) {
            double best = -1e9;
            const D2 c = world_.landmarks().crash_barrier;
            for (double a = 0; a < 2 * pi; a += 0.12) {
                for (double r = 28; r <= 48; r += 4) {
                    const D2 p{c.x + std::cos(a) * r, c.y + std::sin(a) * r};
                    if (world_.nearest_road(p.x, p.y).distance < 12.0) continue;
                    const double hgt = world_.height(p.x, p.y);
                    if (hgt > best) {
                        best = hgt;
                        overlook = p;
                    }
                }
            }
            found = true;
        }
        const double d = go(overlook, 0.8, false);
        if (d < 1.5) {
            timeline_.overlook = time_;
            enter(Phase::Overlook);
        }
        break;
    }
    case Phase::Overlook:
        break;
    case Phase::Done:
        break;
    }
    if (phase_ == Phase::Drive && !impacts_.empty() && impacts_.back().barrier) crashed_ = true;
    if (phase_ == Phase::Idle || phase_ == Phase::Establish) {
        // idle
    }
}

void Sim::record(const TickRecord& r) { trace_.push_back(r); }

void Sim::tick() {
    update_residency();
    script(step_seconds);
    step_character(step_seconds);
    step_vehicle(step_seconds);
    step_props(step_seconds);
    TickRecord r;
    r.t = static_cast<float>(time_);
    if (vehicle_) {
        r.vehicle_speed = vehicle_->state().forward_speed;
        r.vehicle_throttle = vehicle_input_.throttle;
        r.vehicle_steer = vehicle_input_.steer;
        r.vehicle_rpm = 900.0f + 5200.0f * std::min(1.0f, std::fabs(vehicle_->state().forward_speed) / 30.0f + 0.2f * vehicle_input_.throttle);
    }
    r.character_speed = char_speed_;
    r.character_grounded = character_grounded();
    r.driving = driving_;
    r.phase = static_cast<std::uint8_t>(phase_);
    record(r);
    ++tick_;
    time_ = static_cast<double>(tick_) * step_seconds;
}

std::uint64_t Sim::state_hash() const {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    if (character_) mix(&character_->state(), sizeof(Vec3) * 2);
    if (vehicle_) {
        mix(&vehicle_->state().position, sizeof(Vec3));
        mix(&vehicle_->state().orientation, sizeof(VehicleQuat));
        mix(&vehicle_->state().velocity, sizeof(Vec3));
    }
    mix(&time_, sizeof(time_));
    mix(&origin_, sizeof(origin_));
    for (const PropBody& b : bodies_) mix(&b.x, sizeof(double) * 3);
    return h;
}

std::string Sim::checkpoint_json() const {
    // Everything that is not derived: the character, the vehicle (as a parked record), the damage, the script's place in time.
    std::ostringstream o;
    o.precision(17);
    const Vec3 f = character_ ? character_->state().feet : Vec3{};
    const D2 cw = to_world(static_cast<double>(f.x), static_cast<double>(f.y));
    o << "{\"checkpoint_version\":1,\"time\":" << time_ << ",\"tick\":" << tick_ << ",\"phase\":" << static_cast<int>(phase_) << ",\"character\":{\"x\":" << cw.x << ",\"y\":" << cw.y << ",\"z\":"
      << static_cast<double>(f.z) << ",\"yaw\":" << static_cast<double>(char_yaw_) << ",\"visible\":" << (char_visible_ ? 1 : 0) << "},";
    VehicleRecord rec = vehicle_record_;
    if (vehicle_) {
        const auto& st = vehicle_->state();
        const D2 vw = to_world(static_cast<double>(st.position.x), static_cast<double>(st.position.y));
        rec = {true, vw.x, vw.y, static_cast<double>(st.position.z), st.orientation, st.yaw, true};
    }
    o << "\"vehicle\":{\"x\":" << rec.x << ",\"y\":" << rec.y << ",\"z\":" << rec.z << ",\"yaw\":" << static_cast<double>(rec.yaw) << ",\"qx\":" << static_cast<double>(rec.q.x) << ",\"qy\":"
      << static_cast<double>(rec.q.y) << ",\"qz\":" << static_cast<double>(rec.q.z) << ",\"qw\":" << static_cast<double>(rec.q.w) << ",\"damage\":" << static_cast<double>(damage_) << ",\"driving\":" << (driving_ ? 1 : 0)
      << "},\"origin\":[" << origin_.x << "," << origin_.y << "],\"bodies\":[";
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        const PropBody& b = bodies_[i];
        o << (i ? "," : "") << "[" << static_cast<int>(b.type) << "," << b.x << "," << b.y << "," << b.z << "," << static_cast<double>(b.yaw) << "," << (b.debris ? 1 : 0) << "," << static_cast<double>(b.scale) << "]";
    }
    o << "]}";
    return o.str();
}

std::unique_ptr<Sim> Sim::from_checkpoint(const World& world, const std::string& json) {
    const auto num = [&](const std::string& key, std::size_t from = 0) {
        const std::string k = "\"" + key + "\":";
        const std::size_t p = json.find(k, from);
        if (p == std::string::npos) throw std::invalid_argument("checkpoint: missing " + key);
        return std::strtod(json.c_str() + p + k.size(), nullptr);
    };
    if (static_cast<int>(num("checkpoint_version")) != 1) throw std::invalid_argument("checkpoint: unsupported version");
    auto sim = std::make_unique<Sim>(world);
    const std::size_t cpos = json.find("\"character\":");
    const std::size_t vpos = json.find("\"vehicle\":");
    const double cx = num("x", cpos), cy = num("y", cpos), cz = num("z", cpos);
    // Move to the character's cell, then place it.
    const CellKey center{static_cast<int>(std::floor(cx / cell_size)), static_cast<int>(std::floor(cy / cell_size))};
    sim->resident_.clear();
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) sim->resident_.insert({center.first + dx, center.second + dy});
    sim->character_.reset();
    sim->vehicle_.reset();
    const D2 old_origin = sim->origin_;
    (void)old_origin;
    sim->origin_ = {center.first * cell_size, center.second * cell_size};
    sim->rebuild_collision(sim->origin_);
    sim->character_.emplace(Vec3{static_cast<float>(cx - sim->origin_.x), static_cast<float>(cy - sim->origin_.y), static_cast<float>(cz)}, sim->collision_.boxes(), sim->collision_.triangles(), character_config());
    sim->char_yaw_ = static_cast<float>(num("yaw", cpos));
    sim->char_visible_ = num("visible", cpos) > 0.5;
    sim->time_ = num("time");
    sim->tick_ = static_cast<std::uint64_t>(num("tick"));
    sim->phase_ = static_cast<Phase>(static_cast<int>(num("phase")));
    sim->damage_ = static_cast<float>(num("damage", vpos));
    sim->driving_ = num("driving", vpos) > 0.5;
    sim->vehicle_record_ = {true, num("x", vpos), num("y", vpos), num("z", vpos), {static_cast<float>(num("qx", vpos)), static_cast<float>(num("qy", vpos)), static_cast<float>(num("qz", vpos)), static_cast<float>(num("qw", vpos))},
                            static_cast<float>(num("yaw", vpos)), true};
    const CellKey vc{static_cast<int>(std::floor(sim->vehicle_record_.x / cell_size)), static_cast<int>(std::floor(sim->vehicle_record_.y / cell_size))};
    if (sim->resident_.count(vc)) {
        sim->vehicle_.emplace(vehicle_config(), Vec3{static_cast<float>(sim->vehicle_record_.x - sim->origin_.x), static_cast<float>(sim->vehicle_record_.y - sim->origin_.y), static_cast<float>(sim->vehicle_record_.z)}, sim->vehicle_record_.yaw);
    }
    sim->bodies_.clear();
    const std::size_t bpos = json.find("\"bodies\":[");
    if (bpos != std::string::npos) {
        std::size_t p = json.find('[', bpos + 10);
        while (p != std::string::npos && json[p] == '[') {
            PropBody b;
            char* end = nullptr;
            const char* s = json.c_str() + p + 1;
            b.type = static_cast<PropType>(static_cast<int>(std::strtod(s, &end)));
            s = end + 1;
            b.x = std::strtod(s, &end);
            s = end + 1;
            b.y = std::strtod(s, &end);
            s = end + 1;
            b.z = std::strtod(s, &end);
            s = end + 1;
            b.yaw = static_cast<float>(std::strtod(s, &end));
            s = end + 1;
            b.debris = std::strtod(s, &end) > 0.5;
            s = end + 1;
            b.scale = static_cast<float>(std::strtod(s, &end));
            b.settled = true;
            b.radius = b.debris ? 0.12f * b.scale : 0.33f;
            b.height = b.debris ? 0.2f * b.scale : 0.88f;
            sim->bodies_.push_back(b);
            p = json.find('[', static_cast<std::size_t>(end - json.c_str()));
        }
    }
    return sim;
}

} // namespace saige::horizon
