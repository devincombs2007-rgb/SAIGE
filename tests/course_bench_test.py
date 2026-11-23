"""Checks the course benchmark's contract, not its speed: replay determinism, report structure, and honest labeling.

Timing values are never asserted (they depend on the machine). Everything else must be identical between runs.
"""
import json
import subprocess
import sys


def run(binary, fixture, frames, obstacles=64):
    result = subprocess.run(
        [binary, "--fixture", fixture, "--frames", str(frames), "--obstacles", str(obstacles), "--json", "-"],
        capture_output=True, text=True, timeout=100)
    if result.returncode != 0:
        sys.exit(f"bench failed ({result.returncode}): {result.stderr}")
    return json.loads(result.stdout)


def expect(condition, message):
    if not condition:
        sys.exit("FAIL: " + message)


def main():
    binary, fixture = sys.argv[1], sys.argv[2]
    frames = 2500  # long enough to go forward past two chunk boundaries and start back
    first = run(binary, fixture, frames)
    second = run(binary, fixture, frames)

    # Deterministic replay: same inputs and virtual clock give the same poses and chunk activity on every run.
    for key in ("replay_checksum", "final_feet", "chunk_requests", "chunk_commits", "chunk_stage_calls_that_staged",
                "ticks_with_chunk_work", "longest_pending_ticks", "peak_active_chunks", "peak_retained_asset_bytes"):
        expect(first["bench"][key] == second["bench"][key], f"{key} differs between identical runs")
    expect(first["perf"]["ticks"] == second["perf"]["ticks"] == frames, "one fixed tick per frame on the virtual clock")
    expect(first["perf"]["frames"] == frames and first["perf"]["frames_without_tick"] == 0, "no frame without a tick")

    # A different workload must change the replay (otherwise the checksum would prove nothing).
    shorter = run(binary, fixture, frames // 2)
    expect(shorter["bench"]["replay_checksum"] != first["bench"]["replay_checksum"], "checksum does not depend on the run")

    bench, perf = first["bench"], first["perf"]
    expect(perf["schema_version"] == 1 and perf["workload"] == "character_course_replay", "report identity")
    expect(perf["adapter_class"] == "none", "a headless CPU run must not claim a GPU class")
    expect(perf["budget"]["verdict"] == "not_evaluated", "the 16.67 ms target must never be claimed here")
    expect(bench["paced"] is False, "the benchmark is unpaced CPU cost")

    # Streaming really happened: the player left the first chunk, chunks were staged and committed, and eviction kept the set bounded.
    expect(bench["chunk_requests"] >= 3, "the route crosses at least two chunk boundaries")
    expect(bench["chunk_commits"] == bench["chunk_requests"], "every request was committed")
    expect(bench["peak_active_chunks"] <= 9 and bench["peak_retained_asset_bytes"] > 0, "bounded, nonempty residency")

    zones = perf["zones_ms"]
    for name in ("fixed_step", "physics.step", "chunk.request", "chunk.stage_next", "chunk.commit_frame"):
        expect(name in zones and zones[name]["count"] > 0, f"zone {name} was recorded")
    expect(zones["fixed_step"]["count"] == frames and zones["physics.step"]["count"] == frames, "per-tick zones cover every tick")
    expect(perf["phases_ms"]["simulation_per_tick"]["count"] == frames, "per-tick simulation series")
    for summary in [perf["frame_ms"]] + list(zones.values()):
        expect(0 <= summary["min_ms"] <= summary["p50_ms"] <= summary["p95_ms"] <= summary["p99_ms"] <= summary["max_ms"],
               "percentiles are ordered")
    if sys.platform.startswith("linux"):
        expect(perf["process_memory_bytes"]["peak_resident"] and perf["process_memory_bytes"]["peak_resident"] > 0, "memory")
    print("course bench ok")


main()
