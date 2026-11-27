#!/usr/bin/env python3
"""Fails a CI lane when CTest skipped a test that is not on that lane's allowed list (tools/ci/allowed_skips.json).

Usage: check_skips.py --junit build/ctest-junit.xml --lane linux-vulkan-lavapipe [--allowed tools/ci/allowed_skips.json]
Exit 0: every skipped test is allowed for the lane. Exit 1: an unexpected skip, an unknown lane, a JUnit file with no test cases, or an allowed
entry without a reason. A test that is allowed to skip but ran is reported (so the list can be tightened) and does not fail the lane."""
import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def junit_results(path):
    root = ET.parse(path).getroot()
    out = {}
    for case in root.iter("testcase"):
        name = case.get("name", "")
        status = case.get("status", "")
        skipped = case.find("skipped") is not None or status in ("notrun", "skipped")
        failed = case.find("failure") is not None or case.find("error") is not None
        out[name] = "skipped" if skipped else ("failed" if failed else "passed")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--junit", required=True)
    ap.add_argument("--lane", required=True)
    ap.add_argument("--allowed", default=str(Path(__file__).resolve().with_name("allowed_skips.json")))
    a = ap.parse_args(argv)
    cfg = json.loads(Path(a.allowed).read_text(encoding="utf8"))
    lanes = cfg.get("lanes", {})
    if a.lane not in lanes:
        print(f"check_skips: unknown lane {a.lane!r} (known: {', '.join(sorted(lanes))})")
        return 1
    allowed = lanes[a.lane]
    bad_reason = [t for t, why in allowed.items() if not isinstance(why, str) or not why.strip()]
    if bad_reason:
        print(f"check_skips: allowed skips without a reason: {bad_reason}")
        return 1
    results = junit_results(a.junit)
    if not results:
        print(f"check_skips: {a.junit} holds no test cases")
        return 1
    skipped = sorted(t for t, r in results.items() if r == "skipped")
    unexpected = [t for t in skipped if t not in allowed]
    now_running = sorted(t for t in allowed if results.get(t) in ("passed", "failed"))
    missing = sorted(t for t in allowed if t not in results)
    print(f"check_skips [{a.lane}]: {len(results)} tests, {len(skipped)} skipped, {len(unexpected)} not allowed")
    for t in skipped:
        print(f"  skipped {'(ALLOWED) ' if t in allowed else '(NOT ALLOWED) '}{t}" + (f": {allowed[t]}" if t in allowed else ""))
    for t in now_running:
        print(f"  note: {t} is on the allowed list but ran; the entry can be removed")
    for t in missing:
        print(f"  note: {t} is on the allowed list but is not a test of this build")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
