#include "world.hpp"
#include "props.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace saige::horizon {

namespace {

constexpr double pi = std::numbers::pi;

double smoothstep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double lerp(double a, double b, double t) { return a + (b - a) * t; }

std::uint64_t splitmix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

D2 catmull(const D2& p0, const D2& p1, const D2& p2, const D2& p3, double t) {
    const double t2 = t * t, t3 = t2 * t;
    return {0.5 * ((2 * p1.x) + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3),
            0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3)};
}

struct Builder {
    std::array<MeshPart, surface_count> parts;
    std::vector<Triangle> tris;
    std::vector<Aabb> boxes;
    double ox{}, oy{};  // cell corner

    Builder() {
        for (std::size_t i = 0; i < surface_count; ++i) parts[i].surface = static_cast<Surface>(i);
    }

    // A quad a b c d (counter-clockwise seen from the side the normal points to), world coordinates, uv at the corners.
    void quad(Surface s, D2 /*unused*/, const std::array<std::array<double, 3>, 4>& p, const std::array<std::array<double, 2>, 4>& uv, bool collide) {
        MeshPart& part = parts[static_cast<std::size_t>(s)];
        const auto sub = [](const std::array<double, 3>& a, const std::array<double, 3>& b) { return std::array<double, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
        const auto cr = [](const std::array<double, 3>& a, const std::array<double, 3>& b) { return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
        auto n = cr(sub(p[1], p[0]), sub(p[2], p[0]));
        double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-12) {
            n = cr(sub(p[2], p[0]), sub(p[3], p[0]));
            l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        }
        if (l < 1e-12) return;
        n = {n[0] / l, n[1] / l, n[2] / l};
        const std::uint32_t base = static_cast<std::uint32_t>(part.vertices.size());
        for (int i = 0; i < 4; ++i) {
            part.vertices.push_back({static_cast<float>(p[static_cast<std::size_t>(i)][0] - ox), static_cast<float>(p[static_cast<std::size_t>(i)][1] - oy),
                                     static_cast<float>(p[static_cast<std::size_t>(i)][2]), static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]),
                                     static_cast<float>(uv[static_cast<std::size_t>(i)][0]), static_cast<float>(uv[static_cast<std::size_t>(i)][1])});
        }
        part.indices.insert(part.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        if (collide) {
            const auto v = [&](int i) { return Vec3{static_cast<float>(p[static_cast<std::size_t>(i)][0] - ox), static_cast<float>(p[static_cast<std::size_t>(i)][1] - oy), static_cast<float>(p[static_cast<std::size_t>(i)][2])}; };
            tris.push_back({v(0), v(1), v(2)});
            tris.push_back({v(0), v(2), v(3)});
        }
    }
    void tri(Surface s, const std::array<std::array<double, 3>, 3>& p, const std::array<std::array<double, 2>, 3>& uv) {
        MeshPart& part = parts[static_cast<std::size_t>(s)];
        const auto sub = [](const std::array<double, 3>& a, const std::array<double, 3>& b) { return std::array<double, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
        const auto a = sub(p[1], p[0]), b = sub(p[2], p[0]);
        std::array<double, 3> n{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-12) return;
        const std::uint32_t base = static_cast<std::uint32_t>(part.vertices.size());
        for (int i = 0; i < 3; ++i) {
            part.vertices.push_back({static_cast<float>(p[static_cast<std::size_t>(i)][0] - ox), static_cast<float>(p[static_cast<std::size_t>(i)][1] - oy), static_cast<float>(p[static_cast<std::size_t>(i)][2]),
                                     static_cast<float>(n[0] / l), static_cast<float>(n[1] / l), static_cast<float>(n[2] / l), static_cast<float>(uv[static_cast<std::size_t>(i)][0]),
                                     static_cast<float>(uv[static_cast<std::size_t>(i)][1])});
        }
        part.indices.insert(part.indices.end(), {base, base + 1, base + 2});
    }
    // An axis-aligned box (all faces outward, UV by world size / tile), rendered in `wall` and `roof` (the top), collidable as a box.
    void box(Surface wall, Surface top, double x0, double y0, double z0, double x1, double y1, double z1, double tile, bool collide) {
        const auto uvq = [&](double a, double b, double c, double d) { return std::array<std::array<double, 2>, 4>{{{a / tile, b / tile}, {c / tile, b / tile}, {c / tile, d / tile}, {a / tile, d / tile}}}; };
        quad(wall, {}, {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}}, {{{x0 / tile, z0 / tile}, {x1 / tile, z0 / tile}, {x1 / tile, z1 / tile}, {x0 / tile, z1 / tile}}}, false);  // -Y
        quad(wall, {}, {{{x1, y1, z0}, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}}}, {{{x1 / tile, z0 / tile}, {x0 / tile, z0 / tile}, {x0 / tile, z1 / tile}, {x1 / tile, z1 / tile}}}, false);  // +Y
        quad(wall, {}, {{{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}}, {{{y0 / tile, z0 / tile}, {y1 / tile, z0 / tile}, {y1 / tile, z1 / tile}, {y0 / tile, z1 / tile}}}, false);  // +X
        quad(wall, {}, {{{x0, y1, z0}, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}}}, {{{y1 / tile, z0 / tile}, {y0 / tile, z0 / tile}, {y0 / tile, z1 / tile}, {y1 / tile, z1 / tile}}}, false);  // -X
        quad(top, {}, {{{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}}, uvq(x0, y0, x1, y1), false);
        if (collide) boxes.push_back({{static_cast<float>(x0 - ox), static_cast<float>(y0 - oy), static_cast<float>(z0)}, {static_cast<float>(x1 - ox), static_cast<float>(y1 - oy), static_cast<float>(z1)}});
    }
};

} // namespace

