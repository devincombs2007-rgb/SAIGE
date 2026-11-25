// Writes the district and the pedestrians of a running world as plain text for a top-down plot (a data view, not a render).
#include "world.hpp"

#include <cstdio>
#include <cstdlib>

using namespace saige::alive;

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    const char* out = argc > 2 ? argv[2] : "map.txt";
    World w{WorldConfig{}};
    for (double t = 0; t < seconds; t += 0.25) w.step(0.25);
    const District& d = w.district();
    std::FILE* f = std::fopen(out, "w");
    if (!f) return 1;
    auto rect = [&](const char* k, const Rect& r) { std::fprintf(f, "%s %.2f %.2f %.2f %.2f\n", k, r.x0, r.y0, r.x1, r.y1); };
    std::fprintf(f, "bounds %.1f %.1f %.1f %.1f %.1f\n", d.bounds.x0, d.bounds.y0, d.bounds.x1, 140.0, d.quay_y);
    for (const Rect& r : d.roadway) rect("road", r);
    for (const Rect& r : d.sidewalk) rect("walk", r);
    for (const Rect& r : d.park) rect("park", r);
    for (const Pier& p : d.piers) rect("pier", p.deck);
    for (const Building& b : d.buildings) std::fprintf(f, "bld %.2f %.2f %.2f %.2f %d %d\n", b.footprint.x0, b.footprint.y0, b.footprint.x1, b.footprint.y1, static_cast<int>(b.kind), b.interior ? 1 : 0);
    for (const Crosswalk& c : d.crosswalks) std::fprintf(f, "cw %.2f %.2f %.2f %.2f %d\n", c.strip.x0, c.strip.y0, c.strip.x1, c.strip.y1, w.walk_signal(c.id) ? 1 : 0);
    for (const StreetProp& p : d.props) std::fprintf(f, "prop %s %.2f %.2f\n", p.type.c_str(), p.x, p.y);
    for (const BoatSpawn& b : d.boats) std::fprintf(f, "boat %.2f %.2f %.2f %.2f\n", b.x, b.y, b.length, b.beam);
    for (const PedView& p : w.pedestrians()) if (p.visible) std::fprintf(f, "ped %.2f %.2f %d %d\n", p.x, p.y, static_cast<int>(p.state), p.palette);
    std::fclose(f);
    return 0;
}
