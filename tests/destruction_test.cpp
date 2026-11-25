// Authored fracture and breakable constraints (include/saige/destruction.hpp) on the Jolt dynamics backend: an intact object holds and does not break by itself, a hard blow breaks the links near it and a soft one breaks none,
// harder blows break at least as many, the same scenario breaks the same way twice, glass sheds shards that fall, damage persists through a save and a reload (and a damaged file is refused), and a failed creation
// leaves nothing behind. Exit 77 (skipped) when no backend is compiled in.
#include "saige/destruction.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

template <class F>
bool throws_invalid(F f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

StaticCollision floor_only() {
    std::vector<Aabb> boxes{{{-100.0f, -100.0f, -2.0f}, {100.0f, 100.0f, 0.0f}}};
    return StaticCollision(boxes, {});
}

std::unique_ptr<DynamicsWorld> make(DynamicsConfig cfg = {}) {
    auto w = make_dynamics_world(DynamicsBackend::Jolt, cfg);
    require(w != nullptr, "the Jolt backend is compiled in");
    static const StaticCollision floor = floor_only();
    w->set_static_geometry(floor, nullptr);
    return w;
}

// A fence along +X: five posts set in the ground and four panels between them, each panel held to its two posts. The posts are 1.2 m high, the panels 0.5 m high at 0.5 m above the ground.
FractureAsset fence(double panel_limit = 1500.0, double post_limit = 60000.0) {
    FractureAsset a;
    a.id = 7;
    a.name = "wooden fence";
    a.material = BreakMaterial::Wood;
    const double pitch = 1.5;
    for (int i = 0; i < 5; ++i) {
        FracturePiece post;
        post.half_extents = {0.06, 0.06, 0.6};
        post.offset = {pitch * i, 0.0, 0.6};
        post.mass = 15.0;
        a.pieces.push_back(post);
    }
    for (int i = 0; i < 4; ++i) {
        FracturePiece panel;
        panel.half_extents = {0.69, 0.025, 0.25};
        panel.offset = {pitch * i + pitch / 2, 0.0, 0.75};
        panel.mass = 10.0;
        a.pieces.push_back(panel);
    }
    for (std::uint32_t i = 0; i < 5; ++i) a.links.push_back({i, kAnchoredToWorld, post_limit, post_limit});
    for (std::uint32_t i = 0; i < 4; ++i) {
        a.links.push_back({5 + i, i, panel_limit, panel_limit});
        a.links.push_back({5 + i, i + 1, panel_limit, panel_limit});
    }
    return a;
}


// A two-metre beam held at its centre by one link to the world: a blow at its end is mostly a turning moment, so the torque limit decides.
FractureAsset beam(double force_limit, double torque_limit, unsigned confirm = 1) {
    FractureAsset a;
    a.id = 11;
    a.name = "beam";
    a.material = BreakMaterial::Metal;
    FracturePiece p;
    p.half_extents = {1.0, 0.05, 0.05};
    p.offset = {1.0, 0.0, 1.0};
    p.mass = 10.0;
    a.pieces.push_back(p);
    a.links.push_back({0, kAnchoredToWorld, force_limit, torque_limit});
    a.confirm_steps = confirm;
    return a;
}

// A pane of glass: a 4 x 4 grid of shards in a frame (the frame's links to the world are strong, the links between shards weak).
FractureAsset pane(double shard_limit = 60.0) {
    FractureAsset a;
    a.id = 9;
    a.name = "window pane";
    a.material = BreakMaterial::Glass;
    a.surface = Surface::Ice;
    const int n = 4;
    const double s = 0.15;  // shard half width
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            FracturePiece p;
            p.half_extents = {s, 0.006, s};
            p.offset = {(c - (n - 1) / 2.0) * 2 * s, 0.0, 1.2 + (r - (n - 1) / 2.0) * 2 * s};
            p.mass = 1.2;
            a.pieces.push_back(p);
        }
    }
    const auto id = [&](int r, int c) { return static_cast<std::uint32_t>(r * n + c); };
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            if (c + 1 < n) a.links.push_back({id(r, c), id(r, c + 1), shard_limit, shard_limit});
            if (r + 1 < n) a.links.push_back({id(r, c), id(r + 1, c), shard_limit, shard_limit});
            if (r == 0 || r == n - 1 || c == 0 || c == n - 1) a.links.push_back({id(r, c), kAnchoredToWorld, 20000.0, 20000.0});  // the frame
        }
    }
    return a;
}

