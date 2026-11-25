// Showcase 04 world: pedestrians that live in the district. Nothing here is scripted: the checks are on what the simulation, the crowd and the physics produce together.
#include "world.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace saige::alive;

namespace {
int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

double checksum(const World& w) {
    double h = 0.0;
    for (const PedView& p : w.pedestrians()) h += p.x * 1.0001 + p.y * 0.9999 + p.z + static_cast<double>(p.state);
    return h;
}
} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 240.0;
    WorldConfig cfg;
    cfg.residents = 60;
    World w(cfg);
    CHECK(w.pedestrians().size() == 60);

    const auto t0 = std::chrono::steady_clock::now();
    std::uint32_t worst_overlap = 0, worst_stuck = 0;
    double min_sep = 1e9;
    for (double t = 0.0; t < seconds; t += 0.25) {
        w.step(0.25);
        const WorldStats s = w.stats();
        worst_overlap = std::max(worst_overlap, s.overlapping_with_geometry);
        worst_stuck = std::max(worst_stuck, s.stuck);
        if (s.stuck > 0 && std::getenv("ALIVE_DEBUG"))
            for (const PedView& p : w.pedestrians())
                if (p.state == PedState::Walking && p.stalled_s > 100.0) std::printf("STUCK t=%.0f ped %llu at (%.2f,%.2f,%.2f) target (%.2f,%.2f) intent %d\n", t, static_cast<unsigned long long>(p.id), p.x, p.y, p.z, p.target_x, p.target_y, static_cast<int>(p.intent));
        // Bodies never interpenetrate: no two visible pedestrians closer than their radii allow (0.28 m each; a little softness for contact).
        {
            const auto peds = w.pedestrians();
            for (std::size_t i = 0; i < peds.size(); ++i) {
                if (!peds[i].visible) continue;
                for (std::size_t j = i + 1; j < peds.size(); ++j)
                    if (peds[j].visible && std::fabs(peds[i].z - peds[j].z) < 1.0) min_sep = std::min(min_sep, std::hypot(peds[i].x - peds[j].x, peds[i].y - peds[j].y));
            }
        }
        if (static_cast<long>(t) % 30 == 0 && std::fmod(t, 1.0) < 0.25)
            std::printf("t=%5.0fs  %02d:%02d  walk %u wait %u cross %u dwell %u in %u | arrivals %llu (interior %llu, stairs %llu) crossings %llu\n", t, static_cast<int>(s.game_minute / 60.0), static_cast<int>(std::fmod(s.game_minute, 60.0)), s.walking, s.waiting,
                        s.crossing, s.dwelling, s.indoors, static_cast<unsigned long long>(s.venue_arrivals), static_cast<unsigned long long>(s.interior_arrivals), static_cast<unsigned long long>(s.stair_climbs),
                        static_cast<unsigned long long>(s.crossings_started));
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const WorldStats s = w.stats();
    std::printf("simulated %.0f s in %.1f s wall (%.1fx); closest pair of centres %.3f m; worst overlap %u; worst stuck %u\n", seconds, wall, seconds / wall, min_sep, worst_overlap, worst_stuck);

    if (std::getenv("ALIVE_DEBUG"))
        for (const PedView& p : w.pedestrians()) std::printf("ped %llu state %d at (%.1f,%.1f,%.2f) -> (%.1f,%.1f) speed %.2f intent %d\n", static_cast<unsigned long long>(p.id), static_cast<int>(p.state), p.x, p.y, p.z, p.target_x, p.target_y, p.speed, static_cast<int>(p.intent));
    std::printf("cost (ms total): sim+traffic %.0f, decisions %.0f, crowd %.0f, bodies %.0f, stats %.0f\n", s.ms_sim, s.ms_decide, s.ms_crowd, s.ms_bodies, s.ms_stats);

    CHECK(s.crossings_against_signal == 0);
    CHECK(worst_overlap == 0);
    CHECK(worst_stuck == 0);
    CHECK(s.venue_arrivals >= 20);
    CHECK(s.crossings_started >= 3);
    CHECK(s.interior_arrivals >= 1);
    CHECK(min_sep > 0.30);   // two bodies of radius 0.28 m: the centres never come closer than about half of the sum of the radii (contact softness)

    // Determinism: the same seed and the same steps give the same world.
    World a(cfg), b(cfg);
    for (int i = 0; i < 400; ++i) {
        a.step(0.25);
        b.step(0.25);
    }
    CHECK(checksum(a) == checksum(b));
    WorldConfig other = cfg;
    other.seed = 9;
    World c(other);
    for (int i = 0; i < 400; ++i) c.step(0.25);
    CHECK(checksum(a) != checksum(c));

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("world ok\n");
    return EXIT_SUCCESS;
}
