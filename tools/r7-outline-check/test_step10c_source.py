import unittest
from pathlib import Path


SOURCE = Path(
    "src/client/CWorldScene/DistantOutlineDiagnostics.cpp"
)


class Step10CSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = SOURCE.read_text(encoding="utf-8")

    def test_canonical_identity_gate_preserved(self):
        self.assertIn(
            "57dd8955fd7238b00969f6011cdaa13d"
            "ca14daa5849d1f9be64152bd4c7fe5da",
            self.text,
        )
        self.assertIn(
            "canonical Wow.exe SHA/base gate failed",
            self.text,
        )

    def test_default_off_control_preserved(self):
        policy = Path(
            "src/client/CWorldScene/OutlineDiagnosticPolicy.hpp"
        ).read_text(encoding="utf-8")

        self.assertIn("bool enabled=false", policy)
        self.assertIn('get("WXL_R7_OUTLINE_DIAG")', policy)
        self.assertIn('std::strcmp(s,"0")==0', policy)
        self.assertIn("if(!config.enabled)", self.text)

    def test_wmo_observer_seams_present(self):
        for token in (
            "HookWmoCull",
            "HookWmoExt",
            "HookWmoInt",
            "HookWmoEffectBind",
            "wmo_material_after_native_effect_bind",
            "first_Gx_draw_after_wmo_effect_bind",
            "source_shader_id_pre_remap",
            "momt_shader_runtime_id",
            "selected_effect_family",
            "group_has_two_uv",
        ):
            self.assertIn(token, self.text)

    def test_m2_contract_enrichment_present(self):
        for token in (
            "skin_material_layer",
            "skin_texture_combo_index",
            "skin_texture_coord_combo_index",
            "skin_texture_weight_combo_index",
            "skin_texture_transform_combo_index",
            "texture_combiner_combo_count",
            "edgefade_family_classification",
        ):
            self.assertIn(token, self.text)

    def test_direct_hardware_bridge_present(self):
        for token in (
            "step10c04_direct_hardware_bridge",
            "R8MaterialM2TriangleBatch",
            "R8MaterialM2DoodadBatch",
            "m2_batch_after_native_draw",
            "after_native_M2_material_setup",
            "after_native_M2_batch",
            "after_native_WMO_effect_bind",
            "raw_gx_device_draw_calls",
            "raw_gx_device_draw_m2_token_calls",
            "raw_gx_device_draw_wmo_token_calls",
            "hardware_observation_phase",
            "hardware_state",
        ):
            self.assertIn(token, self.text)

    def test_current_section_is_not_stale_context_copy(self):
        self.assertIn(
            "gx::kM2ElementSectionField",
            self.text,
        )

        direct = self.text[
            self.text.index("void ObserveM2Batch"):
            self.text.index("void __fastcall HookM2TriangleBatch")
        ]

        self.assertNotIn(
            "kDrawBatchCtxSectionField",
            direct,
        )

    def test_sampler_observation_is_named_and_read_only(self):
        for token in (
            "min_filter",
            "mag_filter",
            "mip_filter",
            "mip_lod_bias",
            "max_mip_level",
            "max_anisotropy",
            "address_u",
            "address_v",
        ):
            self.assertIn(token, self.text)

        for forbidden in (
            "->SetRenderState(",
            "->SetSamplerState(",
            "->SetTexture(",
            "->SetVertexShader(",
            "->SetPixelShader(",
            "DrawIndexedPrimitive(",
        ):
            self.assertNotIn(forbidden, self.text)

    def test_correlation_never_promoted(self):
        self.assertGreaterEqual(
            self.text.count(
                "ordering_only_not_proven_object_binding"
            ),
            3,
        )

        self.assertIn(
            "direct_native_M2_batch_context_post_call_"
            "not_final_device_ownership",
            self.text,
        )

        self.assertIn(
            "DEFER_TO_OFFLINE_SHADER_HASH_SELECTOR_PROOF",
            self.text,
        )

    def test_observer_hooks_are_outer_read_only_wrappers(self):
        for name in (
            "R8MaterialM2TriangleBatch",
            "R8MaterialM2DoodadBatch",
            "R8MaterialWmoCull",
            "R8MaterialWmoExt",
            "R8MaterialWmoInt",
            "R8MaterialWmoEffectBind",
        ):
            self.assertIn(name, self.text)

        self.assertGreaterEqual(
            self.text.count("-1000"),
            10,
        )


if __name__ == "__main__":
    unittest.main()