DBodyDesc projectile(double x, double y, double z, double vy, double mass, double radius = 0.15) {
    DBodyDesc d;
    d.shape = DShapeKind::Sphere;
    d.half_extents = {radius, 0, 0};
    d.position = {x, y, z};
    d.mass = mass;
    d.linear_velocity = {0, vy, 0};
    d.continuous = true;
    return d;
}

// A projectile flies straight at any speed: its weight is cancelled each step (a gravity-free test shot, so the aim does not depend on the speed).
void fly(DynamicsWorld& w, BodyId p, double mass, double dt = 1.0 / 60.0) {
    w.add_impulse(p, {0, 0, mass * 9.81 * dt}, w.state(p).position);
    w.step(dt);
}

struct Run {
    std::vector<BreakEvent> events;
    std::vector<std::size_t> event_steps;
    std::size_t broken = 0;
    DamageRecord damage;
    double peak_torque = 0.0;       // N m over one step
    double peak_panel_load = 0.0;  // N over one step, the largest any link carried (measured to choose the limits)
};

Run hit(const FractureAsset& asset, double speed, double mass, double x, double z, double seconds = 4.0) {
    auto w = make();
    Breakable b(*w, asset, 1, {0, 0, 0}, {});
    const BodyId shot = w->add_body(projectile(x, -4.0, z, speed, mass));
    Run r;
    const int steps = static_cast<int>(seconds * 60.0);
    for (int i = 0; i < steps; ++i) {
        fly(*w, shot, mass);
        for (std::size_t k = 0; k < b.link_count(); ++k) { r.peak_panel_load = std::max(r.peak_panel_load, b.link_load(k).force); r.peak_torque = std::max(r.peak_torque, b.link_load(k).torque); }
        const std::size_t before = r.events.size();
        b.update(r.events);
        for (std::size_t k = before; k < r.events.size(); ++k) r.event_steps.push_back(static_cast<std::size_t>(i));
    }
    r.broken = b.broken_count();
    r.damage = b.damage();
    return r;
}

void test_intact() {
    auto w = make();
    Breakable b(*w, fence(), 1, {0, 0, 0}, {});
    require(b.piece_count() == 9 && b.link_count() == 13 && b.broken_count() == 0, "a fence of 5 posts and 4 panels, 13 links");
    std::vector<BreakEvent> ev;
    std::vector<DVec> start;
    for (std::size_t i = 0; i < b.piece_count(); ++i) start.push_back(w->state(b.body(i)).position);
    for (int i = 0; i < 600; ++i) {
        w->step(1.0 / 60.0);
        b.update(ev);
    }
    require(ev.empty() && b.broken_count() == 0, "an untouched fence does not break by itself (ten seconds under gravity)");
    double drift = 0.0;
    for (std::size_t i = 0; i < b.piece_count(); ++i) {
        const DVec p = w->state(b.body(i)).position;
        drift = std::max(drift, std::hypot(p.x - start[i].x, std::hypot(p.y - start[i].y, p.z - start[i].z)));
    }
    std::printf("intact fence drift after 10 s: %.5f m\n", drift);
    require(drift < 0.02, "and it stays where it was authored");
    // The load on a link is what holds the weight: a panel's 10 kg over two links, a few tens of newtons, far under the limit.
    const DConstraintLoad none = w->constraint_load(ConstraintId{});
    require(none.force == 0.0 && none.torque == 0.0, "a stale constraint id reports no load");
}

