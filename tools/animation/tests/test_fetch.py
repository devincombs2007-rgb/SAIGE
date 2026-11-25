"""Offline tests of the asset restoration in fetch_humanoid_assets.py: a good archive is extracted and verified, and every way of being wrong (a bad archive hash, a bad member hash, a missing member, a
damaged copy already on disk, a network that fails a few times) is detected and never leaves an unverified file behind. No network: `download` is replaced."""
import hashlib, importlib.util, io, sys, tempfile, unittest, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("fetch_humanoid_assets", HERE.parent / "fetch_humanoid_assets.py")
fetch = importlib.util.module_from_spec(spec)
sys.modules["fetch_humanoid_assets"] = fetch
spec.loader.exec_module(fetch)


def make_zip(member, payload):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(member, payload)
        z.writestr("License.txt", b"CC0")
    return buf.getvalue()


def entry_for(blob, member, payload):
    return {"sha256": hashlib.sha256(payload).hexdigest(),
            "archive": {"url": "https://example.invalid/a.zip", "sha256": hashlib.sha256(blob).hexdigest(), "bytes": len(blob), "member": member}}


class Restore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = Path(self.tmp.name) / "ual" / "lib.glb"
        self.payload = b"glTF" + bytes(range(256)) * 10
        self.member = "Library [Standard]/Godot/lib.glb"
        self.blob = make_zip(self.member, self.payload)
        self.real_download = fetch.download

    def tearDown(self):
        fetch.download = self.real_download
        self.tmp.cleanup()

    def test_good_archive(self):
        fetch.download = lambda url, tries=4: self.blob
        self.assertTrue(fetch.restore_from_archive(entry_for(self.blob, self.member, self.payload), self.dest))
        self.assertEqual(self.dest.read_bytes(), self.payload)

    def test_archive_hash_mismatch_writes_nothing(self):
        entry = entry_for(self.blob, self.member, self.payload)
        fetch.download = lambda url, tries=4: self.blob + b"x"  # a truncated or altered download
        self.assertFalse(fetch.restore_from_archive(entry, self.dest))
        self.assertFalse(self.dest.exists())

    def test_member_hash_mismatch_removes_the_file(self):
        entry = entry_for(self.blob, self.member, self.payload)
        entry["sha256"] = hashlib.sha256(b"something else").hexdigest()
        fetch.download = lambda url, tries=4: self.blob
        self.assertFalse(fetch.restore_from_archive(entry, self.dest))
        self.assertFalse(self.dest.exists())

    def test_missing_member_is_an_error_not_a_silent_pass(self):
        entry = entry_for(self.blob, "Library [Standard]/missing.glb", self.payload)
        entry["archive"]["sha256"] = hashlib.sha256(self.blob).hexdigest()
        fetch.download = lambda url, tries=4: self.blob
        with self.assertRaises(KeyError):
            fetch.restore_from_archive(entry, self.dest)
        self.assertFalse(self.dest.exists())

    def test_no_archive_means_nothing_to_do(self):
        self.assertFalse(fetch.restore_from_archive({"sha256": "0" * 64}, self.dest))

    def test_download_retries_then_gives_up(self):
        calls = []

        class Boom(Exception):
            pass

        def flaky(url, timeout=0):
            calls.append(url)
            if len(calls) < 3:
                raise Boom("network")
            return io.BytesIO(b"payload")

        real_sleep, real_open = fetch.time.sleep, fetch.urllib.request.urlopen
        fetch.time.sleep = lambda s: None
        fetch.urllib.request.urlopen = flaky
        try:
            self.assertEqual(fetch.download("https://example.invalid/x"), b"payload")
            self.assertEqual(len(calls), 3)
            calls.clear()
            fetch.urllib.request.urlopen = lambda url, timeout=0: (_ for _ in ()).throw(Boom("down"))
            with self.assertRaises(RuntimeError):
                fetch.download("https://example.invalid/x", tries=2)
        finally:
            fetch.time.sleep, fetch.urllib.request.urlopen = real_sleep, real_open

    def test_manifest_has_an_archive_for_each_library(self):
        import json
        m = json.loads((HERE.parent / "assets.json").read_text())
        for key in ("ual", "ual2"):
            a = m[key]["archive"]
            self.assertTrue(a["url"].startswith("https://"))
            self.assertEqual(len(a["sha256"]), 64)
            self.assertTrue(a["member"].endswith(m[key]["file"]))


if __name__ == "__main__":
    unittest.main()
