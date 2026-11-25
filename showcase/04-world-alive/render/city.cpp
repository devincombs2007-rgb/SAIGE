#include "city.hpp"

#include "district_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace saige::alive::rend {

namespace {

constexpr float kWallT = 0.35f;

float hashf(std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0) {
    std::uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu ^ (c + 0x165667b1u) * 0xc2b2ae35u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffff) / 16777216.0f;
}


// A quad given in any winding, emitted so that it faces `want`.
void quad_facing(MeshBuilder& mb, Mat m, Float3 a, Float3 b, Float3 c, Float3 d, Float3 want) {
    const Float3 e1 = b - a, e2 = c - a;
    const Float3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    if (n.x * want.x + n.y * want.y + n.z * want.z >= 0) mb.quad(m, a, b, c, d);
    else mb.quad(m, a, d, c, b);
}

// ---- facade -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Face {
    Float3 origin;   // at the face's left corner seen from outside, on the ground
    Float3 u;        // unit vector along the face (left to right)
    Float3 out;      // outward unit normal
    float W = 0;
    [[nodiscard]] Float3 at(float uu, float v, float depth = 0.0f) const { return {origin.x + u.x * uu - out.x * depth, origin.y + u.y * uu - out.y * depth, origin.z + v}; }
};

struct Opening {
    float u0, u1, v0, v1;
    Mat glass;
    bool frame = true;
    bool sill = true;
    float depth = 0.20f;
    bool pane = true;   // a glass pane (false: an open doorway or an opening onto an interior)
};

void emit_face(MeshBuilder& mb, Mat wall, Mat reveal, Mat frame_mat, const Face& f, float H, std::vector<Opening> ops, bool reveals = true) {
    // Horizontal bands from every opening's v range; in each band, wall quads fill what the openings leave.
    std::vector<float> vs{0.0f, H};
    for (const Opening& o : ops) {
        vs.push_back(std::clamp(o.v0, 0.0f, H));
        vs.push_back(std::clamp(o.v1, 0.0f, H));
    }
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end(), [](float a, float b) { return b - a < 1e-4f; }), vs.end());
    for (std::size_t i = 0; i + 1 < vs.size(); ++i) {
        const float va = vs[i], vb = vs[i + 1];
        std::vector<std::pair<float, float>> holes;
        for (const Opening& o : ops)
            if (o.v0 <= va + 1e-4f && o.v1 >= vb - 1e-4f) holes.push_back({o.u0, o.u1});
        std::sort(holes.begin(), holes.end());
        float cur = 0.0f;
        for (const auto& h : holes) {
            if (h.first > cur + 1e-4f) mb.quad_uv(wall, f.at(cur, va), f.at(h.first, va), f.at(h.first, vb), f.at(cur, vb), {{{cur, va}, {h.first, va}, {h.first, vb}, {cur, vb}}});
            cur = std::max(cur, h.second);
        }
        if (cur < f.W - 1e-4f) mb.quad_uv(wall, f.at(cur, va), f.at(f.W, va), f.at(f.W, vb), f.at(cur, vb), {{{cur, va}, {f.W, va}, {f.W, vb}, {cur, vb}}});
    }
    if (!reveals) return;
    for (const Opening& o : ops) {
        const float d = o.depth;
        const Float3 left_n = f.u, right_n = f.u * -1.0f;
        // reveals
        quad_facing(mb, reveal, f.at(o.u0, o.v0), f.at(o.u0, o.v1), f.at(o.u0, o.v1, d), f.at(o.u0, o.v0, d), left_n);
        quad_facing(mb, reveal, f.at(o.u1, o.v0), f.at(o.u1, o.v1), f.at(o.u1, o.v1, d), f.at(o.u1, o.v0, d), right_n);
        if (o.v1 < H - 1e-3f) quad_facing(mb, reveal, f.at(o.u0, o.v1), f.at(o.u1, o.v1), f.at(o.u1, o.v1, d), f.at(o.u0, o.v1, d), {0, 0, -1});
        if (o.v0 > 1e-3f) quad_facing(mb, reveal, f.at(o.u0, o.v0), f.at(o.u1, o.v0), f.at(o.u1, o.v0, d), f.at(o.u0, o.v0, d), {0, 0, 1});
        if (o.pane) quad_facing(mb, o.glass, f.at(o.u0, o.v0, d), f.at(o.u1, o.v0, d), f.at(o.u1, o.v1, d), f.at(o.u0, o.v1, d), f.out);
        if (o.frame && o.pane) {
            // A frame: four strips just in front of the wall, and a window sill.
            const float t = 0.07f, e = 0.025f;
            const auto strip = [&](float a0, float b0, float a1, float b1) { quad_facing(mb, frame_mat, f.at(a0, b0, -e), f.at(a1, b0, -e), f.at(a1, b1, -e), f.at(a0, b1, -e), f.out); };
            strip(o.u0 - t, o.v0 - t, o.u1 + t, o.v0);
            strip(o.u0 - t, o.v1, o.u1 + t, o.v1 + t);
            strip(o.u0 - t, o.v0, o.u0, o.v1);
            strip(o.u1, o.v0, o.u1 + t, o.v1);
            // mullion
            if (o.u1 - o.u0 > 1.0f) strip(0.5f * (o.u0 + o.u1) - 0.025f, o.v0, 0.5f * (o.u0 + o.u1) + 0.025f, o.v1);
            if (o.sill && o.v0 > 0.4f) {
                const Float3 a = f.at(o.u0 - 0.1f, o.v0 - 0.07f, -0.14f), b = f.at(o.u1 + 0.1f, o.v0, 0.0f);
                mb.box(MFrameWhite, {std::min(a.x, b.x), std::min(a.y, b.y), a.z}, {std::max(a.x, b.x), std::max(a.y, b.y), b.z}, 63);
            }
        }
    }
}

// The inside of a wall whose outside is `f`: the wall quads around the same openings, facing into the building.
void emit_inner(MeshBuilder& mb, Mat wall, const Face& f, float H, const std::vector<Opening>& ops) {
    Face g;
    g.u = {-f.u.x, -f.u.y, -f.u.z};
    g.out = {-f.out.x, -f.out.y, -f.out.z};
    g.W = f.W;
    g.origin = {f.origin.x + f.u.x * f.W - f.out.x * kWallT, f.origin.y + f.u.y * f.W - f.out.y * kWallT, 0};
    std::vector<Opening> mirrored;
    for (const Opening& o : ops) {
        Opening m = o;
        m.u0 = f.W - o.u1;
        m.u1 = f.W - o.u0;
        mirrored.push_back(m);
    }
    emit_face(mb, wall, wall, wall, g, H, mirrored, false);
}

Face make_face(const Rect& r, Leg leg) {
    const auto d = leg_dir(leg);
    Face f;
    f.out = {static_cast<float>(d[0]), static_cast<float>(d[1]), 0};
    f.u = {static_cast<float>(-d[1]), static_cast<float>(d[0]), 0};
    switch (leg) {
    case Leg::South: f.origin = {static_cast<float>(r.x0), static_cast<float>(r.y0), 0}; f.W = static_cast<float>(r.w()); break;
    case Leg::East: f.origin = {static_cast<float>(r.x1), static_cast<float>(r.y0), 0}; f.W = static_cast<float>(r.h()); break;
    case Leg::North: f.origin = {static_cast<float>(r.x1), static_cast<float>(r.y1), 0}; f.W = static_cast<float>(r.w()); break;
    case Leg::West: f.origin = {static_cast<float>(r.x0), static_cast<float>(r.y1), 0}; f.W = static_cast<float>(r.h()); break;
    }
    return f;
}

float cafe_height(const Building& b, const Interior* in) {
    return in != nullptr && in->has_stairs ? static_cast<float>(b.height()) - 0.25f : static_cast<float>(b.floor_height) - 0.25f;
}

