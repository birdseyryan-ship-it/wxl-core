from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC_PATH = ROOT / "modules/wxl-modern-render/src/weather/WeatherDiagnostics.cpp"
SRC = SRC_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
CONFIG = (ROOT / "src/config.hpp").read_text(encoding="utf-8")

class WeatherW1ObserverSourceTests(unittest.TestCase):
    def test_default_off_opt_in_gate_exists(self):
        self.assertIn('"WXL_R8_WEATHER_DIAG"', SRC)
        self.assertIn('if (!g_config.enabled)', SRC)
        self.assertIn('observer disabled', SRC)

    def test_canonical_executable_gate_is_exact(self):
        self.assertIn(
            '"57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da"',
            SRC,
        )
        self.assertIn("VerifyCanonicalExecutable()", SRC)
        self.assertIn("Wow.exe authority mismatch", SRC)

    def test_transmorpher_weather_landmarks_are_observer_only(self):
        for marker in (
            "0x00CD7544",
            "0x007846A0",
            "0x0078D0B0",
            "0x00783B60",
            "0x00526A96",
        ):
            self.assertIn(marker, SRC)
        self.assertIn("kSmsWeatherPath", SRC)
        self.assertIn("landmark only; W1 does not hook packet parsing", SRC)

    def test_weather_hooks_forward_native_calls(self):
        self.assertIn(
            "g_origSetType(\n            self, edx, type, intensity, weatherRec, abrupt, transition);",
            SRC,
        )
        self.assertIn("g_origClear(self, edx);", SRC)

    def test_draw_observer_forwards_exactly_once_and_does_not_draw(self):
        self.assertEqual(
            SRC.count("g_origGxDeviceDraw(device, edx, batch, indexed);"),
            1,
        )
        self.assertNotIn("DrawPrimitive(", SRC)
        self.assertNotIn("DrawIndexedPrimitive(", SRC)
        self.assertNotIn("DrawPrimitiveUP(", SRC)
        self.assertNotIn("DrawIndexedPrimitiveUP(", SRC)

    def test_observer_is_read_only_for_d3d_state(self):
        forbidden = (
            "->SetRenderState(",
            "->SetTexture(",
            "->SetPixelShader(",
            "->SetVertexShader(",
            "->SetSamplerState(",
            "->SetRenderTarget(",
            "->SetDepthStencilSurface(",
        )
        for token in forbidden:
            self.assertNotIn(token, SRC)

    def test_precipitation_candidate_filter_is_bounded(self):
        self.assertIn("kDefaultMaxDrawRecords = 8192", SRC)
        self.assertIn("kHardMaxDrawRecords = 32768", SRC)
        self.assertIn("kMaxOutputBytes = 24u * 1024u * 1024u", SRC)
        self.assertIn("D3DRS_ALPHABLENDENABLE", SRC)
        self.assertIn("D3DRS_ALPHATESTENABLE", SRC)
        self.assertIn('"world_scene"', SRC)
        self.assertIn('"liquid"', SRC)

    def test_required_fingerprint_fields_exist(self):
        required = (
            '"prim_type"',
            '"start_index"',
            '"index_count"',
            '"vs_sha256"',
            '"ps_sha256"',
            '"vertex_decl_sha256"',
            '"stream0_stride"',
            '"textures"',
            '"render_state"',
            '"sampler0"',
            '"viewport"',
            '"rt0"',
            '"depth"',
            '"weather_rec_id"',
            '"requested_intensity"',
            '"phase"',
        )
        for token in required:
            self.assertIn(token, SRC)

    def test_weather_asset_paths_are_observed(self):
        self.assertIn("OnBlpLoad", SRC)
        self.assertIn('"weather_blp"', SRC)
        self.assertIn('n.find("rain")', SRC)
        self.assertIn('n.find("snow")', SRC)
        self.assertIn('n.find("sand")', SRC)

    def test_output_declares_no_visual_or_weather_mutation(self):
        self.assertIn('"\\"mutates_render_state\\":false"', SRC)
        self.assertIn('"\\"changes_weather\\":false"', SRC)
        self.assertIn("visual_changes=0", SRC)

    def test_weather_source_is_built_into_warcraftxl(self):
        self.assertIn(
            'modules/wxl-modern-render/src/weather/WeatherDiagnostics.cpp',
            CMAKE,
        )

    def test_controller_overlay_remains_disabled(self):
        self.assertIn("inline constexpr bool imguiOverlay = false;", CONFIG)
        self.assertNotIn("inline constexpr bool imguiOverlay = true;", CONFIG)

if __name__ == "__main__":
    unittest.main()
