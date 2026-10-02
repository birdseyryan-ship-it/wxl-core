"""Audit Step-10C/10C-04 observation integrity without declaring a cause or fix."""

import argparse
import collections
import hashlib
import json
from pathlib import Path

WOW_SHA = "57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da"
ORDERING = "ordering_only_not_proven_object_binding"
M2_DIRECT = (
    "direct_native_M2_batch_context_post_call_not_final_device_ownership"
)


def _hardware(record, phase):
    return (
        record.get("hardware_observation_phase") == phase
        and isinstance(record.get("hardware_state"), dict)
    )


def audit(records):
    if not records or records[0].get("event") != "identity":
        raise ValueError("Missing identity record")

    identity = records[0]

    if (
        identity.get("wow_sha256") != WOW_SHA
        or identity.get("mode") != "read_only"
    ):
        raise ValueError("Unexpected authority/mode")

    bridge_mode = (
        identity.get("step10c04_direct_hardware_bridge") == "enabled"
    )

    seen = set()
    m2_materials = {}
    wmo_materials = {}
    starts, ends = set(), set()

    raw_draw_calls = 0
    raw_draw_m2_tokens = 0
    raw_draw_wmo_tokens = 0

    direct_m2_batches = 0
    m2_material_hardware = 0
    wmo_effect_hardware = 0

    models = collections.defaultdict(
        lambda: {
            "materials": 0,
            "material_hardware_records": 0,
            "direct_batch_records": 0,
            "draw_correlations": 0,
            "alpha": set(),
            "blend": set(),
            "shader_ids": set(),
        }
    )

    wmos = collections.defaultdict(
        lambda: {
            "materials": 0,
            "effect_hardware_records": 0,
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

            if bridge_mode:
                for key in (
                    "raw_gx_device_draw_calls",
                    "raw_gx_device_draw_m2_token_calls",
                    "raw_gx_device_draw_wmo_token_calls",
                ):
                    value = r.get(key)
                    if type(value) is not int or value < 0:
                        raise ValueError(
                            "Missing/invalid Step-10C04 raw draw probe"
                        )

            raw_draw_calls += r.get("raw_gx_device_draw_calls", 0)
            raw_draw_m2_tokens += r.get(
                "raw_gx_device_draw_m2_token_calls", 0
            )
            raw_draw_wmo_tokens += r.get(
                "raw_gx_device_draw_wmo_token_calls", 0
            )

            ends.add(frame)

        elif event == "material_after_native_and_R4":
            if (
                frame not in starts
                or frame in ends
                or not (r.get("owner_flags", 0) & 0x20)
            ):
                raise ValueError(
                    "M2 material is outside its window or not a placed M2"
                )

            if r.get("native_selected_band") != "UNKNOWN":
                raise ValueError(
                    "An unobserved M2 band was promoted to fact"
                )

            if r.get("edgefade_family_classification") not in (
                None,
                "DEFER_TO_OFFLINE_SHADER_HASH_SELECTOR_PROOF",
            ):
                raise ValueError(
                    "EdgeFade was promoted without selector/hash proof"
                )

            phase = r.get("hardware_observation_phase")

            if phase not in (
                None,
                "after_native_M2_material_setup",
            ):
                raise ValueError("Invalid M2 material hardware phase")

            m2_materials[seq] = r
            p = models[r.get("path", "")]
            p["materials"] += 1

            if _hardware(r, "after_native_M2_material_setup"):
                p["material_hardware_records"] += 1
                m2_material_hardware += 1

            for key, source in [
                ("alpha", "element_alpha"),
                ("blend", "blend_mode"),
                ("shader_ids", "skin_shader_id"),
            ]:
                if r.get(source) is not None:
                    p[key].add(r[source])

        elif event == "m2_batch_after_native_draw":
            if (
                frame not in starts
                or frame in ends
                or not (r.get("owner_flags", 0) & 0x20)
            ):
                raise ValueError(
                    "Direct M2 batch is outside its window "
                    "or not a placed M2"
                )

            if r.get("correlation") != M2_DIRECT:
                raise ValueError(
                    "Direct M2 batch context was promoted beyond evidence"
                )

            if r.get("route") not in {
                "triangle_batch",
                "batched_doodad",
            }:
                raise ValueError("Invalid direct M2 batch route")

            if not _hardware(r, "after_native_M2_batch"):
                raise ValueError(
                    "Direct M2 batch lacks its post-call hardware snapshot"
                )

            models[r.get("path", "")]["direct_batch_records"] += 1
            direct_m2_batches += 1

        elif event == "first_Gx_draw_after_material":
            material = m2_materials.get(r.get("material_sequence"))

            if not material or material["world_frame"] != frame:
                raise ValueError(
                    "Invalid/cross-frame M2 material correlation"
                )

            if r.get("correlation") != ORDERING:
                raise ValueError(
                    "M2 ordering correlation was promoted to ownership"
                )

            models[
                material.get("path", "")
            ]["draw_correlations"] += 1

        elif event == "wmo_material_after_native_effect_bind":
            if frame not in starts or frame in ends:
                raise ValueError(
                    "WMO material is outside its sampling window"
                )

            if r.get("correlation") != ORDERING:
                raise ValueError(
                    "WMO MOBA/effect-bind correlation was promoted "
                    "to ownership"
                )

            if (
                r.get("source_shader_id_pre_remap")
                != "UNAVAILABLE_at_runtime_render_seam"
            ):
                raise ValueError(
                    "Unavailable pre-remap WMO shader id was inferred"
                )

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

            phase = r.get("hardware_observation_phase")

            if phase not in (
                None,
                "after_native_WMO_effect_bind",
            ):
                raise ValueError("Invalid WMO effect hardware phase")

            wmo_materials[seq] = r
            p = wmos[r.get("path", "")]
            p["materials"] += 1

            if _hardware(r, "after_native_WMO_effect_bind"):
                p["effect_hardware_records"] += 1
                wmo_effect_hardware += 1

            for key, source in [
                ("runtime_shader_ids", "momt_shader_runtime_id"),
                ("selected_families", "selected_effect_family"),
                ("blend_modes", "momt_blend_mode"),
                ("material_indices", "runtime_material_index"),
            ]:
                if r.get(source) is not None:
                    p[key].add(r[source])

        elif event == "first_Gx_draw_after_wmo_effect_bind":
            material = wmo_materials.get(
                r.get("wmo_material_sequence")
            )

            if not material or material["world_frame"] != frame:
                raise ValueError(
                    "Invalid/cross-frame WMO correlation"
                )

            if r.get("correlation") != ORDERING:
                raise ValueError(
                    "WMO draw ordering was promoted to ownership"
                )

            wmos[
                material.get("path", "")
            ]["draw_correlations"] += 1

    expected = identity.get("windows")
    complete = (
        type(expected) is int
        and len(ends) == expected
        and starts == ends
    )

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

    hardware_bridge_complete = bool(
        bridge_mode
        and complete
        and raw_draw_calls > 0
        and (
            direct_m2_batches > 0
            or wmo_effect_hardware > 0
        )
    )

    return {
        "capture_complete": complete,
        "hardware_bridge_complete": hardware_bridge_complete,
        "windows": len(ends),
        "material_records": len(m2_materials),
        "wmo_material_records": len(wmo_materials),
        "m2_material_hardware_records": m2_material_hardware,
        "direct_m2_batch_records": direct_m2_batches,
        "wmo_effect_hardware_records": wmo_effect_hardware,
        "raw_gx_device_draw_calls": raw_draw_calls,
        "raw_gx_device_draw_m2_token_calls": raw_draw_m2_tokens,
        "raw_gx_device_draw_wmo_token_calls": raw_draw_wmo_tokens,
        "models": model_summary,
        "wmos": wmo_summary,
        "root_cause": "UNKNOWN",
        "fix_qualified": False,
        "edgefade_causal_status": "UNPROVEN",
        "binding_caveat": (
            "M2 direct-batch records are post-call observations tied "
            "to the native M2 batch context, not proof of unique final "
            "device-draw ownership. WMO hardware is read immediately "
            "after native effect bind, not at a proven final draw. "
            "Legacy first-Gx correlations remain ordering only."
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
        [
            json.loads(line)
            for line in raw.decode("utf-8").splitlines()
            if line
        ]
    )

    result["capture_sha256"] = hashlib.sha256(raw).hexdigest()

    text = json.dumps(result, indent=2) + "\n"

    if args.output:
        args.output.write_text(text, encoding="utf-8")

    print(text, end="")

    has_material = (
        result["material_records"]
        or result["wmo_material_records"]
    )

    return (
        0
        if (
            result["capture_complete"]
            and result["hardware_bridge_complete"]
            and has_material
        )
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
