#include "scout.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ostream>
#include <set>
#include <sstream>

namespace saige::alive {

ScoutReport scout_world(const WorldConfig& config, double seconds, double dt) {
    World w(config);
    ScoutReport rep;
    std::set<std::uint64_t> seen_crossing_now, seen_inside;
    for (double t = 0.0; t < seconds; t += dt) {
        w.step(dt);
        const double now = w.time_seconds();
        for (const TrafficEvent& e : w.take_traffic_events()) {
            ScoutEvent se;
            se.t = now;
            se.type = e.type;
            se.vehicle = e.vehicle;
            se.x = e.x;
            se.y = e.y;
            se.energy = e.energy;
            rep.events.push_back(se);
            if (e.type == TrafficEvent::Type::Accident && rep.accident_t < 0) {
                rep.accident_t = now;
                rep.accident_x = e.x;
                rep.accident_y = e.y;
                rep.accident_vehicle = e.vehicle;
            }
            if (e.type == TrafficEvent::Type::EmergencyOnScene && rep.onscene_t < 0) rep.onscene_t = now;
            if (e.type == TrafficEvent::Type::Cleared && rep.cleared_t < 0) rep.cleared_t = now;
        }
        std::set<std::uint64_t> crossing_now;
        for (const PedView& p : w.pedestrians()) {
            if (p.state == PedState::Crossing) {
                crossing_now.insert(p.id);
                if (!seen_crossing_now.count(p.id)) rep.crossings.push_back({now, p.id, p.x, p.y});
            }
            if (p.inside_building && p.visible && !seen_inside.count(p.id)) {
                seen_inside.insert(p.id);
                rep.interior_visits.push_back({now, p.id, p.x, p.y, p.floor});
            }
        }
        seen_crossing_now = crossing_now;
    }
    return rep;
}

namespace {

// One whitespace-separated token as a finite double that consumes the whole token ("12abc", "nan", "inf", "" are rejected).
bool parse_double(const std::string& tok, double& v) {
    if (tok.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(tok.c_str(), &end);
    if (end != tok.c_str() + tok.size() || errno == ERANGE || !std::isfinite(d)) return false;
    v = d;
    return true;
}

bool parse_uint(const std::string& tok, std::uint64_t limit, std::uint64_t& v) {
    if (tok.empty() || tok.size() > 20) return false;
    std::uint64_t r = 0;
    for (char c : tok) {
        if (c < '0' || c > '9') return false;
        r = r * 10 + static_cast<std::uint64_t>(c - '0');
        if (r > limit) return false;
    }
    v = r;
    return true;
}

constexpr double kMaxScoutTime = 1.0e6;      // seconds of simulation: a corrupted exponent is not a time
constexpr double kMaxScoutCoord = 1.0e5;     // metres
constexpr double kMaxScoutEnergy = 1.0e12;   // joules

// -1 ("did not happen") or a time in (0, kMaxScoutTime]. Zero is rejected: it is what a failed numeric read leaves behind.
bool valid_time_or_never(double t) { return t == -1.0 || (t > 0.0 && t <= kMaxScoutTime); }

} // namespace

ScoutParse parse_scout_report(std::string_view text, ScoutReport& out, bool allow_legacy) {
    ScoutParse res;
    ScoutReport rep;
    auto fail = [&](int line, const std::string& what) {
        res.ok = false;
        res.error = (line > 0 ? "line " + std::to_string(line) + ": " : std::string()) + what;
        return res;
    };
    if (text.empty()) return fail(0, "empty scout file (an intentional no-event report still has a header, an accident record and an end line)");
    if (text.find('\0') != std::string_view::npos) return fail(0, "binary data (NUL) in a scout file");
    if (text.back() != '\n') return fail(0, "truncated: the last line has no newline");

    std::istringstream in{std::string(text)};
    std::string line;
    int ln = 0;
    bool header = false, have_accident = false, have_end = false, have_seed = false;
    std::uint64_t declared_events = 0;
    double last_event_t = 0.0;
    bool first = true;
    while (std::getline(in, line)) {
        ++ln;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::vector<std::string> tok;
        for (std::string t; ls >> t;) tok.push_back(t);
        if (tok.empty()) return fail(ln, "blank line");
        if (have_end) return fail(ln, "data after the end line");
        if (first) {
            first = false;
            if (tok[0] == "saige-scout") {
                std::uint64_t ver = 0;
                if (tok.size() != 2 || !parse_uint(tok[1], 1000, ver)) return fail(ln, "malformed header");
                if (ver != 1) return fail(ln, "unsupported scout format version " + tok[1]);
                header = true;
                continue;
            }
            if (!allow_legacy) return fail(ln, "no 'saige-scout 1' header (a headerless legacy file is only read with allow_legacy)");
            res.legacy = true;
        }
        const std::string& tag = tok[0];
        if (tag == "seed") {
            std::uint64_t s = 0;
            if (!header) return fail(ln, "seed record in a file without header");
            if (have_seed) return fail(ln, "duplicate seed record");
            if (have_accident) return fail(ln, "seed record after the accident record");
            if (tok.size() != 2 || !parse_uint(tok[1], 0xFFFFFFFFull, s)) return fail(ln, "malformed seed record");
            res.seed = static_cast<std::uint32_t>(s);
            res.has_seed = have_seed = true;
        } else if (tag == "accident") {
            if (have_accident) return fail(ln, "duplicate accident record");
            if (header && !have_seed) return fail(ln, "accident record before the seed record");
            if (tok.size() != 7) return fail(ln, "accident record needs 6 fields (t x y vehicle onscene cleared)");
            std::uint64_t veh = 0;
            if (!parse_double(tok[1], rep.accident_t) || !parse_double(tok[2], rep.accident_x) || !parse_double(tok[3], rep.accident_y) || !parse_uint(tok[4], 0xFFFFFFFFull, veh) ||
                !parse_double(tok[5], rep.onscene_t) || !parse_double(tok[6], rep.cleared_t))
                return fail(ln, "accident record has a non-numeric, non-finite or out-of-range field");
            rep.accident_vehicle = static_cast<std::uint32_t>(veh);
            if (!valid_time_or_never(rep.accident_t) || !valid_time_or_never(rep.onscene_t) || !valid_time_or_never(rep.cleared_t)) return fail(ln, "accident, on-scene and cleared times must be -1 or in (0, 1e6]");
            if (std::fabs(rep.accident_x) > kMaxScoutCoord || std::fabs(rep.accident_y) > kMaxScoutCoord) return fail(ln, "accident position out of range");
            if (rep.accident_t < 0.0 && (rep.onscene_t >= 0.0 || rep.cleared_t >= 0.0)) return fail(ln, "a response without an accident");
            if (rep.onscene_t > 0.0 && rep.accident_t > 0.0 && rep.onscene_t < rep.accident_t) return fail(ln, "on scene before the accident");
            if (rep.cleared_t > 0.0 && rep.onscene_t > 0.0 && rep.cleared_t < rep.onscene_t) return fail(ln, "cleared before on scene");
            have_accident = true;
        } else if (tag == "event") {
            if (!have_accident) return fail(ln, "event before the accident record");
            const std::size_t want = header ? 7 : 6;
            if (tok.size() != want) return fail(ln, std::string("event record needs ") + (header ? "6 fields (t type vehicle x y energy)" : "5 fields (t type vehicle x y)"));
            ScoutEvent e;
            std::uint64_t type = 0, veh = 0;
            double energy = 0.0;
            if (!parse_double(tok[1], e.t) || !parse_uint(tok[2], 4, type) || !parse_uint(tok[3], 0xFFFFFFFFull, veh) || !parse_double(tok[4], e.x) || !parse_double(tok[5], e.y) || (header && !parse_double(tok[6], energy)))
                return fail(ln, "event record has a non-numeric, non-finite or out-of-range field (type must be 0..4)");
            if (!(e.t >= 0.0 && e.t <= kMaxScoutTime) || e.t < last_event_t) return fail(ln, "event time out of range or earlier than the previous event");
            if (std::fabs(e.x) > kMaxScoutCoord || std::fabs(e.y) > kMaxScoutCoord || !(energy >= 0.0 && energy <= kMaxScoutEnergy)) return fail(ln, "event position or energy out of range");
            last_event_t = e.t;
            e.type = static_cast<TrafficEvent::Type>(type);
            e.vehicle = static_cast<std::uint32_t>(veh);
            e.energy = static_cast<float>(energy);
            rep.events.push_back(e);
        } else if (tag == "end") {
            if (!header) return fail(ln, "end record in a legacy file");
            if (tok.size() != 2 || !parse_uint(tok[1], 100000000ull, declared_events)) return fail(ln, "malformed end record");
            have_end = true;
        } else {
            return fail(ln, "unknown record '" + tag + "'");
        }
    }
    if (!have_accident) return fail(0, "no accident record (an intentional no-event report has one with t = -1)");
    if (header && !have_end) return fail(0, "truncated: no end line");
    if (header && declared_events != rep.events.size()) return fail(0, "truncated or edited: end line declares " + std::to_string(declared_events) + " events, the file has " + std::to_string(rep.events.size()));
    // The accident record must agree with the events: the first Accident event is the accident.
    const ScoutEvent* first_accident = nullptr;
    for (const ScoutEvent& e : rep.events)
        if (e.type == TrafficEvent::Type::Accident) { first_accident = &e; break; }
    if (rep.accident_t > 0.0) {
        if (!first_accident) {
            if (header) return fail(0, "accident record without an Accident event");
        } else if (std::fabs(first_accident->t - rep.accident_t) > 1e-6 || first_accident->vehicle != rep.accident_vehicle) {
            return fail(0, "accident record disagrees with the first Accident event");
        }
    } else if (first_accident) {
        return fail(0, "Accident event but the accident record says none");
    }
    res.ok = true;
    out = std::move(rep);
    return res;
}

void write_scout_report(std::ostream& out, const ScoutReport& rep, std::uint32_t seed) {
    const auto old_precision = out.precision(17);
    out << "saige-scout 1\nseed " << seed << "\n";
    out << "accident " << rep.accident_t << " " << rep.accident_x << " " << rep.accident_y << " " << rep.accident_vehicle << " " << rep.onscene_t << " " << rep.cleared_t << "\n";
    for (const ScoutEvent& e : rep.events) out << "event " << e.t << " " << static_cast<int>(e.type) << " " << e.vehicle << " " << e.x << " " << e.y << " " << static_cast<double>(e.energy) << "\n";
    out << "end " << rep.events.size() << "\n";
    out.precision(old_precision);
}

} // namespace saige::alive
