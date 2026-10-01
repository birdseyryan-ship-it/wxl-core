import copy
import unittest
from audit_outline import audit, WOW_SHA


class Integrity(unittest.TestCase):
    def rows(self):
        return [
            dict(event="identity", sequence=1, world_frame=0, wow_sha256=WOW_SHA, mode="read_only", windows=1),
            dict(event="sample_begin", sequence=2, world_frame=300),
            dict(event="material_after_native_and_R4", sequence=3, world_frame=300, owner_flags=0x20, path="Tree", native_selected_band="UNKNOWN", element_alpha=1, blend_mode=1),
            dict(event="first_Gx_draw_after_material", sequence=4, world_frame=300, material_sequence=3, correlation="ordering_only_not_proven_object_binding"),
            dict(event="sample_end", sequence=5, world_frame=300),
        ]

    def test_complete_does_not_prove_cause(self):
        r = audit(self.rows())
        self.assertTrue(r["capture_complete"])
        self.assertFalse(r["fix_qualified"])
        self.assertEqual(r["root_cause"], "UNKNOWN")
        self.assertEqual(r["models"][0]["draw_correlations"], 1)

    def test_incomplete(self):
        self.assertFalse(audit(self.rows()[:-1])["capture_complete"])

    def test_invalid_evidence_is_rejected(self):
        mutations = [(0, "wow_sha256", "bad"), (3, "sequence", 3),
                     (3, "world_frame", 301), (3, "material_sequence", 999),
                     (2, "owner_flags", 0), (2, "native_selected_band", "5"),
                     (3, "correlation", "PROVEN")]
        for index, key, value in mutations:
            with self.subTest(key=key):
                rows = copy.deepcopy(self.rows())
                rows[index][key] = value
                with self.assertRaises(ValueError):
                    audit(rows)


if __name__ == "__main__":
    unittest.main()
