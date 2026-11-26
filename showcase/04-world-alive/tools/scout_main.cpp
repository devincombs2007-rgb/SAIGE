// Prints what happens in the world of a seed: used to choose which seed a film is shot on (the dice are the simulation's own; the choice of which run to film is an editorial one, and is disclosed).
//
//   alive_scout <seed> [seconds]            the scout file the director reads (format 2; the same writer saige_alive uses)
//   alive_scout --summary <seed> [seconds]  one human-readable line (NOT readable by saige_alive)
//
// The world is configured exactly as saige_alive configures a film (60 residents, 14 vehicles, accidents on, 17:30, time scale 47), so a file written here is the file saige_alive would write.
#include "scout.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace saige::alive;

int main(int argc, char** argv) {
    int i = 1;
    const bool summary = i < argc && std::strcmp(argv[i], "--summary") == 0;
    if (summary) ++i;
    WorldConfig c;
    c.seed = i < argc ? static_cast<std::uint32_t>(std::atoi(argv[i])) : 7u;
    ++i;
    const double seconds = i < argc ? std::atof(argv[i]) : 210.0;
    c.time_scale = 47.0;
    c.start_minute = 17.5 * 60.0;
    const ScoutReport r = scout_world(c, seconds);
    if (!summary) {
        write_scout_report(std::cout, r, c);
        return std::cout ? 0 : 1;
    }
    std::printf("seed %u: accident at %.1f s (%.0f, %.0f) vehicle %u; on scene %.1f; cleared %.1f; crossings %zu; interior visits %zu", c.seed, r.accident_t, r.accident_x, r.accident_y, r.accident_vehicle, r.onscene_t, r.cleared_t, r.crossings.size(), r.interior_visits.size());
    for (const ScoutVisit& v : r.interior_visits) std::printf(" [ped %llu t %.0f floor %d]", static_cast<unsigned long long>(v.ped), v.t, v.floor);
    std::printf("\n");
    return 0;
}
