#include "saige/destruction.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace saige {

namespace {

bool fin(double x) { return std::isfinite(x); }
bool fin(DVec v) { return fin(v.x) && fin(v.y) && fin(v.z); }

DVec rotate(const DQuat& q, DVec v) {
    // v' = v + 2 w (u x v) + 2 u x (u x v), u = (x, y, z)
    const DVec u{q.x, q.y, q.z};
    const DVec uv{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const DVec uuv{u.y * uv.z - u.z * uv.y, u.z * uv.x - u.x * uv.z, u.x * uv.y - u.y * uv.x};
    return {v.x + 2.0 * (q.w * uv.x + uuv.x), v.y + 2.0 * (q.w * uv.y + uuv.y), v.z + 2.0 * (q.w * uv.z + uuv.z)};
}

DQuat mul(const DQuat& a, const DQuat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

void validate(const FractureAsset& a) {
    if (a.pieces.empty() || a.pieces.size() > kMaxFracturePieces) throw std::invalid_argument("FractureAsset: between 1 and 64 pieces are required");
    if (a.confirm_steps == 0 || a.confirm_steps > 600) throw std::invalid_argument("FractureAsset: confirm_steps must be within 1..600");
    for (const FracturePiece& p : a.pieces) {
        if (!fin(p.half_extents) || !(p.half_extents.x > 0.0 && p.half_extents.y > 0.0 && p.half_extents.z > 0.0) || !fin(p.offset) || !(p.mass > 0.0) || !fin(p.mass)) {
            throw std::invalid_argument("FractureAsset: a piece needs a positive finite size and mass and a finite offset");
        }
    }
    for (const FractureLink& l : a.links) {
        if (l.a >= a.pieces.size() || (l.b != kAnchoredToWorld && l.b >= a.pieces.size()) || l.a == l.b) throw std::invalid_argument("FractureAsset: a link names a piece that is not there");
        if (!(l.break_force > 0.0) || !(l.break_torque > 0.0) || std::isnan(l.break_force) || std::isnan(l.break_torque)) throw std::invalid_argument("FractureAsset: a link's break force and torque must be positive");
    }
}

} // namespace

DebrisBurst debris_for_break(const BreakEvent& e) {
    DebrisBurst d;
    d.material = e.material;
    const double s = std::clamp(e.severity, 1.0, 20.0);
    const double m = std::max(e.piece_mass, 0.01);
    // A pane throws many small shards fast, wood a few splinters, stone dust and chunks, metal almost nothing (it bends, it does not shatter, and this model has no bending).
    switch (e.material) {
    case BreakMaterial::Glass: d.count = static_cast<unsigned>(std::lround(12.0 + 4.0 * s)); d.speed = 2.0 + 0.4 * s; d.size = 0.03; break;
    case BreakMaterial::Wood: d.count = static_cast<unsigned>(std::lround(3.0 + 1.5 * s)); d.speed = 1.0 + 0.3 * s; d.size = 0.05 + 0.02 * std::cbrt(m); break;
    case BreakMaterial::Stone: d.count = static_cast<unsigned>(std::lround(6.0 + 2.0 * s)); d.speed = 1.5 + 0.3 * s; d.size = 0.04 + 0.02 * std::cbrt(m); break;
    case BreakMaterial::Plastic: d.count = static_cast<unsigned>(std::lround(2.0 + s)); d.speed = 1.0 + 0.2 * s; d.size = 0.04; break;
    case BreakMaterial::Metal: d.count = static_cast<unsigned>(std::lround(1.0 + 0.5 * s)); d.speed = 0.8 + 0.2 * s; d.size = 0.02; break;
    }
    return d;
}

// ---- persistence -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr char kMagic[4] = {'S', 'D', 'M', 'G'};

struct Writer {
    std::vector<std::uint8_t> bytes;
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu)); }
    void f64(double v) { std::uint64_t u; std::memcpy(&u, &v, 8); for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xffu)); }
};

