from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent
WXL = ROOT.parents[1]
UPSTREAM = Path(os.environ.get("COA_VFOG_TEST_ROOT", str(WXL.parent / "coa-vfog")))


def module(name: str):
    specification = importlib.util.spec_from_file_location(name, ROOT / (name + ".py"))
    loaded = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(loaded)
    return loaded


analyzer = module("analyze_fog_log")
patcher = module("patch_coavolfog_step_cache")
comparator = module("compare_shader_artifacts")


class ShaderArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.baseline = Path(self.temporary.name) / "baseline"
        self.cached = Path(self.temporary.name) / "cached"
        for root in (self.baseline, self.cached):
            (root / "shaders").mkdir(parents=True)
            (root / "waterdata.bin").write_bytes(b"frozen-water")
            (root / "fogdata.bin").write_bytes(b"frozen-fog")
            (root / "shaders/ps_water.h").write_text("const BYTE g_water[] = {0, 3, 255, 255};", encoding="utf-8")
            (root / "shaders/ps_lit_march_mid.h").write_text("const BYTE g_lit[] = {0, 3, 255, 255};", encoding="utf-8")

    def test_ignores_header_metadata_but_refuses_a_water_bytecode_change(self):
        shader = self.cached / "shaders/ps_water.h"
        shader.write_text("metadata\nconst BYTE g_water[] = {0x00, 0x03, 0xff, 0xff};", encoding="utf-8")
        self.assertEqual(comparator.compare(self.baseline, self.cached)["status"], "passed")
        shader.write_text("const BYTE g_water[] = {0, 3, 0, 255};", encoding="utf-8")
        self.assertEqual(comparator.compare(self.baseline, self.cached)["unexpected_changes"], ["ps_water.h"])

    def test_allows_lit_changes_without_claiming_a_speedup(self):
        shader = self.cached / "shaders/ps_lit_march_mid.h"
        shader.write_text("const BYTE g_lit[] = {0, 3, 0, 255};", encoding="utf-8")
        result = comparator.compare(self.baseline, self.cached)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(len(result["changed"]), 1)
        self.assertFalse(result["gpu_speedup_validated"])
        (self.cached / "waterdata.bin").write_bytes(b"changed-water")
        self.assertEqual(comparator.compare(self.baseline, self.cached)["status"], "failed")

    def test_rejects_missing_or_non_bytecode_headers(self):
        (self.cached / "shaders/ps_water.h").unlink()
        with self.assertRaises(ValueError):
            comparator.compare(self.baseline, self.cached)
        (self.cached / "shaders/ps_water.h").write_text("no bytecode", encoding="utf-8")
        with self.assertRaises(ValueError):
            comparator.compare(self.baseline, self.cached)


class LogTests(unittest.TestCase):
    def test_does_not_assign_a_cause_to_disabled_slow_intervals(self):
        log = b"""22:50:00.000 settings from CoAVolFog.ini: LocalLights 1 -> 0
22:50:01.000 settings from CoAVolFog.ini: Temporal 0 -> 0.85
22:50:10.000 frame 600: fog gpu 4.30 ms (median of 600 frames, 0 skipped)
22:50:10.000   camera (1 2 3) target (4 5 6)
22:50:10.000   local points not captured enabled 0;
22:50:20.000 frame 1200: fog gpu 4.30 ms (median of 600 frames, 0 skipped)
22:50:20.000   camera (1 2 3) target (4 5 6)
22:50:20.000   local points not captured enabled 0;
22:50:40.000 frame 1800: fog gpu 21.00 ms (median of 600 frames, 0 skipped)
22:50:40.000   camera (1 2 3) target (4 5 6)
22:50:40.000   local points not captured enabled 0;
"""
        result = analyzer.analyze(log)
        self.assertEqual(result["disabled_stable_records"], 2)
        self.assertEqual(result["disabled_slow_records"], 1)
        self.assertEqual(result["fixed_camera_contrasts"][0]["difference_ms"], 16.7)
        self.assertEqual(result["root_cause"], "UNKNOWN")
        self.assertFalse(result["gpu_speedup_validated"])
        self.assertEqual(result["samples"][-1]["average_frame_rate_from_log"], 30)

    def test_intervals_crossing_a_setting_change_are_excluded(self):
        log = b"""22:50:10.000 frame 600: fog gpu 4.30 ms (median of 600 frames, 0 skipped)
22:50:10.000 local points 2 captured enabled 1;
22:50:12.000 settings from CoAVolFog.ini: LocalLights 1 -> 0
22:50:20.000 frame 1200: fog gpu 21.00 ms (median of 600 frames, 0 skipped)
22:50:20.000 local points not captured enabled 0;
"""
        result = analyzer.analyze(log)
        self.assertEqual(result["disabled_stable_records"], 0)
        self.assertEqual(result["samples"][0]["captured_lights"], 2)


class PatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not UPSTREAM.is_dir():
            raise unittest.SkipTest("pinned upstream checkout is required")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.target = Path(self.temporary.name) / "upstream"
        subprocess.run(["git", "clone", "--quiet", "--shared", "--no-checkout", str(UPSTREAM), str(self.target)],
                       check=True, capture_output=True)
        subprocess.run(["git", "-C", str(self.target), "checkout", "--quiet", patcher.PINNED],
                       check=True, capture_output=True)

    def r6(self):
        subprocess.run(["python", str(WXL / "experiments/r6-forever-water/patch_coavolfog_for_wxl_r6.py"),
                        str(self.target)], check=True, capture_output=True)

    def test_refuses_pristine_upstream_without_accepted_depth_transport(self):
        before = (self.target / "shaders/vf_integrate.hlsli").read_bytes()
        with self.assertRaises(ValueError):
            patcher.prepare(self.target, "cached")
        self.assertEqual(before, (self.target / "shaders/vf_integrate.hlsli").read_bytes())

    def test_prepare_is_read_only_and_retains_all_depth_and_water_files(self):
        self.r6()
        original = {name: (self.target / name).read_bytes() for name in patcher.REQUIRED}
        additions = patcher.prepare(self.target, "cached")
        self.assertNotIn(self.target / "src/msaa_depth.cpp", additions)
        self.assertNotIn(self.target / "src/d3d9_wrap.cpp", additions)
        self.assertTrue(all(data == (self.target / name).read_bytes() for name, data in original.items()))
        self.assertIn(b"#define WXL_R7_STEP_CACHE 1", additions[self.target / "shaders/vf_integrate.hlsli"])
        baseline = patcher.prepare(self.target, "baseline")
        self.assertIn(b"#define WXL_R7_STEP_CACHE 0", baseline[self.target / "shaders/vf_integrate.hlsli"])
        self.assertEqual(additions[self.target / "tests/shaders/r7_reference_march.hlsl"],
                         baseline[self.target / "tests/shaders/r7_reference_march.hlsl"])

    def test_altered_shader_or_existing_payload_fail_before_any_write(self):
        self.r6()
        shader = self.target / "shaders/vf_integrate.hlsli"
        original = shader.read_bytes()
        shader.write_bytes(original + b"\n")
        with self.assertRaises(ValueError):
            patcher.prepare(self.target, "cached")
        shader.write_bytes(original)
        payload = self.target / "tests/r7_step_cache_checks.h"
        payload.write_text("existing", encoding="utf-8")
        with self.assertRaises(ValueError):
            patcher.prepare(self.target, "cached")
        self.assertEqual(payload.read_text(encoding="utf-8"), "existing")


if __name__ == "__main__":
    unittest.main()
