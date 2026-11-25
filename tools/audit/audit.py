#!/usr/bin/env python3
"""Toolchain and dependency audit. Offline and read-only: it runs version probes, compiles a tiny program per C++ standard, and reads the CMake pins.

  python3 tools/audit/audit.py [--json OUT.json] [--markdown OUT.md] [--policy tools/audit/policy.json]

Exit code: 0 when every required tool meets its minimum and every pinned dependency has a hash, 1 otherwise (warnings do not fail). Which adapter Vulkan reports is recorded, and classified
(software or physical) so that nothing measured on a software rasterizer is mistaken for GPU evidence.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def parse_version(text):
    """The first dotted number in `text` as a tuple of ints, or None. 'spirv-val' prints dates like 'SPIRV-Tools v2024.1'; glslang prints '11:15.1.0'."""
    m = re.search(r"(\d+(?:\.\d+)+)", text)
    if m:
        return tuple(int(p) for p in m.group(1).split("."))
    m = re.search(r"\b(\d{4})\b", text)
    return (int(m.group(1)),) if m else None


def at_least(have, need):
    if have is None:
        return False
    need_t = tuple(int(p) for p in need.split("."))
    n = max(len(have), len(need_t))
    return tuple(list(have) + [0] * (n - len(have))) >= tuple(list(need_t) + [0] * (n - len(need_t)))


def run(cmd, timeout=20):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout + p.stderr)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, str(e)


def audit_tools(policy):
    out = []
    for name, spec in policy["tools"].items():
        exe = spec["probe"][0]
        row = {"tool": name, "min": spec["min"], "recommended": spec["recommended"], "optional": bool(spec.get("optional"))}
        if shutil.which(exe) is None:
            row.update(found=False, status="missing-optional" if row["optional"] else "missing")
            out.append(row)
            continue
        rc, text = run(spec["probe"])
        if name == "glslangValidator":  # prints 'Glslang Version: 11:15.1.0': the second number is the release
            m = re.search(r"Version:\s*\d+:(\d+(?:\.\d+)+)", text)
            version = tuple(int(p) for p in m.group(1).split(".")) if m else parse_version(text)
        elif name == "vulkaninfo":
            m = re.search(r"Vulkan Instance Version:\s*(\d+(?:\.\d+)+)", text)
            version = tuple(int(p) for p in m.group(1).split(".")) if m else parse_version(text)
        else:
            version = parse_version(text)
        row["found"] = True
        row["version"] = ".".join(map(str, version)) if version else None
        if version is None:
            row["status"] = "unparsed"
        elif not at_least(version, spec["min"]):
            row["status"] = "below-minimum"
        elif not at_least(version, spec["recommended"]):
            row["status"] = "below-recommended"
        else:
            row["status"] = "ok"
        out.append(row)
    return out


def audit_cxx(policy):
    results = []
    src = "#include <string>\n#include <vector>\nint main(){std::vector<std::string> v{\"a\"};return static_cast<int>(v.size())-1;}\n"
    for compiler in ("g++", "clang++"):
        if shutil.which(compiler) is None:
            continue
        for std in policy["cxx_standards"]["probe"]:
            with tempfile.TemporaryDirectory() as d:
                f = Path(d) / "t.cpp"
                f.write_text(src)
                rc, text = run([compiler, f"-std={std}", "-o", str(Path(d) / "t"), str(f)], timeout=60)
                ok = rc == 0 and run([str(Path(d) / "t")])[0] == 0
            results.append({"compiler": compiler, "standard": std, "builds": ok, "required": std == policy["cxx_standards"]["project"]})
    return results


def pins_in_text(text):
    """Every FetchContent_Declare in a CMake file's text: name, how it is pinned, and whether a content hash or a full commit id is present. (The declarations carry no nested parentheses.)"""
    rows = []
    for m in re.finditer(r"FetchContent_Declare\(\s*(\w+)([^)]*)\)", text):
        body = m.group(2)
        tag = re.search(r"GIT_TAG\s+(\S+)", body)
        url = re.search(r"URL\s+(\S+)", body)
        hashed = bool(re.search(r"URL_HASH\s+SHA256=[0-9a-f]{64}", body))
        commit_pinned = bool(tag and re.fullmatch(r"[0-9a-f]{40}", tag.group(1)))
        rows.append({"name": m.group(1), "pin": tag.group(1) if tag else (url.group(1) if url else None), "content_hash": hashed, "commit_pinned": commit_pinned,
                     "status": "ok" if hashed or commit_pinned else ("tag-only" if tag else "unpinned")})
    return rows


