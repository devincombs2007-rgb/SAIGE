#pragma once

// Authored fracture and breakable constraints (ENV-1, first foundation): an object is a set of **pieces** (rigid bodies the artist authored: the planks and posts of a fence, the shards of a pane) held together by **links**
// (fixed constraints, each with the force and torque it can carry). A link that carries more than its limit for the configured number of consecutive steps (one by default: an impact is a one-step spike) gives way: its
// constraint is removed, the pieces it held move on under the physics that already exists, and a `BreakEvent` says what happened, where, in what material and how hard, so audio, debris effects and the persistent state of
// the world can all follow the same fact. Nothing is simulated that the dynamics world does not already simulate: there is no deformation and no runtime fracturing here, only authored pieces and breakable links (the
// director's order: more advanced deformation and destruction only after these foundations are stable).
//
// Limits, stated: the pieces of one object never collide with each other (they share a self-collision group, so up to 64 pieces per object, and a shard does not collide with its neighbour even after the link broke);
// a link is a fixed constraint, so a piece cannot flex or bend before it breaks; the load a link reports is the solver's last-step force: an impulse spread over the step, so a link's limit is in newtons *per step* of
// 1/60 s (a 20 kg body at 7 m/s carries 140 N s, which is 8.4 kN averaged over one step). Choose limits from measured loads (`Breakable::link_load`), above what the object carries at rest.

#include "saige/dynamics.hpp"
#include "saige/physics_material.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace saige {

inline constexpr std::uint32_t kAnchoredToWorld = 0xffffffffu;
inline constexpr std::size_t kMaxFracturePieces = 64;

struct FracturePiece {
    DShapeKind shape = DShapeKind::Box;
    DVec half_extents{0.5, 0.5, 0.5};
    DVec offset;                  // from the object's origin, in the object's frame
    DQuat rotation;
    double mass = 1.0;            // kg
};

struct FractureLink {
    std::uint32_t a = 0;
    std::uint32_t b = kAnchoredToWorld;  // another piece, or kAnchoredToWorld: held by the ground (a post set in the earth)
    double break_force = 1.0e9;           // N (over one solver step): the load the link carries before it gives way
    double break_torque = 1.0e9;          // N m
};

struct FractureAsset {
    std::uint32_t id = 0;         // identifies the asset in saved damage
    std::string name;
    BreakMaterial material = BreakMaterial::Wood;
    Surface surface = Surface::Default;   // the contact material of every piece
    std::vector<FracturePiece> pieces;
    std::vector<FractureLink> links;
    unsigned confirm_steps = 1;   // consecutive steps over the limit before a link breaks; raise it for a link loaded by a sustained push rather than by an impact
};

// A link that broke: enough for the audio, the debris and the world state to react without looking inside.
struct BreakEvent {
    std::uint32_t instance = 0;
    std::uint32_t link = 0;
    std::uint32_t piece_a = 0, piece_b = kAnchoredToWorld;
    BreakMaterial material = BreakMaterial::Wood;
    Surface surface = Surface::Default;
    double force = 0.0, torque = 0.0;     // the load at the break
    double severity = 1.0;                // load over limit (>= 1): how far past breaking
    double piece_mass = 0.0;              // kg of the lighter piece the link held
    DVec point;                           // world, between the two pieces
};

// What a break calls for in effects, from the event alone (pure): the sound lives in audio_model.hpp (`break_sound`); this is the debris burst a particle system should emit.
struct DebrisBurst {
    unsigned count = 0;
    double speed = 0.0;                   // m/s of the fragments' ejection
    double size = 0.0;                    // m, the typical fragment
    BreakMaterial material = BreakMaterial::Wood;
};
[[nodiscard]] DebrisBurst debris_for_break(const BreakEvent& e);

// The saved state of one instance: which links are gone and where every piece rests, so that a reloaded world shows the same fence broken the same way.
struct DamageRecord {
    std::uint32_t asset_id = 0;
    std::uint32_t instance_id = 0;
    std::vector<std::uint8_t> link_broken;          // one per link, 0 or 1
    std::vector<double> piece_pose;                 // 7 per piece: position xyz, rotation xyzw (world)
};

constexpr std::uint32_t kDamageFormatVersion = 1;

[[nodiscard]] std::vector<std::uint8_t> serialize_damage(const std::vector<DamageRecord>& records);
// Returns nullopt, with `error` set, for bad magic, a version newer than this build reads, a truncated or oversized record, or a checksum mismatch (a corrupted file is refused, never half applied).
[[nodiscard]] std::optional<std::vector<DamageRecord>> deserialize_damage(const std::vector<std::uint8_t>& bytes, std::string& error);

class Breakable {
public:
    // Creates the pieces and the links in the world at `position`/`rotation`. With `restore`, the broken links stay broken (no constraint is created for them) and the pieces start at their saved poses.
    // Throws std::invalid_argument for an empty or oversized asset, a link naming a piece that is not there, a non-positive mass or size, a break limit that is not positive, or a record that does not fit the asset;
    // and whatever the world throws when it is full (everything created so far is removed first).
    Breakable(DynamicsWorld& world, const FractureAsset& asset, std::uint32_t instance_id, DVec position, DQuat rotation, const DamageRecord* restore = nullptr);
    ~Breakable();
    Breakable(const Breakable&) = delete;
    Breakable& operator=(const Breakable&) = delete;

    // Reads the load of every intact link after a world step and breaks those over their limit for the configured number of steps; appends one event per break.
    void update(std::vector<BreakEvent>& events);

    [[nodiscard]] std::size_t piece_count() const { return bodies_.size(); }
    [[nodiscard]] std::size_t link_count() const { return links_.size(); }
    [[nodiscard]] std::size_t broken_count() const;
    [[nodiscard]] bool link_broken(std::size_t link) const;
    [[nodiscard]] DConstraintLoad link_load(std::size_t link) const;  // the load an intact link carries now (zero once broken): for tuning limits and for debug views
    [[nodiscard]] BodyId body(std::size_t piece) const { return bodies_.at(piece); }
    [[nodiscard]] DamageRecord damage() const;
    void remove_from_world();             // removes the pieces and the intact links; the object is gone (also done by the destructor)

private:
    struct LinkState {
        FractureLink link;
        ConstraintId constraint;
        bool broken = false;
        unsigned over = 0;
    };
    DynamicsWorld& world_;
    FractureAsset asset_;
    std::uint32_t instance_;
    std::vector<BodyId> bodies_;
    std::vector<LinkState> links_;
    bool removed_ = false;
};

} // namespace saige
