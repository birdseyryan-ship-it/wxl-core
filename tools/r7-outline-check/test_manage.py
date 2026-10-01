import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import hashlib
import subprocess

SPEC = importlib.util.spec_from_file_location("candidate_manager", Path(__file__).resolve().parents[2] / "experiments/r7-outline/manage_candidate.py")
manager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(manager)


class ReversibleCandidate(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.client, self.package = root / "client", root / "package"
        self.client.mkdir()
        self.package.mkdir()
        self.before = {"Wow.exe": b"test executable", "d3d9.dll": b"frozen proxy", "WarcraftXL.dll": b"accepted core", "CoAVolFog.dll": b"companion", "CoAVolFog.ini": b"LocalLights=0"}
        for name, data in self.before.items():
            (self.client / name).write_bytes(data)
        (self.package / "WarcraftXL.dll").write_bytes(b"diagnostic core")
        self.digest = lambda data: hashlib.sha256(data).hexdigest()
        (self.package / "SHA256SUMS.txt").write_text(self.digest(b"diagnostic core") + "  WarcraftXL.dll\n")
        self.patches = [patch.object(manager, "WOW", self.digest(self.before["Wow.exe"])),
                        patch.object(manager, "D3D", self.digest(self.before["d3d9.dll"])),
                        patch.object(manager, "ACCEPTED", self.digest(self.before["WarcraftXL.dll"])),
                        patch.object(manager.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, "", ""))]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in reversed(self.patches):
            p.stop()
        self.temp.cleanup()

    def run_action(self, action):
        with patch.object(sys, "argv", ["manage_candidate.py", action, "--client", str(self.client), "--package", str(self.package)]):
            manager.main()

    def unchanged(self):
        for name, data in self.before.items():
            if name != "WarcraftXL.dll":
                self.assertEqual((self.client / name).read_bytes(), data)

    def test_install_rollback_and_immutable_files(self):
        self.run_action("install")
        self.assertEqual((self.client / "WarcraftXL.dll").read_bytes(), b"diagnostic core")
        self.unchanged()
        self.run_action("rollback")
        self.assertEqual((self.client / "WarcraftXL.dll").read_bytes(), self.before["WarcraftXL.dll"])
        self.unchanged()

    def test_unknown_live_core_and_running_process_rejected(self):
        (self.client / "WarcraftXL.dll").write_bytes(b"another candidate")
        with self.assertRaises(RuntimeError):
            self.run_action("install")
        with patch.object(manager.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "123 Wow.exe", "")):
            with self.assertRaises(RuntimeError):
                manager.preflight(self.client)
        self.unchanged()

    def test_rollback_refuses_unrelated_later_candidate(self):
        self.run_action("install")
        (self.client / "WarcraftXL.dll").write_bytes(b"later candidate")
        with self.assertRaises(RuntimeError):
            self.run_action("rollback")
        self.assertEqual((self.client / "WarcraftXL.dll").read_bytes(), b"later candidate")

    def test_frozen_proxy_and_manifest_mismatch_rejected(self):
        (self.package / "WarcraftXL.dll").write_bytes(b"corrupted candidate")
        with self.assertRaises(RuntimeError):
            self.run_action("install")
        self.assertEqual((self.client / "WarcraftXL.dll").read_bytes(), self.before["WarcraftXL.dll"])
        (self.client / "d3d9.dll").write_bytes(b"different proxy")
        with self.assertRaises(RuntimeError):
            manager.preflight(self.client)


if __name__ == "__main__":
    unittest.main()
