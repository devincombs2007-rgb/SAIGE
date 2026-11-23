#include "saige/perf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <locale>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace saige::perf {
namespace {

constexpr double ns_per_ms = 1'000'000.0;

double to_ms(std::int64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / ns_per_ms;
}

// Nearest rank on a sorted copy: rank = ceil(n * percent / 100), computed in integers.
std::int64_t nearest_rank(const std::vector<std::int64_t>& sorted, std::size_t percent) {
    const std::size_t rank = (sorted.size() * percent + 99) / 100;
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + '"';
}

void write_summary(std::ostringstream& out, const Summary& s) {
    out << "{\"count\":" << s.count << ",\"mean_ms\":" << s.mean_ms << ",\"min_ms\":" << s.min_ms << ",\"p50_ms\":" << s.p50_ms
        << ",\"p95_ms\":" << s.p95_ms << ",\"p99_ms\":" << s.p99_ms << ",\"max_ms\":" << s.max_ms << '}';
}

void write_optional(std::ostringstream& out, const std::optional<std::uint64_t>& value) {
    if (value) {
        out << *value;
    } else {
        out << "null";
    }
}

} // namespace

Summary summarize(std::span<const std::int64_t> nanoseconds) {
    Summary s;
    s.count = nanoseconds.size();
    if (nanoseconds.empty()) return s;
    std::vector<std::int64_t> sorted(nanoseconds.begin(), nanoseconds.end());
    std::sort(sorted.begin(), sorted.end());
    // Summing in long double keeps a long run's total exact enough that the mean does not drift with run length.
    long double sum = 0;
    for (const auto v : sorted) sum += static_cast<long double>(v);
    s.mean_ms = static_cast<double>(sum / static_cast<long double>(sorted.size())) / ns_per_ms;
    s.min_ms = to_ms(sorted.front());
    s.max_ms = to_ms(sorted.back());
    s.p50_ms = to_ms(nearest_rank(sorted, 50));
    s.p95_ms = to_ms(nearest_rank(sorted, 95));
    s.p99_ms = to_ms(nearest_rank(sorted, 99));
    return s;
}

std::size_t count_over(std::span<const std::int64_t> nanoseconds, std::int64_t threshold_ns) noexcept {
    return static_cast<std::size_t>(std::count_if(nanoseconds.begin(), nanoseconds.end(),
                                                  [threshold_ns](std::int64_t v) { return v > threshold_ns; }));
}

void Zones::record(std::string_view name, std::chrono::nanoseconds duration) {
    auto it = zones_.find(name);
    if (it == zones_.end()) it = zones_.emplace(std::string(name), Series{}).first;
    it->second.add(duration);
}

const Series* Zones::find(std::string_view name) const {
    const auto it = zones_.find(name);
    return it == zones_.end() ? nullptr : &it->second;
}

ProcessMemory sample_process_memory() {
    ProcessMemory memory;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        memory.resident_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
        memory.peak_resident_bytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    const auto kilobytes = [](const std::string& text) -> std::optional<std::uint64_t> {
        std::uint64_t value = 0;
        bool any = false;
        for (const char c : text) {
            if (c >= '0' && c <= '9') {
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
                any = true;
            } else if (any) {
                break;
            }
        }
        return any ? std::optional<std::uint64_t>(value * 1024) : std::nullopt;
    };
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) memory.resident_bytes = kilobytes(line.substr(6));
        if (line.rfind("VmHWM:", 0) == 0) memory.peak_resident_bytes = kilobytes(line.substr(6));
    }
#endif
    return memory;
}

FrameRecorder::FrameRecorder(Options options) : options_(options) {
    if (options.reserve_frames != 0) {
        for (Series* s : {&total_, &input_, &simulation_, &extraction_, &submission_, &per_tick_simulation_, &wall_interval_}) {
            s->reserve(options.reserve_frames);
        }
    }
}