std::uint64_t hash64(std::uint64_t a, std::uint64_t b, std::uint64_t c) { return splitmix(a * 0x9e3779b97f4a7c15ull ^ splitmix(b + 0x1234567ull) ^ splitmix(c * 31ull + 7ull)); }
double unit(std::uint64_t h) { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

std::uint64_t CellData::vertex_count() const {
    std::uint64_t n = 0;
    for (const MeshPart& p : parts) n += p.vertices.size();
    return n;
}
std::uint64_t CellData::triangle_count() const {
    std::uint64_t n = 0;
    for (const MeshPart& p : parts) n += p.indices.size() / 3;
    return n;
}

void World::add_road(std::vector<D2> controls, double width, bool town, double amplitude_start) {
    Road road;
    road.width = width;
    road.town = town;
    // Catmull-Rom through the controls (duplicating the ends), sampled about every metre.
    std::vector<D2> c;
    c.push_back({2 * controls.front().x - controls[1].x, 2 * controls.front().y - controls[1].y});
    c.insert(c.end(), controls.begin(), controls.end());
    c.push_back({2 * controls.back().x - controls[controls.size() - 2].x, 2 * controls.back().y - controls[controls.size() - 2].y});
    for (std::size_t i = 1; i + 2 < c.size(); ++i) {
        const double span = std::hypot(c[i + 1].x - c[i].x, c[i + 1].y - c[i].y);
        const int steps = std::max(2, static_cast<int>(std::ceil(span)));
        for (int k = 0; k < steps; ++k) road.points.push_back(catmull(c[i - 1], c[i], c[i + 1], c[i + 2], static_cast<double>(k) / steps));
    }
    road.points.push_back(controls.back());
    // Resample to a uniform ~1 m spacing along the curve.
    std::vector<D2> uniform{road.points.front()};
    double carry = 0.0;
    for (std::size_t i = 1; i < road.points.size(); ++i) {
        D2 a = uniform.back();
        const D2 b = road.points[i];
        double d = std::hypot(b.x - a.x, b.y - a.y);
        while (d + carry >= 1.0) {
            const double t = (1.0 - carry) / d;
            a = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            uniform.push_back(a);
            d = std::hypot(b.x - a.x, b.y - a.y);
            carry = 0.0;
        }
        carry += d;
    }
    uniform.push_back(road.points.back());
    road.points = std::move(uniform);
    road.s.assign(road.points.size(), 0.0);
    for (std::size_t i = 1; i < road.points.size(); ++i) road.s[i] = road.s[i - 1] + std::hypot(road.points[i].x - road.points[i - 1].x, road.points[i].y - road.points[i - 1].y);
    road.z.assign(road.points.size(), 0.0);
    for (std::size_t i = 0; i < road.points.size(); ++i) {
        const double s = road.s[i];
        const double amp = smoothstep((s - amplitude_start) / 140.0);
        road.z[i] = amp * (5.0 * std::sin(s / 160.0) + 2.5 * std::sin(s / 70.0 + 1.0) + 2.0);
    }
    roads_.push_back(std::move(road));
}

World::World() {
    // Road 0 is the route: through the town, then out into the hills.
    add_road({{-80, 0}, {0, 0}, {120, 0}, {240, 0}, {300, 0}, {380, 38}, {445, 105}, {520, 150}, {600, 125}, {665, 65}, {700, -15}, {728, -95}, {795, -150}, {890, -162}, {990, -125}, {1080, -60}},
             7.2, false, 330.0);
    roads_[0].town = true;  // sidewalks and kerbs apply to its town stretch only (checked by position)
    add_road({{120, -120}, {120, 0}, {120, 120}}, 6.4, true, 1e9);
    add_road({{172, 0}, {172, 10}, {172, 26}}, 6.0, true, 1e9);
    landmarks_.start = {0.0, 0.0};
    landmarks_.gas_station = {172.0, 30.0};
    // The crash site: the outside of the route's left-hand bend near (720, -83). A car that stops steering there leaves the road toward the south-west; the barrier
    // line stands across that path, angled 58 degrees to it.
    landmarks_.crash_s = 985.0;
    landmarks_.crash_barrier = {712.7, -89.5};
    landmarks_.crash_heading = -25.0 * pi / 180.0;
}

double World::town_weight(double x, double y) const {
    const double wx = smoothstep((x + 60.0) / 50.0) * (1.0 - smoothstep((x - 220.0) / 50.0));
    const double wy = smoothstep((y + 80.0) / 50.0) * (1.0 - smoothstep((y - 50.0) / 50.0));
    return wx * wy;
}

double World::noise_height(double x, double y) const {
    double h = 16.0 * std::sin(x / 151.0 + 0.7) * std::sin(y / 127.0 + 2.1) + 9.0 * std::sin((x + y) / 73.0 + 1.4) + 5.0 * std::sin(x / 37.0 - y / 43.0) + 2.2 * std::sin(x / 13.0 + y / 11.0);
    // Distant ranges far to the north and south of the route.
    const double north = std::exp(-std::pow((y - 780.0) / 240.0, 2.0)) * (0.65 + 0.35 * std::sin(x / 260.0 + 0.3));
    const double south = std::exp(-std::pow((y + 900.0) / 280.0, 2.0)) * (0.6 + 0.4 * std::sin(x / 210.0 + 2.0));
    h += 150.0 * north + 120.0 * south;
    return h;
}

RoadSample World::nearest_road(double x, double y, int only_road) const {
    RoadSample best;
    for (std::size_t ri = 0; ri < roads_.size(); ++ri) {
        if (only_road >= 0 && static_cast<std::size_t>(only_road) != ri) continue;
        const Road& r = roads_[ri];
        for (std::size_t i = 0; i + 1 < r.points.size(); ++i) {
            const D2 a = r.points[i], b = r.points[i + 1];
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double px = x - a.x, py = y - a.y;
            if (px * px + py * py > 40000.0 && (px * px + py * py) > best.distance * best.distance + 4.0) continue;
            const double len2 = dx * dx + dy * dy;
            const double t = len2 > 0.0 ? std::clamp((px * dx + py * dy) / len2, 0.0, 1.0) : 0.0;
            const double qx = a.x + dx * t, qy = a.y + dy * t;
            const double d = std::hypot(x - qx, y - qy);
            if (d < best.distance) {
                best.distance = d;
                best.s = lerp(r.s[i], r.s[i + 1], t);
                best.z = lerp(r.z[i], r.z[i + 1], t);
                best.side = (dx * (y - qy) - dy * (x - qx)) >= 0.0 ? 1.0 : -1.0;
                best.road = static_cast<int>(ri);
            }
        }
    }
    return best;
}

double World::height(double x, double y) const {
    double h = lerp(noise_height(x, y), 0.0, town_weight(x, y));
    for (std::size_t ri = 0; ri < roads_.size(); ++ri) {
        const Road& r = roads_[ri];
        // A cheap reject by bounding the polyline.
        const RoadSample q = nearest_road(x, y, static_cast<int>(ri));
        // Out in the country the road has a wide flat shoulder and long gentle embankments; in the town kerbs and short blends.
        const double tw = town_weight(x, y);
        const double half = r.width * 0.5 + 1.6 + (1.0 - tw) * 3.0;
        const double blend = 7.0 + (1.0 - tw) * 36.0;
        if (q.distance >= half + blend) continue;
        const double w = 1.0 - smoothstep((q.distance - half) / blend);
        h = lerp(h, q.z, w);
    }
    return h;
}

D2 World::route_point(double s, double* heading, double* z) const {
    const Road& r = roads_[0];
    s = std::clamp(s, 0.0, r.s.back());
    const auto it = std::upper_bound(r.s.begin(), r.s.end(), s);
    std::size_t i = it == r.s.begin() ? 0 : static_cast<std::size_t>(it - r.s.begin() - 1);
    if (i + 1 >= r.points.size()) i = r.points.size() - 2;
    const double t = (s - r.s[i]) / std::max(1e-9, r.s[i + 1] - r.s[i]);
    if (heading) *heading = std::atan2(r.points[i + 1].y - r.points[i].y, r.points[i + 1].x - r.points[i].x);
    if (z) *z = lerp(r.z[i], r.z[i + 1], t);
    return {lerp(r.points[i].x, r.points[i + 1].x, t), lerp(r.points[i].y, r.points[i + 1].y, t)};
}

MeshPart World::far_terrain(double x0, double y0, double x1, double y1, double step, double drop) const {
    MeshPart part;
    part.surface = Surface::Ground;
    const int nx = static_cast<int>(std::ceil((x1 - x0) / step)) + 1, ny = static_cast<int>(std::ceil((y1 - y0) / step)) + 1;
    part.vertices.reserve(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double x = x0 + i * step, y = y0 + j * step;
            const double e = step * 0.5;
            const double hx = (height(x + e, y) - height(x - e, y)) / (2 * e), hy = (height(x, y + e) - height(x, y - e)) / (2 * e);
            const double nl = std::sqrt(hx * hx + hy * hy + 1.0);
            // Positions are relative to (x0, y0); the caller places the model there.
            part.vertices.push_back({static_cast<float>(x - x0), static_cast<float>(y - y0), static_cast<float>(height(x, y) - drop), static_cast<float>(-hx / nl), static_cast<float>(-hy / nl),
                                     static_cast<float>(1.0 / nl), static_cast<float>(x / 40.0), static_cast<float>(y / 40.0)});
        }
    }
    for (int j = 0; j + 1 < ny; ++j) {
        for (int i = 0; i + 1 < nx; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(j * nx + i), b = a + 1, c = static_cast<std::uint32_t>((j + 1) * nx + i + 1), d = static_cast<std::uint32_t>((j + 1) * nx + i);
            part.indices.insert(part.indices.end(), {a, b, c, a, c, d});
        }
    }
    return part;
}

