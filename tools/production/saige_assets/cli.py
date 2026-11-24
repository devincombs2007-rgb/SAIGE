"""saige-assets: the command line of the shared asset library.

  saige-assets search polyhaven "street lamp" --type models
  saige-assets fetch polyhaven street_lamp_01 --res 1k            # download, hash, describe, write the catalog record (status: candidate)
  saige-assets fetch ambientcg Asphalt031 --res 1K-JPG
  saige-assets fetch khronos CarConcept
  saige-assets fetch opengameart https://opengameart.org/content/<slug> --kind audio
  saige-assets scan PATH --source local --license CC0-1.0          # catalog files you already have (hashes, technical data)
  saige-assets find rock --kind model --redistributable
  saige-assets show model.polyhaven.rock_09 | stats | verify | dedupe | licenses | approve ID | reject ID --reason ...
  saige-assets validate ID                                         # Khronos validator + the engine importer; stores the verdict in the record
  saige-assets services                                            # read-only checks of the keyed services (no credits are spent)
"""
import argparse, json, sys
from pathlib import Path

from . import extract
from .catalog import Catalog, classify
from .common import CACHE_DIR, CATALOG_DIR, dump_json, human, now, sha256_file, slug


def _print_rows(rows):
    for r in rows:
        print("  " + "  ".join(str(v) for v in r.values()))


def cmd_search(a):
    if a.source == "polyhaven":
        from .sources import polyhaven
        _print_rows(polyhaven.search(a.query, a.type, a.category, a.limit))
    elif a.source == "ambientcg":
        from .sources import ambientcg
        _print_rows(ambientcg.search(a.query, a.type or "Material", a.limit))
    elif a.source == "khronos":
        from .sources import khronos
        _print_rows(khronos.search(a.query, a.limit))
    elif a.source == "makehuman":
        from .sources import makehuman
        _print_rows(makehuman.search(a.query, a.type, None, a.limit))
    elif a.source == "opengameart":
        from .sources import opengameart
        _print_rows(opengameart.search(a.query, a.type or "audio", a.limit))
    elif a.source == "freesound":
        from .sources import keyed
        _print_rows([{"id": r["id"], "name": r["name"], "license": r["license"], "dur": r["duration"], "by": r["username"]} for r in keyed.freesound_search(a.query, page_size=a.limit)])
    elif a.source == "sketchfab":
        from .sources import keyed
        _print_rows([{"uid": r["uid"], "name": r["name"], "license": (r.get("license") or {}).get("label"), "faces": r.get("faceCount")} for r in keyed.sketchfab_search(a.query, count=a.limit)])
    else:
        sys.exit(f"search is not available for {a.source}")


def cmd_fetch(a):
    cat = Catalog()
    if a.source == "polyhaven":
        from .sources import polyhaven
        rec = polyhaven.fetch(a.id, a.type, a.res, a.format)
    elif a.source == "ambientcg":
        from .sources import ambientcg
        rec = ambientcg.fetch(a.id, a.res if a.res != "1k" else "1K-JPG")
    elif a.source == "khronos":
        from .sources import khronos
        rec = khronos.fetch(a.id, a.variant)
    elif a.source == "opengameart":
        from .sources import opengameart
        rec = opengameart.fetch(a.id, a.kind)
    elif a.source == "makehuman":
        from .sources import makehuman
        rec = makehuman.fetch(a.id)
    else:
        sys.exit(f"no scripted fetch for {a.source}; see docs/assets/integration-report.md")
    rec, created, changed = cat.put(rec)
    tech = rec["technical"]
    print(f"{'created' if created else 'updated' if changed else 'unchanged'} {rec['id']} v{rec['version']} [{rec['category']}] {rec['license']['spdx']} ({rec['license']['redistribution']}) {human(sum(f['bytes'] for f in rec['files']))}")
    if "triangles" in tech:
        print(f"  {tech['triangles']} triangles, {tech['materials']} materials, textures {human(tech.get('texture_gpu_bytes', 0))} on the GPU, {len(tech['animations'])} clips, {len(tech['skins'])} skins")


