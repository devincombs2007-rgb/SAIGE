"""Tests of check_skips.py (offline)."""
import json, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_skips  # noqa: E402


def junit(cases):
    parts = []
    for name, status in cases.items():
        inner = "<skipped/>" if status == "skipped" else ("<failure/>" if status == "failed" else "")
        parts.append(f'<testcase name="{name}" status="{"notrun" if status == "skipped" else "run"}">{inner}</testcase>')
    return f'<?xml version="1.0"?><testsuite tests="{len(cases)}">{"".join(parts)}</testsuite>'


class CheckSkipsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        (self.d / "allowed.json").write_text(json.dumps({"lanes": {"lane": {"jolt": "needs Jolt"}}}))

    def tearDown(self):
        self.tmp.cleanup()

    def run_with(self, cases, lane="lane", allowed=None):
        (self.d / "j.xml").write_text(junit(cases))
        return check_skips.main(["--junit", str(self.d / "j.xml"), "--lane", lane, "--allowed", str(allowed or self.d / "allowed.json")])

    def test_allowed_skip_passes(self):
        self.assertEqual(self.run_with({"a": "passed", "jolt": "skipped"}), 0)

    def test_unexpected_skip_fails(self):
        self.assertEqual(self.run_with({"a": "skipped", "jolt": "skipped"}), 1)

    def test_allowed_test_that_runs_is_only_a_note(self):
        self.assertEqual(self.run_with({"a": "passed", "jolt": "passed"}), 0)

    def test_unknown_lane_fails(self):
        self.assertEqual(self.run_with({"a": "passed"}, lane="other"), 1)

    def test_empty_junit_fails(self):
        self.assertEqual(self.run_with({}), 1)

    def test_entry_without_reason_fails(self):
        (self.d / "bad.json").write_text(json.dumps({"lanes": {"lane": {"jolt": ""}}}))
        self.assertEqual(self.run_with({"jolt": "skipped"}, allowed=self.d / "bad.json"), 1)

    def test_repository_list_is_well_formed(self):
        cfg = json.loads((Path(check_skips.__file__).with_name("allowed_skips.json")).read_text())
        for lane, entries in cfg["lanes"].items():
            for name, why in entries.items():
                self.assertTrue(why.strip(), f"{lane}/{name}")


if __name__ == "__main__":
    unittest.main()
