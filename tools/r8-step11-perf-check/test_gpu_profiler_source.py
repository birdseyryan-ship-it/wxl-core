from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]

GPU_HDR = (
    ROOT
    / "modules/wxl-modern-render/src/gpu/GpuPerfRing.hpp"
)

GPU_CPP = (
    ROOT
    / "modules/wxl-modern-render/src/gpu/GpuPerfRing.cpp"
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


class AsyncGpuProfilerSourceTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.hdr = GPU_HDR.read_text(
            encoding="utf-8"
        )

        cls.cpp = GPU_CPP.read_text(
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

    def test_ring_is_bounded_and_has_old_frame_gate(self):
        self.assertIn(
            "kRingSize = 8",
            self.cpp,
        )

        self.assertIn(
            "kMinHarvestAge = 4",
            self.cpp,
        )

        self.assertIn(
            "if (age <\n                kMinHarvestAge)",
            self.cpp,
        )

    def test_required_query_types_are_created(self):
        for token in (
            "D3DQUERYTYPE_TIMESTAMP",
            "D3DQUERYTYPE_TIMESTAMPFREQ",
            "D3DQUERYTYPE_TIMESTAMPDISJOINT",
        ):
            self.assertIn(
                token,
                self.cpp,
            )

    def test_disjoint_has_begin_and_end_issue(self):
        self.assertIn(
            "slot.disjoint->Issue(\n                    D3DISSUE_BEGIN)",
            self.cpp,
        )

        self.assertIn(
            "const HRESULT disjointEndHr =",
            self.cpp,
        )

        self.assertIn(
            "slot.disjoint->Issue(\n                    D3DISSUE_END)",
            self.cpp,
        )

    def test_timestamp_and_frequency_use_end_issue(self):
        self.assertIn(
            "slot.frequency->Issue(\n                    D3DISSUE_END)",
            self.cpp,
        )

        self.assertIn(
            "slot.totalBegin->Issue(\n                    D3DISSUE_END)",
            self.cpp,
        )

        self.assertIn(
            "slot.totalEnd->Issue(\n                    D3DISSUE_END)",
            self.cpp,
        )

    def test_getdata_is_nonflushing(self):
        self.assertNotIn(
            "D3DGETDATA_FLUSH",
            self.cpp,
        )

        getdata = re.findall(
            r"GetData\(\s*&output,\s*sizeof\(output\),\s*0\s*\)",
            self.cpp,
            flags=re.S,
        )

        self.assertEqual(
            len(getdata),
            1,
        )

    def test_profiler_has_no_wait_or_spin_primitive(self):
        forbidden = (
            "Sleep(",
            "SleepEx(",
            "WaitForSingleObject(",
            "WaitForMultipleObjects(",
            "SwitchToThread(",
        )

        for token in forbidden:
            self.assertNotIn(
                token,
                self.cpp,
                token,
            )

    def test_default_off_reuses_existing_probe_gate(self):
        self.assertGreaterEqual(
            self.cpp.count(
                "if (!Enabled()"
            ),
            2,
        )

    def test_profiler_core_contains_no_render_mutation(self):
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

    def test_async_contract_is_logged(self):
        expected = (
            '"wxl-r8-perf: gpu_async "',
            '"min_harvest_age=%llu "',
            '"getdata_flags=0 flush=0 "',
            '"wait=0 same_frame=0"',
            '"wxl-r8-perf: gpu_summary "',
            '"wxl-r8-perf: gpu "',
        )

        for token in expected:
            self.assertIn(
                token,
                self.cpp,
            )

    def test_reset_releases_queries_without_harvest(self):
        self.assertIn(
            "void GpuReset() noexcept",
            self.cpp,
        )

        self.assertIn(
            "perf::GpuReset();",
            self.fallback,
        )

        self.assertNotIn(
            "GetData(",
            self.hdr,
        )

    def test_fallback_frame_and_passes_are_instrumented(self):
        expected = (
            "GpuOwner::Fallback",
            "FallbackColourResolve",
            "FallbackDepthStage1",
            "FallbackDepthStage2",
            "AoRawDraw",
            "AoCompositeDraw",
        )

        for token in expected:
            self.assertIn(
                token,
                self.fallback,
            )

    def test_smaa_passes_are_instrumented(self):
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

        self.assertGreaterEqual(
            self.smaa.count(
                "perf::GpuScope"
            ),
            4,
        )

    def test_water_snapshot_transport_is_instrumented(self):
        expected = (
            "GpuOwner::WaterSnapshot",
            "WaterColourCopy",
            "WaterDepthStage1",
            "WaterDepthStage2",
        )

        for token in expected:
            self.assertIn(
                token,
                self.water,
            )

    def test_water_linear_depth_is_instrumented(self):
        self.assertIn(
            "GpuOwner::WaterLinear",
            self.depth,
        )

        self.assertIn(
            "DrawPrimitiveUP",
            self.depth,
        )

    def test_gpu_scope_header_documents_nonblocking_contract(self):
        self.assertIn(
            "bounded query ring",
            self.hdr,
        )

        self.assertIn(
            "fixed minimum frame age",
            self.hdr,
        )


if __name__ == "__main__":
    unittest.main()