struct Style {
    Mat wall, upper_wall, roof;
    bool gable = false;
    bool ribbon = false;      // office glazing
    bool curtain = false;     // fully glazed
    Mat awning = MAwningRed;
    Mat door = MDoorRed;
};

Style style_for(const Building& b) {
    Style s;
    const std::uint32_t h = static_cast<std::uint32_t>(b.id * 2654435761u);
    const Mat bricks[4] = {MBrickA, MBrickB, MBrickC, MBrickPattern};
    const Mat plasters[4] = {MPlasterA, MPlasterB, MPlasterC, MStucco};
    const Mat awn[4] = {MAwningRed, MAwningGreen, MAwningBlue, MAwningYellow};
    const Mat doors[4] = {MDoorRed, MDoorGreen, MDoorBlue, MDoorWood};
    s.awning = awn[(h >> 4) & 3];
    s.door = doors[(h >> 7) & 3];
    switch (b.kind) {
    case BuildingKind::Apartments:
        s.wall = ((h >> 3) & 1) ? bricks[(h >> 8) & 3] : plasters[(h >> 8) & 3];
        s.upper_wall = s.wall;
        s.roof = ((h >> 12) & 1) ? MRoofSlate : MRoofClay;
        s.gable = ((h >> 14) % 3) == 0;
        break;
    case BuildingKind::Offices:
        s.wall = ((h >> 3) & 1) ? MCladdingA : MConcreteTile;
        s.upper_wall = s.wall;
        s.roof = MRoofGrey;
        s.ribbon = true;
        s.curtain = ((h >> 6) & 3) == 0;
        break;
    case BuildingKind::Hotel:
        s.wall = plasters[(h >> 8) & 3];
        s.upper_wall = s.wall;
        s.roof = MRoofSlate;
        break;
    case BuildingKind::Warehouse:
        s.wall = MFactory;
        s.upper_wall = MFactory;
        s.roof = MRoofMetal;
        break;
    case BuildingKind::FireStation:
        s.wall = MBrickA;
        s.upper_wall = MBrickA;
        s.roof = MRoofGrey;
        break;
    default:   // commercial: brick or painted plaster above a shopfront
        s.wall = bricks[(h >> 8) & 3];
        s.upper_wall = ((h >> 3) & 1) ? s.wall : plasters[(h >> 9) & 3];
        s.roof = MRoofGrey;
        break;
    }
    return s;
}

[[nodiscard]] bool inside_other(const District& d, const Building& self, double x, double y) {
    for (const Building& o : d.buildings) {
        if (o.id == self.id) continue;
        if (o.footprint.contains(x, y)) return true;
    }
    return false;
}

} // namespace

struct CityScene::Impl {
    render::SceneRenderer& r;
    const Materials& mats;
    const District& d;
    fs::path cache;
    std::vector<std::string>& warnings;
    std::vector<Placed> statics;
    // signal heads
    render::GpuModel head[4];     // off, red, amber, green
    render::GpuModel ped_head[2]; // stop, walk
    struct HeadPlace {
        std::uint32_t intersection;
        int leg;
        Mat4 world;
    };
    std::vector<HeadPlace> heads;
    struct PedHeadPlace {
        std::uint32_t crosswalk;
        Mat4 world;
    };
    std::vector<PedHeadPlace> ped_heads;
    std::size_t triangles = 0;

    // prop models
    struct PropModel {
        render::GpuModel model;
        float scale = 1.0f;
        float yaw_offset = 0.0f;
        bool ok = false;
        Float3 lo{}, hi{};
    };
    std::map<std::string, PropModel> props;

    Impl(render::SceneRenderer& renderer, const Materials& m, const District& dist, const fs::path& c, std::vector<std::string>& w) : r(renderer), mats(m), d(dist), cache(c), warnings(w) {}

    void add_static(const std::string& name, MeshBuilder& mb, float cx, float cy, float radius) {
        triangles += mb.triangle_count();
        Placed p;
        p.model = upload_model(r, name, mb.finish(), mats.list());
        p.cx = cx;
        p.cy = cy;
        p.radius = radius;
        statics.push_back(std::move(p));
    }

