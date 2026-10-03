"""Audit paired B2A logs. A read-side pass never authorizes constant writes."""
import argparse
import collections
import json
import pathlib
import re

PREFIX = "r8-step11-c28: "
ROOT = pathlib.Path(__file__).resolve().parents[2]
HASHES = re.findall(r'"([a-f0-9]{64})"', (ROOT / "src/client/CWorldScene/WmoC28ProofCore.hpp").read_text())
REGS = [("vs", r) for r in (10, 11, 12, 28, 29)] + [("ps", 31)]


def records(text):
    result = []
    for line in text.splitlines():
        if PREFIX in line:
            payload = line.split(PREFIX, 1)[1]
            if not payload.startswith("{"):
                raise ValueError("unstructured proof error: " + payload)
            result.append(json.loads(payload))
    return result


def audit(off, on):
    problems = []
    off_events, events = records(off), records(on)
    if off_events: problems.append("OFF control contains proof events")
    if "SUBSTITUTION PASS" in off or "SUBSTITUTION PASS" in on:
        problems.append("B1 substitution was active")
    starts = [e for e in events if e["event"] == "startup"]
    if len(starts) != 1 or starts[0].get("read_only") is not True:
        problems.append("missing/ambiguous read-only startup")
    elif any(starts[0].get(k) is not False for k in ("shader_substitution", "constant_mutation", "gx_device_draw_owner", "m2_changed")):
        problems.append("invalid startup safety contract")
    if not any(e["event"] == "device_taps" and e.get("complete") is True for e in events):
        problems.append("no complete device tap")
    for e in events:
        if e["event"] in ("quarantine", "identity_exception"):
            problems.append(e["event"])
        if e["event"] == "device_taps" and e.get("complete") is not True:
            problems.append("incomplete device tap")
    identities, paths, constants, snapshots, begins, ends = {}, {}, {}, {}, {}, {}
    for e in events:
        kind = e["event"]
        target = None
        if kind == "identity": target, key = identities, e["bind"]
        elif kind == "path": target, key = paths, e["bind"]
        elif kind == "constant": target, key = constants, (e["bind"], e["draw"], e["phase"], e["stage"], e["reg"])
        elif kind == "snapshot": target, key = snapshots, (e["bind"], e["draw"], e["phase"])
        elif kind == "draw_begin": target, key = begins, (e["bind"], e["draw"])
        elif kind == "draw_end": target, key = ends, (e["bind"], e["draw"])
        if target is not None:
            if key in target: problems.append("duplicate " + kind)
            target[key] = e
    route_binds = collections.defaultdict(set)
    post_bind_differences = 0
    c28_values = set()
    for bind, identity in identities.items():
        family, vtx, pix = identity["family"], identity["vtx"], identity["pix"]
        low = pix == 5 and vtx in (31,41,51)
        high = pix == 7 and vtx in (61,71,81)
        valid_family = family in ("Diffuse", "Opaque")
        h = 0 if low else 1
        ps = HASHES[4 + (0 if family == "Diffuse" else 2) + (0 if low else 1)]
        if not (valid_family and (low or high) and identity["selected_vs"] == HASHES[h] and
                identity["raw_vs"] == HASHES[h + 2] and identity["ps"] == ps and identity["paired_raw_vtx"] == vtx & ~1):
            problems.append(f"unqualified identity {bind}")
        if bind not in paths: problems.append(f"missing path record {bind}")
    if begins.keys() != ends.keys(): problems.append("incomplete draw pair")
    for (bind, draw), end in ends.items():
        ident = identities.get(bind)
        if ident is None: problems.append("orphan draw"); continue
        if end.get("hr", -1) < 0 or end.get("shader_match") is not True:
            problems.append(f"draw failed or shader changed {bind}:{draw}")
        complete = True
        for phase, ordinal in (("post_bind", 0), ("pre_draw", draw), ("post_draw", draw)):
            snap = snapshots.get((bind, ordinal, phase))
            if not snap or not snap.get("dirty_valid"):
                problems.append(f"missing snapshot {bind}:{draw}:{phase}"); complete = False; continue
            if phase != "post_bind" and (snap["vs"] != ident["selected_vs"] or snap["ps"] != ident["ps"]):
                problems.append(f"wrong hardware shaders {bind}:{draw}:{phase}")
            for stage, reg in REGS:
                c = constants.get((bind, ordinal, phase, stage, reg))
                if not c or c.get("valid") is not True or c.get("hr", -1) < 0 or c.get("bits") is None or c.get("cache_bits") is None:
                    problems.append(f"missing/invalid constant {bind}:{draw}:{phase}:{stage}{reg}"); complete = False
        if not complete: continue
        for stage, reg in REGS:
            pre = constants[bind, draw, "pre_draw", stage, reg]
            post = constants[bind, draw, "post_draw", stage, reg]
            if pre["bits"] != post["bits"] or pre["cache_bits"] != post["cache_bits"]:
                problems.append(f"state changed across downstream draw {bind}:{draw}:{stage}{reg}")
            if stage == "vs" and pre["bits"] != pre["cache_bits"]:
                problems.append(f"VS cache/device mismatch at draw {bind}:{draw}:c{reg}")
        baseline = constants[bind, 0, "post_bind", "vs", 28]
        pre28 = constants[bind, draw, "pre_draw", "vs", 28]
        post_bind_differences += baseline["bits"] != pre28["bits"]
        c28_values.add(tuple(pre28["bits"]))
        route_binds[ident["family"], ident["pix"]].add(bind)
    # These four routes already occurred in B1; require repeatable observations.
    for family in ("Diffuse", "Opaque"):
        for pix in (5,7):
            if len(route_binds[family, pix]) < 2:
                problems.append(f"need >=2 observed binds for {family}/pix{pix}")
    return {"status": "READ_SIDE_PASS" if not problems else "INCOMPLETE_OR_FAIL",
            "problems": sorted(set(problems)), "identity_count": len(identities), "paired_draws": len(ends),
            "c28_distinct_values": len(c28_values), "post_bind_c28_changed_before_draw": post_bind_differences,
            "post_bind_sufficient": "DISPROVEN_IN_CAPTURE" if post_bind_differences else "NOT_PROVEN",
            "bridge_write_restore": "NOT_IMPLEMENTED_OR_QUALIFIED",
            "boundary": "D3D9 dispatch chain entry/return; not an instruction trace inside downstream wrappers",
            "rejected_draws": sum(e["event"] == "reject" for e in events)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--off", required=True, type=pathlib.Path)
    parser.add_argument("--on", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        result = audit(args.off.read_text(errors="replace"), args.on.read_text(errors="replace"))
    except (ValueError, KeyError, IndexError, TypeError) as exc:
        result = {"status": "INCOMPLETE_OR_FAIL", "problems": [str(exc)]}
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["status"] == "READ_SIDE_PASS" else 1)
