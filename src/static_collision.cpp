#include "saige/static_collision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace saige {
namespace {

constexpr std::uint32_t leaf_size = 4;
constexpr std::size_t max_stack = 128;
constexpr double region_margin = 1e-3;  // wider than the margins the sweeps use to cull on their own

using D3 = std::array<double, 3>;

D3 to_d3(Vec3 v) noexcept { return {v.x, v.y, v.z}; }
D3 sub(const D3& a, const D3& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const D3& a, const D3& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross(const D3& a, const D3& b) noexcept { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool valid_box(const Aabb& b) noexcept {
    return finite(b.min) && finite(b.max) && b.min.x < b.max.x && b.min.y < b.max.y && b.min.z < b.max.z;
}

struct Bounds {
    float lo[3];
    float hi[3];
};

Bounds box_bounds(const Aabb& b) noexcept { return {{b.min.x, b.min.y, b.min.z}, {b.max.x, b.max.y, b.max.z}}; }

Bounds triangle_bounds(const Triangle& t) noexcept {
    Bounds b{{t.a.x, t.a.y, t.a.z}, {t.a.x, t.a.y, t.a.z}};
    for (const Vec3 p : {t.b, t.c}) {
        const float v[3] = {p.x, p.y, p.z};
        for (int i = 0; i < 3; ++i) {
            b.lo[i] = std::min(b.lo[i], v[i]);
            b.hi[i] = std::max(b.hi[i], v[i]);
        }
    }
    return b;
}

bool overlap(const float alo[3], const float ahi[3], const float blo[3], const float bhi[3]) noexcept {
    return alo[0] <= bhi[0] && ahi[0] >= blo[0] && alo[1] <= bhi[1] && ahi[1] >= blo[1] && alo[2] <= bhi[2] && ahi[2] >= blo[2];
}

// Entry distance of a ray into the box [lo, hi] within [0, max_t], or nothing. `axis_out`/`sign_out` give the entry face; an origin inside
// returns 0 with axis -1.
std::optional<double> ray_box_entry(const D3& o, const D3& inv, const D3& d, const float lo[3], const float hi[3], double max_t, int* axis_out = nullptr,
                                    double* sign_out = nullptr) noexcept {
    double t_enter = 0.0, t_exit = max_t;
    int axis = -1;
    double sign = 0.0;
    for (int i = 0; i < 3; ++i) {
        if (d[static_cast<std::size_t>(i)] == 0.0) {
            if (o[static_cast<std::size_t>(i)] < lo[i] || o[static_cast<std::size_t>(i)] > hi[i]) return std::nullopt;
            continue;
        }
        double t1 = (lo[i] - o[static_cast<std::size_t>(i)]) * inv[static_cast<std::size_t>(i)];
        double t2 = (hi[i] - o[static_cast<std::size_t>(i)]) * inv[static_cast<std::size_t>(i)];
        double face = -1.0;  // the face entered at t1 is the low one when moving +
        if (t1 > t2) {
            std::swap(t1, t2);
            face = 1.0;
        }
        if (t1 > t_enter) {
            t_enter = t1;
            axis = i;
            sign = face;
        }
        t_exit = std::min(t_exit, t2);
        if (t_enter > t_exit) return std::nullopt;
    }
    if (axis_out != nullptr) *axis_out = axis;
    if (sign_out != nullptr) *sign_out = sign;
    return t_enter;
}

// Möller-Trumbore in double, two-sided; edges and vertices count as hits.
std::optional<double> ray_triangle(const D3& o, const D3& d, const Triangle& tri, double max_t) noexcept {
    const D3 a = to_d3(tri.a), b = to_d3(tri.b), c = to_d3(tri.c);
    const D3 e1 = sub(b, a), e2 = sub(c, a);
    const D3 p = cross(d, e2);
    const double det = dot(e1, p);
    if (std::abs(det) < 1e-14) return std::nullopt;  // parallel (or a degenerate triangle, which the constructor rejects)
    const double inv = 1.0 / det;
    const D3 s = sub(o, a);
    const double u = dot(s, p) * inv;
    if (u < 0.0 || u > 1.0) return std::nullopt;
    const D3 q = cross(s, e1);
    const double v = dot(d, q) * inv;
    if (v < 0.0 || u + v > 1.0) return std::nullopt;
    const double t = dot(e2, q) * inv;
    if (t < 0.0 || t > max_t) return std::nullopt;
    return t;
}

} // namespace

StaticCollision::StaticCollision(std::span<const Aabb> boxes, std::span<const Triangle> triangles)
    : boxes_(boxes.begin(), boxes.end()), triangles_(triangles.begin(), triangles.end()) {
    for (const Aabb& b : boxes_) {
        if (!valid_box(b)) throw std::invalid_argument("StaticCollision: a box is not finite or has no volume");
    }
    for (const Triangle& t : triangles_) {
        if (!is_valid_triangle(t)) throw std::invalid_argument("StaticCollision: a triangle is not finite or has no area");
    }
    if (boxes_.size() + triangles_.size() > std::numeric_limits<std::uint32_t>::max() / 2) {
        throw std::invalid_argument("StaticCollision: too many primitives");
    }
    build();
}

void StaticCollision::build() {
    const std::size_t n = boxes_.size() + triangles_.size();
    if (n == 0) return;
    std::vector<Bounds> bounds(n);
    std::vector<std::array<float, 3>> centroid(n);
    for (std::size_t i = 0; i < n; ++i) {
        bounds[i] = i < boxes_.size() ? box_bounds(boxes_[i]) : triangle_bounds(triangles_[i - boxes_.size()]);
        for (int k = 0; k < 3; ++k) centroid[i][static_cast<std::size_t>(k)] = 0.5f * (bounds[i].lo[k] + bounds[i].hi[k]);
    }
    order_.resize(n);
    for (std::size_t i = 0; i < n; ++i) order_[i] = static_cast<std::uint32_t>(i);
    nodes_.reserve(2 * n / leaf_size + 2);

    struct Task {
        std::uint32_t node, begin, end, depth;
    };
    nodes_.push_back({});
    std::vector<Task> stack{{0, 0, static_cast<std::uint32_t>(n), 1}};
    while (!stack.empty()) {
        const Task task = stack.back();
        stack.pop_back();
        depth_ = std::max(depth_, task.depth);
        Node node{};
        for (int k = 0; k < 3; ++k) {
            node.lo[k] = std::numeric_limits<float>::infinity();
            node.hi[k] = -std::numeric_limits<float>::infinity();
        }
        float clo[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
        float chi[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        for (std::uint32_t i = task.begin; i < task.end; ++i) {
            const std::uint32_t id = order_[i];
            for (int k = 0; k < 3; ++k) {
                node.lo[k] = std::min(node.lo[k], bounds[id].lo[k]);
                node.hi[k] = std::max(node.hi[k], bounds[id].hi[k]);
                clo[k] = std::min(clo[k], centroid[id][static_cast<std::size_t>(k)]);
                chi[k] = std::max(chi[k], centroid[id][static_cast<std::size_t>(k)]);
            }
        }
        const std::uint32_t count = task.end - task.begin;
        if (count <= leaf_size || task.depth >= max_stack / 2) {
            node.first = task.begin;
            node.count = count;
            nodes_[task.node] = node;
            continue;
        }
        // Split at the median centroid along the widest axis of the centroids; ties in the centroid fall back to the id, so the order is total.
        int axis = 0;
        for (int k = 1; k < 3; ++k) {
            if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
        }
        const std::uint32_t mid = task.begin + count / 2;
        std::nth_element(order_.begin() + task.begin, order_.begin() + mid, order_.begin() + task.end, [&](std::uint32_t a, std::uint32_t b) {
            const float ca = centroid[a][static_cast<std::size_t>(axis)], cb = centroid[b][static_cast<std::size_t>(axis)];
            return ca != cb ? ca < cb : a < b;
        });
        const std::uint32_t left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        const std::uint32_t right = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        node.first = left;
        node.right = right;
        node.count = 0;
        nodes_[task.node] = node;
        stack.push_back({right, mid, task.end, task.depth + 1});
        stack.push_back({left, task.begin, mid, task.depth + 1});
    }
}

void StaticCollision::gather(const float lo[3], const float hi[3], std::vector<std::uint32_t>& ids) const {
    if (nodes_.empty()) return;
    std::array<std::uint32_t, max_stack> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = nodes_[stack[--top]];
        if (!overlap(lo, hi, node.lo, node.hi)) continue;
        if (node.count == 0) {
            stack[top++] = node.right;
            stack[top++] = node.first;
            continue;
        }
        for (std::uint32_t i = 0; i < node.count; ++i) {
            const std::uint32_t id = order_[node.first + i];
            const Bounds b = id < boxes_.size() ? box_bounds(boxes_[id]) : triangle_bounds(triangles_[id - boxes_.size()]);
            if (overlap(lo, hi, b.lo, b.hi)) ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
}

void StaticCollision::query_region(const Aabb& region, std::vector<StaticPrimitive>& out) const {
    const float lo[3] = {region.min.x, region.min.y, region.min.z}, hi[3] = {region.max.x, region.max.y, region.max.z};
    std::vector<std::uint32_t> ids;
    gather(lo, hi, ids);
    for (const std::uint32_t id : ids) {
        out.push_back(id < boxes_.size() ? StaticPrimitive{StaticPrimitive::Kind::Box, id}
                                          : StaticPrimitive{StaticPrimitive::Kind::Triangle, id - boxes_.size()});
    }
}

std::optional<RayHit> StaticCollision::raycast(Vec3 origin, Vec3 direction, float max_distance) const {
    if (!finite(origin) || !finite(direction) || !std::isfinite(max_distance) || max_distance < 0.0f) {
        throw std::invalid_argument("raycast: origin and direction must be finite and max_distance finite and non-negative");
    }
    D3 d = to_d3(direction);
    const double length = std::sqrt(dot(d, d));
    if (!(length > 0.0) || !std::isfinite(length)) throw std::invalid_argument("raycast: the direction must not be zero");
    for (double& c : d) c /= length;
    const D3 o = to_d3(origin);
    const D3 inv = {d[0] != 0.0 ? 1.0 / d[0] : 0.0, d[1] != 0.0 ? 1.0 / d[1] : 0.0, d[2] != 0.0 ? 1.0 / d[2] : 0.0};
    if (nodes_.empty()) return std::nullopt;

    double best_t = max_distance;
    std::uint32_t best_id = std::numeric_limits<std::uint32_t>::max();
    int best_axis = -1;
    double best_sign = 0.0;
    std::array<std::uint32_t, max_stack> stack{};
    std::size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = nodes_[stack[--top]];
        const auto entry = ray_box_entry(o, inv, d, node.lo, node.hi, best_t);
        if (!entry) continue;  // beyond the best hit (a tie in distance is still visited: the entry is <= best_t)
        if (node.count == 0) {
            // Nearer child first, so far subtrees are usually culled; the order does not affect the result (ties resolve by id).
            const Node& l = nodes_[node.first];
            const Node& r = nodes_[node.right];
            const auto tl = ray_box_entry(o, inv, d, l.lo, l.hi, best_t);
            const auto tr = ray_box_entry(o, inv, d, r.lo, r.hi, best_t);
            if (tl && tr && *tr < *tl) {
                stack[top++] = node.first;
                stack[top++] = node.right;
            } else {
                stack[top++] = node.right;
                stack[top++] = node.first;
            }
            continue;
        }
        for (std::uint32_t i = 0; i < node.count; ++i) {
            const std::uint32_t id = order_[node.first + i];
            std::optional<double> t;
            int axis = -1;
            double sign = 0.0;
            if (id < boxes_.size()) {
                const Aabb& b = boxes_[id];
                const float lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
                t = ray_box_entry(o, inv, d, lo, hi, best_t, &axis, &sign);
            } else {
                t = ray_triangle(o, d, triangles_[id - boxes_.size()], best_t);
            }
            if (t && (*t < best_t || (*t == best_t && id < best_id))) {
                best_t = *t;
                best_id = id;
                best_axis = axis;
                best_sign = sign;
            }
        }
    }
    if (best_id == std::numeric_limits<std::uint32_t>::max()) return std::nullopt;

    RayHit hit;
    hit.distance = best_t;
    hit.point = {static_cast<float>(o[0] + d[0] * best_t), static_cast<float>(o[1] + d[1] * best_t), static_cast<float>(o[2] + d[2] * best_t)};
    if (best_id < boxes_.size()) {
        hit.primitive = {StaticPrimitive::Kind::Box, best_id};
        if (best_axis >= 0) {
            D3 n{0, 0, 0};
            n[static_cast<std::size_t>(best_axis)] = best_sign;
            hit.normal = {static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2])};
        } else {
            hit.normal = {static_cast<float>(-d[0]), static_cast<float>(-d[1]), static_cast<float>(-d[2])};  // started inside
        }
    } else {
        hit.primitive = {StaticPrimitive::Kind::Triangle, best_id - boxes_.size()};
        const Vec3 n = triangle_normal(triangles_[best_id - boxes_.size()]);
        const double facing = dot(to_d3(n), d);
        const double s = facing > 0.0 ? -1.0 : 1.0;  // face the origin
        hit.normal = {static_cast<float>(n.x * s), static_cast<float>(n.y * s), static_cast<float>(n.z * s)};
    }
    return hit;
}

namespace {

void swept_region(const Capsule& capsule, Vec3 displacement, float lo[3], float hi[3]) noexcept {
    const double r = capsule.radius + region_margin;
    const double f[3] = {capsule.feet.x, capsule.feet.y, capsule.feet.z};
    const double d[3] = {displacement.x, displacement.y, displacement.z};
    for (int i = 0; i < 3; ++i) {
        const double below = i == 2 ? region_margin : r;
        const double above = i == 2 ? capsule.height + region_margin : r;
        const double a = f[i] - below, b = f[i] + d[i] - below;
        const double c = f[i] + above, e = f[i] + d[i] + above;
        lo[i] = static_cast<float>(std::nextafter(static_cast<float>(std::min(a, b)), -std::numeric_limits<float>::infinity()));
        hi[i] = static_cast<float>(std::nextafter(static_cast<float>(std::max(c, e)), std::numeric_limits<float>::infinity()));
    }
}

} // namespace

std::optional<StaticCapsuleHit> StaticCollision::sweep_capsule(const Capsule& capsule, Vec3 displacement) const {
    QueryScratch scratch;
    return sweep_capsule(capsule, displacement, scratch);
}

std::optional<StaticCapsuleHit> StaticCollision::sweep_capsule(const Capsule& capsule, Vec3 displacement, QueryScratch& scratch) const {
    check_sweep_envelope(capsule, displacement);  // also validates the capsule and the displacement, whatever the geometry holds
    float lo[3], hi[3];
    swept_region(capsule, displacement, lo, hi);
    scratch.ids.clear();
    scratch.boxes.clear();
    scratch.triangles.clear();
    scratch.box_ids.clear();
    scratch.triangle_ids.clear();
    gather(lo, hi, scratch.ids);
    for (const std::uint32_t id : scratch.ids) {  // ascending: boxes, then triangles, each in index order
        if (id < boxes_.size()) {
            scratch.boxes.push_back(boxes_[id]);
            scratch.box_ids.push_back(id);
        } else {
            scratch.triangles.push_back(triangles_[id - boxes_.size()]);
            scratch.triangle_ids.push_back(id - boxes_.size());
        }
    }
    std::optional<StaticCapsuleHit> best;
    if (const auto hit = sweep_capsule_boxes(capsule, displacement, scratch.boxes)) {
        best = StaticCapsuleHit{*hit, {StaticPrimitive::Kind::Box, scratch.box_ids[hit->index]}};
        best->hit.index = scratch.box_ids[hit->index];
    }
    if (const auto hit = sweep_capsule_triangles(capsule, displacement, scratch.triangles)) {
        // A box wins an exact tie (it is the analytic result); a triangle must be strictly earlier.
        if (!best || hit->contact_t < best->hit.contact_t) {
            best = StaticCapsuleHit{*hit, {StaticPrimitive::Kind::Triangle, scratch.triangle_ids[hit->index]}};
            best->hit.index = scratch.triangle_ids[hit->index];
        }
    }
    return best;
}

bool StaticCollision::overlaps_capsule(const Capsule& capsule) const {
    QueryScratch scratch;
    return overlaps_capsule(capsule, scratch);
}

bool StaticCollision::overlaps_capsule(const Capsule& capsule, QueryScratch& scratch) const {
    check_sweep_envelope(capsule, Vec3{0.0f, 0.0f, 0.0f});
    float lo[3], hi[3];
    swept_region(capsule, Vec3{0.0f, 0.0f, 0.0f}, lo, hi);
    scratch.ids.clear();
    gather(lo, hi, scratch.ids);
    for (const std::uint32_t id : scratch.ids) {
        if (id < boxes_.size() ? capsule_box_push_out(capsule, boxes_[id]).has_value() : capsule_triangle_push_out(capsule, triangles_[id - boxes_.size()]).has_value()) {
            return true;
        }
    }
    return false;
}

} // namespace saige