    // ---- prop models -------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void load_prop_model(const char* type, const char* folder, float target_height, float yaw_offset = 0.0f, float target_length = 0.0f) {
        PropModel pm;
        const fs::path file = cache / folder / "1k" / (std::string(folder) + "_1k.gltf");
        try {
            pm.model = load_prop(r, file, warnings, &pm.lo, &pm.hi);
            const float h = pm.hi.z - pm.lo.z;
            pm.scale = h > 1e-3f ? target_height / h : 1.0f;
            if (target_length > 0.0f) {
                const float l = std::max(pm.hi.x - pm.lo.x, pm.hi.y - pm.lo.y);
                if (l > 1e-3f) pm.scale = target_length / l;
            }
            pm.yaw_offset = yaw_offset;
            pm.ok = true;
        } catch (const std::exception& e) {
            warnings.push_back(std::string(type) + ": " + e.what());
        }
        props[type] = std::move(pm);
    }

    void load_props() {
        load_prop_model("lamp", "street_lamp_01", 5.2f);
        load_prop_model("bench", "painted_wooden_bench", 0.9f);
        load_prop_model("bin", "metal_trash_can", 0.95f);
        load_prop_model("hydrant", "fire_hydrant", 0.8f);
        load_prop_model("table_round", "round_wooden_table_01", 0.76f);
        load_prop_model("table_square", "wooden_table_02", 0.76f);
        load_prop_model("chair", "plastic_monobloc_chair_01", 0.86f);
        load_prop_model("cafe_chair", "plastic_monobloc_chair_01", 0.86f);
        load_prop_model("plant", "potted_plant_02", 1.25f);
        load_prop_model("sofa", "sofa_02", 0.85f);
        load_prop_model("crate", "wooden_crate_01", 0.62f);
        load_prop_model("planter", "planter_box_01", 0.7f);
    }

    // ---- ground ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void build_ground() {
        MeshBuilder mb;
        const float ext = 300.0f;
        // Land base (courtyards, lots): paving; everything else is laid over it.
        mb.quad(MPaving, {-ext, -ext, 0}, {ext, -ext, 0}, {ext, static_cast<float>(d.quay_y), 0}, {-ext, static_cast<float>(d.quay_y), 0});
        // Beyond the district: a coarse skirt out to the horizon, so that nothing below the skyline shows the sky through.
        mb.quad(MPaving, {-300000, -300000, -0.05f}, {300000, -300000, -0.05f}, {300000, static_cast<float>(d.quay_y), -0.05f}, {-300000, static_cast<float>(d.quay_y), -0.05f});
        for (const Rect& rr : d.roadway) mb.quad(MAsphalt, {static_cast<float>(rr.x0), static_cast<float>(rr.y0), 0.004f}, {static_cast<float>(rr.x1), static_cast<float>(rr.y0), 0.004f}, {static_cast<float>(rr.x1), static_cast<float>(rr.y1), 0.004f}, {static_cast<float>(rr.x0), static_cast<float>(rr.y1), 0.004f});
        // Sidewalks and kerbs
        for (const Rect& rr : d.sidewalk) {
            const float x0 = static_cast<float>(rr.x0), x1 = static_cast<float>(rr.x1), y0 = static_cast<float>(rr.y0), y1 = static_cast<float>(std::min(rr.y1, d.quay_y));
            const bool prom = rr.y0 >= d.promenade.y0 - 1e-6 && rr.y1 <= d.promenade.y1 + 1e-6 && rr.x1 - rr.x0 > 100.0;
            mb.quad(prom ? MPromenade : MSidewalk, {x0, y0, 0.15f}, {x1, y0, 0.15f}, {x1, y1, 0.15f}, {x0, y1, 0.15f});
            mb.quad(MCurb, {x0, y0, 0.0f}, {x1, y0, 0.0f}, {x1, y0, 0.15f}, {x0, y0, 0.15f});
            mb.quad(MCurb, {x1, y1, 0.0f}, {x0, y1, 0.0f}, {x0, y1, 0.15f}, {x1, y1, 0.15f});
            mb.quad(MCurb, {x0, y1, 0.0f}, {x0, y0, 0.0f}, {x0, y0, 0.15f}, {x0, y1, 0.15f});
            mb.quad(MCurb, {x1, y0, 0.0f}, {x1, y1, 0.0f}, {x1, y1, 0.15f}, {x1, y0, 0.15f});
        }
        // The bulkhead of the quay, down to the sea bed.
        {
            const float q = static_cast<float>(d.quay_y);
            mb.quad(MCurb, {ext, q, -8.0f}, {-ext, q, -8.0f}, {-ext, q, 0.15f}, {ext, q, 0.15f});   // faces the water
            // The sea bed.
            mb.quad(MPaving, {-ext, q, -8.0f}, {-ext, q + 400.0f, -8.0f}, {ext, q + 400.0f, -8.0f}, {ext, q, -8.0f});
        }
        // Park
        for (const Rect& p : d.park) {
            mb.quad(MGrass, {static_cast<float>(p.x0), static_cast<float>(p.y0), 0.03f}, {static_cast<float>(p.x1), static_cast<float>(p.y0), 0.03f}, {static_cast<float>(p.x1), static_cast<float>(p.y1), 0.03f}, {static_cast<float>(p.x0), static_cast<float>(p.y1), 0.03f});
        }
        // Piers
        for (const Pier& p : d.piers) {
            const float x0 = static_cast<float>(p.deck.x0), x1 = static_cast<float>(p.deck.x1), y0 = static_cast<float>(p.deck.y0), y1 = static_cast<float>(p.deck.y1), z = static_cast<float>(p.z);
            mb.quad(MPier, {x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z});
            mb.quad(MPier, {x0, y0, z - 0.4f}, {x0, y1, z - 0.4f}, {x0, y1, z}, {x0, y0, z});
            mb.quad(MPier, {x1, y1, z - 0.4f}, {x1, y0, z - 0.4f}, {x1, y0, z}, {x1, y1, z});
            mb.quad(MPier, {x1, y1, z - 0.4f}, {x1, y1, z}, {x0, y1, z}, {x0, y1, z - 0.4f});
            for (float y = y0 + 1.0f; y < y1; y += 4.0f) {
                mb.cylinder(MBark, {x0 + 0.3f, y, -3.0f}, 0.22f, 3.0f + z - 0.0f, 8, false);
                mb.cylinder(MBark, {x1 - 0.3f, y, -3.0f}, 0.22f, 3.0f + z - 0.0f, 8, false);
            }
            // planks
            for (float y = y0 + 0.8f; y < y1; y += 0.8f) mb.quad(MPaintBlack, {x0, y, z + 0.002f}, {x1, y, z + 0.002f}, {x1, y + 0.025f, z + 0.002f}, {x0, y + 0.025f, z + 0.002f});
        }
        build_markings(mb);
        add_static("ground", mb, 0, 0, 1e9f);
    }

    void stripe(MeshBuilder& mb, Mat m, double x0, double y0, double x1, double y1, float z = 0.012f) {
        mb.quad(m, {static_cast<float>(x0), static_cast<float>(y0), z}, {static_cast<float>(x1), static_cast<float>(y0), z}, {static_cast<float>(x1), static_cast<float>(y1), z}, {static_cast<float>(x0), static_cast<float>(y1), z});
    }

    void build_markings(MeshBuilder& mb) {
        const double av[3] = {-90.0, 0.0, 90.0}, st[3] = {-60.0, 0.0, 60.0};
        const double c0 = 10.5;   // markings stay clear of the intersections' stop lines and crosswalks
        // Along the streets (east-west): centre double yellow, edge lines at the parking bays.
        for (const double sy : st) {
            std::vector<double> cuts{-170.0};
            for (const double ax : av) {
                cuts.push_back(ax - c0);
                cuts.push_back(ax + c0);
            }
            cuts.push_back(170.0);
            for (std::size_t i = 0; i + 1 < cuts.size(); i += 2) {
                const double a = cuts[i], b = cuts[i + 1];
                stripe(mb, MMarkYellow, a, sy - 0.14, b, sy - 0.04);
                stripe(mb, MMarkYellow, a, sy + 0.04, b, sy + 0.14);
                for (const int sgn : {-1, 1}) stripe(mb, MMarkWhite, a, sy + sgn * 3.5 - 0.06, b, sy + sgn * 3.5 + 0.06);
            }
        }
        // Along the avenues (north-south).
        for (const double ax : av) {
            std::vector<double> cuts{-141.0};
            for (const double sy : st) {
                cuts.push_back(sy - c0);
                cuts.push_back(sy + c0);
            }
            cuts.push_back(66.0);
            // The last interval runs from the last intersection to the promenade: clip it.
            for (std::size_t i = 0; i + 1 < cuts.size(); i += 2) {
                const double a = cuts[i], b = std::min(cuts[i + 1], 65.0);
                if (b <= a) continue;
                stripe(mb, MMarkYellow, ax - 0.14, a, ax - 0.04, b);
                stripe(mb, MMarkYellow, ax + 0.04, a, ax + 0.14, b);
                for (const int sgn : {-1, 1}) stripe(mb, MMarkWhite, ax + sgn * 3.5 - 0.06, a, ax + sgn * 3.5 + 0.06, b);
            }
        }
        // Stop lines and zebra crossings at every leg of every intersection.
        for (const Intersection& I : d.intersections) {
            for (int li = 0; li < 4; ++li) {
                if (!I.has_leg[static_cast<std::size_t>(li)]) continue;
                const auto dir = leg_dir(static_cast<Leg>(li));
                const std::array<double, 2> rt{dir[1] * -1.0 * -1.0, 0.0};
                (void)rt;
                // Inbound lane on this leg: travelling towards the centre, on the right of travel.
                const std::array<double, 2> travel{-dir[0], -dir[1]};
                const std::array<double, 2> right{travel[1], -travel[0]};
                const double sx = I.x + dir[0] * stop_distance, sy2 = I.y + dir[1] * stop_distance;
                const double ax0 = sx + right[0] * 0.1, ay0 = sy2 + right[1] * 0.1, ax1 = sx + right[0] * 3.45, ay1 = sy2 + right[1] * 3.45;
                const double th = 0.22;
                stripe(mb, MMarkWhite, std::min(ax0, ax1) - std::abs(dir[0]) * th, std::min(ay0, ay1) - std::abs(dir[1]) * th, std::max(ax0, ax1) + std::abs(dir[0]) * th, std::max(ay0, ay1) + std::abs(dir[1]) * th);
            }
        }
        for (const Crosswalk& c : d.crosswalks) {
            const bool along_x = c.strip.w() > c.strip.h();   // the strip is long across the road
            if (along_x) {
                for (double x = c.strip.x0 + 0.25; x + 0.5 <= c.strip.x1 + 1e-6; x += 1.0) stripe(mb, MMarkWhite, x, c.strip.y0, x + 0.5, c.strip.y1);
            } else {
                for (double y = c.strip.y0 + 0.25; y + 0.5 <= c.strip.y1 + 1e-6; y += 1.0) stripe(mb, MMarkWhite, c.strip.x0, y, c.strip.x1, y + 0.5);
            }
        }
        // Parking bays: ticks between spots.
        for (const ParkingSpot& p : d.parking) {
            stripe(mb, MMarkWhite, p.x - 3.15, p.y - 1.0, p.x - 3.05, p.y + 1.0);
        }
    }

    // ---- buildings -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void build_building(const Building& b) {
        MeshBuilder mb;
        const Style st = style_for(b);
        const float H = static_cast<float>(b.height());
        const float fh = static_cast<float>(b.floor_height);
        const Rect& fp = b.footprint;
        const Interior* interior = nullptr;
        for (const Interior& in : d.interiors)
            if (in.building == b.id) interior = &in;
        const bool commercial = is_commercial(b.kind) && b.kind != BuildingKind::Offices;
        const std::uint32_t seed = b.id;
        for (int li = 0; li < 4; ++li) {
            const Leg face_leg = static_cast<Leg>(li);
            const Face f = make_face(fp, face_leg);
            // Hidden: pressed against a neighbour.
            const auto dir = leg_dir(face_leg);
            const double mx = fp.cx() + dir[0] * (fp.w() * 0.5 + 1.0) * (dir[0] != 0 ? 1.0 : 0.0), my = fp.cy() + dir[1] * (fp.h() * 0.5 + 1.0) * (dir[1] != 0 ? 1.0 : 0.0);
            const bool hidden = inside_other(d, b, mx, my);
            if (hidden) continue;
            const bool is_front = face_leg == b.front;
            std::vector<Opening> ops;
            // The door's u position along this face.
            float door_u = 0;
            if (is_front) {
                const double dxw = b.door_x, dyw = b.door_y;
                door_u = static_cast<float>((dxw - static_cast<double>(f.origin.x)) * static_cast<double>(f.u.x) + (dyw - static_cast<double>(f.origin.y)) * static_cast<double>(f.u.y));
            }
            const float bay = (b.kind == BuildingKind::Offices) ? 3.0f : 3.4f;
            const int bays = std::max(1, static_cast<int>(std::floor(f.W / bay)));
            const float bw = f.W / static_cast<float>(bays);
            for (int fl = 0; fl < b.floors; ++fl) {
                const float z0 = static_cast<float>(fl) * fh;
                for (int k = 0; k < bays; ++k) {
                    const float uc = (static_cast<float>(k) + 0.5f) * bw;
                    if (fl == 0 && is_front) {
                        if (commercial || b.kind == BuildingKind::Hotel || b.kind == BuildingKind::Offices || b.kind == BuildingKind::FireStation) continue;   // storefront handled below
                        if (std::fabs(uc - door_u) < 1.5f) continue;   // the door's bay
                    }
                    const bool lit = hashf(seed, static_cast<std::uint32_t>(fl * 31 + k), static_cast<std::uint32_t>(li)) < 0.45f;
                    const Mat glass = lit ? (hashf(seed, static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(fl)) < 0.3f ? MGlassLitCool : MGlassLit) : MGlassDark;
                    Opening o;
                    if (st.curtain) {
                        o = {uc - bw * 0.5f + 0.1f, uc + bw * 0.5f - 0.1f, z0 + 0.5f, z0 + fh - 0.45f, glass};
                    } else if (st.ribbon) {
                        o = {uc - bw * 0.5f + 0.18f, uc + bw * 0.5f - 0.18f, z0 + 0.85f, z0 + 2.45f, glass};
                    } else if (b.kind == BuildingKind::Warehouse) {
                        if (fl > 0 || k % 2) continue;
                        o = {uc - 0.7f, uc + 0.7f, z0 + 2.2f, z0 + 3.0f, glass};
                    } else {
                        const float ww = (b.kind == BuildingKind::Hotel ? 1.5f : 1.25f);
                        o = {uc - ww * 0.5f, uc + ww * 0.5f, z0 + (fl == 0 ? 1.1f : 0.95f), z0 + 2.3f, glass};
                    }
                    ops.push_back(o);
                }
            }
            if (is_front) {
                if (commercial || b.kind == BuildingKind::Hotel) {
                    // Shopfront: a wide window either side of the door, a transom above.
                    const float dw = 1.9f;
                    const Mat sglass = interior != nullptr ? MGlassDark : MGlassLit;
                    Opening left{0.5f, std::max(0.6f, door_u - dw * 0.5f - 0.45f), 0.55f, 2.7f, sglass};
                    Opening right{std::min(f.W - 0.6f, door_u + dw * 0.5f + 0.45f), f.W - 0.5f, 0.55f, 2.7f, sglass};
                    left.pane = right.pane = interior == nullptr;
                    left.depth = right.depth = interior != nullptr ? kWallT : 0.25f;
                    if (left.u1 - left.u0 > 0.8f) ops.push_back(left);
                    if (right.u1 - right.u0 > 0.8f) ops.push_back(right);
                    // Door.
                    Opening door{door_u - dw * 0.5f, door_u + dw * 0.5f, 0.0f, 2.65f, st.door};
                    door.frame = false;
                    door.sill = false;
                    door.pane = interior == nullptr;
                    door.depth = interior != nullptr ? kWallT : 0.3f;
                    ops.push_back(door);
                } else if (b.kind == BuildingKind::Offices) {
                    Opening lobby{std::max(0.5f, door_u - 1.9f), std::min(f.W - 0.5f, door_u + 1.9f), 0.0f, 2.9f, MGlassDark};
                    lobby.sill = false;
                    ops.push_back(lobby);
                } else if (b.kind == BuildingKind::FireStation) {
                    // Two engine doors.
                    for (int k = 0; k < 2; ++k) {
                        const float uc = f.W * (0.28f + 0.44f * static_cast<float>(k));
                        Opening g{uc - 1.9f, uc + 1.9f, 0.0f, 3.5f, MPaintDarkGrey};
                        g.sill = false;
                        ops.push_back(g);
                    }
                } else {
                    Opening door{door_u - 0.55f, door_u + 0.55f, 0.0f, 2.3f, st.door};
                    door.frame = false;
                    door.sill = false;
                    ops.push_back(door);
                }
            }
            if (interior != nullptr && !is_front) ops.clear();
            emit_face(mb, face_leg == b.front && commercial ? st.upper_wall : st.wall, MPlasterB, MFrameWhite, f, H, ops);
            if (interior != nullptr && is_front) emit_inner(mb, MInterior, f, cafe_height(b, interior), ops);
            // Ground-floor band of the front: a plinth.
            {   // a plinth, a little proud of the wall
                const Float3 a = f.at(0.0f, 0.0f, -0.05f), c = f.at(f.W, 0.45f, 0.0f);
                mb.box(MCurb, {std::min(a.x, c.x) - 0.0f, std::min(a.y, c.y) - 0.0f, 0.0f}, {std::max(a.x, c.x) + 0.0f, std::max(a.y, c.y) + 0.0f, 0.45f}, 0);
            }
            // Cornice
            {
                const float cw = 0.22f;
                const Float3 a = f.at(-cw, H - 0.45f, -cw), c = f.at(f.W + cw, H, 0.0f);
                mb.box(MStucco, {std::min(a.x, c.x), std::min(a.y, c.y), H - 0.45f}, {std::max(a.x, c.x), std::max(a.y, c.y), H + 0.05f}, 63);
            }
            // Storefront awning
            if (is_front && commercial) {
                const float wd = 1.55f;
                const float z0 = 3.0f;
                const float u0 = 0.3f, u1 = f.W - 0.3f;
                quad_facing(mb, st.awning, f.at(u0, z0), f.at(u1, z0), f.at(u1, z0 - 0.5f, -wd), f.at(u0, z0 - 0.5f, -wd), {f.out.x, f.out.y, 1.0f});
                quad_facing(mb, st.awning, f.at(u0, z0 - 0.5f, -wd), f.at(u1, z0 - 0.5f, -wd), f.at(u1, z0 - 0.78f, -wd), f.at(u0, z0 - 0.78f, -wd), f.out);
                quad_facing(mb, st.awning, f.at(u0, z0), f.at(u0, z0 - 0.5f, -wd), f.at(u0, z0 - 0.78f, -wd), f.at(u0, z0 - 0.78f, 0.0f), f.u * -1.0f);
                quad_facing(mb, st.awning, f.at(u1, z0), f.at(u1, z0 - 0.5f, -wd), f.at(u1, z0 - 0.78f, -wd), f.at(u1, z0 - 0.78f, 0.0f), f.u);
                // Sign board above the awning.
                const float su = std::clamp(door_u, 2.0f, f.W - 2.0f);
                const Float3 a = f.at(su - 1.6f, 3.35f, -0.12f), c = f.at(su + 1.6f, 3.95f, 0.0f);
                mb.box(MPaintDarkGrey, {std::min(a.x, c.x), std::min(a.y, c.y), 3.35f}, {std::max(a.x, c.x), std::max(a.y, c.y), 3.95f}, 63);
                const Float3 e = f.at(su - 1.45f, 3.42f, -0.125f), g = f.at(su + 1.45f, 3.88f, -0.125f);
                quad_facing(mb, hashf(seed, 7) < 0.5f ? MBulbWarm : MGlassLitCool, e, f.at(su + 1.45f, 3.42f, -0.125f), g, f.at(su - 1.45f, 3.88f, -0.125f), f.out);
            }
            // Balconies on apartments' fronts and sides facing the street.
            if (b.kind == BuildingKind::Apartments && b.floors >= 3 && (is_front || hashf(seed, 99, static_cast<std::uint32_t>(li)) < 0.3f)) {
                for (int fl = 1; fl < b.floors; ++fl)
                    for (int k = 0; k < bays; ++k) {
                        if ((k + fl) % 2) continue;
                        const float uc = (static_cast<float>(k) + 0.5f) * bw;
                        const float z = static_cast<float>(fl) * fh;
                        const Float3 a = f.at(uc - 0.9f, z, -1.15f), c = f.at(uc + 0.9f, z + 0.12f, 0.0f);
                        mb.box(MConcreteTile, {std::min(a.x, c.x), std::min(a.y, c.y), z}, {std::max(a.x, c.x), std::max(a.y, c.y), z + 0.12f}, 63);
                        // railing: front and sides as thin boxes
                        const Float3 r0 = f.at(uc - 0.9f, z + 0.12f, -1.15f), r1 = f.at(uc + 0.9f, z + 1.05f, -1.10f);
                        mb.box(MFrameDark, {std::min(r0.x, r1.x), std::min(r0.y, r1.y), z + 0.12f}, {std::max(r0.x, r1.x), std::max(r0.y, r1.y), z + 1.05f}, 0);
                        for (int rr = 0; rr < 3; ++rr) {
                            const float up = uc - 0.9f + 0.9f * static_cast<float>(rr);
                            const Float3 p0 = f.at(up, z + 0.12f, -1.15f), p1 = f.at(up + 0.04f, z + 1.05f, -1.11f);
                            mb.box(MFrameDark, {std::min(p0.x, p1.x), std::min(p0.y, p1.y), z + 0.12f}, {std::max(p0.x, p1.x), std::max(p0.y, p1.y), z + 1.05f}, 63);
                        }
                        const Float3 t0 = f.at(uc - 0.92f, z + 1.0f, -1.17f), t1 = f.at(uc + 0.92f, z + 1.06f, -1.08f);
                        mb.box(MFrameDark, {std::min(t0.x, t1.x), std::min(t0.y, t1.y), z + 1.0f}, {std::max(t0.x, t1.x), std::max(t0.y, t1.y), z + 1.06f}, 63);
                    }
            }
        }
        build_roof(mb, b, st, H);
        if (interior != nullptr) build_interior(mb, b, *interior);
        const Rect& f2 = b.footprint;
        add_static("building " + std::to_string(b.id), mb, static_cast<float>(f2.cx()), static_cast<float>(f2.cy()), static_cast<float>(0.5 * std::hypot(f2.w(), f2.h()) + 4.0));
    }

    void build_roof(MeshBuilder& mb, const Building& b, const Style& st, float H) {
        const Rect& fp = b.footprint;
        const float x0 = static_cast<float>(fp.x0), x1 = static_cast<float>(fp.x1), y0 = static_cast<float>(fp.y0), y1 = static_cast<float>(fp.y1);
        if (st.gable && b.kind == BuildingKind::Apartments) {
            const bool ridge_x = fp.w() >= fp.h();
            const float rise = 2.3f;
            const float cxm = 0.5f * (x0 + x1), cym = 0.5f * (y0 + y1);
            const float ov = 0.35f;
            if (ridge_x) {
                quad_facing(mb, st.roof, {x0 - ov, y0 - ov, H - 0.1f}, {x1 + ov, y0 - ov, H - 0.1f}, {x1 + ov, cym, H + rise}, {x0 - ov, cym, H + rise}, {0, -0.5f, 1});
                quad_facing(mb, st.roof, {x0 - ov, y1 + ov, H - 0.1f}, {x1 + ov, y1 + ov, H - 0.1f}, {x1 + ov, cym, H + rise}, {x0 - ov, cym, H + rise}, {0, 0.5f, 1});
                mb.tri(MPlasterA, {x0, y0, H}, {x0, y1, H}, {x0, cym, H + rise});
                mb.tri(MPlasterA, {x1, y1, H}, {x1, y0, H}, {x1, cym, H + rise});
            } else {
                quad_facing(mb, st.roof, {x0 - ov, y0 - ov, H - 0.1f}, {x0 - ov, y1 + ov, H - 0.1f}, {cxm, y1 + ov, H + rise}, {cxm, y0 - ov, H + rise}, {-0.5f, 0, 1});
                quad_facing(mb, st.roof, {x1 + ov, y0 - ov, H - 0.1f}, {x1 + ov, y1 + ov, H - 0.1f}, {cxm, y1 + ov, H + rise}, {cxm, y0 - ov, H + rise}, {0.5f, 0, 1});
                mb.tri(MPlasterA, {x1, y0, H}, {x0, y0, H}, {cxm, y0, H + rise});
                mb.tri(MPlasterA, {x0, y1, H}, {x1, y1, H}, {cxm, y1, H + rise});
            }
            // chimney
            mb.box(MBrickA, {x0 + 1.2f, y0 + 1.2f, H}, {x0 + 1.9f, y0 + 1.9f, H + rise + 1.0f}, 63);
            return;
        }
        // Flat roof with a parapet.
        mb.quad(MRoofGrey, {x0, y0, H}, {x1, y0, H}, {x1, y1, H}, {x0, y1, H});
        const float pt = 0.25f, ph = 0.6f;
        mb.box(st.wall, {x0, y0, H}, {x1, y0 + pt, H + ph}, 63);
        mb.box(st.wall, {x0, y1 - pt, H}, {x1, y1, H + ph}, 63);
        mb.box(st.wall, {x0, y0 + pt, H}, {x0 + pt, y1 - pt, H + ph}, 63);
        mb.box(st.wall, {x1 - pt, y0 + pt, H}, {x1, y1 - pt, H + ph}, 63);
        const std::uint32_t s = b.id;
        // Rooftop equipment.
        const int n = 2 + static_cast<int>(hashf(s, 1) * 3.0f);
        for (int i = 0; i < n; ++i) {
            const float px = x0 + 1.5f + hashf(s, 10 + static_cast<std::uint32_t>(i)) * std::max(0.5f, static_cast<float>(fp.w()) - 3.5f);
            const float py = y0 + 1.5f + hashf(s, 40 + static_cast<std::uint32_t>(i)) * std::max(0.5f, static_cast<float>(fp.h()) - 3.5f);
            const float t = hashf(s, 70 + static_cast<std::uint32_t>(i));
            if (t < 0.45f) {
                mb.box(MPaintSteel, {px, py, H}, {px + 1.8f, py + 1.2f, H + 1.0f}, 63);
                mb.cylinder(MPaintDarkGrey, {px + 0.9f, py + 0.6f, H + 1.0f}, 0.45f, 0.08f, 10, true);
            } else if (t < 0.7f) {
                mb.cylinder(MPaintSteel, {px, py, H}, 0.25f, 1.6f, 8, true);
            } else if (t < 0.88f) {
                mb.box(st.wall, {px, py, H}, {px + 2.4f, py + 2.2f, H + 2.4f}, 63);
            } else {
                mb.cylinder(MPaintSteel, {px, py, H + 1.5f}, 0.04f, 2.5f, 6, false);
            }
        }
    }

    // ---- interiors ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void build_interior(MeshBuilder& mb, const Building& b, const Interior& in) {
        const float z0 = 0.15f;
        const float ceil_z = static_cast<float>(b.floor_height) - 0.25f;
        const Rect& room = in.room;
        const float x0 = static_cast<float>(room.x0), x1 = static_cast<float>(room.x1), y0 = static_cast<float>(room.y0), y1 = static_cast<float>(room.y1);
        const bool cafe = in.has_stairs;
        const Mat floor_mat = b.kind == BuildingKind::Bar ? MFloorWoodDark : (b.kind == BuildingKind::Pharmacy || b.kind == BuildingKind::Bakery) ? MFloorTile : MFloorWood;
        mb.quad(floor_mat, {x0, y0, z0 + 0.003f}, {x1, y0, z0 + 0.003f}, {x1, y1, z0 + 0.003f}, {x0, y1, z0 + 0.003f});
        // Inner faces of the walls.
        for (std::size_t wi = 2; wi < in.walls.size(); ++wi) {   // the first two pieces are the front wall: drawn from the facade
            const Rect& w = in.walls[wi];
            const float a0 = static_cast<float>(w.x0), b0 = static_cast<float>(w.y0), a1 = static_cast<float>(w.x1), b1 = static_cast<float>(w.y1);
            const float top = cafe ? static_cast<float>(b.height()) - 0.25f : ceil_z;
            mb.box(MInterior, {a0, b0, z0}, {a1, b1, top}, 63);
        }
        // Ceiling of the ground floor (with the stair well open in the cafe).
        if (!cafe) {
            mb.quad(MCeiling, {x0, y1, ceil_z}, {x1, y1, ceil_z}, {x1, y0, ceil_z}, {x0, y0, ceil_z});
            // A few ceiling lights.
            for (int i = 0; i < 3; ++i) {
                const float cx = x0 + (x1 - x0) * (0.2f + 0.3f * static_cast<float>(i)), cy = 0.5f * (y0 + y1);
                mb.quad(MBulbWarm, {cx - 0.5f, cy + 0.2f, ceil_z - 0.01f}, {cx + 0.5f, cy + 0.2f, ceil_z - 0.01f}, {cx + 0.5f, cy - 0.2f, ceil_z - 0.01f}, {cx - 0.5f, cy - 0.2f, ceil_z - 0.01f});
            }
        } else {
            const float uz = static_cast<float>(in.upper_floor_z);
            const Rect& w = in.stair_footprint;
            const float wx0 = static_cast<float>(w.x0), wx1 = static_cast<float>(w.x1), wy0 = static_cast<float>(w.y0), wy1 = static_cast<float>(w.y1);
            // Ground-floor ceiling = underside of the first-floor slab (with the stair well open).
            const auto slab = [&](float a0, float b0, float a1, float b1) {
                if (a1 - a0 < 1e-3f || b1 - b0 < 1e-3f) return;
                mb.quad(MCeiling, {a0, b1, uz - 0.25f}, {a1, b1, uz - 0.25f}, {a1, b0, uz - 0.25f}, {a0, b0, uz - 0.25f});
                mb.quad(floor_mat == MFloorTile ? MFloorWood : MFloorWood, {a0, b0, uz}, {a1, b0, uz}, {a1, b1, uz}, {a0, b1, uz});
            };
            slab(x0, y0, wx0, y1);
            slab(wx1, y0, x1, y1);
            slab(wx0, y0, wx1, wy0);
            slab(wx0, wy1, wx1, y1);
            // Stairs: 12 treads.
            const bool along_y = (w.h() > w.w());
            const float rise = (uz - 0.15f) / 12.0f;
            const auto& bot = in.stair_bottom;
            const auto& top = in.stair_top;
            const float dir_x = along_y ? 0.0f : (top[0] > bot[0] ? 1.0f : -1.0f);
            const float dir_y = along_y ? (top[1] > bot[1] ? 1.0f : -1.0f) : 0.0f;
            const float run = 0.3f;
            for (int i = 0; i < 12; ++i) {
                const float sa = static_cast<float>(i) * run - 0.15f, sb = sa + run;
                const float sx0 = along_y ? wx0 : (dir_x > 0 ? static_cast<float>(bot[0]) + sa : static_cast<float>(bot[0]) - sb);
                const float sx1 = along_y ? wx1 : (dir_x > 0 ? static_cast<float>(bot[0]) + sb : static_cast<float>(bot[0]) - sa);
                const float sy0 = along_y ? (dir_y > 0 ? static_cast<float>(bot[1]) + sa : static_cast<float>(bot[1]) - sb) : wy0;
                const float sy1 = along_y ? (dir_y > 0 ? static_cast<float>(bot[1]) + sb : static_cast<float>(bot[1]) - sa) : wy1;
                mb.box(MFloorWoodDark, {sx0, sy0, 0.15f}, {sx1, sy1, 0.15f + static_cast<float>(i + 1) * rise}, 63);
            }
            // Handrail along the open side of the well.
            // Upper room: ceiling under the roof slab.
            const float rz = static_cast<float>(b.height()) - 0.25f;
            mb.quad(MCeiling, {x0, y1, rz}, {x1, y1, rz}, {x1, y0, rz}, {x0, y0, rz});
            for (int i = 0; i < 4; ++i) {
                const float cx = x0 + (x1 - x0) * (0.15f + 0.22f * static_cast<float>(i)), cy = 0.5f * (y0 + y1);
                mb.quad(MBulbWarm, {cx - 0.5f, cy + 0.2f, rz - 0.01f}, {cx + 0.5f, cy + 0.2f, rz - 0.01f}, {cx + 0.5f, cy - 0.2f, rz - 0.01f}, {cx - 0.5f, cy - 0.2f, rz - 0.01f});
            }
        }
        // Furniture (procedural and Poly Haven), in the building's own model.
        const auto place = [&](const StreetProp& p) { build_furniture(mb, p); };
        for (const StreetProp& p : in.furniture) place(p);
        for (const StreetProp& p : in.upper_furniture) place(p);
    }

    // Furniture drawn procedurally into a mesh builder (the ones with no model), in world coordinates.
    void build_furniture(MeshBuilder& mb, const StreetProp& p) {
        const float x = static_cast<float>(p.x), y = static_cast<float>(p.y), z = static_cast<float>(p.z);
        const float yaw = static_cast<float>(p.yaw);
        const float s = static_cast<float>(p.scale);
        if (p.type == "counter") {
            mb.oriented_box(MPaintWhite, {x, y, 0}, yaw, {0.5f * s, 0.33f, 0.5f}, z);
            mb.oriented_box(MFloorWoodDark, {x, y, 0}, yaw, {0.5f * s + 0.05f, 0.38f, 0.03f}, z + 1.0f);
            mb.oriented_box(MFrameDark, {x, y, 0}, yaw, {0.5f * s - 0.1f, 0.30f, 0.05f}, z + 0.02f);
        } else if (p.type == "shelf") {
            const float L = 0.5f * s;
            mb.oriented_box(MFrameDark, {x, y, 0}, yaw, {L, 0.20f, 0.03f}, z);
            for (int sh = 0; sh < 5; ++sh) {
                mb.oriented_box(MFrameDark, {x, y, 0}, yaw, {L, 0.20f, 0.02f}, z + 0.1f + 0.4f * static_cast<float>(sh));
                // goods
                const int n = std::max(2, static_cast<int>(L * 2.0f / 0.28f));
                for (int g = 0; g < n; ++g) {
                    const float off = -L + (static_cast<float>(g) + 0.5f) * (2.0f * L / static_cast<float>(n));
                    const float c = std::cos(yaw), sn = std::sin(yaw);
                    const Mat gm = static_cast<Mat>(static_cast<std::uint32_t>(MAwningRed) + static_cast<std::uint32_t>(hashf(static_cast<std::uint32_t>(p.x * 10), static_cast<std::uint32_t>(sh), static_cast<std::uint32_t>(g)) * 4.0f));
                    mb.oriented_box(gm, {x + off * c, y + off * sn, 0}, yaw, {0.10f, 0.14f, 0.13f}, z + 0.12f + 0.4f * static_cast<float>(sh));
                }
            }
            mb.oriented_box(MFrameDark, {x - 0.0f, y, 0}, yaw, {L, 0.012f, 0.9f}, z);
        } else if (p.type == "display_case") {
            mb.oriented_box(MPaintWhite, {x, y, 0}, yaw, {0.5f * s, 0.38f, 0.35f}, z);
            mb.oriented_box(MGlassShard, {x, y, 0}, yaw, {0.5f * s - 0.03f, 0.35f, 0.2f}, z + 0.7f);
            mb.oriented_box(MBrickPattern, {x, y, 0}, yaw, {0.5f * s - 0.1f, 0.28f, 0.04f}, z + 0.8f);
        } else if (p.type == "stool") {
            mb.cylinder(MFrameDark, {x, y, z}, 0.04f, 0.6f, 6, false);
            mb.cylinder(MDoorRed, {x, y, z + 0.6f}, 0.2f, 0.06f, 10, true);
        } else if (p.type == "table_round" || p.type == "table_square" || p.type == "cafe_table") {
            // models are placed separately
        }
    }

    // ---- street furniture -------------------------------------------------------------------------------------------------------------------------------------------------------------
    void build_street_props() {
        // Procedural props are baked into one model per 40 m tile; model-based props become instances of their staged model.
        std::map<std::pair<int, int>, MeshBuilder> tiles;
        std::map<std::pair<int, int>, std::pair<double, double>> tile_centres;
        const auto tile_of = [&](double x, double y) {
            const std::pair<int, int> k{static_cast<int>(std::floor(x / 40.0)), static_cast<int>(std::floor(y / 40.0))};
            tile_centres[k] = {(k.first + 0.5) * 40.0, (k.second + 0.5) * 40.0};
            return k;
        };
        std::map<std::string, std::vector<const StreetProp*>> by_model;
        for (const StreetProp& p : d.props) {
            const float x = static_cast<float>(p.x), y = static_cast<float>(p.y), z = static_cast<float>(p.z);
            const float yaw = static_cast<float>(p.yaw);
            MeshBuilder& mb = tiles[tile_of(p.x, p.y)];
            const auto it = props.find(p.type);
            if (it != props.end() && it->second.ok) {
                by_model[p.type].push_back(&p);
                continue;
            }
            if (p.type == "tree") build_tree(mb, x, y, z, yaw, static_cast<float>(p.scale), static_cast<std::uint32_t>(p.x * 13 + p.y * 7));
            else if (p.type == "bollard") {
                mb.cylinder(MPaintSteel, {x, y, z}, 0.1f, 0.8f, 8, true);
                mb.cylinder(MEmergencyYellow, {x, y, z + 0.55f}, 0.102f, 0.1f, 8, false);
            } else if (p.type == "signal_pole") {
                mb.cylinder(MPaintSteel, {x, y, z}, 0.08f, 4.2f, 8, true);
                mb.box(MPaintSteel, {x - 0.15f, y - 0.15f, z}, {x + 0.15f, y + 0.15f, z + 0.25f}, 63);
            } else if (p.type == "mooring_post") {
                mb.cylinder(MPaintDarkGrey, {x, y, z}, 0.15f, 0.55f, 8, true);
                mb.cylinder(MChrome, {x, y, z + 0.5f}, 0.2f, 0.06f, 8, true);
            } else if (p.type == "sign_post") {
                mb.cylinder(MPaintSteel, {x, y, z}, 0.04f, 2.4f, 6, false);
                mb.oriented_box(MEmergencyYellow, {x, y, 0}, yaw, {0.3f, 0.01f, 0.3f}, z + 1.9f);
            } else if (p.type == "bike_rack") {
                for (int i = 0; i < 4; ++i) {
                    const float c = std::cos(yaw), sn = std::sin(yaw);
                    const float off = -0.45f + 0.3f * static_cast<float>(i);
                    mb.oriented_box(MPaintSteel, {x + off * c, y + off * sn, 0}, yaw + 1.5708f, {0.02f, 0.2f, 0.4f}, z);
                }
            } else if (p.type == "bus_stop") {
                mb.oriented_box(MFrameDark, {x, y, 0}, yaw, {1.6f, 0.45f, 0.03f}, z + 2.5f);
                for (const float off : {-1.5f, 1.5f}) {
                    const float c = std::cos(yaw), sn = std::sin(yaw);
                    mb.oriented_box(MPaintSteel, {x + off * c, y + off * sn, 0}, yaw, {0.04f, 0.4f, 1.3f}, z);
                }
                mb.oriented_box(MGlassShard, {x - 0.0f, y + 0.0f, 0}, yaw + 0.0f, {1.5f, 0.02f, 1.1f}, z + 0.2f);
            } else if (p.type == "kiosk") {
                mb.oriented_box(MPlasterB, {x, y, 0}, yaw, {1.4f, 1.4f, 1.15f}, z);
                mb.oriented_box(MAwningBlue, {x, y, 0}, yaw, {1.7f, 1.7f, 0.07f}, z + 2.35f);
                mb.oriented_box(MGlassLitCool, {x, y, 0}, yaw, {1.0f, 1.42f, 0.5f}, z + 0.9f);
            } else if (p.type == "cafe_table") {
                mb.cylinder(MFrameDark, {x, y, z}, 0.03f, 0.72f, 6, false);
                mb.cylinder(MFloorWood, {x, y, z + 0.72f}, 0.38f, 0.04f, 12, true);
                mb.cylinder(MFrameDark, {x, y, z}, 0.2f, 0.02f, 10, true);
            }
        }
        for (auto& [k, mb] : tiles) {
            if (mb.triangle_count() == 0) continue;
            const auto c = tile_centres[k];
            add_static("props " + std::to_string(k.first) + "," + std::to_string(k.second), mb, static_cast<float>(c.first), static_cast<float>(c.second), 40.0f);
        }
        for (auto& [type, list] : by_model) {
            const PropModel& pm = props[type];
            for (const StreetProp* p : list) {
                Placed pl;
                pl.model = pm.model;
                const float sc = pm.scale * static_cast<float>(p->scale > 0.2 && (type == "tree") ? p->scale : 1.0);
                pl.world = translation({static_cast<float>(p->x), static_cast<float>(p->y), static_cast<float>(p->z) - pm.lo.z * sc}) * rot_z(static_cast<float>(p->yaw) + pm.yaw_offset) * scaling(sc);
                pl.cx = static_cast<float>(p->x);
                pl.cy = static_cast<float>(p->y);
                pl.radius = 8.0f;
                triangles += static_cast<std::size_t>(pm.model.triangle_count());
                statics.push_back(std::move(pl));
            }
        }
        // Interior furniture models.
        for (const Interior& in : d.interiors) {
            for (const auto* list : {&in.furniture, &in.upper_furniture})
                for (const StreetProp& p : *list) {
                    const auto it = props.find(p.type);
                    if (it == props.end() || !it->second.ok) continue;
                    const PropModel& pm = it->second;
                    Placed pl;
                    pl.model = pm.model;
                    const float sc = pm.scale;
                    pl.world = translation({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) - pm.lo.z * sc}) * rot_z(static_cast<float>(p.yaw) + pm.yaw_offset) * scaling(sc);
                    pl.cx = static_cast<float>(p.x);
                    pl.cy = static_cast<float>(p.y);
                    pl.radius = 5.0f;
                    triangles += static_cast<std::size_t>(pm.model.triangle_count());
                    statics.push_back(std::move(pl));
                }
        }
    }

    void build_tree(MeshBuilder& mb, float x, float y, float z, float yaw, float scale, std::uint32_t seed) {
        const float h = (4.2f + hashf(seed, 3) * 1.8f) * scale;
        mb.cone(MBark, {x, y, z}, 0.16f * scale, 0.07f * scale, h * 0.62f, 8);
        const int blobs = 5;
        for (int i = 0; i < blobs; ++i) {
            const float a = yaw + static_cast<float>(i) * 1.256f + hashf(seed, static_cast<std::uint32_t>(i)) * 0.6f;
            const float rr = (i == 0 ? 0.0f : 0.9f + hashf(seed, 10 + static_cast<std::uint32_t>(i)) * 0.6f) * scale;
            const float cz = z + h * 0.6f + (i == 0 ? 0.9f : hashf(seed, 20 + static_cast<std::uint32_t>(i)) * 1.2f) * scale;
            const float rad = (1.5f + hashf(seed, 30 + static_cast<std::uint32_t>(i)) * 0.5f) * scale;
            mb.sphere((i % 2) ? MFoliageDark : MFoliage, {x + std::cos(a) * rr, y + std::sin(a) * rr, cz}, rad, 4, 8);
        }
    }

    // ---- signal heads ------------------------------------------------------------------------------------------------------------------------------------------------------------------
    void build_signals() {
        for (int state = 0; state < 4; ++state) {
            MeshBuilder mb;
            mb.box(MPaintBlack, {-0.17f, -0.1f, 0.0f}, {0.17f, 0.1f, 1.0f}, 63);
            const Mat lamps[3] = {state == 1 ? MSignalRed : MSignalOff, state == 2 ? MSignalAmber : MSignalOff, state == 3 ? MSignalGreen : MSignalOff};
            for (int i = 0; i < 3; ++i) {
                const float zc = 0.83f - 0.33f * static_cast<float>(i);
                mb.quad(lamps[i], {-0.1f, -0.103f, zc - 0.1f}, {0.1f, -0.103f, zc - 0.1f}, {0.1f, -0.103f, zc + 0.1f}, {-0.1f, -0.103f, zc + 0.1f});
            }
            // visors
            for (int i = 0; i < 3; ++i) {
                const float zc = 0.83f - 0.33f * static_cast<float>(i);
                mb.box(MPaintBlack, {-0.14f, -0.2f, zc + 0.1f}, {0.14f, -0.1f, zc + 0.13f}, 63);
            }
            head[state] = upload_model(r, "signal head", mb.finish(), mats.list());
        }
        for (int state = 0; state < 2; ++state) {
            MeshBuilder mb;
            mb.box(MPaintBlack, {-0.17f, -0.08f, 0.0f}, {0.17f, 0.08f, 0.4f}, 63);
            mb.quad(state == 1 ? MSignalWalk : MSignalRed, {-0.12f, -0.083f, 0.05f}, {0.12f, -0.083f, 0.05f}, {0.12f, -0.083f, 0.35f}, {-0.12f, -0.083f, 0.35f});
            ped_head[state] = upload_model(r, "ped signal", mb.finish(), mats.list());
        }
        // Placement: heads on the existing signal poles (their yaw faces the intersection), two each: one for the leg whose traffic comes from the pole's own side axis, one for the perpendicular one.
        for (const StreetProp& p : d.props) {
            if (p.type != "signal_pole") continue;
            // The intersection this pole belongs to.
            const Intersection* in = nullptr;
            double best = 1e18;
            for (const Intersection& I : d.intersections) {
                const double dd = std::hypot(I.x - p.x, I.y - p.y);
                if (dd < best) {
                    best = dd;
                    in = &I;
                }
            }
            if (in == nullptr) continue;
            const int sx = p.x > in->x ? 1 : -1, sy = p.y > in->y ? 1 : -1;
            // A pole on the far side of the intersection from the traffic it serves: NE serves the South leg (northbound) and the West leg (eastbound); SW the North leg and East leg.
            struct Serve {
                int leg;
                float face_yaw;
            };
            const Serve serves[2] = {sx > 0 ? Serve{static_cast<int>(Leg::South), static_cast<float>(-kPi * 0.5)} : Serve{static_cast<int>(Leg::North), static_cast<float>(kPi * 0.5)},
                                     sx > 0 ? Serve{static_cast<int>(Leg::West), static_cast<float>(kPi)} : Serve{static_cast<int>(Leg::East), 0.0f}};
            (void)sy;
            for (const Serve& sv : serves) {
                if (!in->has_leg[static_cast<std::size_t>(sv.leg)]) continue;
                // The head faces the traffic: its -y face (the lamps) points along the facing direction.
                const float yaw = sv.face_yaw + static_cast<float>(kPi * 0.5);
                heads.push_back({in->id, sv.leg, translation({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) + 3.2f}) * rot_z(yaw)});
            }
        }
        for (const Crosswalk& c : d.crosswalks) {
            for (int end = 0; end < 2; ++end) {
                const auto& pt = end == 0 ? c.a : c.b;
                const auto dir = leg_dir(c.leg);
                // Face along the crossing direction towards the opposite kerb.
                const double fx = (end == 0 ? c.b[0] - c.a[0] : c.a[0] - c.b[0]), fy = (end == 0 ? c.b[1] - c.a[1] : c.a[1] - c.b[1]);
                const float yaw = static_cast<float>(std::atan2(fy, fx)) + static_cast<float>(kPi * 0.5);
                (void)dir;
                ped_heads.push_back({c.id, translation({static_cast<float>(pt[0]), static_cast<float>(pt[1]), 0.15f + 2.2f}) * rot_z(yaw)});
            }
        }
    }
};

