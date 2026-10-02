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


if __name__ == "__main__":
    unittest.main()