def cmd_scan(a):
    cat = Catalog()
    p = Path(a.path)
    files = [p] if p.is_file() else sorted(x for x in p.rglob("*") if x.is_file() and x.suffix.lower() in (".glb", ".gltf", ".png", ".jpg", ".hdr", ".exr", ".wav", ".flac", ".ogg", ".mp3", ".opus"))
    for f in files:
        kind = a.kind or ("model" if f.suffix.lower() in (".glb", ".gltf") else "audio" if f.suffix.lower() in (".wav", ".flac", ".ogg", ".mp3", ".opus") else "hdri" if f.suffix.lower() in (".hdr", ".exr") else "texture")
        tech = extract.describe(f)
        category, _ = classify(kind, f.stem, [])
        rel = f.resolve()
        try:
            rel_path = str(rel.relative_to(CACHE_DIR))
        except ValueError:
            rel_path = str(rel)
        rec = {"schema": 1, "id": f"{kind}.{a.source}.{slug(f.stem)}", "kind": kind, "category": category, "name": f.stem, "description": "", "tags": [],
               "source": {"name": a.source, "source_id": f.name, "url": a.url or "", "retrieved": now(), "acquire": a.acquire or "manual"}, "creators": [c for c in (a.creator or "").split(";") if c],
               "license": {"spdx": a.license, "url": "", "commercial_use": a.license.startswith(("CC0", "CC-BY", "MIT")) or "unknown", "redistribution": a.redistribution, "attribution": a.attribution or "", "ai_training": "unknown", "notes": ""},
               "files": [{"path": rel_path, "sha256": sha256_file(f), "bytes": f.stat().st_size, "role": "content", "format": f.suffix.lstrip(".")}], "technical": tech, "status": "candidate", "suitability": {"runtime_ready": None, "notes": []}, "version": 1}
        rec, created, changed = cat.put(rec)
        print(f"{'created' if created else 'updated' if changed else 'unchanged'} {rec['id']}")


def cmd_find(a):
    n = 0
    for r in Catalog().search(" ".join(a.text), a.kind, a.category, a.status, a.redistributable):
        t = r.get("technical", {})
        extra = f"{t['triangles']} tris" if "triangles" in t else f"{t['duration_s']} s" if "duration_s" in t else ""
        print(f"{r['id']:<48} {r['category']:<22} {r['license']['spdx']:<12} {r['status']:<9} {extra}")
        n += 1
    print(f"{n} record(s)")


def cmd_show(a):
    r = Catalog().get(a.id)
    if not r:
        sys.exit("no such record")
    print(json.dumps(r, indent=1))


def cmd_stats(a):
    print(json.dumps(Catalog().stats(), indent=1))


def cmd_verify(a):
    ok, problems = Catalog().verify(CACHE_DIR)
    print(f"{ok} file(s) verified against the catalog hashes")
    for p in problems:
        print("  PROBLEM", *p)
    sys.exit(1 if problems and not a.allow_missing else 0)


def cmd_dedupe(a):
    d = Catalog().duplicates()
    for x in d:
        print("  duplicate", *x)
    print(f"{len(d)} duplicate file(s) across records")


def cmd_licenses(a):
    rows = {}
    for _, r in Catalog().records():
        k = (r["license"]["spdx"], r["license"]["redistribution"])
        rows.setdefault(k, []).append(r["id"])
    for (spdx, red), ids in sorted(rows.items()):
        print(f"{spdx:<24} {red:<26} {len(ids)}")


def cmd_mark(a):
    cat = Catalog()
    r = cat.get(a.id)
    if not r:
        sys.exit("no such record")
    r["status"] = a.status
    if a.reason:
        r.setdefault("suitability", {}).setdefault("notes", []).append(a.reason)
    cat.validate(r)
    dump_json(cat.path_for(r), r)
    print(f"{a.id} -> {a.status}")


def _find_inspector():
    import os, shutil
    for c in (os.environ.get("SAIGE_ASSET_INSPECT"), shutil.which("saige_asset_inspect"), *(str(p) for p in Path("/tmp").glob("b*/tools/production/inspect/saige_asset_inspect")),
              *(str(p) for p in (Path(__file__).resolve().parents[3] / "build").rglob("saige_asset_inspect"))):
        if c and Path(c).exists():
            return c
    return None


