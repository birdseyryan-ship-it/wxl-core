import copy
import json
import unittest
from audit_c28 import audit, records, HASHES, REGS, PREFIX


def capture():
    events = [{"event":"startup", "read_only":True, "shader_substitution":False, "constant_mutation":False,
               "gx_device_draw_owner":False, "m2_changed":False}, {"event":"device_taps", "complete":True}]
    bind = 0
    for family in ("Diffuse", "Opaque"):
        for pix in (5,7):
            for sample in range(2):
                bind += 1
                low = pix == 5
                vs, raw = HASHES[0 if low else 1], HASHES[2 if low else 3]
                ps = HASHES[4 + (0 if family == "Diffuse" else 2) + (0 if low else 1)]
                events += [{"event":"identity", "bind":bind, "family":family, "vtx":31 if low else 61, "pix":pix,
                            "selected_vs":vs, "raw_vs":raw, "ps":ps, "paired_raw_vtx":30 if low else 60},
                           {"event":"path", "bind":bind, "path":"WORLD\\TEST.WMO"}]
                for phase, draw in (("post_bind",0), ("pre_draw",1), ("post_draw",1)):
                    events.append({"event":"snapshot", "bind":bind, "draw":draw, "phase":phase,
                                   "vs":vs, "ps":ps, "dirty_valid":True, "dirty":[255,0,255,0]})
                    for stage, reg in REGS:
                        value = [reg, sample, 0, 0]
                        events.append({"event":"constant", "bind":bind, "draw":draw, "phase":phase, "stage":stage,
                                       "reg":reg, "hr":0, "valid":True, "bits":value, "cache_bits":value})
                events += [{"event":"draw_begin", "bind":bind, "draw":1},
                           {"event":"draw_end", "bind":bind, "draw":1, "hr":0, "shader_match":True}]
    return events


def text(events): return "\n".join(PREFIX + json.dumps(e) for e in events)


class C28Audit(unittest.TestCase):
    def test_complete_capture_does_not_overclaim_post_bind_or_write_bridge(self):
        result = audit("", text(capture()))
        self.assertEqual(result["status"], "READ_SIDE_PASS")
        self.assertEqual(result["post_bind_sufficient"], "NOT_PROVEN")
        self.assertEqual(result["bridge_write_restore"], "NOT_IMPLEMENTED_OR_QUALIFIED")

    def test_stale_post_bind_is_a_result_not_failure_of_later_read(self):
        events = capture()
        row = next(e for e in events if e["event"] == "constant" and e["reg"] == 28 and e["phase"] == "post_bind")
        row["bits"] = [99,99,99,99]
        result = audit("", text(events))
        self.assertEqual(result["status"], "READ_SIDE_PASS")
        self.assertEqual(result["post_bind_sufficient"], "DISPROVEN_IN_CAPTURE")

    def test_off_contamination_and_empty_capture_fail(self):
        self.assertNotEqual(audit(text(capture()), text(capture()))["status"], "READ_SIDE_PASS")
        self.assertNotEqual(audit("", "")["status"], "READ_SIDE_PASS")

    def test_failures_cannot_turn_into_zero_value_success(self):
        for kind in ("missing", "getter", "cache", "shader", "draw", "quarantine", "duplicate", "substitution"):
            events = capture()
            row = next(e for e in events if e["event"] == "constant" and e["reg"] == 28 and e["phase"] == "pre_draw")
            if kind == "missing": events.remove(row)
            elif kind == "getter": row["hr"] = -1; row["valid"] = False
            elif kind == "cache": row["cache_bits"] = [9,9,9,9]
            elif kind == "shader": next(e for e in events if e["event"] == "identity")["selected_vs"] = "bad"
            elif kind == "draw": next(e for e in events if e["event"] == "draw_end")["hr"] = -1
            elif kind == "quarantine": events.append({"event":"quarantine"})
            elif kind == "duplicate": events.append(copy.deepcopy(row))
            on = text(events) + ("\nSUBSTITUTION PASS" if kind == "substitution" else "")
            self.assertNotEqual(audit("", on)["status"], "READ_SIDE_PASS", kind)

    def test_truncated_json_is_rejected(self):
        with self.assertRaises(ValueError): records(PREFIX + '{"event":')


if __name__ == "__main__": unittest.main()
