#include "saige/project.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

void benchmark(const saige::Project& project, unsigned samples, double load_ms) {
    std::vector<double> measurements;
    measurements.reserve(samples);
    for (unsigned i = 0; i < samples; ++i) {
        const auto start = Clock::now();
        const auto matrices = saige::resolve_project_matrices(project);
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start);
        if (matrices.size() != project.nodes.size()) throw std::logic_error("Scene resolution lost nodes");
        measurements.push_back(elapsed.count());
    }
    std::ranges::sort(measurements);
    const auto percentile = [&](unsigned value) {
        const auto rank = (measurements.size() * value + 99) / 100;
        return measurements[rank - 1];
    };
    std::cout << "{\"schema_version\":1,\"benchmark\":\"project_resolve\",\"samples\":" << samples
              << ",\"node_count\":" << project.nodes.size() << ",\"asset_count\":" << project.assets.size()
              << ",\"load_ms\":" << load_ms
              << ",\"resolve_p50_ms\":" << percentile(50) << ",\"resolve_p95_ms\":" << percentile(95)
              << ",\"matrix_payload_bytes\":" << project.nodes.size() * sizeof(saige::WorldMatrix)
              << ",\"gpu_time_ms\":null}\n";
}
} // namespace

int main(int argc, char** argv) {
    std::cout.imbue(std::locale::classic());
    const auto verb = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    if (!((verb == "validate" && argc == 3) ||
          ((verb == "save-as" || verb == "benchmark") && argc == 4))) {
        std::cerr << "Usage: saige_project_tool validate PROJECT | save-as PROJECT OUTPUT | benchmark PROJECT SAMPLES\n";
        return 2;
    }

    try {
        const auto start = Clock::now();
        const auto project = saige::load_project(std::filesystem::path(argv[2]));
        const auto load_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (verb == "save-as") {
            saige::save_project(project, std::filesystem::path(argv[3]));
        } else if (verb == "benchmark") {
            const std::string_view raw(argv[3]);
            unsigned samples{};
            const auto end = raw.data() + raw.size();
            const auto [next, result] = std::from_chars(raw.data(), end, samples);
            if (result != std::errc{} || next != end || samples == 0 || samples > 1000) {
                std::cerr << "SAMPLES must be between 1 and 1000\n";
                return 2;
            }
            benchmark(project, samples, load_ms);
        } else {
            std::cout << "{\"schema_version\":1,\"assets\":" << project.assets.size()
                      << ",\"nodes\":" << project.nodes.size() << "}\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