def cmd_validate(a):
    """Runs the Khronos validator and the engine's own importer on a record's model, and stores the verdict in the record's `suitability`."""
    import os, subprocess
    cat = Catalog()
    r = cat.get(a.id)
    if not r:
        sys.exit("no such record")
    model = next((f for f in r["files"] if f["format"] in ("glb", "gltf") and f["role"] in ("model", "content")), None)
    if model is None:
        sys.exit("the record has no glTF model file")
    path = CACHE_DIR / model["path"]
    here = Path(__file__).resolve().parents[1]
    notes, checked, ready = [], [], True
    v = subprocess.run(["node", str(here / "prep" / "validate_gltf.mjs"), str(path)], capture_output=True, text=True, env={**os.environ})
    try:
        vr = json.loads(v.stdout.strip().splitlines()[-1])
        checked.append("khronos-gltf-validator")
        if vr["errors"]:
            ready = False
            notes.append(f"validator: {vr['errors']} error(s): {'; '.join(vr['first_errors'])}")
        else:
            notes.append(f"validator: no errors, {vr['warnings']} warning(s)")
    except Exception:  # noqa: BLE001
        notes.append("validator did not run (is the Node toolchain installed?)")
    insp = _find_inspector()
    if insp:
        i = subprocess.run([insp, str(path)], capture_output=True, text=True)
        try:
            ir = json.loads(i.stdout)
            checked.append("saige_asset_inspect")
            if not ir.get("runtime_ready", False):
                ready = False
                notes += ["engine: " + x for x in (ir.get("reasons") or [ir.get("error", "rejected")])]
            else:
                notes.append(f"engine: loads, {ir['triangles']} triangles, textures {human(ir['texture_gpu_bytes'])} on the GPU")
            notes += ["engine advisory: " + x for x in ir.get("advisories", [])]
        except Exception:  # noqa: BLE001
            notes.append("the engine inspector produced no result")
    else:
        notes.append("saige_asset_inspect is not built: the engine importer was not consulted (build the target, or set SAIGE_ASSET_INSPECT)")
        ready = None
    r["suitability"] = {"runtime_ready": ready, "checked_with": ", ".join(checked), "notes": notes}
    cat.validate(r)
    dump_json(cat.path_for(r), r)
    print(f"{a.id}: runtime_ready={ready}")
    for n in notes:
        print("  " + n)


def cmd_services(a):
    from .sources import keyed
    import os
    for name, fn in keyed.SERVICE_CHECKS.items():
        envs = {"freesound": "FREESOUND_API_KEY", "elevenlabs": "ELEVENLABS_API_KEY", "tripo": "TRIPO_API_KEY", "meshy": "MESHY_API_KEY", "sketchfab": "SKETCHFAB_API_TOKEN"}
        if not os.environ.get(envs[name]):
            print(f"  {name:<11} no key ({envs[name]} not set)")
            continue
        try:
            res = fn()
            print(f"  {name:<11} ok  {json.dumps(res)[:160]}")
        except Exception as e:  # noqa: BLE001
            print(f"  {name:<11} FAILED {str(e)[:140]}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="saige-assets", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("search"); s.add_argument("source"); s.add_argument("query", nargs="?", default=""); s.add_argument("--type"); s.add_argument("--category"); s.add_argument("--limit", type=int, default=20); s.set_defaults(fn=cmd_search)
    s = sub.add_parser("fetch"); s.add_argument("source"); s.add_argument("id"); s.add_argument("--type", default="models"); s.add_argument("--res", default="1k"); s.add_argument("--format", default="hdr"); s.add_argument("--variant", default="glTF-Binary"); s.add_argument("--kind", default="model"); s.set_defaults(fn=cmd_fetch)
    s = sub.add_parser("scan"); s.add_argument("path"); s.add_argument("--source", default="local"); s.add_argument("--kind"); s.add_argument("--license", default="LicenseRef-unknown"); s.add_argument("--redistribution", default="unknown"); s.add_argument("--creator"); s.add_argument("--attribution"); s.add_argument("--url"); s.add_argument("--acquire"); s.set_defaults(fn=cmd_scan)
    s = sub.add_parser("find"); s.add_argument("text", nargs="*"); s.add_argument("--kind"); s.add_argument("--category"); s.add_argument("--status"); s.add_argument("--redistributable", action="store_true"); s.set_defaults(fn=cmd_find)
    s = sub.add_parser("show"); s.add_argument("id"); s.set_defaults(fn=cmd_show)
    sub.add_parser("stats").set_defaults(fn=cmd_stats)
    s = sub.add_parser("verify"); s.add_argument("--allow-missing", action="store_true"); s.set_defaults(fn=cmd_verify)
    sub.add_parser("dedupe").set_defaults(fn=cmd_dedupe)
    sub.add_parser("licenses").set_defaults(fn=cmd_licenses)
    s = sub.add_parser("approve"); s.add_argument("id"); s.add_argument("--reason"); s.set_defaults(fn=cmd_mark, status="approved")
    s = sub.add_parser("reject"); s.add_argument("id"); s.add_argument("--reason"); s.set_defaults(fn=cmd_mark, status="rejected")
    sub.add_parser("services").set_defaults(fn=cmd_services)
    s = sub.add_parser("validate"); s.add_argument("id"); s.set_defaults(fn=cmd_validate)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