void FrameRecorder::record(const CpuFrameMetrics& frame) {
    ++frames_;
    ticks_ += frame.ticks;
    peak_instances_ = std::max(peak_instances_, frame.instance_count);
    total_.add(frame.total);
    input_.add(frame.input);
    simulation_.add(frame.simulation);
    extraction_.add(frame.extraction);
    submission_.add(frame.submission);
    if (frame.ticks > 0) {
        per_tick_simulation_.add(frame.simulation / static_cast<std::int64_t>(frame.ticks));
    } else {
        ++frames_without_tick_;
    }
    max_ticks_in_frame_ = std::max(max_ticks_in_frame_, frame.ticks);
    if (options_.measure_wall_interval) {
        const auto now = std::chrono::steady_clock::now();
        if (last_frame_end_) wall_interval_.add(std::chrono::duration_cast<std::chrono::nanoseconds>(now - *last_frame_end_));
        last_frame_end_ = now;
    }
    if (options_.memory_every_frames != 0 && (frames_ == 1 || frames_ % options_.memory_every_frames == 0)) {
        const ProcessMemory sample = sample_process_memory();
        if (sample.resident_bytes) memory_.resident_bytes = sample.resident_bytes;
        if (sample.peak_resident_bytes) {
            memory_.peak_resident_bytes = std::max(memory_.peak_resident_bytes.value_or(0), *sample.peak_resident_bytes);
        }
    }
}

std::string to_json(const FrameRecorder& frames, const Zones& zones, const ReportContext& context) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.setf(std::ios::fixed);
    out.precision(4);

    const Summary total = frames.total().summary();
    const Summary wall = frames.wall_interval().summary();
    const auto budget_ns = static_cast<std::int64_t>(context.budget_ms * ns_per_ms);
    const std::size_t over_budget = count_over(frames.total().samples(), budget_ns);
    const std::size_t over_double = count_over(frames.total().samples(), budget_ns * 2);

    // The verdict is deliberately hard to earn: only a run marked as qualified target hardware on a physical GPU gets one.
    std::string verdict = "not_evaluated";
    std::string reason;
    if (context.adapter_class != "physical") {
        reason = "adapter class '" + context.adapter_class + "' does not measure real GPU hardware";
    } else if (!context.qualified_target_hardware) {
        reason = "the target hardware, resolution and graphics preset are not documented for this run";
    } else {
        verdict = total.p99_ms <= context.budget_ms ? "within_budget" : "over_budget";
        reason = "p99 CPU frame time against the budget on the qualified target hardware";
    }

    out << "{\"schema_version\":1"
        << ",\"workload\":" << json_string(context.workload) << ",\"build\":" << json_string(context.build)
        << ",\"adapter_class\":" << json_string(context.adapter_class) << ",\"fixed_step_ns\":" << context.fixed_step_ns
        << ",\"frames\":" << frames.frames() << ",\"ticks\":" << frames.ticks()
        << ",\"frames_without_tick\":" << frames.frames_without_tick() << ",\"max_ticks_in_frame\":" << frames.max_ticks_in_frame()
        << ",\"peak_instances\":" << frames.peak_instances();

    out << ",\"frame_ms\":";
    write_summary(out, total);
    out << ",\"average_fps\":" << (total.mean_ms > 0.0 ? 1000.0 / total.mean_ms : 0.0);
    out << ",\"wall_interval_ms\":";
    write_summary(out, wall);
    out << ",\"wall_average_fps\":" << (wall.mean_ms > 0.0 ? 1000.0 / wall.mean_ms : 0.0);
    out << ",\"phases_ms\":{\"input\":";
    write_summary(out, frames.input().summary());
    out << ",\"simulation\":";
    write_summary(out, frames.simulation().summary());
    out << ",\"extraction\":";
    write_summary(out, frames.extraction().summary());
    out << ",\"submission\":";
    write_summary(out, frames.submission().summary());
    out << ",\"simulation_per_tick\":";
    write_summary(out, frames.per_tick_simulation().summary());
    out << '}';

    out << ",\"budget\":{\"budget_ms\":" << context.budget_ms << ",\"frames_over_budget\":" << over_budget
        << ",\"frames_over_2x_budget\":" << over_double << ",\"verdict\":" << json_string(verdict)
        << ",\"verdict_reason\":" << json_string(reason) << '}';

    out << ",\"zones_ms\":{";
    bool first = true;
    for (const auto& [name, series] : zones.all()) {
        out << (first ? "" : ",") << json_string(name) << ':';
        write_summary(out, series.summary());
        first = false;
    }
    out << '}';

    out << ",\"process_memory_bytes\":{\"resident\":";
    write_optional(out, frames.memory().resident_bytes);
    out << ",\"peak_resident\":";
    write_optional(out, frames.memory().peak_resident_bytes);
    out << "}}";
    return out.str();
}

} // namespace saige::perf
