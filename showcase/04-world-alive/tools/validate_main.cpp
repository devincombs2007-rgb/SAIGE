// alive_validate [seed...]: builds each district and prints the validation findings (a summary by code, then every finding). Exit code 0 when no finding is an error or worse.
#include "district_validate.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

int main(int argc, char** argv) {
    using namespace saige::alive;
    bool verbose = false, dump = false;
    std::vector<std::uint32_t> seeds;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-v") verbose = true;
        else if (a == "--buildings") dump = true;
        else seeds.push_back(static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 10)));
    }
    if (seeds.empty()) seeds.push_back(8);
    int blocking = 0;
    for (const std::uint32_t seed : seeds) {
        const District d = build_district(seed);
        if (dump) {
            for (const Building& b : d.buildings) std::printf("B %u %s %.4f %.4f %.4f %.4f floors %d fh %.3f front %d interior %d\n", b.id, kind_name(b.kind), b.footprint.x0, b.footprint.y0, b.footprint.x1, b.footprint.y1, b.floors, b.floor_height, static_cast<int>(b.front), b.interior ? 1 : 0);
            continue;
        }
        const ValidationReport rep = validate_district(d);
        std::map<std::string, int> by_code;
        for (const Finding& f : rep.findings) ++by_code[f.code + " (" + severity_name(f.severity) + ")"];
        std::printf("seed %u: %zu buildings, %zu props, %zu findings (%zu error or worse)\n", seed, d.buildings.size(), d.props.size(), rep.findings.size(), rep.count_at_least(Severity::Error));
        for (const auto& [k, v] : by_code) std::printf("  %-40s %d\n", k.c_str(), v);
        if (verbose)
            for (const Finding& f : rep.findings) std::printf("  [%s] %s at (%.2f, %.2f): %s\n", severity_name(f.severity), f.code.c_str(), f.x, f.y, f.message.c_str());
        if (rep.has_blocking()) ++blocking;
    }
    return blocking == 0 ? 0 : 1;
}
