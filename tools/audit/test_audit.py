"""Offline tests of the audit's parsing and judgement (no tools needed). Run: python3 -m unittest discover -s tools/audit"""
import json, sys, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audit  # noqa: E402


class AuditTests(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(audit.parse_version("cmake version 3.28.3"), (3, 28, 3))
        self.assertEqual(audit.parse_version("SPIRV-Tools v2025.1 unknown"), (2025, 1))
        self.assertIsNone(audit.parse_version("no digits here"))

    def test_at_least(self):
        self.assertTrue(audit.at_least((3, 28, 3), "3.21"))
        self.assertTrue(audit.at_least((3, 21), "3.21.0"))
        self.assertFalse(audit.at_least((3, 20, 9), "3.21"))
        self.assertFalse(audit.at_least(None, "1"))

    def test_pins(self):
        text = """
        FetchContent_Declare(a
            URL https://x/y.tar
            URL_HASH SHA256=%s)
        FetchContent_Declare(b
            GIT_REPOSITORY https://x/b.git
            GIT_TAG v1.2.3
        )
        FetchContent_Declare(c
            GIT_TAG %s)
        FetchContent_Declare(d
            URL https://x/z.tar)
        """ % ("a" * 64, "b" * 40)
        rows = {r["name"]: r["status"] for r in audit.pins_in_text(text)}
        self.assertEqual(rows, {"a": "ok", "b": "tag-only", "c": "ok", "d": "unpinned"})

    def test_repository_pins_are_found_and_hashed_or_flagged(self):
        rows = audit.audit_pins({})
        names = {r["name"] for r in rows}
        self.assertTrue({"saige_json", "saige_stb_image", "saige_vulkan_headers"} <= names, names)
        by = {r["name"]: r["status"] for r in rows}
        self.assertEqual(by["saige_json"], "ok")      # URL_HASH
        self.assertEqual(by["saige_stb_image"], "ok")  # URL_HASH
        self.assertNotEqual(by.get("saige_vulkan_headers"), "unpinned")

    def test_policy_is_valid_and_report_fails_on_a_missing_required_tool(self):
        policy = json.loads((Path(__file__).with_name("policy.json")).read_text())
        for name, spec in policy["tools"].items():
            self.assertIn("min", spec, name)
            self.assertIn("recommended", spec, name)
            self.assertTrue(audit.at_least(tuple(int(x) for x in spec["recommended"].split(".")), spec["min"]), name)
        policy["tools"]["definitely-not-installed"] = {"min": "1", "recommended": "1", "probe": ["definitely-not-installed", "--version"]}
        report = audit.build_report(policy)
        self.assertTrue(any("definitely-not-installed" in f for f in report["failures"]))
        policy["tools"]["definitely-not-installed"]["optional"] = True
        report = audit.build_report(policy)
        self.assertFalse(any("definitely-not-installed" in f for f in report["failures"]))


if __name__ == "__main__":
    unittest.main()
