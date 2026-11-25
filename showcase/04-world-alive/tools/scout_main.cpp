// Prints what happens in the world of a seed: used to choose which seed a film is shot on (the dice are the simulation's own; the choice of which run to film is an editorial one, and is disclosed).
#include "scout.hpp"

#include <cstdio>
#include <cstdlib>

using namespace saige::alive;

int main(int argc, char** argv) {
    WorldConfig c;
    c.seed = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 7u;
    const double seconds = argc > 2 ? std::atof(argv[2]) : 210.0;
    c.time_scale = 47.0;
    c.start_minute = 18 * 60 + 30;
    const ScoutReport r = scout_world(c, seconds);
    std::printf("seed %u: accident at %.1f s (%.0f, %.0f) vehicle %u; on scene %.1f; cleared %.1f; crossings %zu; interior visits %zu", c.seed, r.accident_t, r.accident_x, r.accident_y, r.accident_vehicle, r.onscene_t, r.cleared_t, r.crossings.size(), r.interior_visits.size());
    for (const ScoutVisit& v : r.interior_visits) std::printf(" [ped %llu t %.0f floor %d]", static_cast<unsigned long long>(v.ped), v.t, v.floor);
    std::printf("\n");
    return 0;
}
