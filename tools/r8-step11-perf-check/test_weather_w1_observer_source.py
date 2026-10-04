from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC_PATH = ROOT / "modules/wxl-modern-render/src/weather/WeatherDiagnostics.cpp"
SRC = SRC_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
CONFIG = (ROOT / "src/config.hpp").read_text(encoding="utf-8")

class WeatherW1R4ObserverSourceTests(unittest.TestCase):
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

    def test_transmorpher_weather_landmarks_remain_observer_only(self):
        for marker in (
            "0x00CD7544",
            "0x007846A0",
            "0x0078D0B0",
            "0x00783B60",
            "0x00526A96",
        ):
            self.assertIn(marker, SRC)
        self.assertIn("landmark only; W1 does not hook packet parsing", SRC)

    def test_weather_hooks_forward_native_calls(self):
        self.assertIn(
            "g_origSetType(\n            self, edx, type, intensity, weatherRec, abrupt, transition);",
            SRC,
        )
        self.assertIn("g_origClear(self, edx);", SRC)

    def test_direct_d3d_slots_are_observed(self):
        required = (
            "gxoff::vt::kDrawPrimitive",
            "gxoff::vt::kDrawIndexedPrimitive",
            "gxoff::vt::kDrawPrimitiveUP",
            "gxoff::vt::kDrawIndexedPrimitiveUP",
            "R8WeatherW1D3DDrawPrimitiveObserver",
            "R8WeatherW1D3DDrawIndexedPrimitiveObserver",
            "R8WeatherW1D3DDrawPrimitiveUPObserver",
            "R8WeatherW1D3DDrawIndexedPrimitiveUPObserver",
        )
        for token in required:
            self.assertIn(token, SRC)
        self.assertNotIn("R8WeatherW1GxDrawObserver", SRC)

    def test_each_direct_detour_forwards_to_native_original(self):
        self.assertEqual(SRC.count("g_origDrawPrimitive("), 1)
        self.assertEqual(SRC.count("g_origDrawIndexedPrimitive("), 1)
        self.assertEqual(SRC.count("g_origDrawPrimitiveUP("), 1)
        self.assertEqual(SRC.count("g_origDrawIndexedPrimitiveUP("), 1)

    def test_observer_does_not_issue_its_own_draw_or_state_mutation(self):
        forbidden = (
            "d->DrawPrimitive(",
            "d->DrawIndexedPrimitive(",
            "d->DrawPrimitiveUP(",
            "d->DrawIndexedPrimitiveUP(",
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

    def test_candidate_filter_matches_world_space_transparent_discovery(self):
        self.assertIn('"WXL_R8_WEATHER_DIAG_MIN_PRIMS"', SRC)
        self.assertIn("D3DRS_ZENABLE", SRC)
        self.assertIn("D3DRS_ZWRITEENABLE", SRC)
        self.assertIn("D3DRS_ALPHABLENDENABLE", SRC)
        self.assertIn("D3DUSAGE_RENDERTARGET", SRC)
        self.assertIn("key.zWrite != FALSE", SRC)
        self.assertIn("key.alphaBlend == FALSE", SRC)
        self.assertIn("primitiveCount < g_config.minPrimitives", SRC)

    def test_output_is_aggregated_by_weather_segment(self):
        self.assertIn("FlushDirectCandidates", SRC)
        self.assertIn('FlushDirectCandidates("before_weather_set_type")', SRC)
        self.assertIn('FlushDirectCandidates("before_weather_clear")', SRC)
        self.assertIn('FlushDirectCandidates("periodic_900_frames")', SRC)
        self.assertIn('\\"event\\":\\"direct_draw_aggregate\\"', SRC)
        self.assertIn("std::vector<DirectCandidateStats>", SRC)

    def test_precipitation_capture_is_bounded(self):
        self.assertIn("kDefaultMaxDrawRecords = 8192", SRC)
        self.assertIn("kHardMaxDrawRecords = 32768", SRC)
        self.assertIn("kMaxOutputBytes = 24u * 1024u * 1024u", SRC)
        self.assertIn("2048u", SRC)
        self.assertIn("g_drawRecords >= g_config.maxDrawRecords", SRC)

    def test_required_aggregate_fingerprint_fields_exist(self):
        required = (
            '\\"draw_kind\\"',
            '\\"prim_type\\"',
            '\\"calls\\"',
            '\\"total_primitives\\"',
            '\\"min_primitives\\"',
            '\\"max_primitives\\"',
            '\\"texture\\"',
            '\\"vs_sha256\\"',
            '\\"ps_sha256\\"',
            '\\"vertex_decl_sha256\\"',
            '\\"stream0_stride\\"',
            '\\"render_state\\"',
            '\\"sampler0\\"',
            '\\"weather_rec_id\\"',
            '\\"requested_intensity\\"',
            '\\"first_phase\\"',
            '\\"last_phase\\"',
        )
        for token in required:
            self.assertIn(token, SRC)

    def test_weather_asset_paths_are_observed(self):
        self.assertIn("OnBlpLoad", SRC)
        self.assertIn('\\"weather_blp\\"', SRC)
        self.assertIn('n.find("rain")', SRC)
        self.assertIn('n.find("snow")', SRC)
        self.assertIn('n.find("sand")', SRC)

    def test_output_declares_no_visual_or_weather_mutation(self):
        self.assertIn("mutates_render_state", SRC)
        self.assertIn("changes_weather", SRC)
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
