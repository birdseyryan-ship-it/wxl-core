import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]

POLICY = (
    ROOT
    / "src"
    / "client"
    / "CWorldScene"
    / "StructuralMaterialPolicy.hpp"
).read_text(encoding="utf-8")

RUNTIME = (
    ROOT
    / "src"
    / "client"
    / "CWorldScene"
    / "StructuralMaterialRuntime.cpp"
).read_text(encoding="utf-8")

CANDIDATE = (
    ROOT
    / "src"
    / "client"
    / "CWorldScene"
    / "WmoPerPixelCandidate.cpp"
).read_text(encoding="utf-8")


class StructuralMaterialSourceTest(unittest.TestCase):
    def test_master_and_independent_routes_exist(self):
        self.assertIn(
            '"WXL_R8_STRUCTURAL_MATERIALS"',
            RUNTIME,
        )
        self.assertIn(
            '"WXL_R8_WMO_MATERIALS"',
            RUNTIME,
        )
        self.assertIn(
            '"WXL_R8_M2_MATERIALS"',
            RUNTIME,
        )

    def test_master_off_returns_before_any_install(self):
        master = RUNTIME.index(
            "if (!g_config.master)"
        )
        first_install = RUNTIME.index(
            "InstallWmoSubstrate();"
        )

        self.assertLess(
            master,
            first_install,
        )

    def test_wmo_uses_direct_render_scope_and_effect_bind(self):
        self.assertIn(
            "wmo::kExtRender",
            RUNTIME,
        )
        self.assertIn(
            "wmo::kIntRender",
            RUNTIME,
        )
        self.assertIn(
            "sh::kEffectBind",
            RUNTIME,
        )

    def test_m2_uses_post_native_material_setup(self):
        self.assertIn(
            "m2::kSetupMaterial",
            RUNTIME,
        )

        hook = RUNTIME[
            RUNTIME.index("HookM2SetupMaterial"):
            RUNTIME.index("HookWmoEffectBind")
        ]

        original = hook.index(
            "g_origM2SetupMaterial("
        )
        candidate_gate = hook.index(
            "if (!g_config.m2)"
        )

        self.assertLess(
            original,
            candidate_gate,
        )

    def test_wmo_native_bind_runs_before_candidate_classification(self):
        hook = RUNTIME[
            RUNTIME.index("HookWmoEffectBind"):
            RUNTIME.index("struct WmoRenderScope")
        ]

        native = hook.index(
            "g_origWmoEffectBind("
        )
        classify = hook.index(
            "CurrentWmoFamily()"
        )

        self.assertLess(
            native,
            classify,
        )

    def test_no_gx_device_draw_ownership(self):
        self.assertNotIn(
            "kGxDeviceDraw",
            RUNTIME,
        )
        self.assertNotIn(
            "kDrawTriangleBatch",
            RUNTIME,
        )
        self.assertNotIn(
            "kDrawBatchDoodad",
            RUNTIME,
        )

    def test_11b01_has_no_d3d_or_shader_state_mutation(self):
        forbidden = (
            "SetVertexShader",
            "SetPixelShader",
            "SetRenderState",
            "SetSamplerState",
            "SetTexture(",
            "kShaderConstantsSet",
            "ShaderConstantsSetHelperFn",
            "mem::Patch",
            "VirtualProtect",
        )

        for token in forbidden:
            self.assertNotIn(
                token,
                RUNTIME,
                token,
            )

    def test_initial_wmo_family_allowlist_is_exact(self):
        body = POLICY[
            POLICY.index("IsInitialWmoCandidate"):
            POLICY.index("IsInitialM2RawShaderId")
        ]

        self.assertIn(
            "WmoFamily::Diffuse",
            body,
        )
        self.assertIn(
            "WmoFamily::Opaque",
            body,
        )
        self.assertIn(
            "WmoFamily::Specular",
            body,
        )

        for name in (
            "WmoFamily::Metal",
            "WmoFamily::Env",
            "WmoFamily::EnvMetal",
            "WmoFamily::Composite",
        ):
            self.assertNotIn(
                name,
                body,
            )

    def test_m2_initial_raw_ids_are_exact(self):
        body = POLICY[
            POLICY.index("IsInitialM2RawShaderId"):
            POLICY.index(
                "IsExplicitlyDeferredM2RawShaderId"
            )
        ]

        self.assertRegex(
            body,
            r"shaderId\s*==\s*0\s*\|\|\s*shaderId\s*==\s*16",
        )

    def test_m2_8002_is_explicitly_deferred(self):
        self.assertIn(
            "shaderId == 0x8002u",
            POLICY,
        )

        initial = POLICY[
            POLICY.index("IsInitialM2RawShaderId"):
            POLICY.index(
                "IsExplicitlyDeferredM2RawShaderId"
            )
        ]

        self.assertNotIn(
            "0x8002",
            initial,
        )

    def test_no_edgefade_fix_claim_or_implementation(self):
        combined = (
            POLICY.lower()
            + "\n"
            + RUNTIME.lower()
        )

        self.assertNotIn(
            "edgefade",
            combined,
        )

    def test_substrate_explicitly_reports_no_mutation(self):
        self.assertIn(
            "mutation=0",
            RUNTIME,
        )
        self.assertIn(
            "substitution=OFF",
            RUNTIME,
        )


    def test_11b02a_selector_proof_is_explicitly_opt_in(self):
        self.assertIn(
            '"WXL_R8_WMO_SELECTOR_PROOF"',
            RUNTIME,
        )
        self.assertIn(
            "if (g_config.wmoSelectorProof)",
            RUNTIME,
        )

    def test_11b02a_reads_active_collection_slots_directly(self):
        self.assertIn(
            "sh::kCollectionVtxSlots",
            RUNTIME,
        )
        self.assertIn(
            "sh::kCollectionPixSlots",
            RUNTIME,
        )
        self.assertIn(
            "SnapshotCollectionWrapper",
            RUNTIME,
        )

    def test_11b02a_hashes_wrapper_bytecode_not_hardware_shader(self):
        self.assertIn(
            "sh::kCgxShaderByteLen",
            RUNTIME,
        )
        self.assertIn(
            "sh::kCgxShaderBytePtr",
            RUNTIME,
        )
        self.assertIn(
            "diag::Sha256::Of",
            RUNTIME,
        )
        self.assertNotIn(
            "GetVertexShader(",
            RUNTIME,
        )
        self.assertNotIn(
            "GetPixelShader(",
            RUNTIME,
        )

    def test_11b02a_records_even_raw_vertex_sibling(self):
        self.assertIn(
            "vtxIdx & ~1u",
            RUNTIME,
        )
        self.assertIn(
            "paired_raw_vtx",
            RUNTIME,
        )
        self.assertIn(
            "paired_vs_sha",
            RUNTIME,
        )

    def test_11b02a_remains_read_only(self):
        forbidden = (
            "kGxStateSet",
            "SetVertexShader",
            "SetPixelShader",
            "SetVertexShaderConstantF",
            "SetPixelShaderConstantF",
            "CreateVertexShader",
            "CreatePixelShader",
            "D3DAssemble",
            "kShaderConstantsSet",
            "mem::Patch",
            "VirtualProtect",
        )

        for token in forbidden:
            self.assertNotIn(
                token,
                RUNTIME,
                token,
            )


    def test_11b02b1_is_explicitly_opt_in(self):
        self.assertIn(
            '"WXL_R8_WMO_PERPIXEL"',
            RUNTIME,
        )
        self.assertIn(
            "if (g_config.wmoPerPixel)",
            RUNTIME,
        )
        self.assertIn(
            "TryBindWmoPerPixelCandidate(",
            RUNTIME,
        )

    def test_11b02b1_runs_only_after_native_effect_bind(self):
        hook = RUNTIME[
            RUNTIME.index("HookWmoEffectBind"):
            RUNTIME.index("struct WmoRenderScope")
        ]

        native = hook.index(
            "g_origWmoEffectBind("
        )
        candidate = hook.index(
            "TryBindWmoPerPixelCandidate("
        )

        self.assertLess(
            native,
            candidate,
        )

    def test_11b02b1_uses_only_proven_simple_selected_vs_hashes(self):
        expected = (
            "868fbe1c6807c6dd978ae18f7c7176ac1764c40eb1b7c72f1b2e0fe28c598ca7",
            "b70c0dfa2206d332576336e0f166832101aa7cff9f5fddfb8343aff6a4677328",
        )

        for value in expected:
            self.assertIn(
                value,
                CANDIDATE,
            )

        deferred = (
            "61d9d4d8f1e010c56c8c63cb2fadafabbd22a165096175ad3ebdf47671d8c56f",
            "abd0d3f61c3241095d5b1b0c0e679e822305247ff247e361a491b22bdd4e4c7f",
            "3eb44db2e8ebea42a75a31586992e009b8143cc0936d50ed7b9d617eb5341a9c",
            "9fb6cd66679f8672a492f03ba5379bf78e6363c4c848037b2c07ce820da981f8",
            "eadeaa75d7d6cfeec043ac75ae845747b83094a145b29f1462479fc544fa3715",
        )

        for value in deferred:
            self.assertNotIn(
                value,
                CANDIDATE,
            )

    def test_11b02b1_raw_sibling_hashes_are_exact(self):
        self.assertIn(
            "929f23fcaa265eb0a40f22dd58774669132043d98bcd111e09a058806c1f380e",
            CANDIDATE,
        )
        self.assertIn(
            "1d29d928f3416de9e21eca61fd77bb1e8054a2d541e9cd84b2b9c145f6803fe7",
            CANDIDATE,
        )

    def test_11b02b1_pixel_hash_allowlist_is_exact(self):
        for value in (
            "24c2c665054205b408bba34848906b73fadb33aba72abff3a1545d8804ff3a9b",
            "259824c48d1717aef69a51a09b04bd519a01e75afe4c3eec34b719d1b01f5bb1",
            "6b2c56523812cdffe77383c1dbc481abe73d96bef4ef12808d996f1d5221def1",
            "e761d8f215688ac549a49ca4faf20ec15d0ea3e0c8084985f0a8732bcdfa7937",
        ):
            self.assertIn(
                value,
                CANDIDATE,
            )

        self.assertNotIn(
            "e836057fdcef86cada3f3405158ac95be4dc8dea387036ff1f276067cae9443f",
            CANDIDATE,
        )

    def test_11b02b1_only_targets_diffuse_and_opaque(self):
        self.assertIn(
            "family != WmoFamily::Diffuse",
            CANDIDATE,
        )
        self.assertIn(
            "family != WmoFamily::Opaque",
            CANDIDATE,
        )
        self.assertNotIn(
            "WmoFamily::Specular",
            CANDIDATE,
        )

    def test_11b02b1_uses_even_sibling_and_exact_wrapper_hashes(self):
        self.assertIn(
            "vtxIdx & ~1u",
            CANDIDATE,
        )
        self.assertIn(
            "WrapperHash(",
            CANDIDATE,
        )
        self.assertIn(
            "diag::Sha256::Of",
            CANDIDATE,
        )

    def test_11b02b1_moves_exact_simple_wrath_lighting_to_pixel(self):
        for token in (
            "mov o9.xyz, c12",
            "mov o10.xyz, c10",
            "mov o2.z, c11.x",
            "mov o3.w, c11.z",
            "mov o4.w, c29.z",
            "nrm r30.xyz, v3",
            "dp3_sat r31.x, -v8, r30",
            "mad_sat r30.xyz, r31.x, r31.yzww, v9",
            "mad_sat r30.xyz, v0, r30, r31",
        ):
            self.assertIn(
                token,
                CANDIDATE,
            )

    def test_11b02b1_preserves_native_ps_by_surgical_patch(self):
        self.assertIn(
            "NormalizeDisassembly(",
            CANDIDATE,
        )
        self.assertIn(
            "PatchPixelAssembly(",
            CANDIDATE,
        )
        self.assertIn(
            '"mul r1.xyz, r0.w, v0\\n"',
            CANDIDATE,
        )

    def test_11b02b1_uses_gxstate_not_direct_setshader(self):
        self.assertIn(
            "sh::kGxStateSet",
            CANDIDATE,
        )
        self.assertIn(
            "sh::kStateVertexShader",
            CANDIDATE,
        )
        self.assertIn(
            "sh::kStatePixelShader",
            CANDIDATE,
        )

        self.assertNotIn(
            "SetVertexShader(",
            CANDIDATE,
        )
        self.assertNotIn(
            "SetPixelShader(",
            CANDIDATE,
        )

    def test_11b02b1_does_not_own_draw_submission(self):
        for token in (
            "kGxDeviceDraw",
            "kDrawTriangleBatch",
            "kDrawBatchDoodad",
        ):
            self.assertNotIn(
                token,
                CANDIDATE,
            )

    def test_11b02b1_has_render_boundary_restore(self):
        self.assertIn(
            "NoteWmoPerPixelNativeBind();",
            RUNTIME,
        )
        self.assertIn(
            "RestoreWmoPerPixelIfPending();",
            RUNTIME,
        )
        self.assertIn(
            "g_restorePending",
            CANDIDATE,
        )


if __name__ == "__main__":
    unittest.main()