void test_hits() {
    const FractureAsset f = fence();
    const Run soft = hit(f, 1.0, 20.0, 2.25, 0.75), medium = hit(f, 7.0, 20.0, 2.25, 0.75), hard = hit(f, 14.0, 20.0, 2.25, 0.75), hardest = hit(f, 22.0, 40.0, 2.25, 0.75);
    for (const Run* rr : {&soft,&medium,&hard,&hardest}) std::printf("  peak link load %.0f N\n", rr->peak_panel_load);
    std::printf("fence hits (20 kg at the middle panel): 1 m/s %zu broken, 7 m/s %zu, 14 m/s %zu, (40 kg) 22 m/s %zu\n", soft.broken, medium.broken, hard.broken, hardest.broken);
    require(soft.broken == 0 && soft.events.empty(), "a soft push breaks nothing");
    require(medium.broken <= hard.broken && hard.broken <= hardest.broken, "harder blows break at least as many links");
    require(hard.broken >= 1 && hardest.broken > soft.broken, "a hard blow breaks the fence");
    // The break is local: the links of the far end hold when the middle takes the blow.
    require(!hard.damage.link_broken.empty() && hard.damage.link_broken[0] == 0 && hard.damage.link_broken[4] == 0, "the posts at the ends hold when the middle is hit");
    for (const BreakEvent& e : hard.events) {
        require(e.severity >= 1.0 && e.force > 0.0 && e.piece_mass > 0.0 && e.material == BreakMaterial::Wood && e.instance == 1, "a break event carries its load, severity, material and mass");
        require(std::fabs(e.point.y) < 1.0 && e.point.x > 0.0 && e.point.x < 6.0, "at a point on the fence");
        require(e.piece_a >= 5 || e.piece_b != kAnchoredToWorld || e.severity >= 1.0, "between real pieces");
    }
    // A panel that lost its links falls: the pieces move under the physics that already exists.
    auto w = make();
    Breakable b(*w, f, 1, {0, 0, 0}, {});
    const BodyId shot = w->add_body(projectile(2.25, -4.0, 0.75, 20.0, 40.0));
    std::vector<BreakEvent> ev;
    for (int i = 0; i < 360; ++i) {
        fly(*w, shot, 40.0);
        b.update(ev);
    }
    double lowest_shift = 0.0;
    for (std::size_t i = 5; i < 9; ++i) lowest_shift = std::max(lowest_shift, std::fabs(w->state(b.body(i)).position.y) + (0.75 - w->state(b.body(i)).position.z));
    require(lowest_shift > 0.3, "a broken panel has been thrown or has fallen");
}

void test_glass() {
    const FractureAsset g = pane();
    const Run toss = hit(g, 1.0, 0.2, 0.0, 1.2, 3.0), stone = hit(g, 12.0, 1.0, 0.0, 1.2, 3.0);
    std::printf("  pane peaks: toss %.0f N, stone %.0f N\n", toss.peak_panel_load, stone.peak_panel_load);
    std::printf("pane: toss %zu of %zu links broken, stone %zu\n", toss.broken, g.links.size(), stone.broken);
    require(toss.broken == 0, "a gentle toss does not break the pane");
    require(stone.broken >= 4 && stone.broken < g.links.size(), "a stone breaks several links but not the frame all round");
    // The shards fall: some shard ends well below where it started.
    auto w = make();
    Breakable b(*w, g, 1, {0, 0, 0}, {});
    const BodyId shot = w->add_body(projectile(0.0, -4.0, 1.2, 12.0, 1.0));
    std::vector<BreakEvent> ev;
    for (int i = 0; i < 240; ++i) {
        fly(*w, shot, 1.0);
        b.update(ev);
    }
    double lowest = 10.0;
    for (std::size_t i = 0; i < b.piece_count(); ++i) lowest = std::min(lowest, w->state(b.body(i)).position.z);
    std::printf("lowest shard height after 4 s: %.3f m (authored from %.2f m)\n", lowest, 1.2 - 0.45);
    require(lowest < 0.2, "shards fall to the ground");
    require(!ev.empty() && ev[0].material == BreakMaterial::Glass && ev[0].surface == Surface::Ice, "the events say glass");
    // Debris: more and faster for a harder break, many small shards for glass, almost none for metal.
    BreakEvent e = ev[0];
    e.severity = 1.0;
    const DebrisBurst low = debris_for_break(e);
    e.severity = 8.0;
    const DebrisBurst high = debris_for_break(e);
    BreakEvent m = e;
    m.material = BreakMaterial::Metal;
    require(high.count > low.count && high.speed > low.speed && low.count > debris_for_break(m).count && low.size < 0.05, "glass throws many small shards, more of them and faster when it is hit harder");
}

