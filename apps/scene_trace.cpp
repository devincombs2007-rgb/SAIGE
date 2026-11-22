#include "saige/project_session.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <locale>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

class ScriptedDesktop final : public saige::IDesktopPlatform {
public:
    explicit ScriptedDesktop(std::uint64_t limit) : limit_(limit) {}

    std::chrono::steady_clock::time_point now() override { return time_; }

    bool poll_events() override {
        if (frames_ == limit_) return false;
        ++frames_;
        time_ += 16'666'667ns;
        return true;
    }

    saige::NativeWindowHandle native_window() const noexcept override { return {}; }
    saige::SurfaceState surface() const noexcept override { return {}; }

    bool key_down(saige::Key key) const noexcept override {
        return key == saige::Key::w && frames_ <= (limit_ + 1) / 2;
    }

    saige::LookDelta consume_look_delta() noexcept override {
        if (frames_ == (limit_ + 1) / 2 && !std::exchange(turned_, true)) {
            return {-24.F, 8.F};
        }
        return {};
    }

private:
    std::chrono::steady_clock::time_point time_{};
    std::uint64_t limit_;
    std::uint64_t frames_{};
    bool turned_{};
};

class TraceSink final : public saige::IFrameSink {
public:
    void submit(const saige::FrameView& frame) override {
        last_camera = frame.camera;
        matrix_count = static_cast<std::size_t>(std::ranges::count_if(frame.instances, [](const auto& instance) {
            return instance.world_matrix.has_value();
        }));
    }

    std::optional<saige::CameraView> last_camera;
    std::size_t matrix_count{};
};

class TraceMetrics final : public saige::ICpuFrameObserver {
public:
    void record(const saige::CpuFrameMetrics& frame) override {
        total.push_back(frame.total.count());
        input.push_back(frame.input.count());
        simulation.push_back(frame.simulation.count());
        extraction.push_back(frame.extraction.count());
        submission.push_back(frame.submission.count());
        peak_instances = std::max(peak_instances, frame.instance_count);
    }

    std::vector<std::int64_t> total, input, simulation, extraction, submission;
    std::size_t peak_instances{};
};

double percentile_ms(std::vector<std::int64_t>& durations, std::size_t percentile) {
    std::ranges::sort(durations);
    const auto rank = (durations.size() * percentile + 99) / 100;
    return static_cast<double>(durations[rank - 1]) / 1'000'000.0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: saige_scene_trace PROJECT FRAME_COUNT\n";
        return 2;
    }
    std::cout.imbue(std::locale::classic());
    std::uint64_t frames{};
    const std::string_view count(argv[2]);
    const auto end = count.data() + count.size();
    const auto [next, error] = std::from_chars(count.data(), end, frames);
    if (error != std::errc{} || next != end || frames == 0 || frames > 100'000) {
        std::cerr << "FRAME_COUNT must be 1 to 100000\n";
        return 2;
    }
    try {
        ScriptedDesktop desktop(frames);
        const auto open_started = std::chrono::steady_clock::now();
        saige::ProjectSession session(std::filesystem::path(argv[1]), &desktop);
        const auto open_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - open_started).count();
        TraceSink sink;
        TraceMetrics metrics;
        for (auto* series : {&metrics.total, &metrics.input, &metrics.simulation,
                             &metrics.extraction, &metrics.submission}) {
            series->reserve(static_cast<std::size_t>(frames));
        }
        const auto stats = saige::run(session.world(), desktop, session, sink, std::nullopt, {}, &metrics);
        const auto position = sink.last_camera->position;
        std::cout << "{\"schema_version\":1,\"renderer\":\"none\",\"workload\":\"scripted_project\","
                  << "\"frames\":" << stats.frames << ",\"ticks\":" << stats.ticks
                  << ",\"instances\":" << metrics.peak_instances << ",\"matrix_instances\":" << sink.matrix_count
                  << ",\"project_open_ms\":" << open_ms
                  << ",\"frame_p50_ms\":" << percentile_ms(metrics.total, 50)
                  << ",\"frame_p95_ms\":" << percentile_ms(metrics.total, 95)
                  << ",\"input_p95_ms\":" << percentile_ms(metrics.input, 95)
                  << ",\"simulation_p95_ms\":" << percentile_ms(metrics.simulation, 95)
                  << ",\"extract_p95_ms\":" << percentile_ms(metrics.extraction, 95)
                  << ",\"submit_p95_ms\":" << percentile_ms(metrics.submission, 95)
                  << ",\"snapshot_payload_bytes\":" << metrics.peak_instances * sizeof(saige::RenderInstance)
                  << ",\"camera_position\":[" << position.x << ',' << position.y << ',' << position.z << ']'
                  << ",\"gpu_time_ms\":null,\"gpu_memory_bytes\":null}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
