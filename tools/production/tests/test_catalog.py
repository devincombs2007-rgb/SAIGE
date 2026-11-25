"""Offline tests of the asset catalog, extractors and prep tools (no network). Run: python3 -m unittest discover -s tools/production/tests  (exit code 77 from run_tests.py when dependencies are missing)."""
import json, os, struct, sys, tempfile, unittest, wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import numpy as np  # noqa: E402

from saige_assets import extract  # noqa: E402
from saige_assets.catalog import Catalog, classify  # noqa: E402


def record(asset_id="model.test.crate", sha="a" * 64, category="prop.environment", redistribution="allowed"):
    return {"schema": 1, "id": asset_id, "kind": "model", "category": category, "name": "Crate", "description": "", "tags": ["crate"],
            "source": {"name": "test", "source_id": "crate", "url": "https://example.invalid/crate"}, "creators": ["tester"],
            "license": {"spdx": "CC0-1.0", "commercial_use": True, "redistribution": redistribution}, "files": [{"path": "test/crate.glb", "sha256": sha, "bytes": 10}],
            "technical": {"triangles": 12}, "status": "candidate", "suitability": {"runtime_ready": None, "notes": []}, "version": 1}


def write_glb(path, triangles=1):
    """A minimal valid GLB with one triangle mesh (positions only)."""
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]] * triangles, dtype="<f4")
    binary = pos.tobytes()
    js = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "tri"}], "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
          "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3 * triangles, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}], "bufferViews": [{"buffer": 0, "byteLength": len(binary)}], "buffers": [{"byteLength": len(binary)}]}
    jb = json.dumps(js).encode()
    jb += b" " * (-len(jb) % 4)
    binary += b"\0" * (-len(binary) % 4)
    total = 12 + 8 + len(jb) + 8 + len(binary)
    Path(path).write_bytes(struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(jb), 0x4E4F534A) + jb + struct.pack("<II", len(binary), 0x004E4942) + binary)


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cat = Catalog(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_put_creates_and_validates(self):
        rec, created, changed = self.cat.put(record())
        self.assertTrue(created)
        self.assertFalse(changed)
        self.assertEqual(self.cat.get("model.test.crate")["version"], 1)

    def test_changed_hash_bumps_version_and_keeps_history(self):
        self.cat.put(record(sha="a" * 64))
        rec, created, changed = self.cat.put(record(sha="b" * 64))
        self.assertFalse(created)
        self.assertTrue(changed)
        self.assertEqual(rec["version"], 2)
        self.assertEqual(rec["history"][0]["files"][0]["sha256"], "a" * 64)

    def test_same_hash_keeps_status(self):
        rec, *_ = self.cat.put(record())
        rec["status"] = "approved"
        from saige_assets.common import dump_json
        dump_json(self.cat.path_for(rec), rec)  # what `saige-assets approve` does
        again, _, changed = self.cat.put(record())
        self.assertFalse(changed)
        self.assertEqual(again["status"], "approved")

    def test_schema_rejects_bad_records(self):
        bad = record()
        bad["license"]["redistribution"] = "sure"
        with self.assertRaises(Exception):
            self.cat.put(bad)
        bad = record(category="nonsense.category")
        with self.assertRaises(ValueError):
            self.cat.put(bad)

    def test_duplicates_found_by_hash(self):
        self.cat.put(record("model.test.a", sha="c" * 64))
        self.cat.put(record("model.test.b", sha="c" * 64))
        self.assertEqual(len(self.cat.duplicates()), 1)

    def test_search_filters_by_redistribution(self):
        self.cat.put(record("model.test.open", redistribution="allowed"))
        self.cat.put(record("model.test.closed", sha="d" * 64, redistribution="private-only"))
        ids = {r["id"] for r in self.cat.search("", license_ok=True)}
        self.assertEqual(ids, {"model.test.open"})

    def test_classify_uses_words_within_kind(self):
        self.assertEqual(classify("model", "Street Lamp 01", ["lighting"])[0], "infrastructure.road")
        self.assertEqual(classify("audio", "car engine idle loop", [])[0], "audio.vehicle")
        self.assertEqual(classify("material", "Asphalt 033", ["road"])[0], "material.pbr")

    def test_verify_detects_changed_files(self):
        cache = Path(self.tmp.name) / "cache"
        (cache / "test").mkdir(parents=True)
        f = cache / "test" / "crate.glb"
        f.write_bytes(b"hello")
        from saige_assets.common import sha256_file
        rec = record(sha=sha256_file(f))
        rec["files"][0]["bytes"] = 5
        self.cat.put(rec)
        ok, problems = self.cat.verify(cache)
        self.assertEqual((ok, problems), (1, []))
        f.write_bytes(b"tampered")
        ok, problems = self.cat.verify(cache)
        self.assertEqual(len(problems), 1)


class ExtractTests(unittest.TestCase):
    def test_glb_counts(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "t.glb"
            write_glb(p, triangles=4)
            t = extract.describe(p)
            self.assertEqual(t["triangles"], 4)
            self.assertEqual(t["vertices"], 12)
            self.assertEqual(t["bounds_max"], [1, 1, 0])

    def test_audio_metadata(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "tone.wav"
            sr = 48000
            x = (0.5 * np.sin(2 * np.pi * 440 * np.arange(sr) / sr) * 32767).astype("<i2")
            with wave.open(str(p), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(x.tobytes())
            t = extract.describe(p)
            self.assertAlmostEqual(t["duration_s"], 1.0, places=2)
            self.assertAlmostEqual(t["peak_dbfs"], -6.0, delta=0.2)
            self.assertLess(t["loop_seam_ratio"], 2.0)  # a clean loop: the wrap is one ordinary sample step (up to pi/2 of the mean step for a sine)
            click = np.concatenate([x[:sr // 2], (x[:sr // 2] // 2)])  # a level jump at the wrap
            with wave.open(str(p), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(np.concatenate([click, np.full(100, 20000, dtype="<i2")]).astype("<i2").tobytes())
            self.assertGreater(extract.describe(p)["loop_seam_ratio"], 20)


if __name__ == "__main__":
    unittest.main()
