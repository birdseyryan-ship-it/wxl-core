"""Audit observation integrity without declaring an outline cause or a fix."""
import argparse
import collections
import hashlib
import json
from pathlib import Path

WOW_SHA = "57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da"


def audit(records):
    if not records or records[0].get("event") != "identity":
        raise ValueError("Missing identity record")
    identity = records[0]
    if identity.get("wow_sha256") != WOW_SHA or identity.get("mode") != "read_only":
        raise ValueError("Unexpected authority/mode")
    seen = set()
    materials = {}
    starts, ends = set(), set()
    paths = collections.defaultdict(lambda: {"materials": 0, "draw_correlations": 0, "alpha": set(), "blend": set(), "shader_ids": set()})
    previous = 0
    for r in records:
        seq = r.get("sequence")
        if type(seq) is not int or seq <= previous or seq in seen:
            raise ValueError("Non-increasing or duplicate sequence")
        previous = seq
        seen.add(seq)
        frame = r.get("world_frame")
        if type(frame) is not int or frame < 0:
            raise ValueError("Invalid frame")
        event = r["event"]
        if event == "sample_begin":
            if frame in starts:
                raise ValueError("Duplicate sampling window")
            starts.add(frame)
        elif event == "sample_end":
            if frame not in starts or frame in ends:
                raise ValueError("Unmatched sampling end")
            ends.add(frame)
        elif event == "material_after_native_and_R4":
            if frame not in starts or frame in ends or not (r.get("owner_flags", 0) & 0x20):
                raise ValueError("Material is outside its window or not a placed M2")
            if r.get("native_selected_band") != "UNKNOWN":
                raise ValueError("An unobserved band was promoted to fact")
            materials[seq] = r
            p = paths[r.get("path", "")]
            p["materials"] += 1
            for key, source in [("alpha", "element_alpha"), ("blend", "blend_mode"), ("shader_ids", "skin_shader_id")]:
                if r.get(source) is not None:
                    p[key].add(r[source])
        elif event == "first_Gx_draw_after_material":
            material = materials.get(r.get("material_sequence"))
            if not material or material["world_frame"] != frame:
                raise ValueError("Invalid/cross-frame material correlation")
            if r.get("correlation") != "ordering_only_not_proven_object_binding":
                raise ValueError("An ordering correlation was promoted to ownership")
            paths[material.get("path", "")]["draw_correlations"] += 1
    expected = identity.get("windows")
    complete = type(expected) is int and len(ends) == expected and starts == ends
    summary = []
    for path, data in sorted(paths.items()):
        summary.append({"path": path, **{k: sorted(v) if isinstance(v, set) else v for k, v in data.items()}})
    return {"capture_complete": complete, "windows": len(ends), "material_records": len(materials),
            "models": summary, "root_cause": "UNKNOWN", "fix_qualified": False,
            "binding_caveat": "Gx draw correlations establish order, not proven object ownership; hardware fields are read after the native call."}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    raw = args.capture.read_bytes()
    if len(raw) > 16 * 1024 * 1024:
        raise ValueError("Capture exceeds runtime byte budget")
    result = audit([json.loads(line) for line in raw.decode("utf-8").splitlines() if line])
    result["capture_sha256"] = hashlib.sha256(raw).hexdigest()
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if result["capture_complete"] and result["material_records"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
