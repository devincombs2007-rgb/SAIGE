// Windows and lamps must come on gradually. The first version of Showcase 03 chose one of four pre-scaled material sets by the sun's elevation (switching at 6, 1 and -4 degrees), so every lit surface changed
// in a single frame at three moments of the film: measured in the render, a lit window's pixel went 159 -> 65 (sunrise) and 106 -> 178, 186 -> 243, 248 -> 255 (dusk) between two frames half a second apart.
// This checks the continuous ramp that replaced it: bounded change per film frame at 30 and 60 frames per second, monotone through the evening, off by day and full at the end, plus a control that the old
// switch fails the same bound (so the check can fail).
#include "time_of_day.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace saige::horizon;

namespace {

int failures = 0;

void check(bool ok, const char* what, double value = 0.0)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (%.5f)\n", what, value);
        ++failures;
    }
}

// What the film used first: four material sets, emission scaled 0, 0.06, 0.35, 1.
double old_switch_gain(double elevation_deg)
{
    if (elevation_deg > 6.0) return 0.0;
    if (elevation_deg > 1.0) return 0.06;
    if (elevation_deg > -4.0) return 0.35;
    return 1.0;
}

double gain_at_time(double t, double (*gain)(double))
{
    double el = 0, az = 0;
    sun_angles(t, el, az);
    return gain(el);
}

} // namespace

int main()
{
    constexpr double film_end = 142.0;
    for (const double fps : {30.0, 60.0}) {
        const double dt = 1.0 / fps;
        double worst = 0.0, worst_old = 0.0, worst_t = 0.0;
        for (double t = 0.0; t + dt <= film_end; t += dt) {
            const double step = std::fabs(gain_at_time(t + dt, emissive_gain_at) - gain_at_time(t, emissive_gain_at));
            if (step > worst) {
                worst = step;
                worst_t = t;
            }
            worst_old = std::max(worst_old, std::fabs(gain_at_time(t + dt, old_switch_gain) - gain_at_time(t, old_switch_gain)));
        }
        std::printf("%.0f fps: largest per-frame change of the emission %.4f (at %.2f s); the four-level switch it replaced: %.4f\n", fps, worst, worst_t, worst_old);
        check(worst < 0.008, "no frame changes the emission by more than 0.8 percent of full", worst);
        check(worst_old > 0.2, "control: the old four-level switch does jump", worst_old);
    }
    // Monotone through the evening (the sun only goes down after t = 100), off at noon, full at the end.
    double previous = -1.0;
    for (double t = 100.0; t <= film_end; t += 0.05) {
        const double g = gain_at_time(t, emissive_gain_at);
        check(g + 1e-12 >= previous, "the emission never falls while the sun sets", t);
        previous = g;
    }
    check(gain_at_time(60.0, emissive_gain_at) == 0.0, "no emission at noon", gain_at_time(60.0, emissive_gain_at));
    check(gain_at_time(film_end, emissive_gain_at) > 0.99, "full emission at the end", gain_at_time(film_end, emissive_gain_at));
    check(gain_at_time(0.0, emissive_gain_at) < 0.1, "the film opens with the lights nearly off", gain_at_time(0.0, emissive_gain_at));
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