std::uint64_t fnv1a(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

struct Reader {
    const std::vector<std::uint8_t>& b;
    std::size_t at = 0;
    bool bad = false;
    std::uint32_t u32() {
        if (at + 4 > b.size()) { bad = true; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 4;
        return v;
    }
    double f64() {
        if (at + 8 > b.size()) { bad = true; return 0.0; }
        std::uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= static_cast<std::uint64_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 8;
        double v;
        std::memcpy(&v, &u, 8);
        return v;
    }
};

} // namespace

std::vector<std::uint8_t> serialize_damage(const std::vector<DamageRecord>& records) {
    Writer w;
    w.bytes.insert(w.bytes.end(), kMagic, kMagic + 4);
    w.u32(kDamageFormatVersion);
    w.u32(static_cast<std::uint32_t>(records.size()));
    for (const DamageRecord& r : records) {
        w.u32(r.asset_id);
        w.u32(r.instance_id);
        w.u32(static_cast<std::uint32_t>(r.link_broken.size()));
        w.u32(static_cast<std::uint32_t>(r.piece_pose.size() / 7));
        for (const std::uint8_t b : r.link_broken) w.bytes.push_back(b ? 1 : 0);
        for (const double v : r.piece_pose) w.f64(v);
    }
    const std::uint64_t sum = fnv1a(w.bytes.data(), w.bytes.size());
    w.u32(static_cast<std::uint32_t>(sum & 0xffffffffu));
    w.u32(static_cast<std::uint32_t>(sum >> 32));
    return w.bytes;
}

std::optional<std::vector<DamageRecord>> deserialize_damage(const std::vector<std::uint8_t>& bytes, std::string& error) {
    if (bytes.size() < 4 + 4 + 4 + 8 || std::memcmp(bytes.data(), kMagic, 4) != 0) {
        error = "not a damage file (bad magic)";
        return std::nullopt;
    }
    const std::uint64_t stored = static_cast<std::uint64_t>(bytes[bytes.size() - 8]) | (static_cast<std::uint64_t>(bytes[bytes.size() - 7]) << 8) | (static_cast<std::uint64_t>(bytes[bytes.size() - 6]) << 16) |
                                 (static_cast<std::uint64_t>(bytes[bytes.size() - 5]) << 24) | (static_cast<std::uint64_t>(bytes[bytes.size() - 4]) << 32) | (static_cast<std::uint64_t>(bytes[bytes.size() - 3]) << 40) |
                                 (static_cast<std::uint64_t>(bytes[bytes.size() - 2]) << 48) | (static_cast<std::uint64_t>(bytes[bytes.size() - 1]) << 56);
    if (stored != fnv1a(bytes.data(), bytes.size() - 8)) {
        error = "the damage file is corrupted (checksum mismatch)";
        return std::nullopt;
    }
    Reader r{bytes};
    r.at = 4;
    const std::uint32_t version = r.u32();
    if (version == 0 || version > kDamageFormatVersion) {
        error = "the damage file is version " + std::to_string(version) + ", newer than this build reads (" + std::to_string(kDamageFormatVersion) + ")";
        return std::nullopt;
    }
    const std::uint32_t count = r.u32();
    if (count > 4096) {
        error = "too many damage records";
        return std::nullopt;
    }
    std::vector<DamageRecord> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        DamageRecord d;
        d.asset_id = r.u32();
        d.instance_id = r.u32();
        const std::uint32_t links = r.u32(), pieces = r.u32();
        if (r.bad || links > 4096 || pieces > kMaxFracturePieces || r.at + links + 56ull * pieces > bytes.size()) {
            error = "a damage record is truncated or oversized";
            return std::nullopt;
        }
        for (std::uint32_t k = 0; k < links; ++k) d.link_broken.push_back(bytes[r.at++] ? 1 : 0);
        for (std::uint32_t k = 0; k < 7 * pieces; ++k) d.piece_pose.push_back(r.f64());
        out.push_back(std::move(d));
    }
    if (r.bad || r.at + 8 != bytes.size()) {
        error = "the damage file has trailing or missing bytes";
        return std::nullopt;
    }
    return out;
}

// ---- the object ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

