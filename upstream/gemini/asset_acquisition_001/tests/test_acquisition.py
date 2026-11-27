import hashlib
import http.server
import json
import os
import struct
import tempfile
import threading
import unittest
import wave
import zipfile

from saige_acq.extractors import (
    classify_pbr_map,
    extract_audio_metadata,
    extract_model_metadata,
    extract_texture_metadata,
)
from saige_acq.models import CatalogEntry
from saige_acq.validator import validate_catalog_entry
from saige_acquire import run_acquisition


def make_synthetic_glb(path: str) -> None:
    gltf = {
        "asset": {"version": "2.0"},
        "nodes": [{"name": "RootBone"}, {"name": "SpineBone"}],
        "skins": [{"joints": [0, 1]}],
        "animations": [{"name": "Walk_Forward"}],
        "accessors": [
            {"count": 3, "min": [-1.0, 0.0, -1.0], "max": [1.0, 2.0, 1.0]},
            {"count": 6},
        ],
        "meshes": [
            {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}
        ],
    }
    raw_json = json.dumps(gltf).encode("utf-8")
    pad = (4 - (len(raw_json) % 4)) % 4
    raw_json += b" " * pad
    total_len = 12 + 8 + len(raw_json)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total_len))
        f.write(struct.pack("<II", len(raw_json), 0x4E4F534A))
        f.write(raw_json)


def make_synthetic_wav(path: str, sr: int = 48000, ch: int = 2, frames: int = 24000) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(b"\x00\x00" * ch * frames)


def make_synthetic_png(path: str, w: int = 2048, h: int = 2048) -> None:
    ihdr_data = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    ihdr_chunk = struct.pack(">I", 13) + b"IHDR" + ihdr_data + b"\x00\x00\x00\x00"
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(ihdr_chunk)


