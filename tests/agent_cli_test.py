"""Test the process interface against a compiled engine."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path(sys.argv.pop(1)).resolve()
RUNNER = ROOT / "tools" / "agent" / "run_project.py"


def invoke(*args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15)


class AgentCliTest(unittest.TestCase):
    def test_no_frames(self):
        result = invoke(ENGINE, "--frames", "0", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), dict(schema_version=1, frames=0, ticks=0, instances=0, marker_x=0))

    def test_project_runs_sample(self):
        project = json.loads((ROOT / "examples/moving-marker/saige.project.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "project with spaces.json"
            project["frames"] = 3
            path.write_text(json.dumps(project))
            result = invoke(sys.executable, RUNNER, path, "--engine", ENGINE)
        self.assertEqual(result.returncode, 0, result.stderr)
        message = json.loads(result.stdout)
        self.assertEqual(message["schema_version"], 1)
        self.assertEqual(message["frames"], 3)
        self.assertEqual(message["instances"], 1)
        self.assertGreater(message["ticks"], 0)
        self.assertAlmostEqual(message["marker_x"], message["ticks"] * 0.016666667, places=5)

    def test_malformed_cli(self):
        for args in (("--frames", "-1"), ("--frames", "18446744073709551616"), ("--json", "--json"), ("--frames",)):
            with self.subTest(args=args):
                result = invoke(ENGINE, *args)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")

    def test_invalid_project_is_rejected_before_launch(self):
        for data in ('{"schema_version":1,"sample":"moving-marker","frames":true}',
                     '{"schema_version":1,"sample":"moving-marker","frames":2,"frames":0}',
                     '{"schema_version":2,"sample":"moving-marker","frames":1}'):
            with self.subTest(data=data), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "project.json"
                path.write_text(data)
                result = invoke(sys.executable, RUNNER, path, "--engine", Path(directory) / "missing")
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
