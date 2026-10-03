from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]

HDR = (
    ROOT
    / "modules/wxl-modern-render/src/gpu/PerfProbe.hpp"
)

CPP = (
    ROOT
    / "modules/wxl-modern-render/src/gpu/PerfProbe.cpp"
)

FALLBACK = (
    ROOT
    / "modules/wxl-modern-render/src/gpu/D3D9Fallback.cpp"
)

SMAA = (
    ROOT
    / "modules/wxl-modern-render/src/gpu/D3D9Smaa.cpp"
)

WATER = (
    ROOT
    / "modules/wxl-modern-render/src/water/WaterDiagnostics.cpp"
)

DEPTH = (
    ROOT
    / "modules/wxl-modern-render/src/water/Slot3DepthRuntime.cpp"
)


class PerfProbeSourceTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.hdr = HDR.read_text(
            encoding="utf-8"
        )

        cls.cpp = CPP.read_text(
            encoding="utf-8"
        )

        cls.fallback = FALLBACK.read_text(
            encoding="utf-8"
        )

        cls.smaa = SMAA.read_text(
            encoding="utf-8"
        )

        cls.water = WATER.read_text(
            encoding="utf-8"
        )

        cls.depth = DEPTH.read_text(
            encoding="utf-8"
        )

    def test_feature_flag_is_exact(self):
        self.assertIn(
            '"WXL_R8_PERF_PROBE"',
            self.cpp,
        )

    def test_default_off_environment_contract(self):
        self.assertIn(
            "if (n == 0 ||",
            self.cpp,
        )

        self.assertIn(
            "return false;",
            self.cpp,
        )

    def test_runtime_activation_marker_precedes_gated_timing(self):
        activation = "perf::ReportActivation();"
        timing = "perf::CpuScope framePerf("

        self.assertIn(
            activation,
            self.fallback,
        )

        self.assertIn(
            timing,
            self.fallback,
        )

        self.assertLess(
            self.fallback.index(activation),
            self.fallback.index(timing),
        )

    def test_activation_marker_records_runtime_environment(self):
        expected = (
            '"wxl-r8-perf: activation "',
            '"WXL_R8_PERF_PROBE"',
            '"env_present=%u env_len=%lu "',
            '"raw=%s enabled=%u"',
            "GetEnvironmentVariableA",
        )

        for token in expected:
            self.assertIn(
                token,
                self.cpp,
            )

    def test_first_enabled_frame_boundary_is_observable(self):
        self.assertIn(
            '"wxl-r8-perf: frame_boundary_first "',
            self.cpp,
        )

    def test_qpc_is_cpu_clock(self):
        self.assertIn(
            "QueryPerformanceCounter",
            self.cpp,
        )

        self.assertIn(
            "QueryPerformanceFrequency",
            self.cpp,
        )

    def test_gpu_phase_is_capability_only(self):
        self.assertIn(
            "D3DQUERYTYPE_TIMESTAMP",
            self.cpp,
        )

        self.assertIn(
            "D3DQUERYTYPE_TIMESTAMPFREQ",
            self.cpp,
        )

        self.assertIn(
            "D3DQUERYTYPE_TIMESTAMPDISJOINT",
            self.cpp,
        )

        self.assertNotIn(
            "GetData(",
            self.cpp,
        )

        self.assertNotIn(
            "D3DGETDATA_FLUSH",
            self.cpp,
        )

        self.assertNotIn(
            "Issue(",
            self.cpp,
        )

    def test_probe_core_contains_no_render_mutation(self):
        forbidden = (
            "SetRenderTarget(",
            "SetDepthStencilSurface(",
            "SetTexture(",
            "SetPixelShader(",
            "SetVertexShader(",
            "SetRenderState(",
            "DrawPrimitive(",
            "DrawPrimitiveUP(",
            "DrawIndexedPrimitive(",
            "StretchRect(",
        )

        for token in forbidden:
            self.assertNotIn(
                token,
                self.cpp,
                token,
            )

    def test_bounded_summary_window(self):
        self.assertIn(
            "kEmitEveryFrames",
            self.cpp,
        )

        self.assertIn(
            "kSampleCapacity",
            self.cpp,
        )

    def test_fallback_regions_present(self):
        expected = (
            "FallbackFrameTotal",
            "FallbackStateBlock",
            "FallbackEndScene",
            "FallbackColourResolve",
            "FallbackDepthStage1",
            "FallbackDepthStage2",
            "FallbackBeginScene",
            "AoRawDraw",
            "AoCompositeDraw",
        )

        for token in expected:
            self.assertIn(
                token,
                self.fallback,
            )

    def test_smaa_regions_present(self):
        expected = (
            "SmaaTotal",
            "SmaaEdgeDraw",
            "SmaaBlendDraw",
            "SmaaNeighborhoodDraw",
        )

        for token in expected:
            self.assertIn(
                token,
                self.smaa,
            )

    def test_water_snapshot_regions_present(self):
        expected = (
            "WaterSnapshotTotal",
            "WaterEndScene",
            "WaterColourCopy",
            "WaterDepthStage1",
            "WaterDepthStage2",
            "WaterBeginScene",
        )

        for token in expected:
            self.assertIn(
                token,
                self.water,
            )

    def test_water_linear_depth_region_present(self):
        self.assertIn(
            "WaterLinearDepth",
            self.depth,
        )

    def test_reset_invalidates_perf_probe(self):
        self.assertIn(
            "perf::Reset();",
            self.fallback,
        )

    def test_summary_is_aggregate_not_per_draw_log(self):
        self.assertIn(
            '"wxl-r8-perf: cpu_summary "',
            self.cpp,
        )

        self.assertIn(
            '"wxl-r8-perf: cpu "',
            self.cpp,
        )

    def test_gpu_capability_log_declares_no_wait(self):
        self.assertIn(
            "capability_only=1 issue=0 getdata=0 wait=0",
            self.cpp,
        )

    def test_header_documents_default_off(self):
        self.assertIn(
            "WXL_R8_PERF_PROBE=1",
            self.hdr,
        )

        self.assertIn(
            "Default OFF",
            self.hdr,
        )


if __name__ == "__main__":
    unittest.main()