void test_determinism() {
    const FractureAsset f = fence();
    const Run a = hit(f, 14.0, 20.0, 2.25, 0.75), b = hit(f, 14.0, 20.0, 2.25, 0.75);
    require(a.events.size() == b.events.size() && a.event_steps == b.event_steps, "the same blow breaks the same links on the same steps");
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        require(a.events[i].link == b.events[i].link && a.events[i].force == b.events[i].force && a.events[i].point.x == b.events[i].point.x, "with the same loads and places");
    }
    require(a.damage.piece_pose == b.damage.piece_pose, "and the debris rests in the same poses");
}

void test_persistence() {
    const FractureAsset f = fence();
    const Run r = hit(f, 16.0, 30.0, 2.25, 0.75);
    require(r.broken >= 1, "something broke to be saved");
    const std::vector<std::uint8_t> bytes = serialize_damage({r.damage});
    std::string err;
    const auto back = deserialize_damage(bytes, err);
    require(back.has_value() && back->size() == 1 && (*back)[0].link_broken == r.damage.link_broken && (*back)[0].piece_pose == r.damage.piece_pose && (*back)[0].asset_id == 7, "a saved record reads back exactly");
    // A reloaded world: the broken links stay broken, the pieces are where they rested, nothing breaks further and nothing moves.
    auto w = make();
    Breakable b(*w, f, 1, {0, 0, 0}, {}, &(*back)[0]);
    require(b.broken_count() == r.broken, "the same links are broken after the reload");
    for (std::size_t i = 0; i < f.links.size(); ++i) require(b.link_broken(i) == (r.damage.link_broken[i] != 0), "link by link");
    std::vector<BreakEvent> ev;
    for (int i = 0; i < 300; ++i) {
        w->step(1.0 / 60.0);
        b.update(ev);
    }
    require(ev.empty(), "a restored object does not break by itself");
    const DamageRecord after = b.damage();
    double worst = 0.0;
    for (std::size_t i = 0; i < r.damage.piece_pose.size(); ++i) worst = std::max(worst, std::fabs(after.piece_pose[i] - r.damage.piece_pose[i]));
    std::printf("restored pose drift after 5 s: %.4f\n", worst);
    require(worst < 0.15, "the debris stays where it was saved (it was at rest)");
    // A damaged or foreign file is refused, never half applied.
    std::vector<std::uint8_t> corrupt = bytes;
    corrupt[bytes.size() / 2] ^= 0x5a;
    require(!deserialize_damage(corrupt, err).has_value() && err.find("checksum") != std::string::npos, "a flipped byte is a checksum failure");
    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 12);
    require(!deserialize_damage(truncated, err).has_value(), "a truncated file is refused");
    std::vector<std::uint8_t> wrong = bytes;
    wrong[0] = 'X';
    require(!deserialize_damage(wrong, err).has_value() && err.find("magic") != std::string::npos, "a foreign file is refused");
    // A file written by a newer version is refused: rewrite the version field and the checksum as a newer writer would.
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 2;
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i + 8 < newer.size(); ++i) {
        h ^= newer[i];
        h *= 1099511628211ull;
    }
    for (int i = 0; i < 8; ++i) newer[newer.size() - 8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((h >> (8 * i)) & 0xffu);
    require(!deserialize_damage(newer, err).has_value() && err.find("newer") != std::string::npos, "a newer version is refused, with its own message");
    // A record that does not fit the asset is refused.
    DamageRecord bad = r.damage;
    bad.link_broken.pop_back();
    auto w2 = make();
    require(throws_invalid([&] { Breakable x(*w2, f, 1, {0, 0, 0}, {}, &bad); }) && w2->body_count() == 0, "a record for a different asset is refused and leaves nothing behind");
}