Breakable::Breakable(DynamicsWorld& world, const FractureAsset& asset, std::uint32_t instance_id, DVec position, DQuat rotation, const DamageRecord* restore) : world_(world), asset_(asset), instance_(instance_id) {
    validate(asset);
    if (!fin(position) || !fin(rotation.x) || !fin(rotation.y) || !fin(rotation.z) || !fin(rotation.w)) throw std::invalid_argument("Breakable: the placement must be finite");
    if (restore != nullptr && (restore->link_broken.size() != asset.links.size() || restore->piece_pose.size() != 7 * asset.pieces.size() || restore->asset_id != asset.id)) {
        throw std::invalid_argument("Breakable: the damage record does not fit the asset");
    }
    try {
        const std::uint32_t group = 1000u + instance_id;  // the pieces of one object never collide with each other
        for (std::size_t i = 0; i < asset.pieces.size(); ++i) {
            const FracturePiece& p = asset.pieces[i];
            DBodyDesc d;
            d.shape = p.shape;
            d.half_extents = p.half_extents;
            d.mass = p.mass;
            d.surface = asset.surface;
            d.self_collision_group = group;
            if (restore != nullptr) {
                const double* q = &restore->piece_pose[7 * i];
                d.position = {q[0], q[1], q[2]};
                d.rotation = {q[3], q[4], q[5], q[6]};
            } else {
                const DVec o = rotate(rotation, p.offset);
                d.position = {position.x + o.x, position.y + o.y, position.z + o.z};
                d.rotation = mul(rotation, p.rotation);
            }
            bodies_.push_back(world.add_body(d));
        }
        for (std::size_t i = 0; i < asset.links.size(); ++i) {
            LinkState s;
            s.link = asset.links[i];
            if (restore != nullptr && restore->link_broken[i]) {
                s.broken = true;
            } else {
                DConstraintDesc c;
                c.kind = DConstraintKind::Fixed;
                c.a = bodies_[s.link.a];
                if (s.link.b != kAnchoredToWorld) c.b = bodies_[s.link.b];
                s.constraint = world.add_constraint(c);
            }
            links_.push_back(s);
        }
    } catch (...) {
        remove_from_world();
        throw;
    }
}

Breakable::~Breakable() { remove_from_world(); }

void Breakable::remove_from_world() {
    if (removed_) return;
    removed_ = true;
    for (LinkState& l : links_) {
        if (!l.broken && l.constraint.valid()) world_.remove_constraint(l.constraint);
        l.broken = true;
    }
    for (const BodyId b : bodies_) world_.remove_body(b);
    bodies_.clear();
}

void Breakable::update(std::vector<BreakEvent>& events) {
    if (removed_) return;
    for (std::size_t i = 0; i < links_.size(); ++i) {
        LinkState& l = links_[i];
        if (l.broken) continue;
        const DConstraintLoad load = world_.constraint_load(l.constraint);
        const bool over = load.force > l.link.break_force || load.torque > l.link.break_torque;
        l.over = over ? l.over + 1 : 0;
        if (l.over < asset_.confirm_steps) continue;
        BreakEvent e;
        e.instance = instance_;
        e.link = static_cast<std::uint32_t>(i);
        e.piece_a = l.link.a;
        e.piece_b = l.link.b;
        e.material = asset_.material;
        e.surface = asset_.surface;
        e.force = load.force;
        e.torque = load.torque;
        e.severity = std::max(1.0, std::max(load.force / l.link.break_force, load.torque / l.link.break_torque));
        const DBodyState sa = world_.state(bodies_[l.link.a]);
        e.piece_mass = asset_.pieces[l.link.a].mass;
        e.point = sa.position;
        if (l.link.b != kAnchoredToWorld) {
            const DBodyState sb = world_.state(bodies_[l.link.b]);
            e.point = {0.5 * (sa.position.x + sb.position.x), 0.5 * (sa.position.y + sb.position.y), 0.5 * (sa.position.z + sb.position.z)};
            e.piece_mass = std::min(e.piece_mass, asset_.pieces[l.link.b].mass);
        }
        world_.remove_constraint(l.constraint);
        l.broken = true;
        events.push_back(e);
    }
}

std::size_t Breakable::broken_count() const {
    std::size_t n = 0;
    for (const LinkState& l : links_) n += l.broken ? 1u : 0u;
    return n;
}

bool Breakable::link_broken(std::size_t link) const { return links_.at(link).broken; }

DConstraintLoad Breakable::link_load(std::size_t link) const {
    const LinkState& l = links_.at(link);
    return l.broken ? DConstraintLoad{} : world_.constraint_load(l.constraint);
}

DamageRecord Breakable::damage() const {
    DamageRecord d;
    d.asset_id = asset_.id;
    d.instance_id = instance_;
    for (const LinkState& l : links_) d.link_broken.push_back(l.broken ? 1 : 0);
    for (const BodyId b : bodies_) {
        const DBodyState s = world_.state(b);
        d.piece_pose.insert(d.piece_pose.end(), {s.position.x, s.position.y, s.position.z, s.rotation.x, s.rotation.y, s.rotation.z, s.rotation.w});
    }
    return d;
}

} // namespace saige