CellData World::build_cell(int cx, int cy) const {
    CellData cell;
    cell.cx = cx;
    cell.cy = cy;
    Builder b;
    b.ox = cx * cell_size;
    b.oy = cy * cell_size;
    const double ox = b.ox, oy = b.oy;

    // ---- Terrain: a 2 m grid.
    constexpr int n = 32;
    constexpr double step = cell_size / n;
    {
        MeshPart& g = b.parts[static_cast<std::size_t>(Surface::Ground)];
        std::vector<double> hz(static_cast<std::size_t>((n + 1) * (n + 1)));
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) hz[static_cast<std::size_t>(j * (n + 1) + i)] = height(ox + i * step, oy + j * step);
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                const double x = ox + i * step, y = oy + j * step;
                const double hx = (height(x + 1.0, y) - height(x - 1.0, y)) * 0.5, hy = (height(x, y + 1.0) - height(x, y - 1.0)) * 0.5;
                const double nl = std::sqrt(hx * hx + hy * hy + 1.0);
                g.vertices.push_back({static_cast<float>(i * step), static_cast<float>(j * step), static_cast<float>(hz[static_cast<std::size_t>(j * (n + 1) + i)]), static_cast<float>(-hx / nl),
                                      static_cast<float>(-hy / nl), static_cast<float>(1.0 / nl), static_cast<float>(x / 6.0), static_cast<float>(y / 6.0)});
            }
        }
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const std::uint32_t a = static_cast<std::uint32_t>(j * (n + 1) + i), bb = a + 1, c = static_cast<std::uint32_t>((j + 1) * (n + 1) + i + 1), d = static_cast<std::uint32_t>((j + 1) * (n + 1) + i);
                g.indices.insert(g.indices.end(), {a, bb, c, a, c, d});
                const auto v = [&](std::uint32_t k) { return Vec3{g.vertices[k].px, g.vertices[k].py, g.vertices[k].pz}; };
                b.tris.push_back({v(a), v(bb), v(c)});
                b.tris.push_back({v(a), v(c), v(d)});
            }
        }
    }

    // ---- Roads: strips along each centerline whose segment midpoints fall in this cell.
    const auto in_cell = [&](double x, double y) { return x >= ox && x < ox + cell_size && y >= oy && y < oy + cell_size; };
    const auto road_at = [&](const Road& r, std::size_t i, double side, double offset) {
        const D2 a = r.points[std::min(i + 1, r.points.size() - 1)], p = r.points[i], q = r.points[i > 0 ? i - 1 : 0];
        double dx = a.x - q.x, dy = a.y - q.y;
        const double l = std::hypot(dx, dy);
        dx /= l;
        dy /= l;
        return std::array<double, 2>{p.x - dy * side * offset, p.y + dx * side * offset};
    };
    for (std::size_t ri = 0; ri < roads_.size(); ++ri) {
        const Road& r = roads_[ri];
        const double half = r.width * 0.5;
        for (std::size_t i = 1; i + 1 < r.points.size(); ++i) {
            const D2 m{(r.points[i].x + r.points[i + 1].x) * 0.5, (r.points[i].y + r.points[i + 1].y) * 0.5};
            if (!in_cell(m.x, m.y)) continue;
            const auto strip = [&](Surface surf, double off0, double off1, double lift, double u_scale, bool mirror_v = false) {
                for (const double side : {1.0, -1.0}) {
                    if (surf == Surface::Road && side < 0) continue;  // the asphalt is one strip across the whole width
                    const double o0 = surf == Surface::Road ? -half : off0, o1 = surf == Surface::Road ? half : off1;
                    const double a0 = side * o0, a1 = side * o1;
                    // Corners of this and the next sample, (left, right) across.
                    const auto pt = [&](std::size_t k, double across) {
                        const auto xy = road_at(r, k, 1.0, across);
                        return std::array<double, 3>{xy[0], xy[1], r.z[k] + lift};
                    };
                    (void)mirror_v;
                    const std::array<double, 3> p0 = pt(i, a0), p1 = pt(i, a1), p2 = pt(i + 1, a1), p3 = pt(i + 1, a0);
                    const double v0 = r.s[i] / u_scale, v1 = r.s[i + 1] / u_scale;
                    // Winding so the normal is +Z: (across a0 -> a1 along the travel direction) is clockwise from above when a0 < a1 (left is +); flip when needed.
                    if (a0 < a1) {
                        b.quad(surf, {}, {{p1, p0, p3, p2}}, {{{a1 / u_scale, v0}, {a0 / u_scale, v0}, {a0 / u_scale, v1}, {a1 / u_scale, v1}}}, false);
                    } else {
                        b.quad(surf, {}, {{p0, p1, p2, p3}}, {{{a0 / u_scale, v0}, {a1 / u_scale, v0}, {a1 / u_scale, v1}, {a0 / u_scale, v1}}}, false);
                    }
                }
            };
            strip(Surface::Road, 0, 0, 0.035, 3.6);
            strip(Surface::Shoulder, half, half + 1.5, 0.012, 2.4);
            // Lines: continuous edge lines, a dashed centre line.
            const double s_mid = (r.s[i] + r.s[i + 1]) * 0.5;
            for (const double side : {1.0, -1.0}) {
                const double o = half - 0.4;
                const auto pt = [&](std::size_t k, double across) {
                    const auto xy = road_at(r, k, 1.0, across);
                    return std::array<double, 3>{xy[0], xy[1], r.z[k] + 0.045};
                };
                const double w = 0.06;
                const auto p0 = pt(i, side * o - w), p1 = pt(i, side * o + w), p2 = pt(i + 1, side * o + w), p3 = pt(i + 1, side * o - w);
                b.quad(Surface::Marking, {}, {{p1, p0, p3, p2}}, {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}, false);
            }
            if (std::fmod(s_mid, 9.0) < 3.0) {
                const double w = 0.07;
                const auto pt = [&](std::size_t k, double across) {
                    const auto xy = road_at(r, k, 1.0, across);
                    return std::array<double, 3>{xy[0], xy[1], r.z[k] + 0.045};
                };
                const auto p0 = pt(i, -w), p1 = pt(i, w), p2 = pt(i + 1, w), p3 = pt(i + 1, -w);
                b.quad(Surface::Marking, {}, {{p1, p0, p3, p2}}, {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}, false);
            }
            // Sidewalks along town stretches, where not crossed by another road.
            const bool town_stretch = r.town && town_weight(m.x, m.y) > 0.5 && ri != 2;
            if (town_stretch) {
                for (const double side : {1.0, -1.0}) {
                    const double c = side * (half + 1.3);
                    const auto xy = road_at(r, i, 1.0, c);
                    bool crossed = false;
                    for (std::size_t rj = 0; rj < roads_.size() && !crossed; ++rj) {
                        if (rj == ri) continue;
                        crossed = nearest_road(xy[0], xy[1], static_cast<int>(rj)).distance < roads_[rj].width * 0.5 + 1.6;
                    }
                    if (crossed) continue;
                    const double ia = side * (half + 0.15), oa = side * (half + 2.45);
                    const auto pin0 = road_at(r, i, 1.0, ia), pout0 = road_at(r, i, 1.0, oa), pin1 = road_at(r, i + 1, 1.0, ia), pout1 = road_at(r, i + 1, 1.0, oa);
                    const double z0 = r.z[i], z1 = r.z[i + 1], top = 0.15;
                    const double v0 = r.s[i] / 3.0, v1 = r.s[i + 1] / 3.0;
                    // Top surface (normal +Z) and the kerb face toward the road.
                    if (side > 0) {
                        b.quad(Surface::Sidewalk, {}, {{{pin0[0], pin0[1], z0 + top}, {pin1[0], pin1[1], z1 + top}, {pout1[0], pout1[1], z1 + top}, {pout0[0], pout0[1], z0 + top}}},
                               {{{0, v0}, {0, v1}, {0.8, v1}, {0.8, v0}}}, true);
                        b.quad(Surface::Sidewalk, {}, {{{pin0[0], pin0[1], z0}, {pin1[0], pin1[1], z1}, {pin1[0], pin1[1], z1 + top}, {pin0[0], pin0[1], z0 + top}}}, {{{0, 0}, {1, 0}, {1, 0.1}, {0, 0.1}}}, true);
                    } else {
                        b.quad(Surface::Sidewalk, {}, {{{pin0[0], pin0[1], z0 + top}, {pout0[0], pout0[1], z0 + top}, {pout1[0], pout1[1], z1 + top}, {pin1[0], pin1[1], z1 + top}}},
                               {{{0, v0}, {0.8, v0}, {0.8, v1}, {0, v1}}}, true);
                        b.quad(Surface::Sidewalk, {}, {{{pin1[0], pin1[1], z1}, {pin0[0], pin0[1], z0}, {pin0[0], pin0[1], z0 + top}, {pin1[0], pin1[1], z1 + top}}}, {{{0, 0}, {1, 0}, {1, 0.1}, {0, 0.1}}}, true);
                    }
                }
            }
        }
    }

    // ---- The town: buildings along the main street, the gas station.
    const auto building = [&](double x0, double y0, double w, double d, double h, std::uint64_t seed, double street_y) {
        if (!in_cell(x0 + w * 0.5, y0 + d * 0.5)) return;
        const bool brick = unit(hash64(seed, 1)) < 0.5;
        const Surface wall = brick ? Surface::WallBrick : Surface::WallPlaster;
        const bool gabled = unit(hash64(seed, 2)) < 0.65;
        const double gz = height(x0 + w * 0.5, y0 + d * 0.5);
        b.box(wall, gabled ? wall : Surface::RoofMetal, x0, y0, gz - 0.6, x0 + w, y0 + d, gz + h, 3.0, true);
        if (gabled) {
            const double ridge = 2.2 + unit(hash64(seed, 3)) * 1.2;
            const double zr = gz + h;
            const Surface roof = Surface::RoofTiles;
            const double ym = (y0 + y0 + d) * 0.5;
            // Two roof planes along X rising to a ridge at the middle of the depth, a small overhang, and gable triangles on the ends.
            const double o = 0.35;
            b.quad(roof, {}, {{{x0 - o, y0 - o, zr - 0.15}, {x0 + w + o, y0 - o, zr - 0.15}, {x0 + w + o, ym, zr + ridge}, {x0 - o, ym, zr + ridge}}}, {{{0, 0}, {(w + 2 * o) / 2.4, 0}, {(w + 2 * o) / 2.4, (d / 2 + o) / 2.4}, {0, (d / 2 + o) / 2.4}}}, false);
            b.quad(roof, {}, {{{x0 + w + o, y0 + d + o, zr - 0.15}, {x0 - o, y0 + d + o, zr - 0.15}, {x0 - o, ym, zr + ridge}, {x0 + w + o, ym, zr + ridge}}}, {{{0, 0}, {(w + 2 * o) / 2.4, 0}, {(w + 2 * o) / 2.4, (d / 2 + o) / 2.4}, {0, (d / 2 + o) / 2.4}}}, false);
            b.tri(wall, {{{x0, y0 + d, zr}, {x0, y0, zr}, {x0, ym, zr + ridge - 0.15}}}, {{{0, 0}, {d / 3, 0}, {d / 6, ridge / 3}}});
            b.tri(wall, {{{x0 + w, y0, zr}, {x0 + w, y0 + d, zr}, {x0 + w, ym, zr + ridge - 0.15}}}, {{{0, 0}, {d / 3, 0}, {d / 6, ridge / 3}}});
        }
        // Windows and a door on the facade that faces the street (street_y is the side: y0 for a building on the right of the street, y0 + d on the left).
        const bool face_low = std::fabs(street_y - y0) < std::fabs(street_y - (y0 + d));
        const double fy = face_low ? y0 - 0.03 : y0 + d + 0.03;
        const double sgn = face_low ? 1.0 : -1.0;
        const int floors = std::max(1, static_cast<int>(h / 3.2));
        const int cols = std::max(1, static_cast<int>(w / 3.4));
        for (int f = 0; f < floors; ++f) {
            for (int c = 0; c < cols; ++c) {
                const double cxw = x0 + (c + 0.5) * w / cols;
                const double wz0 = gz + 1.0 + f * 3.1, wz1 = wz0 + 1.45, hw = 0.62;
                const bool lit = unit(hash64(seed, 100 + f, c)) < 0.5;
                if (f == 0 && c == cols / 2) {
                    b.quad(Surface::Door, {}, {{{cxw - sgn * 0.55, fy, gz}, {cxw + sgn * 0.55, fy, gz}, {cxw + sgn * 0.55, fy, gz + 2.15}, {cxw - sgn * 0.55, fy, gz + 2.15}}}, {{{0, 0}, {1, 0}, {1, 2}, {0, 2}}}, false);
                    continue;
                }
                b.quad(lit ? Surface::WindowLit : Surface::WindowDark, {}, {{{cxw - sgn * hw, fy, wz0}, {cxw + sgn * hw, fy, wz0}, {cxw + sgn * hw, fy, wz1}, {cxw - sgn * hw, fy, wz1}}}, {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}, false);
            }
        }
    };
    // Main street: buildings on both sides between x = 6 and x = 112 and again past the cross street up to the gas station.
    {
        const double setback = 11.0;
        for (const double side : {1.0, -1.0}) {
            double x = 6.0;
            int k = 0;
            while (x < 236.0) {
                const std::uint64_t seed = hash64(static_cast<std::uint64_t>(k + 1000), side > 0 ? 1 : 2);
                const double w = 10.0 + unit(hash64(seed, 11)) * 8.0, d = 9.0 + unit(hash64(seed, 12)) * 6.0, h = 5.5 + unit(hash64(seed, 13)) * 4.5;
                const bool cross = x + w > 110.0 && x < 130.0;
                const bool gas = side > 0 && x + w > 142.0 && x < 204.0;
                if (!cross && !gas) {
                    if (side > 0) building(x, setback, w, d, h, seed, setback);
                    else building(x, -setback - d, w, d, h, seed, -setback);
                }
                x += w + 2.5 + unit(hash64(seed, 14)) * 4.0;
                ++k;
            }
        }
    }
    // The gas station: an apron, a canopy on four columns, three pump islands and a shop.
    {
        const double ax0 = 146.0, ax1 = 200.0, ay0 = 5.0, ay1 = 46.0;
        if (in_cell((ax0 + ax1) * 0.5, (ay0 + ay1) * 0.5) || in_cell(ax0, ay0) || in_cell(ax1, ay1)) {
            // The apron, clipped per cell by emitting 8 m tiles whose centres are in this cell.
            for (double x = ax0; x < ax1; x += 8.0) {
                for (double y = ay0; y < ay1; y += 8.0) {
                    if (!in_cell(x + 4.0, y + 4.0)) continue;
                    const double x1 = std::min(x + 8.0, ax1), y1 = std::min(y + 8.0, ay1);
                    b.quad(Surface::Apron, {}, {{{x, y, 0.03}, {x1, y, 0.03}, {x1, y1, 0.03}, {x, y1, 0.03}}}, {{{x / 4.0, y / 4.0}, {x1 / 4.0, y / 4.0}, {x1 / 4.0, y1 / 4.0}, {x / 4.0, y1 / 4.0}}}, false);
                }
            }
            const auto emit_if = [&](double x, double y, auto&& fn) {
                if (in_cell(x, y)) fn();
            };
            // The shop at the back of the apron.
            emit_if(173.0, 40.0, [&] {
                b.box(Surface::WallBrick, Surface::RoofMetal, 160.0, 36.0, 0.0, 186.0, 46.0, 4.2, 3.0, true);
                // A big lit shop window along the front and a door.
                b.quad(Surface::WindowLit, {}, {{{163.0, 35.97, 1.0}, {182.0, 35.97, 1.0}, {182.0, 35.97, 3.0}, {163.0, 35.97, 3.0}}}, {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}, false);
                b.quad(Surface::Door, {}, {{{183.0, 35.97, 0.0}, {185.0, 35.97, 0.0}, {185.0, 35.97, 2.2}, {183.0, 35.97, 2.2}}}, {{{0, 0}, {1, 0}, {1, 2}, {0, 2}}}, false);
            });
            // The canopy: four columns and a slab with a bright underside (lit at night).
            emit_if(172.0, 22.0, [&] {
                for (const double cxp : {160.0, 184.0}) {
                    for (const double cyp : {14.0, 30.0}) b.box(Surface::WallPlaster, Surface::WallPlaster, cxp - 0.4, cyp - 0.4, 0.0, cxp + 0.4, cyp + 0.4, 5.2, 3.0, true);
                }
                b.box(Surface::RoofMetal, Surface::RoofMetal, 155.0, 9.0, 5.2, 189.0, 35.0, 5.7, 3.0, false);
                b.quad(Surface::WindowLit, {}, {{{156.0, 10.0, 5.19}, {156.0, 34.0, 5.19}, {188.0, 34.0, 5.19}, {188.0, 10.0, 5.19}}}, {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}}, false);
            });
        }
    }

    // The ending's stage is dressed by hand: no trees or rocks on the crash site, the walk away from it or the overlook the walker stops at, so that the cameras there see the
    // people and the wreck (the clearing is part of the world, a pure function of the position like everything else).
    const auto hero_clearing = [&](double x, double y) {
        const double crash_x = landmarks_.crash_barrier.x, crash_y = landmarks_.crash_barrier.y;
        if (std::hypot(x - crash_x, y - crash_y) < 22.0) return true;
        const double ox = 731.8, oy = -118.5;
        if (std::hypot(x - ox, y - oy) < 26.0) return true;
        // the corridor between them
        const double ax = crash_x, ay = crash_y + 3.0, bx = ox, by = oy;
        const double dx = bx - ax, dy = by - ay;
        const double t = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
        return std::hypot(x - (ax + t * dx), y - (ay + t * dy)) < 12.0;
    };
    // ---- Props.
    const auto add_prop_collision = [&](const PropInstance& inst) {
        if (inst.scale <= 0.0f || !prop_spec(inst.type).solid) return;
        append_box_hull(b.tris, placed_hull_box(inst), ox, oy, 0.4);
    };
    const auto prop = [&](PropType t, double x, double y, float yaw, float scale) {
        if (!in_cell(x, y)) return;
        const PropInstance inst{t, x, y, height(x, y), yaw, scale};
        cell.props.push_back(inst);
        add_prop_collision(inst);
    };
    // Street lamps along the main street through the town, on the sidewalks, every 24 m.
    for (double x = 12.0; x < 232.0; x += 24.0) {
        for (const double side : {1.0, -1.0}) {
            if (x > 104.0 && x < 140.0) continue;  // the intersection
            if (side > 0 && x > 140.0 && x < 204.0) continue;  // the gas station frontage
            const double y = side * (3.6 + 1.9);
            if (in_cell(x, y)) {
                const PropInstance lamp{PropType::Lamp, x, y, 0.0, side > 0 ? static_cast<float>(-pi / 2) : static_cast<float>(pi / 2), 1.0f};
                cell.props.push_back(lamp);
                add_prop_collision(lamp);
            }
        }
    }
    // Pump islands, benches, barrels and a utility box.
    for (const double px : {158.0, 172.0, 186.0}) {
        prop(PropType::UtilityBox, px, 22.0, 0.0f, 1.3f);
        prop(PropType::UtilityBox, px, 21.2, 0.0f, 0.0f);
    }
    prop(PropType::Bench, 190.0, 38.0, static_cast<float>(pi), 1.6f);
    prop(PropType::BarrelStack, 196.0, 40.0, 0.7f, 1.1f);
    // Scatter vegetation and rocks on an 8 m jittered grid, off the roads and out of the town.
    for (int j = 0; j < 8; ++j) {
        for (int i = 0; i < 8; ++i) {
            const std::uint64_t h = hash64(static_cast<std::uint64_t>(cx * 8 + i + 100000), static_cast<std::uint64_t>(cy * 8 + j + 100000), 77);
            const double x = ox + (i + unit(hash64(h, 1))) * 8.0, y = oy + (j + unit(hash64(h, 2))) * 8.0;
            if (town_weight(x, y) > 0.35) continue;
            if (hero_clearing(x, y)) continue;
            const RoadSample q = nearest_road(x, y);
            const double clear = roads_[static_cast<std::size_t>(std::max(q.road, 0))].width * 0.5 + 3.5;
            if (q.distance < clear) continue;
            const double roll = unit(hash64(h, 3));
            const float yaw = static_cast<float>(unit(hash64(h, 4)) * 2 * pi);
            const float sc = static_cast<float>(0.8 + unit(hash64(h, 5)) * 0.7);
            // Denser woods away from the road.
            const double density = std::clamp((q.distance - clear) / 40.0, 0.0, 1.0);
            if (roll < 0.10 + 0.40 * density) prop(unit(hash64(h, 6)) < 0.55 ? PropType::TreePine : PropType::TreeRound, x, y, yaw, sc);
            else if (roll < 0.30 + 0.35 * density) prop(unit(hash64(h, 7)) < 0.5 ? PropType::ShrubA : PropType::ShrubB, x, y, yaw, sc * 2.4f);
            else if (roll < 0.34 + 0.30 * density) prop(unit(hash64(h, 8)) < 0.5 ? PropType::RockLarge : PropType::RockSmall, x, y, yaw, sc * (unit(hash64(h, 9)) < 0.5 ? 18.0f : 30.0f));
        }
    }

    // ---- Barriers beyond the outside of the crash bend: a line of concrete pieces, angled across the way a car leaves the road there (visual props plus collision hulls).
    {
        const double hdg = landmarks_.crash_heading;
        const double dxv = std::cos(hdg), dyv = std::sin(hdg);
        for (int k = -9; k < 9; ++k) {
            const double bx = landmarks_.crash_barrier.x + dxv * (k + 0.5) * 1.64, by = landmarks_.crash_barrier.y + dyv * (k + 0.5) * 1.64;
            if (!in_cell(bx, by)) continue;
            const PropInstance barrier{PropType::Barrier, bx, by, height(bx, by), static_cast<float>(hdg), 1.0f};
            cell.props.push_back(barrier);
            add_prop_collision(barrier);
        }
    }

    for (MeshPart& p : b.parts) {
        if (!p.vertices.empty()) cell.parts.push_back(std::move(p));
    }
    cell.triangles = std::move(b.tris);
    cell.boxes = std::move(b.boxes);
    return cell;
}

} // namespace saige::horizon
