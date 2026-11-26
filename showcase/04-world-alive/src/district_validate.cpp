#include "district_validate.hpp"

#include "district_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace saige::alive {

namespace {

[[nodiscard]] double overlap_area(const Rect& a, const Rect& b) {
    const double w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    const double h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    return w > 0.0 && h > 0.0 ? w * h : 0.0;
}

[[nodiscard]] Rect plan_of(const Aabb& b) {
    return {static_cast<double>(b.min.x), static_cast<double>(b.min.y), static_cast<double>(b.max.x), static_cast<double>(b.max.y)};
}

[[nodiscard]] Rect expand(const Rect& r, double m) { return {r.x0 - m, r.y0 - m, r.x1 + m, r.y1 + m}; }

[[nodiscard]] std::string fmt(const char* f, double a = 0, double b = 0, double c = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

void add(ValidationReport& r, const char* code, Severity s, std::string msg, double x, double y, std::vector<std::uint32_t> ids) {
    Finding f;
    f.code = code;
    f.severity = s;
    f.message = std::move(msg);
    f.x = x;
    f.y = y;
    f.ids = std::move(ids);
    r.findings.push_back(std::move(f));
}

[[nodiscard]] bool inside_other_building(const District& d, const Building& self, double x, double y) {
    for (const Building& o : d.buildings) {
        if (o.id == self.id) continue;
        if (o.footprint.contains(x, y)) return true;
    }
    return false;
}

// Parking bays are 2 m wide and 6.2 m long, parallel to the x axis (the generator parks along the east-west streets).
[[nodiscard]] Rect spot_rect(const ParkingSpot& s) { return {s.x - 3.1, s.y - 1.0, s.x + 3.1, s.y + 1.0}; }

} // namespace

const char* severity_name(Severity s) {
    switch (s) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    case Severity::Critical: return "critical";
    }
    return "?";
}

std::size_t ValidationReport::count(const std::string& code) const {
    return static_cast<std::size_t>(std::count_if(findings.begin(), findings.end(), [&](const Finding& f) { return f.code == code; }));
}

std::size_t ValidationReport::count_at_least(Severity s) const {
    return static_cast<std::size_t>(std::count_if(findings.begin(), findings.end(), [&](const Finding& f) { return f.severity >= s; }));
}

FaceFrame face_frame(const Rect& r, Leg leg) {
    const auto dir = leg_dir(leg);
    FaceFrame f;
    f.out = {dir[0], dir[1]};
    f.u = {-dir[1], dir[0]};
    switch (leg) {
    case Leg::South: f.origin = {r.x0, r.y0}; f.length = r.w(); break;
    case Leg::East: f.origin = {r.x1, r.y0}; f.length = r.h(); break;
    case Leg::North: f.origin = {r.x1, r.y1}; f.length = r.w(); break;
    case Leg::West: f.origin = {r.x0, r.y1}; f.length = r.h(); break;
    }
    return f;
}

std::vector<std::pair<double, double>> exposed_face_spans(const District& d, const Building& b, Leg leg, double step, double min_span) {
    const FaceFrame f = face_frame(b.footprint, leg);
    std::vector<std::pair<double, double>> spans;
    const int n = std::max(1, static_cast<int>(std::ceil(f.length / step)));
    double open = -1.0;
    for (int i = 0; i <= n; ++i) {
        const double s = std::min(f.length, f.length * static_cast<double>(i) / static_cast<double>(n));
        // Probe a little outside the face, at the cell's centre so that a neighbour that starts or ends exactly on a cell boundary is classed by the cell it covers.
        const double sc = i < n ? std::min(f.length, f.length * (static_cast<double>(i) + 0.5) / static_cast<double>(n)) : s;
        const double px = f.origin[0] + f.u[0] * sc + f.out[0] * 0.3, py = f.origin[1] + f.u[1] * sc + f.out[1] * 0.3;
        const bool covered = inside_other_building(d, b, px, py);
        if (i == n) {
            if (open >= 0.0) spans.push_back({open, f.length});
            break;
        }
        if (!covered && open < 0.0) open = s;
        if (covered && open >= 0.0) {
            spans.push_back({open, s});
            open = -1.0;
        }
    }
    std::erase_if(spans, [&](const std::pair<double, double>& p) { return p.second - p.first < min_span; });
    return spans;
}