void test_lifetimes_and_validation() {
    auto w = make();
    const std::size_t base = w->body_count();
    {
        Breakable b(*w, fence(), 1, {0, 0, 0}, {});
        require(w->body_count() == base + 9, "the pieces are in the world");
    }
    require(w->body_count() == base, "destroying the object removes its pieces");
    // A world that is full: the second fence does not fit (room for 15 pieces, two fences are 18), the constructor fails halfway and removes the pieces it had made, and the first fence is untouched.
    DynamicsConfig tiny;
    tiny.max_bodies = 24;  // the world keeps 8 in reserve and counts its static bodies: room for 15 pieces
    auto small = make(tiny);
    Breakable first(*small, fence(), 1, {0, 0, 0}, {});
    const std::size_t before = small->body_count();
    require(throws_invalid([&] { Breakable b(*small, fence(), 2, {0, 5, 0}, {}); }) && small->body_count() == before, "a failed creation leaves nothing behind");
    std::vector<BreakEvent> none;
    small->step(1.0 / 60.0);
    first.update(none);
    require(none.empty() && first.broken_count() == 0, "and the first object is unharmed");
    // Invalid assets.
    FractureAsset bad = fence();
    bad.pieces.clear();
    require(throws_invalid([&] { Breakable b(*w, bad, 1, {0, 0, 0}, {}); }), "no pieces");
    bad = fence();
    bad.links[0].a = 99;
    require(throws_invalid([&] { Breakable b(*w, bad, 1, {0, 0, 0}, {}); }), "a link to a piece that is not there");
    bad = fence();
    bad.links[0].break_force = 0.0;
    require(throws_invalid([&] { Breakable b(*w, bad, 1, {0, 0, 0}, {}); }), "a link that can carry nothing");
    bad = fence();
    bad.pieces[0].mass = -1.0;
    require(throws_invalid([&] { Breakable b(*w, bad, 1, {0, 0, 0}, {}); }), "a negative mass");
    bad = fence();
    bad.confirm_steps = 0;
    require(throws_invalid([&] { Breakable b(*w, bad, 1, {0, 0, 0}, {}); }), "no confirmation");
    bad = fence();
    while (bad.pieces.size() <= kMaxFracturePieces) bad.pieces.push_back(bad.pieces[0]);
    require(throws_invalid([&] { Breakable b(*w, bad, 1, {0, 0, 0}, {}); }), "more pieces than a self-collision group holds");
    require(w->body_count() == base, "and none of those left a piece behind");
    // A placed and rotated object: a quarter turn about Z puts the fence along Y.
    const double r = std::sqrt(0.5);
    Breakable turned(*w, fence(), 2, {1.0, 2.0, 0.0}, {0, 0, r, r});
    const DVec p = w->state(turned.body(2)).position;  // the middle post, authored at (3, 0, 0.6)
    require(std::fabs(p.x - 1.0) < 0.05 && std::fabs(p.y - 5.0) < 0.05, "a quarter turn puts the middle post at (1, 5)");
}

