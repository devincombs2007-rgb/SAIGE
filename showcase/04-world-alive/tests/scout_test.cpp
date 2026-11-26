// Showcase 04: the scout file. The producers (saige_alive, alive_scout) and the consumer (the director, through saige_alive) share one writer and one strict parser (src/scout.cpp). History: the
// documented way to make a scout file ("alive_scout 8 230 > scout8.txt") wrote a one-line human summary that saige_alive then parsed as an accident at t = 0 with no response, so every
// tool that followed that recipe ran a director with no accident. These tests pin the format: the round trip, the director's choice of the accident from a known seed, the legacy layout of
// the delivered film's own file, and a list of malformed inputs that must each be refused (including every truncation of a valid file).
#include "director.hpp"
#include "scout.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

using namespace saige::alive;

namespace {

int failures = 0;

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        if (!(cond)) {                                                                                        \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                                     \
            std::printf(__VA_ARGS__);                                                                         \
            std::printf("\n");                                                                                \
            ++failures;                                                                                       \
        }                                                                                                     \
    } while (0)

// The first lines of the scout file the delivered Showcase 04 film was shot from (seed 8; layout without header, seed or end line). Provenance caveat: this is the content of the file on the
// production machine as found afterwards, in the app-written layout; that it is byte for byte what the render read is not proven by anything kept with the film.
const char* const kFilmLegacy =
    "accident 106.19999999999526 25.613384246826172 65.501319885253906 8 152.03333333334646 177.03333333336636\n"
    "event 106.19999999999526 0 8 25.613384246826172 65.501319885253906\n"
    "event 106.19999999999526 1 8 25.613384246826172 65.501319885253906\n"
    "event 112.23333333332825 2 16 -49.281647499729303 -61\n"
    "event 130.86666666666295 0 17 95.503257751464844 -54\n";

WorldConfig film_config(std::uint32_t seed) {
    WorldConfig c;
    c.seed = seed;
    c.time_scale = 47.0;
    c.start_minute = 17.5 * 60.0;
    return c;
}

std::string serialized(const ScoutReport& r, std::uint32_t seed) {
    std::ostringstream o;
    write_scout_report(o, r, seed);
    return o.str();
}

bool parses(const std::string& text, bool legacy = false) {
    ScoutReport r;
    return parse_scout_report(text, r, legacy).ok;
}

std::string why(const std::string& text, bool legacy = false) {
    ScoutReport r;
    const ScoutParse p = parse_scout_report(text, r, legacy);
    return p.ok ? std::string("accepted") : p.error;
}

std::string replace_first(std::string s, const std::string& from, const std::string& to) {
    const auto pos = s.find(from);
    if (pos != std::string::npos) s.replace(pos, from.size(), to);
    return s;
}

bool same_report(const ScoutReport& a, const ScoutReport& b) {
    if (a.accident_t != b.accident_t || a.accident_x != b.accident_x || a.accident_y != b.accident_y || a.accident_vehicle != b.accident_vehicle || a.onscene_t != b.onscene_t || a.cleared_t != b.cleared_t || a.events.size() != b.events.size()) return false;
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        const ScoutEvent &x = a.events[i], &y = b.events[i];
        if (x.t != y.t || x.type != y.type || x.vehicle != y.vehicle || x.x != y.x || x.y != y.y || x.energy != y.energy) return false;
    }
    return true;
}

} // namespace