CityScene::CityScene(render::SceneRenderer& renderer, const Materials& materials, const District& district, const fs::path& cache, std::vector<std::string>& warnings) : impl_(std::make_unique<Impl>(renderer, materials, district, cache, warnings)) {
    Impl& i = *impl_;
    i.load_props();
    i.build_ground();
    for (const Building& b : i.d.buildings) i.build_building(b);
    i.build_street_props();
    i.build_signals();
}
CityScene::~CityScene() = default;

void CityScene::collect(std::vector<render::ModelDraw>& out, Float3 eye, float far_distance) const {
    // The district continues beyond its edge as scenery: copies of the same blocks, offset by the grid's period, drawn without any simulation (the roads and streets line up with the district's own).
    static const float offsets[6][2] = {{0, 0}, {270, 0}, {-270, 0}, {0, -180}, {270, -180}, {-270, -180}};
    for (const auto& o : offsets) {
        const Mat4 shift = translation({o[0], o[1], 0.0f});
        for (const Placed& p : impl_->statics) {
            if (p.radius < 1e8f) {
                const float dx = p.cx + o[0] - eye.x, dy = p.cy + o[1] - eye.y;
                if (std::sqrt(dx * dx + dy * dy) - p.radius > far_distance) continue;
            } else if (o[0] != 0.0f || o[1] != 0.0f) {
                // The ground model is huge: a copy matters only while the eye is within reach of its district.
                if (std::fabs(eye.x - o[0]) > 400.0f || std::fabs(eye.y - o[1]) > 400.0f) continue;
            }
            out.push_back({&p.model, o[0] == 0.0f && o[1] == 0.0f ? p.world : shift * p.world});
        }
    }
}

void CityScene::collect_signals(std::vector<render::ModelDraw>& out, const std::function<int(std::uint32_t, int)>& vehicle_state, const std::function<bool(std::uint32_t)>& walk) const {
    for (const Impl::HeadPlace& h : impl_->heads) out.push_back({&impl_->head[std::clamp(vehicle_state(h.intersection, h.leg), 0, 3)], h.world});
    for (const Impl::PedHeadPlace& h : impl_->ped_heads) out.push_back({&impl_->ped_head[walk(h.crosswalk) ? 1 : 0], h.world});
}

std::size_t CityScene::draw_count() const { return impl_->statics.size(); }
std::size_t CityScene::triangle_count() const { return impl_->triangles; }

} // namespace saige::alive::rend
