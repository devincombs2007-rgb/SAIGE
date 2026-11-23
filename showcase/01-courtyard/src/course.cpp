#include "course.hpp"

#include "saige/gfx_assets/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace saige::showcase {

namespace {

float num(const gfx::JsonValue& v, std::string_view key, float fallback)
{
    const gfx::JsonValue* m = v.find(key);
    return m != nullptr && m->is_number() ? static_cast<float>(m->as_number()) : fallback;
}

Vec3 vec3(const gfx::JsonValue& v, const char* what)
{
    if (!v.is_array() || v.size() != 3) {
        throw std::runtime_error(std::string("scene.json: ") + what + " must be [x, y, z]");
    }
    return {static_cast<float>(v.at(0).as_number()), static_cast<float>(v.at(1).as_number()), static_cast<float>(v.at(2).as_number())};
}

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void add(float f)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        for (int i = 0; i < 4; ++i) {
            h ^= (bits >> (8 * i)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    void add(uint32_t u)
    {
        for (int i = 0; i < 4; ++i) {
            h ^= (u >> (8 * i)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
};

} // namespace

const char* to_string(EventKind kind) noexcept
{
    switch (kind) {
    case EventKind::Jump: return "jump";
    case EventKind::Land: return "land";
    case EventKind::StepUp: return "step_up";
    case EventKind::Blocked: return "blocked";
    case EventKind::WaypointReached: return "waypoint_reached";
    case EventKind::WaypointTimedOut: return "waypoint_timed_out";
    case EventKind::WaypointFailed: return "waypoint_failed";
    }
    return "unknown";
}

SceneData load_scene(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const gfx::JsonParseResult parsed = gfx::parse_json(buffer.str());
    if (!parsed.ok()) {
        throw std::runtime_error(path.string() + ": " + parsed.error);
    }
    const gfx::JsonValue& root = *parsed.value;
    if (root["schema_version"].as_index() != 1) {
        throw std::runtime_error("scene.json: unsupported schema_version");
    }
    SceneData s;
    const gfx::JsonValue& ch = root["character"];
    s.radius = num(ch, "radius", 0.3f);
    s.height = num(ch, "height", 1.7f);
    if (const gfx::JsonValue* spawn = ch.find("spawn")) {
        s.spawn = vec3(*spawn, "character.spawn");
    }
    const gfx::JsonValue& colliders = root["colliders"];
    for (std::size_t i = 0; i < colliders.size(); ++i) {
        const gfx::JsonValue& c = colliders.at(i);
        s.colliders.push_back({vec3(c["min"], "collider.min"), vec3(c["max"], "collider.max")});
        s.collider_names.push_back(c["name"].as_string());
    }
    const gfx::JsonValue& decals = root["decals"];
    for (std::size_t i = 0; i < decals.size(); ++i) {
        const gfx::JsonValue& d = decals.at(i);
        s.decals.push_back({num(d, "x", 0), num(d, "y", 0), num(d, "z", 0), num(d, "width", 1), num(d, "depth", 1)});
    }
    const gfx::JsonValue& route = root["route"];
    for (std::size_t i = 0; i < route.size(); ++i) {
        const gfx::JsonValue& r = route.at(i);
        Waypoint w;
        w.x = num(r, "x", 0);
        w.y = num(r, "y", 0);
        w.sprint = r["sprint"].as_bool(false);
        w.speed = num(r, "speed", 1.0f);
        w.jump_distance = num(r, "jump_distance", -1.0f);
        w.max_seconds = num(r, "max_seconds", 20.0f);
        w.timed = r["timed"].as_bool(false);
        s.route.push_back(w);
    }
    if (s.route.empty()) {
        throw std::runtime_error("scene.json: the route is empty");
    }
    return s;
}

float surface_height(const SceneData& scene, float x, float y, float z_limit)
{
    float best = 0.0f;
    for (const Aabb& b : scene.colliders) {
        if (x >= b.min.x && x <= b.max.x && y >= b.min.y && y <= b.max.y && b.max.z <= z_limit && b.max.z > best) {
            best = b.max.z;
        }
    }
    return best;
}

CourseResult run_course(const SceneData& scene, uint32_t max_ticks)
{
    CapsuleCharacterConfig config;
    config.radius = scene.radius;
    config.height = scene.height;
    CapsuleCharacter character(scene.spawn, scene.colliders, config);

    CourseResult result;
    result.trace.reserve(max_ticks);
    Fnv fnv;
    std::size_t index = 0;
    uint32_t waypoint_ticks = 0;
    bool jumped_here = false;
    bool jump_held = false;
    bool was_grounded = character.state().grounded;
    float previous_z = character.state().feet.z;
    Vec3 previous_feet = character.state().feet;
    uint32_t blocked_ticks = 0;

    for (uint32_t tick = 0; tick < max_ticks && index < scene.route.size(); ++tick) {
        const Waypoint& wp = scene.route[index];
        const CapsuleCharacterState& before = character.state();
        const float dx = wp.x - before.feet.x;
        const float dy = wp.y - before.feet.y;
        const float distance = std::hypot(dx, dy);

        CapsuleInput input;
        if (distance > 1e-4f) {
            input.forward = dx / distance * wp.speed;  // +X is forward and +Y left in world axes
            input.left = dy / distance * wp.speed;
        }
        input.sprint = wp.sprint;
        const bool want_jump = wp.jump_distance >= 0.0f && distance <= wp.jump_distance && !jumped_here && before.grounded;
        input.jump = want_jump;  // a single tick: the rising edge is what the controller reacts to
        if (want_jump) {
            jumped_here = true;
        }
        jump_held = input.jump;

        character.step(input, fixed_step);
        const CapsuleCharacterState& after = character.state();

        if (jump_held && !after.grounded && was_grounded) {
            result.events.push_back({tick, EventKind::Jump, static_cast<uint32_t>(index)});
        }
        if (after.grounded && !was_grounded) {
            result.events.push_back({tick, EventKind::Land, static_cast<uint32_t>(index)});
        }
        if (after.grounded && was_grounded && after.feet.z - previous_z > 0.08f) {
            result.events.push_back({tick, EventKind::StepUp, static_cast<uint32_t>(index)});
        }
        // Blocked: the character is told to move at speed but makes almost no headway (a wall, a column, a crate).
        const float headway = std::hypot(after.feet.x - previous_feet.x, after.feet.y - previous_feet.y) / (1.0f / 60.0f);
        const float wanted = (wp.sprint ? config.sprint_speed : config.walk_speed) * wp.speed;
        if (after.grounded && distance > 1.0f && headway < 0.25f * wanted && tick > 30) {
            if (++blocked_ticks == 6) {
                result.events.push_back({tick, EventKind::Blocked, static_cast<uint32_t>(index)});
            }
        } else {
            blocked_ticks = 0;
        }
        result.distance += std::hypot(after.feet.x - previous_feet.x, after.feet.y - previous_feet.y);
        result.max_height = std::max(result.max_height, after.feet.z);
        was_grounded = after.grounded;
        previous_z = after.feet.z;
        previous_feet = after.feet;

        result.trace.push_back({after.feet, after.velocity, after.grounded, static_cast<uint32_t>(index)});
        fnv.add(after.feet.x);
        fnv.add(after.feet.y);
        fnv.add(after.feet.z);
        fnv.add(after.velocity.x);
        fnv.add(after.velocity.y);
        fnv.add(after.velocity.z);
        fnv.add(static_cast<uint32_t>(after.grounded));

        ++waypoint_ticks;
        const float arrival = wp.jump_distance >= 0.0f ? 0.25f : 0.45f;
        const bool arrived = std::hypot(wp.x - after.feet.x, wp.y - after.feet.y) < arrival && after.grounded;
        const bool timed_out = static_cast<float>(waypoint_ticks) / 60.0f >= wp.max_seconds;
        if (arrived || timed_out) {
            if (arrived) {
                result.events.push_back({tick, EventKind::WaypointReached, static_cast<uint32_t>(index)});
            } else if (wp.timed) {
                result.events.push_back({tick, EventKind::WaypointTimedOut, static_cast<uint32_t>(index)});
            } else {
                result.events.push_back({tick, EventKind::WaypointFailed, static_cast<uint32_t>(index)});
                ++result.failed_waypoints;
            }
            ++index;
            waypoint_ticks = 0;
            jumped_here = false;
        }
    }
    result.finished = index >= scene.route.size();
    result.hash = fnv.h;
    return result;
}

} // namespace saige::showcase
