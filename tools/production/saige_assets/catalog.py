"""The asset catalog: one JSON record per asset under assets/catalog/<category-root>/<id>.json (metadata only, never binaries), validated against assets/schema/asset.schema.json.

Identity: `id` is persistent. A change in the files (a different hash set) bumps `version` and appends to `history`; the old hashes stay in the history so a build can name the exact revision.
"""
import json
from pathlib import Path

from .common import CATALOG_DIR, SCHEMA_PATH, TAXONOMY_PATH, dump_json, load_json, now, sha256_file


def taxonomy():
    return load_json(TAXONOMY_PATH)


def classify(kind, name, tags, source_categories=()):
    """The best taxonomy category for an asset from its words, within its kind. Returns (category, score)."""
    words = " ".join(str(x) for x in [name or "", *tags, *source_categories] if x).lower().replace("-", " ").replace("_", " ")
    best, best_score = None, 0
    for cat, spec in taxonomy()["categories"].items():
        if spec["kind"] != kind:
            continue
        score = sum(1 for k in spec["keywords"] if k.replace("_", " ") in words)
        if score > best_score:
            best, best_score = cat, score
    if best is None:
        fallback = {"model": "prop.environment", "material": "material.pbr", "hdri": "hdri", "animation": "animation.skeletal", "texture": "particle.texture", "ui": "ui.asset", "audio": "audio.ambience"}
        return fallback.get(kind, "prop.environment"), 0
    return best, best_score


class Catalog:
    def __init__(self, root=None):
        self.root = Path(root or CATALOG_DIR)

    def path_for(self, rec):
        return self.root / rec["category"].split(".")[0] / (rec["id"] + ".json")

    def records(self):
        if not self.root.exists():
            return
        for p in sorted(self.root.rglob("*.json")):
            yield p, load_json(p)

    def get(self, asset_id):
        for p, r in self.records():
            if r["id"] == asset_id:
                return r
        return None

    def validate(self, rec):
        import jsonschema
        jsonschema.validate(rec, load_json(SCHEMA_PATH))
        tax = taxonomy()["categories"]
        if rec["category"] not in tax:
            raise ValueError(f"unknown category {rec['category']}")
        return True

    def put(self, rec):
        """Insert or update a record, bumping the version when the file hashes changed. Returns (record, created, changed)."""
        old = self.get(rec["id"])
        created = old is None
        changed = False
        if old:
            old_hashes = sorted(f["sha256"] for f in old["files"])
            new_hashes = sorted(f["sha256"] for f in rec["files"])
            changed = old_hashes != new_hashes
            rec["version"] = old["version"] + (1 if changed else 0)
            rec["history"] = list(old.get("history", []))
            if changed:
                rec["history"].append({"version": old["version"], "retired": now(), "files": [{"path": f["path"], "sha256": f["sha256"]} for f in old["files"]]})
            for keep in ("status", "suitability"):
                if keep in old and not changed:
                    rec[keep] = old[keep]
            oldp = self.path_for(old)
            if oldp.exists() and oldp != self.path_for(rec):
                oldp.unlink()
        else:
            rec.setdefault("version", 1)
            rec.setdefault("history", [])
        self.validate(rec)
        dump_json(self.path_for(rec), rec)
        return rec, created, changed

    def search(self, text="", kind=None, category=None, status=None, license_ok=None):
        text = text.lower()
        for _, r in self.records():
            if kind and r["kind"] != kind:
                continue
            if category and not r["category"].startswith(category):
                continue
            if status and r["status"] != status:
                continue
            if license_ok and r["license"]["redistribution"] not in ("allowed", "allowed-with-attribution"):
                continue
            hay = " ".join([r["id"], r["name"], r.get("description", ""), " ".join(r.get("tags", [])), r["category"]]).lower()
            if all(t in hay for t in text.split()):
                yield r

    def verify(self, cache_root):
        """Re-hash every cached file of every record; returns (ok_count, problems)."""
        ok, problems = 0, []
        for _, r in self.records():
            for f in r["files"]:
                p = Path(cache_root) / f["path"]
                if not p.exists():
                    problems.append((r["id"], f["path"], "missing from the cache"))
                elif sha256_file(p) != f["sha256"]:
                    problems.append((r["id"], f["path"], "hash differs from the catalog"))
                else:
                    ok += 1
        return ok, problems

    def duplicates(self):
        """Records that share a file hash (exact duplicates)."""
        seen = {}
        dups = []
        for _, r in self.records():
            for f in r["files"]:
                other = seen.get(f["sha256"])
                if other and other != r["id"]:
                    dups.append((f["sha256"][:12], other, r["id"], f["path"]))
                seen.setdefault(f["sha256"], r["id"])
        return dups

    def stats(self):
        by_kind, by_cat, by_license, total_tris, total_tex, total_bytes = {}, {}, {}, 0, 0, 0
        for _, r in self.records():
            by_kind[r["kind"]] = by_kind.get(r["kind"], 0) + 1
            by_cat[r["category"]] = by_cat.get(r["category"], 0) + 1
            by_license[r["license"]["spdx"]] = by_license.get(r["license"]["spdx"], 0) + 1
            t = r.get("technical", {})
            total_tris += t.get("triangles", 0)
            total_tex += t.get("texture_gpu_bytes", 0)
            total_bytes += sum(f["bytes"] for f in r["files"])
        return {"records": sum(by_kind.values()), "by_kind": by_kind, "by_category": by_cat, "by_license": by_license, "triangles": total_tris, "texture_gpu_bytes": total_tex, "file_bytes": total_bytes}
