// Showcase 03: the simulation. One fixed 60 Hz step drives a streamed world (64 m cells prepared in the background, three-by-three resident around the
// focus entity, the local origin migrated to the focus cell), Core's capsule character controller, the rigid-body vehicle (saige/vehicle.hpp), a scripted player
// (inputs only: it never writes a position) and a handful of physical props. The simulation never reads a clock: which cells are resident is a function of where the
// focus entity is, and a cell that is not ready when it is needed is waited for, so a replay of the same script is bit-identical.
//
// Frame: the local frame is world meters minus `origin()` (the corner of the focus cell), so every float stays small; the host draws in it.
#pragma once

#include "saige/capsule_character.hpp"
#include "saige/static_collision.hpp"
#include "saige/vehicle.hpp"
#include "world.hpp"

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace saige::horizon {

using CellKey = std::pair<int, int>;

struct StreamStats {
    std::uint64_t cells_prepared = 0;     // CPU cells built (on a worker thread or on demand)
    std::uint64_t prefetch_hits = 0;      // a needed cell that a background task had already finished
    std::uint64_t blocking_waits = 0;     // a needed cell the simulation had to wait for
    std::uint64_t residency_changes = 0;  // times the resident set changed
    std::uint64_t origin_migrations = 0;
    std::size_t resident_cells = 0;
    std::uint64_t resident_triangles = 0;
    std::uint64_t resident_collision_triangles = 0;
};

enum class Phase : std::uint8_t { Establish, Idle, Walk, Run, Sprint, Jump, Approach, EnterCar, Drive, Crash, Aftermath, ExitCar, WalkAway, Overlook, Done };
[[nodiscard]] const char* phase_name(Phase p);

struct Door {
    float angle = 0.0f;  // 0 closed, up to ~1.15 rad open
};

struct PropBody {
    PropType type{};
    double x{}, y{}, z{};        // world meters (the body's base point)
    float vx{}, vy{}, vz{};
    float yaw{}, pitch{}, roll{};
    float wy{}, wp{}, wr{};      // angular velocity
    float scale = 1.0f;
    float radius = 0.3f;         // for the ground and car contacts
    float height = 0.9f;
    bool settled = false;
    bool debris = false;         // a fragment (concrete chunk), not a placed object
    float age = 0.0f;
};

struct DustPuff {
    double x{}, y{}, z{};
    float vx{}, vy{}, vz{};
    float age = 0.0f, life = 2.0f, size = 0.6f;
};

struct ImpactEvent {
    double time{};
    float speed{};
    double x{}, y{}, z{};  // world
    float nx{}, ny{}, nz{};
    float energy{};
    bool barrier = false;
};

struct TickRecord {
    float t{};
    float vehicle_speed{}, vehicle_throttle{}, vehicle_steer{}, vehicle_rpm{};
    float character_speed{};
    bool character_grounded{};
    bool driving{};
    std::uint8_t phase{};
};

class Sim {
public:
    static constexpr double step_seconds = 1.0 / 60.0;

    explicit Sim(const World& world);
    ~Sim();

    void tick();
    [[nodiscard]] double time() const noexcept { return time_; }
    [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    // ---- State in the local frame (meters from origin()), for the renderer.
    [[nodiscard]] D2 origin() const noexcept { return origin_; }
    [[nodiscard]] std::uint64_t origin_epoch() const noexcept { return origin_epoch_; }
    [[nodiscard]] const std::set<CellKey>& resident() const noexcept { return resident_; }
    [[nodiscard]] std::shared_ptr<const CellData> cell(const CellKey& k) const;
    [[nodiscard]] const StaticCollision& collision() const noexcept { return collision_; }

    [[nodiscard]] Vec3 character_feet() const noexcept;       // local
    [[nodiscard]] float character_yaw() const noexcept { return char_yaw_; }
    [[nodiscard]] float character_speed() const noexcept { return char_speed_; }
    [[nodiscard]] float gait_phase() const noexcept { return gait_; }
    [[nodiscard]] bool character_visible() const noexcept { return char_visible_; }
    [[nodiscard]] bool character_grounded() const noexcept;
    [[nodiscard]] float character_turn() const noexcept { return char_turn_; }

    [[nodiscard]] bool vehicle_present() const noexcept { return vehicle_.has_value(); }
    [[nodiscard]] const Vehicle* vehicle() const noexcept { return vehicle_ ? &*vehicle_ : nullptr; }
    [[nodiscard]] bool driving() const noexcept { return driving_; }
    [[nodiscard]] const Door& door() const noexcept { return door_; }
    [[nodiscard]] const VehicleInput& vehicle_input() const noexcept { return vehicle_input_; }
    [[nodiscard]] float damage() const noexcept { return damage_; }                 // 0 intact, 1 heavily damaged
    [[nodiscard]] Vec3 damage_point() const noexcept { return damage_point_; }       // in the car's local frame
    [[nodiscard]] Vec3 damage_dir() const noexcept { return damage_dir_; }

    [[nodiscard]] const std::vector<PropBody>& bodies() const noexcept { return bodies_; }
    [[nodiscard]] const std::vector<DustPuff>& dust() const noexcept { return dust_; }
    [[nodiscard]] const std::vector<ImpactEvent>& impacts() const noexcept { return impacts_; }
    [[nodiscard]] const StreamStats& stream_stats() const noexcept { return stats_; }
    [[nodiscard]] const std::vector<TickRecord>& trace() const noexcept { return trace_; }

    // World coordinates of a local point.
    [[nodiscard]] D2 to_world(double lx, double ly) const noexcept { return {lx + origin_.x, ly + origin_.y}; }
    [[nodiscard]] double world_height(double wx, double wy) const { return world_.height(wx, wy); }

    // The checkpoint: the character, the (possibly parked) vehicle, the script's position in time and the damage, as JSON. Restoring continues the same run.
    [[nodiscard]] std::string checkpoint_json() const;
    [[nodiscard]] static std::unique_ptr<Sim> from_checkpoint(const World& world, const std::string& json);
    [[nodiscard]] std::uint64_t state_hash() const;

    // The script's phase boundaries, for the director (seconds of sim time).
    struct Timeline {
        double walk = 0, run = 0, sprint = 0, jump = 0, approach = 0, enter = 0, drive = 0, crash = 0, aftermath = 0, exit = 0, walk_away = 0, overlook = 0, end = 0;
    };
    [[nodiscard]] const Timeline& timeline() const noexcept { return timeline_; }

private:
    struct VehicleRecord {
        bool exists = true;
        double x{}, y{}, z{};     // world, the chassis box center
        VehicleQuat q;
        float yaw{};
        bool parked = true;
    };
    struct Cache;

    void update_residency();
    void rebuild_collision(D2 new_origin);
    void step_character(double dt);
    void step_vehicle(double dt);
    void script(double dt);
    void step_props(double dt);
    void record(const TickRecord& r);
    void handle_impacts();
    void spawn_debris(const ImpactEvent& e);
    [[nodiscard]] VehicleInput drive_inputs();

    const World& world_;
    std::unique_ptr<Cache> cache_;
    std::set<CellKey> resident_;
    D2 origin_{};
    std::uint64_t origin_epoch_ = 0;
    StaticCollision collision_;
    QueryScratch scratch_;
    StreamStats stats_;

    std::optional<CapsuleCharacter> character_;
    float char_yaw_ = 0.0f, char_speed_ = 0.0f, gait_ = 0.0f, char_turn_ = 0.0f;
    bool char_visible_ = true;
    CapsuleInput char_input_{};

    std::optional<Vehicle> vehicle_;
    VehicleRecord vehicle_record_;
    VehicleInput vehicle_input_{};
    bool driving_ = false;
    Door door_;
    float damage_ = 0.0f;
    Vec3 damage_point_{};
    Vec3 damage_dir_{};
    double route_s_ = 0.0;

    std::vector<PropBody> bodies_;
    std::vector<DustPuff> dust_;
    std::vector<ImpactEvent> impacts_;
    std::vector<TickRecord> trace_;

    Phase phase_ = Phase::Establish;
    Timeline timeline_;
    double time_ = 0.0;
    std::uint64_t tick_ = 0;
    double phase_time_ = 0.0;
    int waypoint_ = 0;
    bool crashed_ = false;
    double rest_time_ = 0.0;
    double enter_progress_ = 0.0;
    Vec3 enter_from_{};
    Vec3 exit_to_{};
    bool jumped_ = false;
};

} // namespace saige::horizon
