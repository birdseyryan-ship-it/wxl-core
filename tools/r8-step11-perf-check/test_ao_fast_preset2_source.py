from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]

FALLBACK = (
    ROOT
    / "modules/wxl-modern-render/src/gpu/D3D9Fallback.cpp"
)


class FastAoPreset2SourceTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.src = FALLBACK.read_text(
            encoding="utf-8"
        )

    def test_feature_flag_is_exact_and_default_off(self):
        self.assertIn(
            '"WXL_R8_AO_FAST_PRESET2"',
            self.src,
        )

        self.assertIn(
            "if (n == 0 ||\n                    n >= sizeof(raw))",
            self.src,
        )

        self.assertIn(
            "return false;",
            self.src,
        )

    def test_fast_shader_is_separate_and_released(self):
        self.assertIn(
            "g_aoFastProductionShader",
            self.src,
        )

        self.assertIn(
            "SafeRelease(g_aoFastProductionShader);",
            self.src,
        )

    def test_fast_path_is_production_only(self):
        self.assertIn(
            "const bool fastProductionAo =\n"
            "            productionAo &&\n"
            "            AoFastPreset2Enabled();",
            self.src,
        )

        self.assertIn(
            "if (fastProductionAo)",
            self.src,
        )

        self.assertIn(
            "shader = g_aoFastProductionShader;",
            self.src,
        )

        self.assertIn(
            "shader = g_aoProofShader;",
            self.src,
        )

    def test_full_resolution_target_policy_is_unchanged(self):
        self.assertIn(
            "const bool fullResAo =\n"
            "                !AoHalfResolutionFallbackEnabled();",
            self.src,
        )

        self.assertNotIn(
            "WXL_R8_AO_FAST_PRESET2",
            self.src[
                self.src.index("bool EnsureAoTarget("):
                self.src.index("void RestoreDeviceState(", self.src.index("bool EnsureAoTarget("))
            ],
        )

    def test_fast_shader_keeps_twelve_tap_topology(self):
        start = self.src.index(
            'static const char* kAoFastProductionPs'
        )

        end = self.src.index(
            ')HLSL";',
            start,
        )

        shader = self.src[start:end]

        self.assertEqual(
            shader.count("occ += horizonPair("),
            6,
        )

        self.assertIn(
            "0.32",
            shader,
        )

        self.assertIn(
            "0.72",
            shader,
        )

    def test_accepted_preset2_constants_are_specialized(self):
        start = self.src.index(
            'static const char* kAoFastProductionPs'
        )

        end = self.src.index(
            ')HLSL";',
            start,
        )

        shader = self.src[start:end]

        for token in (
            "0.42",
            "1.58",
            "0.022",
            "0.026",
            "1.10",
            "18.0",
            "55.0",
            "3.0",
        ):
            self.assertIn(
                token,
                shader,
            )

    def test_hot_radial_pow_is_replaced_by_cubic(self):
        start = self.src.index(
            'static const char* kAoFastProductionPs'
        )

        end = self.src.index(
            ')HLSL";',
            start,
        )

        shader = self.src[start:end]

        self.assertIn(
            "falloff *\n        falloff *\n        falloff",
            shader,
        )

        self.assertNotIn(
            "max(aoControl.w, 1.0)",
            shader,
        )

    def test_paired_sincos_is_used(self):
        start = self.src.index(
            'static const char* kAoFastProductionPs'
        )

        end = self.src.index(
            ')HLSL";',
            start,
        )

        shader = self.src[start:end]

        self.assertIn(
            "sincos(angle, s, c);",
            shader,
        )

        self.assertIn(
            "float2 d3 = -d0;",
            shader,
        )

        self.assertIn(
            "float2 d4 = -d1;",
            shader,
        )

        self.assertIn(
            "float2 d5 = -d2;",
            shader,
        )

    def test_raw_pass_uses_selected_shader(self):
        self.assertIn(
            "device->SetPixelShader(\n                shader);",
            self.src,
        )

    def test_fast_path_has_explicit_runtime_marker(self):
        self.assertIn(
            '"wxl-r8-opt: fast AO Preset-2 requested "',
            self.src,
        )

        self.assertIn(
            '"wxl-r8-opt: fast full-res AO Preset-2 shader ready "',
            self.src,
        )


if __name__ == "__main__":
    unittest.main()