int main() {
    // 1. Round trip from the simulation, seed 8: write, parse, identical to the last bit; the accident is the one the delivered film has.
    const std::uint32_t seed = 8;
    const ScoutReport rep = scout_world(film_config(seed), 25.0 + 180.0 + 1.0);
    const std::string text = serialized(rep, seed);
    ScoutReport back;
    const ScoutParse pr = parse_scout_report(text, back);
    CHECK(pr.ok, "round trip refused: %s", pr.error.c_str());
    CHECK(pr.has_seed && pr.seed == seed && !pr.legacy, "seed %u legacy %d", pr.seed, pr.legacy ? 1 : 0);
    CHECK(same_report(rep, back), "the parsed report differs from the written one");
    CHECK(serialized(back, seed) == text, "write(parse(write(x))) differs from write(x)");
    CHECK(std::fabs(rep.accident_t - 106.19999999999526) < 1e-9 && rep.accident_vehicle == 8, "seed 8's accident is at %.9f s, vehicle %u (the delivered film's file: 106.199999999995 s, vehicle 8)", rep.accident_t, rep.accident_vehicle);
    CHECK(rep.onscene_t > rep.accident_t && rep.cleared_t > rep.onscene_t, "the response is not after the accident: %.1f %.1f %.1f", rep.accident_t, rep.onscene_t, rep.cleared_t);

    // 2. The director picks the accident from a known seed, and a report without one gives a different film.
    {
        World w(film_config(seed));
        const rend::Director with(w.district(), back, 180.0, 25.0);
        const double film_t = rep.accident_t - 25.0;
        int wreck = 0, scene = 0;
        for (const auto& s : with.list()) {
            if (s.second == "the wreck" && std::fabs(s.first - (film_t + 0.9)) < 1e-6) ++wreck;
            if (s.second == "at the scene") ++scene;
        }
        CHECK(wreck == 1 && scene == 1, "expected the wreck shot 0.9 s after the accident (film time %.1f) and one scene shot, found %d and %d", film_t, wreck, scene);
        ScoutReport none = back;
        none.accident_t = none.onscene_t = none.cleared_t = -1.0;
        none.events.clear();
        const rend::Director without(w.district(), none, 180.0, 25.0);
        bool any = false;
        for (const auto& s : without.list()) any = any || s.second == "the wreck" || s.second == "at the scene";
        CHECK(!any, "a report with no accident still gets the accident shots");
        CHECK(with.list() != without.list(), "the accident does not change the shot list");
    }

    // 3. An intentional "nothing happened" report is valid and is not an empty file.
    {
        ScoutReport none;
        none.accident_t = none.onscene_t = none.cleared_t = -1.0;
        const std::string t = serialized(none, 3);
        ScoutReport r;
        const ScoutParse p = parse_scout_report(t, r);
        CHECK(p.ok && r.accident_t == -1.0 && r.events.empty(), "no-event report: %s", p.error.c_str());
        CHECK(!parses(""), "an empty file was accepted");
        CHECK(!parses("\n"), "a blank file was accepted");
    }

    // 4. The film's own legacy layout: refused by default, read with allow_legacy, and identical to what the app wrote.
    {
        CHECK(!parses(kFilmLegacy), "a headerless file was read without allow_legacy");
        ScoutReport r;
        const ScoutParse p = parse_scout_report(kFilmLegacy, r, true);
        CHECK(p.ok && p.legacy && !p.has_seed, "legacy file: %s", p.error.c_str());
        CHECK(r.events.size() == 4 && r.accident_vehicle == 8 && std::fabs(r.accident_t - 106.2) < 1e-9, "legacy content: %zu events, accident %.3f", r.events.size(), r.accident_t);
        CHECK(!parses(replace_first(kFilmLegacy, "event 106.19999999999526 1 8", "event 106.19999999999526 1 9"), true), "legacy: the accident record disagreed with its event and was accepted");
    }

    // 5. The human summary (what the old alive_scout printed) is not a scout file, in either mode.
    {
        const std::string human = "seed 8: accident at 106.2 s (26, 66) vehicle 8; on scene 152.0; cleared 177.0; crossings 58; interior visits 2 [ped 1051 t 0 floor 0]\n";
        CHECK(!parses(human) && !parses(human, true), "the one-line summary was accepted: %s", why(human, true).c_str());
    }

    // 6. Malformed inputs, each one refused with a reason. `ok` is a valid file with two events, edited one way each time.
    {
        ScoutReport small;
        small.accident_t = 10.0;
        small.accident_x = 1.5;
        small.accident_y = -2.5;
        small.accident_vehicle = 3;
        small.onscene_t = 20.0;
        small.cleared_t = 30.0;
        small.events.push_back({10.0, TrafficEvent::Type::Accident, 3, 1.5, -2.5, 1000.0f});
        small.events.push_back({20.0, TrafficEvent::Type::EmergencyOnScene, 7, 1.0, 2.0, 0.0f});
        const std::string ok = serialized(small, 5);
        CHECK(parses(ok), "the control file was refused: %s", why(ok).c_str());
        struct Bad {
            const char* name;
            std::string text;
        };
        const std::string acc = "accident 10 1.5 -2.5 3 20 30\n";
        const Bad bad[] = {
            {"no header", replace_first(ok, "saige-scout 1\n", "")},
            {"unknown version", replace_first(ok, "saige-scout 1", "saige-scout 2")},
            {"no seed", replace_first(ok, "seed 5\n", "")},
            {"duplicate seed", replace_first(ok, "seed 5\n", "seed 5\nseed 5\n")},
            {"no end line", replace_first(ok, "end 2\n", "")},
            {"end count too high", replace_first(ok, "end 2", "end 3")},
            {"end count too low", replace_first(ok, "end 2", "end 1")},
            {"data after end", ok + "event 25 0 1 0 0 0\n"},
            {"no newline at the end", ok.substr(0, ok.size() - 1)},
            {"duplicate accident record", replace_first(ok, acc, acc + acc)},
            {"no accident record", replace_first(ok, acc, "")},
            {"event before accident", replace_first(ok, acc, "event 1 0 1 0 0 0\n" + acc)},
            {"event type 5", replace_first(ok, "event 20 3", "event 20 5")},
            {"event type -1", replace_first(ok, "event 20 3", "event 20 -1")},
            {"event type 1.5", replace_first(ok, "event 20 3", "event 20 1.5")},
            {"nan field", replace_first(ok, "event 20 3 7 1 2", "event 20 3 7 nan 2")},
            {"inf field", replace_first(ok, "event 20 3 7 1 2", "event 20 3 7 1 inf")},
            {"trailing junk in a number", replace_first(ok, "event 20 3 7 1 2", "event 20 3 7 1 2x")},
            {"negative energy", replace_first(ok, "event 10 1 3 1.5 -2.5 1000", "event 10 1 3 1.5 -2.5 -1")},
            {"huge time", replace_first(ok, "event 20 3", "event 1e30 4")},
            {"events out of order", replace_first(replace_first(ok, "event 10 1 3 1.5 -2.5 1000\n", ""), "end 2", "event 10 1 3 1.5 -2.5 1000\nend 2")},
            {"missing field", replace_first(ok, "event 20 3 7 1 2 0", "event 20 3 7 1 2")},
            {"extra field", replace_first(ok, "event 20 3 7 1 2 0", "event 20 3 7 1 2 0 9")},
            {"duplicate end line", ok + "end 2\n"},
            {"on-scene time zero", replace_first(ok, acc, "accident 10 1.5 -2.5 3 0 30\n")},
            {"cleared time zero", replace_first(ok, acc, "accident 10 1.5 -2.5 3 20 0\n")},
            {"accident time zero", replace_first(ok, acc, "accident 0 1.5 -2.5 3 20 30\n")},
            {"accident time -2", replace_first(ok, acc, "accident -2 1.5 -2.5 3 20 30\n")},
            {"response before the accident", replace_first(ok, acc, "accident 10 1.5 -2.5 3 5 30\n")},
            {"cleared before on scene", replace_first(ok, acc, "accident 10 1.5 -2.5 3 20 15\n")},
            {"accident record without an Accident event", replace_first(ok, "event 10 1 3 1.5 -2.5 1000\n", "")},
            {"accident record disagrees with its event", replace_first(ok, "event 10 1 3", "event 10 1 4")},
            {"Accident event but the record says none", replace_first(ok, acc, "accident -1 0 0 0 -1 -1\n")},
            {"unknown record", replace_first(ok, "end 2", "frobnicate 1\nend 2")},
            {"NUL byte", replace_first(ok, "seed 5", std::string("seed 5\0 ", 8))},
            {"seed too large", replace_first(ok, "seed 5", "seed 4294967296")},
        };
        for (const Bad& b : bad) CHECK(!parses(b.text), "'%s' was accepted", b.name);
        // The error names the problem: spot-check a few.
        CHECK(why(replace_first(ok, "end 2", "end 3")).find("declares 3") != std::string::npos, "end-count message: %s", why(replace_first(ok, "end 2", "end 3")).c_str());
        CHECK(why(replace_first(ok, acc, acc + acc)).find("line 4") != std::string::npos, "duplicate message: %s", why(replace_first(ok, acc, acc + acc)).c_str());

        // Every strict prefix of a valid file is refused (a file cut anywhere is never a smaller valid report); the whole file is not.
        int accepted_prefixes = 0;
        const std::string full = serialized(rep, seed);
        for (std::size_t n = 0; n < full.size(); ++n)
            if (parses(full.substr(0, n))) ++accepted_prefixes;
        {
            ScoutReport none;
            none.accident_t = none.onscene_t = none.cleared_t = -1.0;
            const std::string nt = serialized(none, 3);
            for (std::size_t n = 0; n < nt.size(); ++n)
                if (parses(nt.substr(0, n))) ++accepted_prefixes;   // an intentional no-event report cut anywhere is not a smaller no-event report
        }
        CHECK(accepted_prefixes == 0, "%d truncated prefixes of a %zu byte file were accepted", accepted_prefixes, full.size());
    }

    if (failures == 0) std::printf("scout tests passed\n");
    return failures == 0 ? 0 : 1;
}
