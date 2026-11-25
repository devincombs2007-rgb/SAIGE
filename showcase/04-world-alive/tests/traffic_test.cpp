// Showcase 04 traffic: cars on the engine's vehicle physics, steered and braked by what the traffic simulation wants, with the signals, the gap rule, an accident that the physics produces and the response to it.
#include "world.hpp"

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
} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 150.0;
    // ---- ordinary traffic: no accidents ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        WorldConfig cfg;
        cfg.residents = 20;
        cfg.vehicles = 14;
        cfg.accidents = false;
        World w(cfg);
        double min_gap_centres = 1e9;
        std::uint32_t most_vehicles = 0, most_stopped = 0;
        for (double t = 0; t < seconds; t += 0.25) {
            w.step(0.25);
            const TrafficStats ts = w.traffic_stats();
            if (std::getenv("ALIVE_DEBUG"))
                for (const TrafficEvent& e : w.take_traffic_events()) std::printf("EVENT t=%.1f type %d veh %u at (%.1f, %.1f) speed %.1f energy %.0f kind %d\n", t, static_cast<int>(e.type), e.vehicle, e.x, e.y, e.speed, e.energy, e.surface_kind);
            most_vehicles = std::max(most_vehicles, ts.vehicles);
            most_stopped = std::max(most_stopped, ts.stopped_at_signal);
            const auto vs = w.vehicles();
            for (std::size_t i = 0; i < vs.size(); ++i)
                for (std::size_t j = i + 1; j < vs.size(); ++j) min_gap_centres = std::min(min_gap_centres, std::hypot(vs[i].x - vs[j].x, vs[i].y - vs[j].y));
            if (static_cast<long>(t) % 20 == 0 && std::fmod(t, 1.0) < 0.25)
                std::printf("t=%4.0f vehicles %u moving %u at-signal %u spawned %llu despawned %llu impacts %llu max-lane-error %.2f m\n", t, ts.vehicles, ts.moving, ts.stopped_at_signal, static_cast<unsigned long long>(ts.spawned),
                            static_cast<unsigned long long>(ts.despawned), static_cast<unsigned long long>(ts.impacts), ts.max_lane_error_m);
        }
        const TrafficStats ts = w.traffic_stats();
        std::printf("traffic cost: %.0f ms signals+sim, %.0f ms vehicles+physics over %.0f s; closest centres %.2f m\n", ts.ms_traffic, ts.ms_vehicles, seconds, min_gap_centres);
        CHECK(most_vehicles >= 8);
        CHECK(ts.despawned >= 2);                  // cars complete their journeys and leave
        CHECK(ts.red_light_runs == 0);
        CHECK(ts.impacts == 0);                    // nothing hits anything in ordinary driving
        CHECK(ts.lane_departures == 0);
        CHECK(ts.max_lane_error_m < 1.4);
        CHECK(most_stopped >= 1);                  // the signals do stop cars
        CHECK(min_gap_centres > 3.0);              // no two cars ever overlap (no vehicle-vehicle contact in the engine: the gap rule keeps them apart)
    }
    // ---- an accident and the response: the distraction is the only chance event; what it leads to is the physics, the traffic simulation and the world simulation ---------------------------------------------
    {
        WorldConfig cfg;
        cfg.residents = 40;
        cfg.vehicles = 14;
        cfg.accidents = false;
        World w(cfg);
        double t = 0.0;
        while (w.traffic_stats().vehicles < 8 && t < 120.0) {
            w.step(0.25);
            t += 0.25;
        }
        // Choose a car at speed on a long straight, and let its driver look away.
        std::uint32_t victim = 0;
        for (int tries = 0; tries < 400 && victim == 0; ++tries) {
            w.step(0.25);
            t += 0.25;
            for (const VehicleView& v : w.vehicles())
                if (v.speed > 8.0 && !v.siren && w.traffic_driver().distract(v.id, 2.4)) {
                    victim = v.id;
                    break;
                }
        }
        CHECK(victim != 0);
        int accidents = 0, dispatched = 0, on_scene = 0, cleared = 0, impacts = 0;
        double t_accident = -1, t_clear = -1;
        const double t_start = t;
        std::uint64_t reactions_before = w.stats().danger_reactions;
        for (; t < t_start + 220.0; t += 0.25) {
            w.step(0.25);
            for (const TrafficEvent& e : w.take_traffic_events()) {
                if (std::getenv("ALIVE_DEBUG")) std::printf("EVENT t=%.1f type %d veh %u at (%.1f, %.1f) speed %.1f energy %.0f kind %d\n", t - t_start, static_cast<int>(e.type), e.vehicle, e.x, e.y, e.speed, e.energy, e.surface_kind);
                switch (e.type) {
                case TrafficEvent::Type::Impact: ++impacts; break;
                case TrafficEvent::Type::Accident: ++accidents; t_accident = t - t_start; break;
                case TrafficEvent::Type::EmergencyDispatched: ++dispatched; break;
                case TrafficEvent::Type::EmergencyOnScene: ++on_scene; break;
                case TrafficEvent::Type::Cleared: ++cleared; t_clear = t - t_start; break;
                }
            }
        }
        const WorldStats ws = w.stats();
        const TrafficStats ts = w.traffic_stats();
        std::printf("accident at +%.1f s, cleared at +%.1f s; impacts %d, dispatched %d, on scene %d; residents' reactions %llu; red-light runs %llu\n", t_accident, t_clear, impacts, dispatched, on_scene,
                    static_cast<unsigned long long>(ws.danger_reactions - reactions_before), static_cast<unsigned long long>(ts.red_light_runs));
        CHECK(accidents == 1);                       // the car left its lane, hit the roadside, and was wrecked by what it hit
        CHECK(impacts >= 1);
        CHECK(dispatched == 2);                      // an ambulance and a fire engine, a few seconds after
        CHECK(on_scene >= 1);                        // they drove there through the signals (preempted for them) and stopped by the wreck
        CHECK(cleared == 1);                         // the road reopened after they had been on scene
        CHECK(ws.danger_reactions > reactions_before);   // the residents nearby heard of it
        CHECK(ws.crossings_against_signal == 0);
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("traffic ok\n");
    return EXIT_SUCCESS;
}
