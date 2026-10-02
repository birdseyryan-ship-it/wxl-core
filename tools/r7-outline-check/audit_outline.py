"""Audit Step-10C observation integrity without declaring a cause or fix."""

import argparse
import collections
import hashlib
import json
from pathlib import Path

WOW_SHA = "57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da"
ORDERING = "ordering_only_not_proven_object_binding"


def audit(records):
    if not records or records[0].get("event") != "identity":
        raise ValueError("Missing identity record")

    identity = records[0]
    if identity.get("wow_sha256") != WOW_SHA or identity.get("mode") != "read_only":
        raise ValueError("Unexpected authority/mode")

    seen = set()
    m2_materials = {}
    wmo_materials = {}
    starts, ends = set(), set()

    models = collections.defaultdict(
        lambda: {
            "materials": 0,
            "draw_correlations": 0,
            "alpha": set(),
            "blend": set(),
            "shader_ids": set(),
        }
    )

    wmos = collections.defaultdict(
        lambda: {
            "materials": 0,
            "draw_correlations": 0,
            "runtime_shader_ids": set(),
            "selected_families": set(),
            "blend_modes": set(),
            "material_indices": set(),
        }
    )

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
                raise ValueError("M2 material is outside its window or not a placed M2")

            if r.get("native_selected_band") != "UNKNOWN":
                raise ValueError("An unobserved M2 band was promoted to fact")

            if r.get("edgefade_family_classification") not in (
                None,
                "DEFER_TO_OFFLINE_SHADER_HASH_SELECTOR_PROOF",
            ):
                raise ValueError("EdgeFade was promoted without selector/hash proof")

            m2_materials[seq] = r
            p = models[r.get("path", "")]
            p["materials"] += 1

            for key, source in [
                ("alpha", "element_alpha"),
                ("blend", "blend_mode"),
                ("shader_ids", "skin_shader_id"),
            ]:
                if r.get(source) is not None:
                    p[key].add(r[source])

        elif event == "first_Gx_draw_after_material":
            material = m2_materials.get(r.get("material_sequence"))
            if not material or material["world_frame"] != frame:
                raise ValueError("Invalid/cross-frame M2 material correlation")

            if r.get("correlation") != ORDERING:
                raise ValueError("M2 ordering correlation was promoted to ownership")

            models[material.get("path", "")]["draw_correlations"] += 1

        elif event == "wmo_material_after_native_effect_bind":
            if frame not in starts or frame in ends:
                raise ValueError("WMO material is outside its sampling window")

            if r.get("correlation") != ORDERING:
                raise ValueError("WMO MOBA/effect-bind correlation was promoted to ownership")

            if r.get("source_shader_id_pre_remap") != "UNAVAILABLE_at_runtime_render_seam":
                raise ValueError("Unavailable pre-remap WMO shader id was inferred")

            family = r.get("selected_effect_family")
            if family not in {
                "Diffuse",
                "Specular",
                "Metal",
                "Env",
                "Opaque",
                "EnvMetal",
                "Composite",
                "UNKNOWN",
            }:
                raise ValueError("Invalid selected WMO family")

            wmo_materials[seq] = r
            p = wmos[r.get("path", "")]
            p["materials"] += 1

            for key, source in [
                ("runtime_shader_ids", "momt_shader_runtime_id"),
                ("selected_families", "selected_effect_family"),
                ("blend_modes", "momt_blend_mode"),
                ("material_indices", "runtime_material_index"),
            ]:
                if r.get(source) is not None:
                    p[key].add(r[source])

        elif event == "first_Gx_draw_after_wmo_effect_bind":
            material = wmo_materials.get(r.get("wmo_material_sequence"))
            if not material or material["world_frame"] != frame:
                raise ValueError("Invalid/cross-frame WMO correlation")

            if r.get("correlation") != ORDERING:
                raise ValueError("WMO draw ordering was promoted to ownership")

            wmos[material.get("path", "")]["draw_correlations"] += 1

    expected = identity.get("windows")
    complete = type(expected) is int and len(ends) == expected and starts == ends

    model_summary = []
    for path, data in sorted(models.items()):
        model_summary.append(
            {
                "path": path,
                **{
                    k: sorted(v) if isinstance(v, set) else v
                    for k, v in data.items()
                },
            }
        )

    wmo_summary = []
    for path, data in sorted(wmos.items()):
        wmo_summary.append(
            {
                "path": path,
                **{
                    k: sorted(v) if isinstance(v, set) else v
                    for k, v in data.items()
                },
            }
        )

    return {
        "capture_complete": complete,
        "windows": len(ends),
        "material_records": len(m2_materials),
        "wmo_material_records": len(wmo_materials),
        "models": model_summary,
        "wmos": wmo_summary,
        "root_cause": "UNKNOWN",
        "fix_qualified": False,
        "edgefade_causal_status": "UNPROVEN",
        "binding_caveat": (
            "M2 and WMO Gx draw correlations establish order, not proven object ownership; "
            "hardware fields are read after the native draw call."
        ),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    raw = args.capture.read_bytes()
    if len(raw) > 16 * 1024 * 1024:
        raise ValueError("Capture exceeds runtime byte budget")

    result = audit(
        [json.loads(line) for line in raw.decode("utf-8").splitlines() if line]
    )

    result["capture_sha256"] = hashlib.sha256(raw).hexdigest()
    text = json.dumps(result, indent=2) + "\n"

    if args.output:
        args.output.write_text(text, encoding="utf-8")

    print(text, end="")

    has_material = result["material_records"] or result["wmo_material_records"]
    return 0 if result["capture_complete"] and has_material else 1


if __name__ == "__main__":
    raise SystemExit(main())
