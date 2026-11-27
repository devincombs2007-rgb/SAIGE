// Showcase 04: the traffic of the district on the engine's own vehicle physics. Every moving car, van, ambulance and fire engine is a `Vehicle` (a raycast-suspension rigid body that collides with the static
// world and is damaged by it); the traffic simulation (traffic_simulation.hpp) decides how fast it wants to go (signals, the car ahead, occupied crosswalks) and the driver here turns that into throttle, brake and
// steering and lets the physics decide what the car actually does. The traffic simulation never moves a car: `use_external_physics` is set, the lane and distance along it are read back from the chassis every
// step, and the simulation's control output is only an input to the physics.
//
// Limits, stated: the engine has no vehicle-vehicle contact, so a car that is too close to the one ahead is held back by a gap rule (here and in the traffic simulation) and not by a collision between them; a
// car can hit the static world (parked cars, lamp posts, the kerb, a building) and is damaged by it, but it does not move what it hits (parked cars are scenery with a box); lanes are straight segments, so a
// turn follows the chord of its connector.
#pragma once

#include "district.hpp"
#include "saige/vehicle.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace saige::alive {

enum class VehicleKind : std::uint8_t { Sedan, Hatchback, Van, Taxi, Ambulance, FireEngine };

// The physics configuration of a kind of vehicle (the renderer reads the wheel mounts, the radius and the box from it so that what is drawn sits where the physics puts it).
[[nodiscard]] VehicleConfig vehicle_config_for(VehicleKind kind);

// Hazard lamps of a disabled vehicle: one small lamp at each of the four corners of the VISIBLE body. The hazard model holds exactly kHazardLampsPerModel lamp and the renderer draws it once per corner
// (kHazardCorners draws), at hazard_lamp_offsets(), in the body frame. Owner-reported defect "hazard lights detached from the vehicle body": the delivered code drew a four-lamp model four times, shifted by
// the opposite corner (16 lamps, 12 of them 2 m from their corner), and, once that was fixed, placed the corners from the PHYSICS box, which is not the visible mesh (the Car Concept body is shorter and
// lower than the chassis box): a rendered frame still showed a lamp hanging in the air behind the car. The corners now come from the body's own bounds.
struct BodyBox {
    std::array<float, 3> lo{}, hi{};  // the visible body's bounds in the body frame (x forward, y left, z up)
};
inline constexpr int kHazardLampsPerModel = 1;
inline constexpr int kHazardCorners = 4;
inline constexpr float kHazardInsetX = 0.10f;    // m inside the end of the body
inline constexpr float kHazardInsetY = 0.18f;    // m inside the side of the body
inline constexpr float kHazardHeightFrac = 0.45f;  // height of the lamp between the body's lowest and highest point
[[nodiscard]] std::array<std::array<float, 3>, kHazardCorners> hazard_lamp_offsets(const BodyBox& body);
// Hazard-lamp anchors ON THE SURFACE of a body mesh: for each corner the mesh vertex, in the middle height band (25 to 75 percent), that lies farthest toward the corner (score sx * x + 0.6 * sy * y).
// `xyz` is the mesh's vertices (x forward, y left, z up, any common origin); the result is in the same space. A bounding-box corner is not on a rounded body: the second fix (box corners) still left a
// lamp hanging about 0.4 m behind the visible rear of the Car Concept in a rendered frame.
[[nodiscard]] std::array<std::array<float, 3>, kHazardCorners> hazard_surface_anchors(const std::vector<float>& xyz);
// World position of a body-frame point of a vehicle at `pos` with orientation `q` (x, y, z, w): pos + rotate(q, p). The renderer places a lamp with the equivalent matrix; the tests check this in world space across poses.
[[nodiscard]] std::array<double, 3> body_point_to_world(const std::array<double, 3>& pos, const std::array<float, 4>& q, const std::array<float, 3>& p);
// The inverse: a world point into the body frame.
[[nodiscard]] std::array<double, 3> world_point_to_body(const std::array<double, 3>& pos, const std::array<float, 4>& q, const std::array<double, 3>& w);
// The same for vehicles drawn from their own physics box (vans, ambulance, fire engine: their bodies are built from the box).
[[nodiscard]] std::array<std::array<float, 3>, kHazardCorners> hazard_lamp_offsets(const VehicleConfig& cfg);

struct VehicleView {
    std::uint32_t id = 0;
    VehicleKind kind = VehicleKind::Sedan;
    double x = 0, y = 0, z = 0;            // the chassis box centre
    std::array<float, 4> q{0, 0, 0, 1};    // orientation (x, y, z, w)
    double yaw = 0;
    double speed = 0;                      // m/s forward
    std::array<float, 4> wheel_spin{};
    std::array<float, 2> wheel_steer{};    // front left, front right
    std::array<float, 4> wheel_compression{};
    bool braking = false;
    bool siren = false;
    bool disabled = false;                 // wrecked at the roadside, waiting for the response
    float damage = 0.0f;                   // the worst zone, 0..1
    std::array<float, 6> zones{};          // damage by zone: front, rear, left, right, roof, underbody (0..1)
    int palette = 0;
    int engine_rpm = 0;
};

struct TrafficEvent {
    enum class Type : std::uint8_t { Impact, Accident, EmergencyDispatched, EmergencyOnScene, Cleared } type = Type::Impact;
    std::uint32_t vehicle = 0;
    double x = 0, y = 0;
    float speed = 0.0f;     // approach speed of an impact, m/s
    float energy = 0.0f;    // J lost
    float impulse = 0.0f;   // N s
    std::uint8_t surface_kind = 0;  // 0 other, 1 parked car, 2 building, 3 post, 4 kerb/ground
};

struct TrafficStats {
    std::uint32_t vehicles = 0, moving = 0, stopped_at_signal = 0, emergency = 0, disabled = 0;
    std::uint64_t spawned = 0, despawned = 0, impacts = 0, accidents = 0, dispatches = 0, cleared = 0;
    std::uint64_t red_light_runs = 0;      // a car crossing its stop line on red (must stay zero)
    std::uint64_t lane_departures = 0;     // steps in which a car in lane was more than 1.5 m from its lane's centreline (outside accident handling)
    double max_lane_error_m = 0.0;
    double ms_traffic = 0, ms_vehicles = 0;
};

class TrafficDriver {
public:
    // `d.traffic` is stepped by the driver (it owns the signals too); `target_vehicles` is the population the district keeps on its streets.
    TrafficDriver(District& d, std::uint32_t seed, int target_vehicles, bool accidents_enabled);
    ~TrafficDriver();
    TrafficDriver(const TrafficDriver&) = delete;
    TrafficDriver& operator=(const TrafficDriver&) = delete;

    void step(double dt);   // fixed steps of 1/60 s
    [[nodiscard]] std::vector<VehicleView> vehicles() const;
    [[nodiscard]] std::vector<TrafficEvent> take_events();
    [[nodiscard]] TrafficStats stats() const;
    // Positions of the moving cars as obstacles for the pedestrians' crowd solver: x, y, vx, vy, radius.
    [[nodiscard]] std::vector<std::array<double, 5>> obstacles() const;
    // Dispatches the response from the fire station to the lane nearest to (x, y).
    void dispatch_emergency(double x, double y);
    // For tests and tools: makes the vehicle `id` lose its driver's attention now for `seconds` (it drifts toward the kerb), the same event the world's chance of an accident triggers.
    bool distract(std::uint32_t id, double seconds);
    [[nodiscard]] double time() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige::alive
