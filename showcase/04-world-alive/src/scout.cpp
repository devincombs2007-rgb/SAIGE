#include "scout.hpp"

#include <set>

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

} // namespace saige::alive