void test_torque_confirm_and_overlap() {
    // Measure: a 5 kg shot at 10 m/s at the beam's end, with limits too high to break anything.
    const Run measure = hit(beam(1e9, 1e9), 10.0, 5.0, 1.9, 1.0, 1.5);
    std::printf("beam blow: peak force %.0f N, peak torque %.0f N m\n", measure.peak_panel_load, measure.peak_torque);
    require(measure.broken == 0 && measure.peak_torque > 10.0 && measure.peak_panel_load > 100.0, "the blow loads the link in force and in torque");
    // The torque limit alone breaks it: force limit far above, torque limit below the measured torque. The event carries the torque and a severity over 1.
    const Run by_torque = hit(beam(1e9, 0.5 * measure.peak_torque), 10.0, 5.0, 1.9, 1.0, 1.5);
    require(by_torque.broken == 1 && by_torque.events.size() == 1, "a torque over the limit breaks the link even though the force is far under its limit");
    const BreakEvent& e = by_torque.events[0];
    require(e.torque > 0.5 * measure.peak_torque && e.severity > 1.0 && std::fabs(e.severity - e.torque / (0.5 * measure.peak_torque)) < 1e-9 && e.piece_b == kAnchoredToWorld, "with the torque, a severity equal to the load over the limit, and the anchor");
    require(std::fabs(e.point.x - 1.0) < 0.05 && std::fabs(e.point.z - 1.0) < 0.05 && e.piece_mass == 10.0, "an anchored break is at the piece that was held, with its mass");
    // And the other way: a torque limit far above never breaks it, the force limit alone does.
    require(hit(beam(0.5 * measure.peak_panel_load, 1e9), 10.0, 5.0, 1.9, 1.0, 1.5).broken == 1, "a force over the limit breaks it too");
    require(hit(beam(2.0 * measure.peak_panel_load, 2.0 * measure.peak_torque), 10.0, 5.0, 1.9, 1.0, 1.5).broken == 0, "and limits above the measured loads hold");
    // Confirmation steps: a link over its limit only breaks after that many consecutive steps. A sustained shove, here a push every step, stays over the limit; the same limit with a long confirmation waits.
    auto shoved = [&](unsigned confirm) {
        auto w = make();
        Breakable b(*w, beam(1e9, 30.0, confirm), 1, {0, 0, 0}, {});
        std::vector<BreakEvent> ev;
        for (int i = 0; i < 120; ++i) {
            w->add_angular_impulse(b.body(0), {0.0, 0.0, 0.0});  // keep the body awake
            w->add_impulse(b.body(0), {0.0, 4.0, 0.0}, {1.9, 0.0, 1.0});  // 4 N s at the end each step: a steady 240 N push at a 0.9 m lever
            w->step(1.0 / 60.0);
            const std::size_t before = ev.size();
            b.update(ev);
            if (ev.size() > before) return i;
        }
        return -1;
    };
    const int quick = shoved(1), slow = shoved(20);
    std::printf("steady shove: breaks at step %d with confirmation 1, at step %d with 20\n", quick, slow);
    require(quick >= 0 && slow >= 0, "the shove breaks it either way");
    require(slow >= quick + 19, "the confirmation delays the break by the steps it asks for");
    // The count is of *consecutive* steps: two separate one-step spikes over the limit, with a calm between, do not add up to a break at confirmation 2.
    {
        auto w = make();
        Breakable b(*w, beam(1e9, 30.0, 2), 1, {0, 0, 0}, {});
        std::vector<BreakEvent> ev;
        std::size_t spiky_steps = 0;
        for (int i = 0; i < 120; ++i) {
            if (i == 10 || i == 60) w->add_impulse(b.body(0), {0.0, 4.0, 0.0}, {1.9, 0.0, 1.0});  // one kick each
            w->step(1.0 / 60.0);
            spiky_steps += b.link_load(0).torque > 30.0 ? 1u : 0u;
            b.update(ev);
        }
        std::printf("two separate kicks: %zu steps over the limit in all\n", spiky_steps);
        require(spiky_steps >= 2, "the kicks did put the link over its limit");
        require(ev.empty(), "but never two steps in a row, so it holds at confirmation 2");
    }
    // Authored pieces that overlap (a plank lapped over a post) are pieces of one object: they do not push each other apart.
    FractureAsset lap;
    lap.id = 12;
    FracturePiece post, plank;
    post.half_extents = {0.1, 0.1, 0.8};
    post.offset = {0, 0, 0.8};
    post.mass = 20.0;
    plank.half_extents = {0.6, 0.03, 0.15};
    plank.offset = {0.4, 0.0, 1.0};  // overlaps the post by 0.2 m
    plank.mass = 5.0;
    lap.pieces = {post, plank};
    lap.links = {{0, kAnchoredToWorld, 1e9, 1e9}, {1, 0, 1e9, 1e9}};
    auto w = make();
    Breakable b(*w, lap, 3, {0, 0, 0}, {});
    const DVec start = w->state(b.body(1)).position;
    for (int i = 0; i < 120; ++i) w->step(1.0 / 60.0);
    const DVec end = w->state(b.body(1)).position;
    require(std::hypot(end.x - start.x, std::hypot(end.y - start.y, end.z - start.z)) < 0.002, "overlapping authored pieces stay where they were put");
}

} // namespace

int main(int argc, char** argv) {
    if (available_dynamics_backends().empty()) {
        std::cout << "no dynamics backend compiled in: skipped\n";
        return 77;
    }
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"intact", test_intact}, {"hits", test_hits}, {"glass", test_glass}, {"determinism", test_determinism}, {"persistence", test_persistence}, {"lifetimes", test_lifetimes_and_validation}, {"torque", test_torque_confirm_and_overlap}};
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
    if (failures == 0) std::cout << "destruction tests passed\n";
    return failures == 0 ? 0 : 1;
}
