#!/usr/bin/env python3
"""Validate bounded R6 NDJSON and optionally extract exact D3D9 shader bytes.

No register is declared free. This validates capture integrity, not visual fidelity.
"""
import argparse
import hashlib
import json
from pathlib import Path

EVENTS = {'session','device','boundary','shader','draw','world_scene','water_candidate','snapshot_attempt','post_water_draw','summary','lost','reset','world_leave','shutdown','error'}

def audit(path, extract=None):
    path = Path(path)
    if path.stat().st_size > 32 * 1024 * 1024:
        raise ValueError('capture exceeds hard 32 MiB ceiling')
    seen = {}; counts = {}; draws = []; snapshots = []; last_sequence = -1; limits = None
    with path.open(encoding='utf-8') as f:
        for number, line in enumerate(f, 1):
            if len(line) > 262144:
                raise ValueError(f'oversized record {number}')
            r = json.loads(line)
            if r.get('schema') != 1 or r.get('event') not in EVENTS:
                raise ValueError(f'invalid envelope {number}')
            if any(not isinstance(r.get(k), int) or r[k] < 0 for k in ('frame','generation','sequence')):
                raise ValueError(f'invalid counters {number}')
            if r['sequence'] <= last_sequence:
                raise ValueError(f'non-monotonic sequence {number}')
            last_sequence = r['sequence']; event = r['event']; counts[event] = counts.get(event, 0) + 1
            if event == 'session':
                limits = r
                if r.get('replacement_supported') is not False:
                    raise ValueError('replacement must remain disabled in native-preserving candidate')
                if r.get('copy_supported') not in (False, True):
                    raise ValueError('copy support declaration missing')
            if event == 'shader':
                key = (r['generation'], r['id'])
                if key in seen and seen[key] != r['sha256']:
                    raise ValueError('shader ID reused for different bytes')
                seen[key] = r['sha256']
                if 'bytecode_le_hex' in r:
                    code = bytes.fromhex(r['bytecode_le_hex'])
                    if len(code) != r['bytes'] or len(code) > 65536 or hashlib.sha256(code).hexdigest() != r['sha256']:
                        raise ValueError('shader size/hash mismatch')
                    if extract:
                        out = Path(extract); out.mkdir(parents=True, exist_ok=True)
                        target = out / (r['sha256'] + '.' + r['stage'] + '.bin')
                        if target.exists() and target.read_bytes() != code:
                            raise ValueError('existing shader output differs')
                        if not target.exists(): target.write_bytes(code)
            if event == 'draw':
                if r.get('replacement_allowed') is not False:
                    raise ValueError('draw is not explicitly native-only')
                expected = {'Magma':2,'ProcWater':3,'Water':1,'WaterNoSpec':1,'Unknown':0}[r['material_class']]
                if r['selector_by_verified_class'] != expected:
                    raise ValueError('material selector/class mismatch')
                for stage in ('vs','ps'):
                    if r[stage+'_sha256'] != 'UNKNOWN' and seen.get((r['generation'],r[stage+'_id'])) != r[stage+'_sha256']:
                        raise ValueError('draw shader missing/mismatched; inspect dropped-record counters')
                c = r.get('constants')
                if c:
                    for stage, maximum in [('vs',256),('ps',224)]:
                        n = c[stage+'_float4_count']
                        if not 0 <= n <= maximum: raise ValueError('constant range overflow')
                        if c[stage+'_float_hr'] >= 0 and len(bytes.fromhex(c[stage+'_float_le_hex'])) != n*16:
                            raise ValueError('constant capture length mismatch')
                draws.append(r)
            if event == 'snapshot_attempt':
                if limits is None:
                    raise ValueError('snapshot attempt precedes session header')
                if r.get('replacement_allowed') is not False:
                    raise ValueError('snapshot attempt is not explicitly native-only')
                if r.get('attempted') is True:
                    cap = limits.get('copy_attempt_cap')
                    index = r.get('attempt_index')
                    if not isinstance(cap, int) or cap < 0 or not isinstance(index, int) or not 1 <= index <= cap:
                        raise ValueError('snapshot attempt exceeds declared cap')
                    if r.get('snapshot_valid') is True:
                        for key in ('end_scene_hr','stretch_rect_hr','begin_scene_hr'):
                            if not isinstance(r.get(key), int) or r[key] < 0:
                                raise ValueError(f'valid snapshot has failing {key}')
                        for key in ('source_destination_distinct','descriptor_compatible','state_preserved'):
                            if r.get(key) is not True:
                                raise ValueError(f'valid snapshot lacks {key}')
                    snapshots.append(r)
                elif r.get('attempted') is not False:
                    raise ValueError('snapshot record lacks attempted boolean')
    if limits is None: raise ValueError('session header missing')
    if len(draws) > limits['max_draws']: raise ValueError('process draw ceiling exceeded')
    if len(snapshots) > limits.get('copy_attempt_cap', 0): raise ValueError('process snapshot ceiling exceeded')
    return {'file':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'events':counts,'draws':len(draws),'snapshots':len(snapshots),'shader_identities':len(seen),
            'classes':sorted({r['material_class'] for r in draws}),
            'providers':sorted({r['provider'] for r in draws}),
            'limits':'No live visual/performance validation or spare-register conclusion follows from this audit.'}

if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--extract',type=Path)
    a=p.parse_args();print(json.dumps(audit(a.capture,a.extract),indent=2))