bool legacy_face_hidden(const District& d, const Building& b, Leg leg) {
    const Rect& fp = b.footprint;
    const auto dir = leg_dir(leg);
    const double mx = fp.cx() + dir[0] * (fp.w() * 0.5 + 1.0) * (dir[0] != 0 ? 1.0 : 0.0);
    const double my = fp.cy() + dir[1] * (fp.h() * 0.5 + 1.0) * (dir[1] != 0 ? 1.0 : 0.0);
    return inside_other_building(d, b, mx, my);
}

unsigned interior_wall_face_mask(const Rect& fp, const Rect& w, double eps) {
    unsigned mask = 63;
    if (std::abs(w.x0 - fp.x0) < eps) mask &= ~1u;
    if (std::abs(w.x1 - fp.x1) < eps) mask &= ~2u;
    if (std::abs(w.y0 - fp.y0) < eps) mask &= ~4u;
    if (std::abs(w.y1 - fp.y1) < eps) mask &= ~8u;
    return mask;
}

ValidationReport validate_district(const District& d) {
    ValidationReport rep;

    // ---- buildings ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    for (std::size_t i = 0; i < d.buildings.size(); ++i) {
        const Building& a = d.buildings[i];
        for (std::size_t j = i + 1; j < d.buildings.size(); ++j) {
            const Building& b = d.buildings[j];
            const double area = overlap_area(a.footprint, b.footprint);
            if (area > 0.05) {
                add(rep, "BLD_OVERLAP", Severity::Critical, fmt("buildings overlap by %.2f m2", area), 0.5 * (std::max(a.footprint.x0, b.footprint.x0) + std::min(a.footprint.x1, b.footprint.x1)),
                    0.5 * (std::max(a.footprint.y0, b.footprint.y0) + std::min(a.footprint.y1, b.footprint.y1)), {a.id, b.id});
            }
        }
        for (std::size_t k = 0; k < d.roadway.size(); ++k) {
            const double area = overlap_area(a.footprint, d.roadway[k]);
            if (area > 0.05) add(rep, "BLD_ON_ROAD", Severity::Critical, fmt("building stands on the roadway (%.2f m2)", area), a.footprint.cx(), a.footprint.cy(), {a.id});
        }
        for (std::size_t k = 0; k < d.sidewalk.size(); ++k) {
            const double area = overlap_area(a.footprint, d.sidewalk[k]);
            if (area > 0.05) add(rep, "BLD_ON_SIDEWALK", Severity::Error, fmt("building stands on the sidewalk (%.2f m2)", area), a.footprint.cx(), a.footprint.cy(), {a.id});
        }
    }
    // Faces of two different buildings that face the same way and lie in the same plane with an overlapping extent draw on top of each other: the depth buffer cannot tell them apart.
    for (std::size_t i = 0; i < d.buildings.size(); ++i) {
        for (std::size_t j = i + 1; j < d.buildings.size(); ++j) {
            const Building& a = d.buildings[i];
            const Building& b = d.buildings[j];
            for (int li = 0; li < 4; ++li) {
                const Leg leg = static_cast<Leg>(li);
                const bool horiz = (leg == Leg::South || leg == Leg::North);   // faces whose plane is y = const
                const auto plane = [&](const Rect& r) { return horiz ? (leg == Leg::South ? r.y0 : r.y1) : (leg == Leg::East ? r.x1 : r.x0); };
                if (std::abs(plane(a.footprint) - plane(b.footprint)) > 0.005) continue;
                const double lo = std::max(horiz ? a.footprint.x0 : a.footprint.y0, horiz ? b.footprint.x0 : b.footprint.y0);
                const double hi = std::min(horiz ? a.footprint.x1 : a.footprint.y1, horiz ? b.footprint.x1 : b.footprint.y1);
                if (hi - lo > 0.1) {
                    add(rep, "WALL_COPLANAR", Severity::Critical, fmt("two buildings' %.1f m wide faces lie in the same plane and fight for the depth buffer", hi - lo),
                        horiz ? 0.5 * (lo + hi) : plane(a.footprint), horiz ? plane(a.footprint) : 0.5 * (lo + hi), {a.id, b.id});
                }
            }
        }
    }

    // ---- props -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    struct PropBox {
        std::uint32_t index;
        Aabb box;
        Rect plan;
    };
    std::vector<PropBox> boxes;
    for (std::size_t i = 0; i < d.props.size(); ++i) {
        Aabb b{};
        if (!prop_box(d.props[i], b)) continue;
        boxes.push_back({static_cast<std::uint32_t>(i), b, plan_of(b)});
    }
    for (const PropBox& pb : boxes) {
        const StreetProp& p = d.props[pb.index];
        const bool car = p.type == "parked_car";
        if (!car) {
            for (const Rect& road : d.roadway) {
                const double area = overlap_area(pb.plan, road);
                if (area > 1e-3) {
                    add(rep, "PROP_ON_ROAD", Severity::Error, fmt("prop stands on the roadway (%.3f m2)", area) + " [" + p.type + "]", p.x, p.y, {pb.index});
                    break;
                }
            }
            for (std::size_t k = 0; k < d.parking.size(); ++k) {
                if (overlap_area(pb.plan, spot_rect(d.parking[k])) > 1e-3) {
                    add(rep, "PROP_IN_PARKING", Severity::Error, "furniture stands in a parking bay [" + p.type + "]", p.x, p.y, {pb.index, static_cast<std::uint32_t>(k)});
                    break;
                }
            }
        }
        for (const Building& bd : d.buildings) {
            const double area = overlap_area(pb.plan, bd.footprint);
            if (area > 1e-3) {
                add(rep, "PROP_IN_BUILDING", Severity::Error, fmt("outdoor prop inside a building footprint (%.3f m2)", area) + " [" + p.type + "]", p.x, p.y, {pb.index, bd.id});
                break;
            }
        }
    }
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        for (std::size_t j = i + 1; j < boxes.size(); ++j) {
            const PropBox& a = boxes[i];
            const PropBox& b = boxes[j];
            if (a.box.min.z >= b.box.max.z || b.box.min.z >= a.box.max.z) continue;
            const double area = overlap_area(a.plan, b.plan);
            if (area > 1e-3) {
                add(rep, "PROP_OVERLAP", area > 0.02 ? Severity::Error : Severity::Warning, fmt("two props overlap by %.3f m2", area) + " [" + d.props[a.index].type + " / " + d.props[b.index].type + "]",
                    d.props[a.index].x, d.props[a.index].y, {a.index, b.index});
            }
        }
    }
    for (std::size_t k = 0; k < d.parking.size(); ++k) {
        const Rect r = spot_rect(d.parking[k]);
        for (const Building& bd : d.buildings) {
            if (overlap_area(r, bd.footprint) > 0.05) {
                add(rep, "PARKING_ON_BUILDING", Severity::Error, "a parking bay overlaps a building", d.parking[k].x, d.parking[k].y, {static_cast<std::uint32_t>(k), bd.id});
                break;
            }
        }
        for (const Rect& sw : d.sidewalk) {
            if (overlap_area(r, sw) > 0.05) {
                add(rep, "PARKING_ON_SIDEWALK", Severity::Warning, "a parking bay overlaps the sidewalk", d.parking[k].x, d.parking[k].y, {static_cast<std::uint32_t>(k)});
                break;
            }
        }
        bool on_road = false;
        for (const Rect& road : d.roadway) on_road = on_road || overlap_area(r, road) >= 0.95 * r.w() * r.h();
        if (!on_road) add(rep, "PARKING_OFF_ROAD", Severity::Warning, "a parking bay is not on the roadway", d.parking[k].x, d.parking[k].y, {static_cast<std::uint32_t>(k)});
    }

    // ---- interiors -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    for (const Interior& in : d.interiors) {
        const Building* bd = nullptr;
        for (const Building& b : d.buildings)
            if (b.id == in.building) bd = &b;
        const auto furniture_check = [&](const std::vector<StreetProp>& list, const std::vector<Rect>& walls, const char* tag) {
            std::vector<std::pair<Rect, std::size_t>> plans;
            for (std::size_t i = 0; i < list.size(); ++i) {
                Aabb b{};
                if (!prop_box(list[i], b)) continue;
                const Rect pr = plan_of(b);
                plans.push_back({pr, i});
                const bool inside = pr.x0 >= in.room.x0 - 1e-6 && pr.y0 >= in.room.y0 - 1e-6 && pr.x1 <= in.room.x1 + 1e-6 && pr.y1 <= in.room.y1 + 1e-6;
                if (!inside) add(rep, "ROOM_FURNITURE_OUTSIDE", Severity::Error, std::string(tag) + " furniture pokes through a wall [" + list[i].type + "]", list[i].x, list[i].y, {in.building, static_cast<std::uint32_t>(i)});
                for (const Rect& w : walls) {
                    if (overlap_area(pr, w) > 1e-3) {
                        add(rep, "ROOM_FURNITURE_IN_WALL", Severity::Error, std::string(tag) + " furniture intersects a wall [" + list[i].type + "]", list[i].x, list[i].y, {in.building, static_cast<std::uint32_t>(i)});
                        break;
                    }
                }
            }
            for (std::size_t i = 0; i < plans.size(); ++i)
                for (std::size_t j = i + 1; j < plans.size(); ++j) {
                    const double area = overlap_area(plans[i].first, plans[j].first);
                    if (area > 0.02)
                        add(rep, "ROOM_FURNITURE_OVERLAP", Severity::Error, fmt("%.3f m2 of two pieces of furniture overlap", area) + std::string(" [") + list[plans[i].second].type + " / " + list[plans[j].second].type + "]",
                            list[plans[i].second].x, list[plans[i].second].y, {in.building, static_cast<std::uint32_t>(plans[i].second), static_cast<std::uint32_t>(plans[j].second)});
                }
        };
        furniture_check(in.furniture, in.walls, "ground-floor");
        if (!in.upper_furniture.empty()) furniture_check(in.upper_furniture, in.upper_walls, "upper-floor");
        // The door must be reachable: a 1.0 m wide strip from the doorway into the room is clear of furniture.
        if (bd != nullptr) {
            const auto dir = leg_dir(bd->front);
            const double x0 = bd->door_x, y0 = bd->door_y;
            const Rect strip = std::abs(dir[0]) > 0.5 ? Rect{std::min(x0, x0 - dir[0] * 2.5), y0 - 0.5, std::max(x0, x0 - dir[0] * 2.5), y0 + 0.5} : Rect{x0 - 0.5, std::min(y0, y0 - dir[1] * 2.5), x0 + 0.5, std::max(y0, y0 - dir[1] * 2.5)};
            for (std::size_t i = 0; i < in.furniture.size(); ++i) {
                Aabb b{};
                if (prop_box(in.furniture[i], b) && overlap_area(plan_of(b), strip) > 1e-3)
                    add(rep, "DOOR_BLOCKED", Severity::Error, "furniture blocks the doorway [" + in.furniture[i].type + "]", in.furniture[i].x, in.furniture[i].y, {in.building, static_cast<std::uint32_t>(i)});
            }
        }
        if (in.has_stairs) {
            const Rect foot = expand(in.stair_footprint, 0.3);
            const Rect bottom = {in.stair_bottom[0] - 0.8, in.stair_bottom[1] - 0.8, in.stair_bottom[0] + 0.8, in.stair_bottom[1] + 0.8};
            const Rect top = {in.stair_top[0] - 0.8, in.stair_top[1] - 0.8, in.stair_top[0] + 0.8, in.stair_top[1] + 0.8};
            for (std::size_t i = 0; i < in.furniture.size(); ++i) {
                Aabb b{};
                if (!prop_box(in.furniture[i], b)) continue;
                const Rect pr = plan_of(b);
                if (overlap_area(pr, foot) > 1e-3 || overlap_area(pr, bottom) > 1e-3)
                    add(rep, "STAIR_BLOCKED", Severity::Error, "furniture blocks the staircase or its foot landing [" + in.furniture[i].type + "]", in.furniture[i].x, in.furniture[i].y, {in.building, static_cast<std::uint32_t>(i)});
            }
            for (std::size_t i = 0; i < in.upper_furniture.size(); ++i) {
                Aabb b{};
                if (!prop_box(in.upper_furniture[i], b)) continue;
                if (overlap_area(plan_of(b), top) > 1e-3 || overlap_area(plan_of(b), foot) > 1e-3)
                    add(rep, "STAIR_TOP_BLOCKED", Severity::Error, "furniture blocks the top landing of the staircase [" + in.upper_furniture[i].type + "]", in.upper_furniture[i].x, in.upper_furniture[i].y, {in.building, static_cast<std::uint32_t>(i)});
            }
        }
    }
    return rep;
}

} // namespace saige::alive