def audit_pins(policy):
    rows = []
    for path in sorted(list(ROOT.rglob("CMakeLists.txt")) + list(ROOT.rglob("*.cmake"))):
        if any(part in ("_deps", "node_modules") or part.startswith("build") for part in path.relative_to(ROOT).parts):
            continue
        for row in pins_in_text(path.read_text(errors="ignore")):
            row["file"] = str(path.relative_to(ROOT))
            rows.append(row)
    return rows


def audit_vulkan():
    if shutil.which("vulkaninfo") is None:
        return {"available": False}
    rc, text = run(["vulkaninfo", "--summary"], timeout=60)
    devices = re.findall(r"deviceName\s*=\s*(.+)", text)
    types = re.findall(r"deviceType\s*=\s*(\S+)", text)
    rows = []
    for i, name in enumerate(devices):
        t = types[i] if i < len(types) else "unknown"
        software = "CPU" in t or "llvmpipe" in name.lower() or "lavapipe" in name.lower() or "swiftshader" in name.lower()
        rows.append({"name": name.strip(), "type": t, "classification": "software" if software else "physical"})
    return {"available": rc == 0, "devices": rows, "physical_gpu_present": any(r["classification"] == "physical" for r in rows)}


def build_report(policy):
    tools = audit_tools(policy)
    cxx = audit_cxx(policy)
    pins = audit_pins(policy)
    vk = audit_vulkan()
    failures, warnings = [], []
    for t in tools:
        if t["status"] in ("missing", "below-minimum"):
            failures.append(f"{t['tool']}: {t['status']} (need >= {t['min']})")
        elif t["status"] in ("below-recommended", "unparsed"):
            warnings.append(f"{t['tool']}: {t['status']} ({t.get('version')}; recommended {t['recommended']})")
    for c in cxx:
        if c["required"] and not c["builds"]:
            failures.append(f"{c['compiler']} cannot build -std={c['standard']}")
    for p in pins:
        if p["status"] == "unpinned":
            failures.append(f"{p['name']} ({p['file']}): not pinned")
        elif p["status"] == "tag-only":
            warnings.append(f"{p['name']} ({p['file']}): pinned by tag only, no content hash ({p['pin']})")
    for key, spec in policy["dependencies"].items():
        if "newest_reviewed" in spec:
            warnings.append(f"{key}: expected {spec['pinned_expected']}, newest reviewed {spec['newest_reviewed']}: {spec.get('action', '')}")
    if vk.get("available") and not vk.get("physical_gpu_present"):
        warnings.append("Vulkan reports only software adapters: results are correctness evidence, not GPU performance evidence")
    return {"schema_version": 1, "tools": tools, "cxx": cxx, "pins": pins, "vulkan": vk, "failures": failures, "warnings": warnings}


def to_markdown(r):
    lines = ["# Toolchain and dependency audit", ""]
    lines += ["| Tool | Found | Min | Recommended | Status |", "| --- | --- | --- | --- | --- |"]
    for t in r["tools"]:
        lines.append(f"| {t['tool']} | {t.get('version') or '-'} | {t['min']} | {t['recommended']} | {t['status']} |")
    lines += ["", "| Compiler | Standard | Builds |", "| --- | --- | --- |"]
    for c in r["cxx"]:
        lines.append(f"| {c['compiler']} | {c['standard']} | {'yes' if c['builds'] else 'no'} |")
    lines += ["", "| Pinned dependency | File | Pin | Status |", "| --- | --- | --- | --- |"]
    for p in r["pins"]:
        lines.append(f"| {p['name']} | {p['file']} | {p['pin']} | {p['status']} |")
    if r["vulkan"].get("devices"):
        lines += ["", "| Vulkan adapter | Type | Classification |", "| --- | --- | --- |"]
        for d in r["vulkan"]["devices"]:
            lines.append(f"| {d['name']} | {d['type']} | {d['classification']} |")
    lines += ["", "## Failures", ""] + ([f"* {f}" for f in r["failures"]] or ["none"])
    lines += ["", "## Warnings", ""] + ([f"* {w}" for w in r["warnings"]] or ["none"])
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--policy", default=str(Path(__file__).with_name("policy.json")))
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    args = ap.parse_args(argv)
    policy = json.loads(Path(args.policy).read_text())
    report = build_report(policy)
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1) + "\n")
    md = to_markdown(report)
    if args.markdown:
        Path(args.markdown).write_text(md)
    else:
        print(md)
    return 1 if report["failures"] else 0


if __name__ == "__main__":
    sys.exit(main())
