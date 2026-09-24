import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from audit_capture import audit

class CaptureChecks(unittest.TestCase):
    def fixture(self):
        code=b'\x00\x03\xfe\xff\xff\xff\x00\x00'; digest=hashlib.sha256(code).hexdigest()
        base={'schema':1,'frame':0,'generation':1}
        return [dict(base,event='session',sequence=1,copy_supported=True,replacement_supported=False,max_draws=2,copy_attempt_cap=8),
                dict(base,event='shader',sequence=2,id=1,stage='vs',bytes=8,sha256=digest,bytecode_le_hex=code.hex()),
                dict(base,event='draw',sequence=3,replacement_allowed=False,material_class='ProcWater',selector_by_verified_class=3,provider='terrain',vs_id=1,vs_sha256=digest,ps_id=0,ps_sha256='UNKNOWN')]
    def run_records(self,records):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'capture.ndjson';p.write_text(''.join(json.dumps(r)+'\n' for r in records));return audit(p,Path(d)/'shaders')
    def test_roundtrip(self): self.assertEqual(self.run_records(self.fixture())['draws'],1)
    def test_hash_corruption(self):
        r=self.fixture();r[1]['bytecode_le_hex']='00'*8
        with self.assertRaises(ValueError):self.run_records(r)
    def test_selector_trap(self):
        r=self.fixture();r[2]['selector_by_verified_class']=2
        with self.assertRaises(ValueError):self.run_records(r)
    def test_missing_shader(self):
        r=self.fixture();r.pop(1)
        with self.assertRaises(ValueError):self.run_records(r)
    def test_draw_limit(self):
        r=self.fixture();r[0]['max_draws']=0
        with self.assertRaises(ValueError):self.run_records(r)
    def test_sequence(self):
        r=self.fixture();r[2]['sequence']=1
        with self.assertRaises(ValueError):self.run_records(r)
    def test_snapshot_valid(self):
        r=self.fixture()
        r.append(dict(r[0],event='snapshot_attempt',sequence=4,attempt_index=1,replacement_allowed=False,
                      attempted=True,snapshot_valid=True,end_scene_hr=0,stretch_rect_hr=0,begin_scene_hr=0,
                      source_destination_distinct=True,descriptor_compatible=True,state_preserved=True))
        self.assertEqual(self.run_records(r)['snapshots'],1)
    def test_snapshot_invalid_success_claim(self):
        r=self.fixture()
        r.append(dict(r[0],event='snapshot_attempt',sequence=4,attempt_index=1,replacement_allowed=False,
                      attempted=True,snapshot_valid=True,end_scene_hr=0,stretch_rect_hr=-1,begin_scene_hr=0,
                      source_destination_distinct=True,descriptor_compatible=True,state_preserved=True))
        with self.assertRaises(ValueError):self.run_records(r)
    def test_snapshot_cap(self):
        r=self.fixture();r[0]['copy_attempt_cap']=0
        r.append(dict(r[0],event='snapshot_attempt',sequence=4,attempt_index=1,replacement_allowed=False,attempted=True,snapshot_valid=False))
        with self.assertRaises(ValueError):self.run_records(r)
    def test_snapshot_skipped_record(self):
        r=self.fixture()
        r.append(dict(r[0],event='snapshot_attempt',sequence=4,replacement_allowed=False,attempted=False,reason='source_rt_unavailable'))
        self.assertEqual(self.run_records(r)['snapshots'],0)
if __name__=='__main__':unittest.main()
