import copy
import unittest

from audit_outline import ORDERING, WOW_SHA, audit


class Integrity(unittest.TestCase):
    def rows(self):
        return [
            dict(
                event="identity",
                sequence=1,
                world_frame=0,
                wow_sha256=WOW_SHA,
                mode="read_only",
                windows=1,
            ),
            dict(event="sample_begin", sequence=2, world_frame=300),
            dict(
                event="material_after_native_and_R4",
                sequence=3,
                world_frame=300,
                owner_flags=0x20,
                path="Tree",
                native_selected_band="UNKNOWN",
                edgefade_family_classification="DEFER_TO_OFFLINE_SHADER_HASH_SELECTOR_PROOF",
                element_alpha=1,
                blend_mode=1,
            ),
            dict(
                event="first_Gx_draw_after_material",
                sequence=4,
                world_frame=300,
                material_sequence=3,
                correlation=ORDERING,
            ),
            dict(event="sample_end", sequence=5, world_frame=300),
        ]

    def wmo_rows(self):
        return [
            dict(
                event="identity",
                sequence=1,
                world_frame=0,
                wow_sha256=WOW_SHA,
                mode="read_only",
                windows=1,
            ),
            dict(event="sample_begin", sequence=2, world_frame=300),
            dict(
                event="wmo_material_after_native_effect_bind",
                sequence=3,
                world_frame=300,
                correlation=ORDERING,
                source_shader_id_pre_remap="UNAVAILABLE_at_runtime_render_seam",
                path="World\\wmo\\test.wmo",
                runtime_material_index=4,
                momt_shader_runtime_id=1,
                momt_blend_mode=0,
                selected_effect_family="Specular",
            ),
            dict(
                event="first_Gx_draw_after_wmo_effect_bind",
                sequence=4,
                world_frame=300,
                wmo_material_sequence=3,
                correlation=ORDERING,
            ),
            dict(event="sample_end", sequence=5, world_frame=300),
        ]

    def test_complete_does_not_prove_cause(self):
        r = audit(self.rows())
        self.assertTrue(r["capture_complete"])
        self.assertFalse(r["fix_qualified"])
        self.assertEqual(r["root_cause"], "UNKNOWN")
        self.assertEqual(r["edgefade_causal_status"], "UNPROVEN")
        self.assertEqual(r["models"][0]["draw_correlations"], 1)

    def test_wmo_complete_does_not_prove_ownership_or_cause(self):
        r = audit(self.wmo_rows())
        self.assertTrue(r["capture_complete"])
        self.assertFalse(r["fix_qualified"])
        self.assertEqual(r["root_cause"], "UNKNOWN")
        self.assertEqual(r["wmo_material_records"], 1)
        self.assertEqual(r["wmos"][0]["draw_correlations"], 1)
        self.assertEqual(r["wmos"][0]["selected_families"], ["Specular"])

    def test_incomplete(self):
        self.assertFalse(audit(self.rows()[:-1])["capture_complete"])

    def test_invalid_m2_evidence_is_rejected(self):
        mutations = [
            (0, "wow_sha256", "bad"),
            (3, "sequence", 3),
            (3, "world_frame", 301),
            (3, "material_sequence", 999),
            (2, "owner_flags", 0),
            (2, "native_selected_band", "5"),
            (2, "edgefade_family_classification", "EdgeFade"),
            (3, "correlation", "PROVEN"),
        ]

        for index, key, value in mutations:
            with self.subTest(key=key):
                rows = copy.deepcopy(self.rows())
                rows[index][key] = value
                with self.assertRaises(ValueError):
                    audit(rows)

    def test_invalid_wmo_evidence_is_rejected(self):
        mutations = [
            (2, "correlation", "PROVEN"),
            (2, "source_shader_id_pre_remap", 22),
            (2, "selected_effect_family", "Parallax"),
            (3, "correlation", "PROVEN"),
            (3, "wmo_material_sequence", 999),
            (3, "world_frame", 301),
        ]

        for index, key, value in mutations:
            with self.subTest(key=key):
                rows = copy.deepcopy(self.wmo_rows())
                rows[index][key] = value
                with self.assertRaises(ValueError):
                    audit(rows)


if __name__ == "__main__":
    unittest.main()
