import hashlib
import json
import pathlib
import re
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = "537790002f54ad54e84024fe2973f8bbcaff78e2"
RUNTIME_PATH = "src/client/CWorldScene/StructuralMaterialRuntime.cpp"
RUNTIME = (ROOT / RUNTIME_PATH).read_text()
PROOF = (ROOT / "src/client/CWorldScene/WmoC28Proof.cpp").read_text()
CORE = (ROOT / "src/client/CWorldScene/WmoC28ProofCore.hpp").read_text()
FIXTURES = pathlib.Path(__file__).parent / "fixtures"


def base_file(path):
    return subprocess.check_output(["git", "show", f"{BASE}:{path}"], cwd=ROOT).decode().replace("\r\n", "\n")


def function(text, name):
    start = text.index(name + "(")
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if text[end] == "{": depth += 1
        if text[end] == "}": depth -= 1
        end += 1
    return text[start:end]


class C28Source(unittest.TestCase):
    def test_opt_in_and_install_gates(self):
        self.assertIn("bool wmoC28Proof = false;", RUNTIME)
        self.assertIn('ReadBoolEnvironment("WXL_R8_WMO_C28_PROOF", c28ProofValid)', RUNTIME)
        self.assertIn("perPixelValid && c28ProofValid && proofCompatible", RUNTIME)
        install = function(RUNTIME, "InstallStructuralMaterialSubstrate")
        self.assertLess(install.index("if (!g_config.master)"), install.index("InitializeWmoC28Proof()"))
        self.assertIn("if (g_config.wmoC28Proof) InitializeWmoC28Proof();", install)
        self.assertIn("static ProofEvents events;", function(PROOF, "InitializeWmoC28Proof"))

    def test_b1_source_is_byte_equivalent_after_eol_normalization(self):
        for path in ("src/client/CWorldScene/WmoPerPixelCandidate.cpp", "src/client/CWorldScene/WmoPerPixelCandidate.hpp"):
            self.assertEqual(base_file(path), (ROOT / path).read_text())

    def test_m2_function_and_install_unchanged(self):
        old = base_file(RUNTIME_PATH)
        for name in ("HookM2SetupMaterial", "InstallM2Substrate"):
            self.assertEqual(function(old, name), function(RUNTIME, name))

    def test_proof_has_getters_only(self):
        both = PROOF + CORE
        for name in ("SetVertexShader", "SetPixelShader", "SetVertexShaderConstantF", "SetPixelShaderConstantF",
                     "CreateVertexShader", "CreatePixelShader", "SetTexture", "SetRenderState", "SetSamplerState",
                     "GxStateSet", "kGxDeviceDraw", "kShaderConstantsSet", "WriteProcessMemory"):
            self.assertNotIn(name, both)
        self.assertIn("GetVertexShaderConstantF", CORE)
        self.assertIn("GetPixelShaderConstantF", CORE)
        self.assertNotIn("WmoPerPixelCandidate.hpp", PROOF)

    def test_identity_invalidated_before_native_and_scope_exit(self):
        hook = function(RUNTIME, "HookWmoEffectBind")
        self.assertLess(hook.index("InvalidateWmoC28Proof()"), hook.index("g_origWmoEffectBind("))
        self.assertLess(hook.index("g_origWmoEffectBind("), hook.index("LogWmoC28Proof("))
        self.assertIn("LeaveWmoC28ProofScope()", RUNTIME)
        for name in ("EndFrame", "Lost", "Reset"):
            self.assertIn("pending.active = false", function(PROOF, name))

    def test_all_d3d_draw_entrypoints_preserve_forward_arguments(self):
        expected = {"HookDP": "c->dp(d, t, start, n)", "HookDIP": "c->dip(d, t, base, min, vertices, start, n)",
                    "HookDPUP": "c->dpup(d, t, n, data, stride)", "HookDIPUP": "c->dipup(d, t, min, vertices, n, indices, fmt, data, stride)"}
        for name, call in expected.items():
            self.assertEqual(function(PROOF, name).count(call), 1)

    def test_no_pointer_hash_cache_and_bounded_capture(self):
        self.assertNotIn("unordered_map", PROOF)
        self.assertIn("kSamplesPerIdentity = 8", PROOF)
        self.assertIn("kMaxIdentities = 64", PROOF)
        self.assertIn("kDrawsPerBind = 2", PROOF)
        self.assertIn("s.size() > 980", PROOF)
        self.assertIn("before.vs != ticket.vs || before.ps != ticket.ps", PROOF)

    def test_exact_shader_fixtures_and_udiffuse_equation(self):
        manifest = json.loads((FIXTURES / "manifest.json").read_text())
        hashes = re.findall(r'"([a-f0-9]{64})"', CORE)
        self.assertEqual(len(hashes), 8)
        for h in hashes:
            self.assertEqual(hashlib.sha256((FIXTURES / (h + ".sm3")).read_bytes()).hexdigest(), h)
        for h, slots in ((hashes[0], [31,41,51]), (hashes[1], [61,71,81])):
            entries = [r for r in manifest["slots"] if r["SHA256"] == h]
            self.assertEqual([int(r["SLOT"]) for r in entries], slots)
            self.assertTrue(all("MapObjUDiffuse_T1" in r["SOURCE_FILE"] for r in entries))
            asm = (FIXTURES / (h + ".asm")).read_text()
            for op in ("dp3_sat r1.x, -c12, r2", "mad_sat r1.xyz, r1.x, r2, c10", "mad r1.xyz, c28, r1, v2", "add_sat o1.xyz, r1, c29"):
                self.assertIn(op, asm)
            self.assertNotRegex(asm, r"\bc(?:17|18|21|22|25|26|27)\b")

    def test_candidate_ps_register_is_unused_by_exact_native_ps(self):
        for h in re.findall(r'"([a-f0-9]{64})"', CORE)[4:]:
            self.assertNotRegex((FIXTURES / (h + ".asm")).read_text(), r"\bc31\b")


if __name__ == "__main__":
    unittest.main()
