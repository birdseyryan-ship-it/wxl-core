# R7 Silhouette Diagnosis 01 — linked back to R4 far-clip work — 2026-10-01

## Live discriminator

At the same visible distant-object outline:
- Temporal=0.85 -> artifact present
- Temporal=0 -> artifact unchanged

Therefore the outline is not caused by CoAVolFog temporal history/reprojection.

## Historical linkage

The user confirms the artifact dates back to the R4 far-clip/object-distance work,
before the current R7 atmosphere stack. R4 extended the client far clip from its
native ~1583.33 ceiling to 2112 and proportionally extended the native five-band
object fade table. R4 also separately fixed distant foliage alpha-test coverage
with WXL_ALPHA_KEY_REF_BYTE=64.

This makes the current fog stack an amplifier/revealer of an older distant-LOD
artifact rather than the root cause.

## Current strongest hypothesis

A native distant-object LOD/fade/material path remains unscaled or mismatched
relative to the extended R4 visibility distances. CoAVolFog then makes the
contrast discontinuity easier to see in dense haze.

Do not patch CoAVolFog temporal/upsample logic for this symptom unless a later
control disproves the R4 linkage.

## Next discriminator

Temporarily test the same camera with:
1. accepted R7 stack retained;
2. LocalLights=0 for performance headroom;
3. Temporal restored to 0.85;
4. only WXL_EXTENDED_OBJECT_DISTANCE disabled at launch while keeping
   WXL_EXTENDED_FARCLIP enabled.

Interpretation:
- if the outlined object disappears with the extended object table disabled,
  the bug is in the R4 object-distance / LOD / fade family;
- if the outline persists on terrain/WMO geometry independent of object-distance
  extension, isolate far-clip/terrain/WMO behavior separately.

This is a diagnostic launch only. Do not alter the frozen R4 production source
or accepted defaults yet.
