from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / "modules/wxl-modern-render/src/gpu/D3D9Fallback.cpp").read_text(encoding="utf-8")

class AoDisableDiagnosticSourceTests(unittest.TestCase):
    def test_flag_exists_and_defaults_off(self):
        self.assertIn('"WXL_R8_AO_DISABLE"', SRC)
        self.assertIn("if (n == 0 ||\n                    n >= sizeof(raw))", SRC)
        self.assertIn("return false;", SRC)

    def test_only_production_ao_is_gated(self):
        self.assertIn("const bool productionAoDisabled =\n            AoProductionDisabled();", SRC)
        self.assertIn("g_ambientOcclusion &&\n            !productionAoDisabled;", SRC)

    def test_ao_proof_path_is_not_disabled(self):
        self.assertIn("const bool aoProof =\n            !depthProof &&\n            requestedAoProofMode > 0;", SRC)
        self.assertIn("const bool aoActive =\n            aoProof || productionAo;", SRC)

    def test_smaa_can_still_run_when_production_ao_disabled(self):
        self.assertIn("const bool useSmaa =", SRC)
        self.assertIn("smaaEnabled;", SRC)
        self.assertNotIn("productionAoDisabled &&\n            smaaEnabled", SRC)

    def test_runtime_marker_exists(self):
        self.assertIn(
            '"wxl-r8-diag: production CE AO disabled "',
            SRC,
        )
        self.assertIn(
            '"by WXL_R8_AO_DISABLE=1; AA remains available"',
            SRC,
        )

    def test_a3_scale_and_fast_shader_remain_present(self):
        self.assertIn("WXL_R8_AO_SCALE_PERCENT", SRC)
        self.assertIn("WXL_R8_AO_FAST_PRESET2", SRC)
        self.assertIn("g_aoFastProductionShader", SRC)

if __name__ == "__main__":
    unittest.main()
