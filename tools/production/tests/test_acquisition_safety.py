"""Offline tests of acquisition safety (2026-11-27; review of Gemini Asset Acquisition 001, #43 comment 7209985020): verified and bounded downloads,
resume, host policy for redirects, path containment, safe archive extraction and glTF dependency completeness. A local HTTP server stands in for the
sources; nothing here reaches the network."""
import hashlib, io, json, os, stat, sys, tempfile, threading, unittest, zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from saige_assets import common, extract, safety  # noqa: E402
from saige_assets.sources import ambientcg, polyhaven  # noqa: E402

PAYLOAD = bytes(range(256)) * 400  # 102400 bytes


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        port = self.server.server_address[1]
        if self.path == "/file" or self.path == "/norange":
            rng = self.headers.get("Range")
            if rng and self.path == "/file":
                start = int(rng.split("=")[1].split("-")[0])
                body = PAYLOAD[start:]
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{len(PAYLOAD) - 1}/{len(PAYLOAD)}")
            else:
                body = PAYLOAD
                self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/redirect-out":
            self.send_response(302)
            self.send_header("Location", f"http://localhost:{port}/file")  # another host name than the allowed 127.0.0.1
            self.end_headers()
        elif self.path == "/json":
            body = json.dumps({"ok": True}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()


class Server:
    def __enter__(self):
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        return self

    def __exit__(self, *a):
        self.httpd.shutdown()


MD5 = hashlib.md5(PAYLOAD).hexdigest()


class DownloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        safety.REQUIRE_HTTPS = False
        cls.srv = Server().__enter__()

    @classmethod
    def tearDownClass(cls):
        cls.srv.__exit__()
        safety.REQUIRE_HTTPS = True

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = Path(self.tmp.name) / "a" / "file.bin"

    def tearDown(self):
        self.tmp.cleanup()

    def get(self, path, **kw):
        kw.setdefault("allowed_hosts", ("127.0.0.1",))
        kw.setdefault("retries", 1)
        return common.download(self.srv.base + path, self.dest, **kw)

    def test_verified_download_then_cache(self):
        self.assertEqual(self.get("/file", expected_size=len(PAYLOAD), expected_md5=MD5), "downloaded")
        self.assertEqual(self.dest.read_bytes(), PAYLOAD)
        self.assertEqual(self.get("/file", expected_size=len(PAYLOAD), expected_md5=MD5), "cached")

    def test_corrupt_cache_is_downloaded_again(self):
        self.dest.parent.mkdir(parents=True)
        self.dest.write_bytes(b"x" * len(PAYLOAD))  # right size, wrong content
        self.assertEqual(self.get("/file", expected_size=len(PAYLOAD), expected_md5=MD5), "downloaded")
        self.assertEqual(self.dest.read_bytes(), PAYLOAD)

    def test_checksum_mismatch_is_refused_and_leaves_nothing(self):
        with self.assertRaises(safety.IntegrityError):
            self.get("/file", expected_size=len(PAYLOAD), expected_md5="0" * 32)
        self.assertFalse(self.dest.exists())
        self.assertFalse(Path(str(self.dest) + ".part").exists())

    def test_size_mismatch_is_refused(self):
        with self.assertRaises(safety.IntegrityError):
            self.get("/file", expected_size=len(PAYLOAD) + 1)

    def test_resume_uses_a_matching_content_range(self):
        part = Path(str(self.dest) + ".part")
        part.parent.mkdir(parents=True)
        part.write_bytes(PAYLOAD[:40000])
        self.assertEqual(self.get("/file", expected_size=len(PAYLOAD), expected_md5=MD5), "resumed")
        self.assertEqual(self.dest.read_bytes(), PAYLOAD)

    def test_a_server_that_ignores_range_restarts_cleanly(self):
        part = Path(str(self.dest) + ".part")
        part.parent.mkdir(parents=True)
        part.write_bytes(b"garbage" * 1000)
        self.assertEqual(self.get("/norange", expected_size=len(PAYLOAD), expected_md5=MD5), "downloaded")
        self.assertEqual(self.dest.read_bytes(), PAYLOAD)

    def test_size_limit(self):
        with self.assertRaises(safety.IntegrityError):
            self.get("/file", max_bytes=1000)
        with self.assertRaises(safety.IntegrityError):
            self.get("/file", expected_size=10 ** 12)

    def test_redirect_to_another_host_is_refused(self):
        with self.assertRaises(safety.UnsafePath):
            self.get("/redirect-out")
        self.assertFalse(self.dest.exists())

    def test_json_through_the_same_policy(self):
        self.assertEqual(common.http_get(self.srv.base + "/json", expect_json=True, allowed_hosts=("127.0.0.1",), retries=1), {"ok": True})
        with self.assertRaises(safety.UnsafePath):
            common.http_get(self.srv.base + "/json", expect_json=True, allowed_hosts=("example.org",), retries=1)

    def test_https_is_required_by_default(self):
        safety.REQUIRE_HTTPS = True
        try:
            self.assertFalse(safety.host_allowed("http://api.polyhaven.com/x", polyhaven.HOSTS))
            self.assertTrue(safety.host_allowed("https://dl.polyhaven.org/file/x", polyhaven.HOSTS))
            self.assertFalse(safety.host_allowed("https://dl.polyhaven.org.evil.example/x", polyhaven.HOSTS))
        finally:
            safety.REQUIRE_HTTPS = False


class PathAndArchiveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_contained(self):
        self.assertEqual(safety.contained(self.base, "textures/a.jpg"), (self.base / "textures" / "a.jpg").resolve())
        for bad in ("../x", "a/../../x", "/etc/passwd", "a\\..\\x", "C:/x", "", "x\x00"):
            with self.assertRaises(safety.UnsafePath, msg=bad):
                safety.contained(self.base, bad)
        (self.base / "link").symlink_to("/tmp")
        with self.assertRaises(safety.UnsafePath):
            safety.contained(self.base, "link/x")

    def zip_with(self, entries, name="t.zip"):
        p = self.base / name
        with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
            for n, data, mode in entries:
                info = zipfile.ZipInfo(n)
                info.external_attr = (mode or (stat.S_IFREG | 0o644)) << 16
                z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
        return p

    def test_safe_extract_accepts_a_normal_archive(self):
        z = self.zip_with([("Asphalt_Color.jpg", b"a" * 100, None), ("sub/Asphalt_NormalGL.png", b"b" * 100, None)])
        out = safety.safe_extract(z, self.base / "out")
        self.assertEqual(sorted(p.name for p in out), ["Asphalt_Color.jpg", "Asphalt_NormalGL.png"])

    def test_safe_extract_refuses_zip_slip_symlinks_and_bombs(self):
        cases = {
            "slip": [("../../evil.txt", b"x", None)],
            "absolute": [("/tmp/evil.txt", b"x", None)],
            "symlink": [("link", b"/etc/passwd", stat.S_IFLNK | 0o777)],
            "bomb": [("big.bin", b"\0" * (8 << 20), None)],
        }
        for name, entries in cases.items():
            z = self.zip_with(entries, name + ".zip")
            with self.assertRaises(safety.UnsafePath, msg=name):
                safety.safe_extract(z, self.base / ("out_" + name))
            self.assertFalse(any((self.base / ("out_" + name)).rglob("*")), name)  # nothing was written
        self.assertFalse((self.base.parent / "evil.txt").exists())

    def test_member_limit(self):
        z = self.zip_with([(f"f{i}.txt", b"x", None) for i in range(20)])
        with self.assertRaises(safety.UnsafePath):
            safety.safe_extract(z, self.base / "out", max_members=10)

    def test_gltf_dependencies(self):
        d = self.base / "m"
        d.mkdir()
        (d / "m.bin").write_bytes(b"\0" * 4)
        gltf = {"asset": {"version": "2.0"}, "buffers": [{"uri": "m.bin", "byteLength": 4}], "images": [{"uri": "textures/a.jpg"}, {"uri": "data:image/png;base64,AA=="}, {"uri": "../outside.jpg"}]}
        (d / "m.gltf").write_text(json.dumps(gltf))
        self.assertEqual(sorted(safety.missing_gltf_dependencies(d / "m.gltf")), ["../outside.jpg", "textures/a.jpg"])
        (d / "textures").mkdir()
        (d / "textures" / "a.jpg").write_bytes(b"\xff\xd8")
        self.assertEqual(safety.missing_gltf_dependencies(d / "m.gltf"), ["../outside.jpg"])

    def test_text_named_glb_is_not_a_model(self):
        p = self.base / "fake.glb"
        p.write_bytes(b"hello world")  # Gemini's validator accepted exactly this
        with self.assertRaises(Exception):
            extract.read_gltf_json(p)


class SourceTests(unittest.TestCase):
    def test_polyhaven_picks_every_include_with_its_md5(self):
        files = {"gltf": {"1k": {"gltf": {"url": "https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/lamp/lamp_1k.gltf", "size": 10, "md5": "a" * 32,
                                             "include": {"lamp.bin": {"url": "https://dl.polyhaven.org/x/lamp.bin", "size": 5, "md5": "b" * 32},
                                                         "textures/lamp_diff_1k.jpg": {"url": "https://dl.polyhaven.org/x/d.jpg", "size": 6, "md5": "c" * 32}}}}}}
        picks = polyhaven._pick_files(files, "models", "1k", "hdr")
        self.assertEqual([p[4] for p in picks], ["lamp_1k.gltf", "lamp.bin", "textures/lamp_diff_1k.jpg"])
        self.assertTrue(all(p[2] for p in picks))

    def test_hostile_ids_are_refused_before_any_request(self):
        def no_network(*a, **k):
            raise AssertionError("no request may be made for a hostile id")
        saved = (polyhaven.http_get, ambientcg.http_get)
        polyhaven.http_get = ambientcg.http_get = no_network
        try:
            for bad in ("../../etc", "a/../../b", "/abs"):
                with self.assertRaises(safety.UnsafePath):
                    polyhaven.fetch(bad)
                with self.assertRaises(safety.UnsafePath):
                    ambientcg.fetch(bad)
        finally:
            polyhaven.http_get, ambientcg.http_get = saved


if __name__ == "__main__":
    unittest.main()
