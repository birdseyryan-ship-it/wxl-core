from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
SRC=(ROOT/"modules/wxl-modern-render/src/gpu/D3D9Fallback.cpp").read_text(encoding="utf-8")

class AoScaleSourceTests(unittest.TestCase):
    def test_flag_and_default(self):
        self.assertIn('"WXL_R8_AO_SCALE_PERCENT"', SRC)
        self.assertIn("return 100;", SRC)
    def test_allowed_scales(self):
        for v in ("case 50:","case 67:","case 70:","case 75:","case 80:","case 100:"):
            self.assertIn(v,SRC)
    def test_half_res_alias_preserved(self):
        self.assertIn("if (AoHalfResolutionFallbackEnabled())\n                    return 50;", SRC)
    def test_fullres_composite_unchanged(self):
        self.assertIn("CreateTexture(\n                    fullW,\n                    fullH,", SRC)
    def test_scaled_raw_target(self):
        self.assertIn("aoScalePercent", SRC)
        self.assertIn("raw_scale_percent=%d", SRC)
    def test_fast_preset2_still_present(self):
        self.assertIn("WXL_R8_AO_FAST_PRESET2", SRC)
        self.assertIn("g_aoFastProductionShader", SRC)

if __name__=="__main__":
    unittest.main()