class TestSaigeAcquisition(unittest.TestCase):
    def test_extractors_and_pbr_classification(self):
        with tempfile.TemporaryDirectory() as td:
            glb = os.path.join(td, "sedan.glb")
            make_synthetic_glb(glb)
            mm = extract_model_metadata(glb)
            self.assertIsNotNone(mm)
            self.assertEqual(mm.triangle_count, 2)
            self.assertTrue(mm.has_rig)
            self.assertEqual(mm.bone_names, ["RootBone", "SpineBone"])
            self.assertEqual(mm.animation_names, ["Walk_Forward"])

            wav = os.path.join(td, "v8_idle.wav")
            make_synthetic_wav(wav, sr=48000, ch=2, frames=24000)
            am = extract_audio_metadata(wav)
            self.assertIsNotNone(am)
            self.assertEqual(am.sample_rate_hz, 48000)
            self.assertEqual(am.channels, 2)
            self.assertAlmostEqual(am.duration_sec, 0.5, places=3)

            png = os.path.join(td, "asphalt_nor_gl_2k.png")
            make_synthetic_png(png, 2048, 2048)
            tm = extract_texture_metadata(png)
            self.assertIsNotNone(tm)
            self.assertEqual(tm.width, 2048)
            self.assertEqual(tm.pbr_map_type, "normal")
            self.assertEqual(classify_pbr_map("brick_basecolor.png"), "base_color")
            self.assertEqual(classify_pbr_map("metal_roughness.png"), "roughness")

    def test_license_validation_rejects_unverified_or_commercial_rip(self):
        bad = CatalogEntry(
            asset_id="bad_01",
            name="Ripped Asset",
            category="vehicles",
            subcategory="cars",
            source_name="Unknown",
            source_url="https://example.com/bad",
            creator="Unknown",
            license_spdx="PROPRIETARY",
            redistribution_allowed=False,
            acquisition_mode="direct_url",
        )
        ok, errs = validate_catalog_entry(bad)
        self.assertFalse(ok)
        self.assertGreaterEqual(len(errs), 2)

    def test_mock_http_acquisition_retry_resume_duplicates_and_zip(self):
        with tempfile.TemporaryDirectory() as td:
            srv_dir = os.path.join(td, "srv")
            os.makedirs(srv_dir)
            glb_path = os.path.join(srv_dir, "sunset.glb")
            make_synthetic_glb(glb_path)
            glb_bytes = open(glb_path, "rb").read()
            glb_sha = hashlib.sha256(glb_bytes).hexdigest()

            attempts = {"glb": 0}

            class MockHandler(http.server.BaseHTTPRequestHandler):
                def log_message(self, fmt, *args):
                    pass

                def do_GET(self):
                    if self.path.startswith("/files/sunset_jhbcentral"):
                        payload = json.dumps(
                            {
                                "gltf": {
                                    "2k": {
                                        "gltf": {
                                            "url": f"http://127.0.0.1:{self.server.server_port}/dl/sunset.glb"
                                        }
                                    }
                                }
                            }
                        ).encode("utf-8")
                        self.send_response(200)
                        self.send_header("Content-Type", "application/json")
                        self.end_headers()
                        self.wfile.write(payload)
                        return

                    if self.path == "/dl/sunset.glb":
                        attempts["glb"] += 1
                        if attempts["glb"] == 1:
                            self.send_response(503)
                            self.end_headers()
                            return
                        rng = self.headers.get("Range")
                        if rng and rng.startswith("bytes="):
                            start = int(rng.split("=")[1].split("-")[0])
                            self.send_response(206)
                            self.end_headers()
                            self.wfile.write(glb_bytes[start:])
                        else:
                            self.send_response(200)
                            self.end_headers()
                            self.wfile.write(glb_bytes)
                        return

                    self.send_response(404)
                    self.end_headers()

            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
            th = threading.Thread(target=server.serve_forever, daemon=True)
            th.start()
            try:
                cat_path = os.path.join(td, "cat.json")
                with open(cat_path, "w", encoding="utf-8") as f:
                    json.dump(
                        {
                            "entries": [
                                {
                                    "asset_id": "ph_01",
                                    "name": "Test GLB 1",
                                    "category": "environment_props",
                                    "subcategory": "urban",
                                    "source_name": "Poly Haven",
                                    "source_url": "https://polyhaven.com/a/sunset_jhbcentral",
                                    "creator": "Poly Haven",
                                    "license_spdx": "CC0-1.0",
                                    "redistribution_allowed": True,
                                    "acquisition_mode": "polyhaven_api",
                                    "api_slug": "sunset_jhbcentral",
                                    "preferred_format": "gltf",
                                    "preferred_resolution": "2k",
                                    "expected_sha256": glb_sha,
                                },
                                {
                                    "asset_id": "ph_02_dup",
                                    "name": "Test GLB Duplicate",
                                    "category": "vehicles",
                                    "subcategory": "sedans",
                                    "source_name": "Poly Haven",
                                    "source_url": "https://polyhaven.com/a/sunset_jhbcentral",
                                    "creator": "Poly Haven",
                                    "license_spdx": "CC0-1.0",
                                    "redistribution_allowed": True,
                                    "acquisition_mode": "polyhaven_api",
                                    "api_slug": "sunset_jhbcentral",
                                    "preferred_format": "gltf",
                                    "preferred_resolution": "2k",
                                    "expected_sha256": glb_sha,
                                },
                            ]
                        },
                        f,
                    )

                out_dir = os.path.join(td, "out_lib")
                out_zip = os.path.join(td, "SAIGE_Master_Asset_Library_001.zip")
                summary = run_acquisition(
                    catalog_path=cat_path,
                    output_dir=out_dir,
                    output_zip=out_zip,
                    polyhaven_base_url=f"http://127.0.0.1:{server.server_port}",
                )
                self.assertEqual(summary.downloaded_count, 2)
                self.assertEqual(summary.failed_count, 0)
                self.assertEqual(summary.duplicate_count, 1)
                self.assertTrue(os.path.exists(out_zip))

                with zipfile.ZipFile(out_zip, "r") as zf:
                    names = set(zf.namelist())
                    self.assertIn("MASTER_MANIFEST.json", names)
                    self.assertIn("ASSET_CATALOG.csv", names)
                    self.assertIn("SHA256SUMS.txt", names)
                    self.assertIn("LICENSES.md", names)
                    self.assertIn("IMPORT_GUIDE.md", names)
            finally:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
