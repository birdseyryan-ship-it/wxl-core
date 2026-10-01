# R7 distant-outline diagnostic — candidate, not a qualified fix

Primary status: BLOCKED on a live, affected-object draw capture. No production
rendering change has been made. Do not promote this DLL to the accepted baseline.

The uploaded source tar and screenshot directory were empty. The real diagnostic
branch was recovered at bf79f50f84cbace06dd58ae7f15084f53f44c8ed, tree
17dec30e12bca7d5c93129e71702566d8ec77a8b. Existing R4/R5/R6 code is unchanged.

## Findings and confidence

PROVEN in source: R4 changes seeds only while the native table builder runs, then
restores them. The native builder derives live distance/fade/cull tables. Native
fade widths are retained. R4's alpha override calls native material setup and
changes only mode-1 alpha reference to elementAlpha * 64/255, clamped.

STRONGLY SUPPORTED by curated reverse-engineering comments: M2 has no per-frame
skin LOD selection in this build. +0x194 is max co-instances, not an LOD multiplier.
Skin profile is initially selected using the device bone budget. Element alpha
at +0x0C is native instance/color/weight alpha, not itself a distance-band selector.
The co-instance draw route shares batch alpha; compatibility selects that route.
These comments were not re-proven by disassembling Wow.exe in this run.

PROVEN as supplied live observations: Temporal=0 leaves the defect unchanged.
This rejects temporal history as the necessary cause. It does not identify the
responsible object or exclude every other compositor/depth/material interaction.

HYPOTHESES: mip/alpha coverage, a material/draw-route mismatch, another native
fade/cull stage, or WMO rendering. The coarse object-distance-off A/B only shows
whether the object is retained by that table. Disappearance alone does NOT prove
the table is responsible for drawing it incorrectly.

UNKNOWN: the affected model/category, actual selected native band, exact final
draw association, actual sampled mip and root cause. No fix claim is justified.

## What the diagnostic observes

| Stage / category | Observations | Remaining limitation |
|---|---|---|
| Placed M2, ADT or WMO | owner flag, model path, placement, distance, native alpha, skin pointer, current section and source shader/material IDs | placement origin ADT vs WMO not proven |
| Native material setup 0x81FE90 | post-native/post-R4 material and element alpha, all five cutoff/fade tables | selected native distance band remains UNKNOWN |
| Native readiness 0x824FC0 | unchanged return, only two curated doodad drain callers | readiness is not a distance-culling decision |
| Batch compatibility 0x824550 | unchanged native return and matching instance | compatibility alone does not prove which later draw ran |
| Gx draw 0x6A3620 | caller VA, primitive descriptor, first draw after material setter, post-call D3D state, shader SHA256, texture dimensions/levels/filter/bias | ordering correlation needs validation; post-call state is not a pre-draw state trace |
| WMO geometry / terrain / horizon | existing curated source map only | no object-specific draw trace captured by this M2 diagnostic |

No visibility, alpha, blend, LOD, geometry, batch-compatibility or D3D state is
changed. Diagnostics default OFF and fail closed on invalid flags or wrong
canonical Wow.exe SHA/base. Sampling and output have strict bounds. No new device
vtable swaps are added. The observer chains through existing native hooks.

## Live test

Keep the accepted R7 profile: LocalLights=0, Temporal=0.85, farclip 2112, alpha
reference 64, water quality 3/reflections .75, native 8x MSAA. Retain your current
R7 glow/grading/GodRays settings. This package does not edit CoAVolFog.ini.

Extract the verified package to `~/Downloads/R7_OUTLINE_LIVE_TEST_20261001` and
close WoW. Run this complete guarded block to install the candidate:

```bash
READY=1
CLIENT="/home/rbirdsey/Games/WoW-3.3.5a-CLASSIC-GFX-DEV"
PKG="$HOME/Downloads/R7_OUTLINE_LIVE_TEST_20261001"
if pgrep -af '[Ww]ow\.exe$'; then
    printf '%s\n' 'WoW is running; close it first.'
    READY=0
else
    PROC_STATUS=$?
    if [ "$PROC_STATUS" -ne 1 ]; then READY=0; fi
fi
if ! command -v python3 >/dev/null 2>&1; then READY=0; fi
if [ ! -f "$PKG/WarcraftXL.dll" ]; then READY=0; fi
if [ "$READY" -eq 1 ]; then
    python3 "$PKG/manage_candidate.py" install --client "$CLIENT" --package "$PKG"
fi
```

Use the entire launch-options line in `LAUNCH_OPTIONS.txt`. Stand at the affected
camera before logout, then log in there. It captures world frames 1800, 1920 and
2040 (roughly 30–34 seconds at 60 FPS). Hold camera position and take a screenshot
around the capture time showing which object is wrong. Verify the `r7-outline:
read-only capture ...` line in Logs/wxl-core.log. Sampling is not an FPS benchmark;
its inspection and logging deliberately add temporary overhead on sampled frames.

Then close WoW and collect `Logs/r7-outline-*.ndjson`, `Logs/wxl-core.log`,
CoAVolFog.ini, CoAVolFog.log and the screenshot. Run the audit against the selected
capture, if desired: `python3 audit_outline.py CAPTURE.ndjson --output audit.json`.
It reports capture integrity; it deliberately cannot declare a cause or qualified fix.

Optional separate visibility discriminator, with the same camera/INI: change only
`WXL_EXTENDED_OBJECT_DISTANCE=1` to `WXL_EXTENDED_OBJECT_DISTANCE=0` in the full
launch line. Keep WXL_EXTENDED_FARCLIP=1 and alpha ref 64. Restore it afterward.
Disappearance shows a retention difference; an object visible in BOTH modes but
rendering differently provides stronger causal evidence. Do not use this as the fix.

## Rollback

Close WoW; run this entire block. It restores the hash-verified accepted WXL from
the recorded backup and refuses to overwrite an unrelated later candidate.

```bash
READY=1
CLIENT="/home/rbirdsey/Games/WoW-3.3.5a-CLASSIC-GFX-DEV"
PKG="$HOME/Downloads/R7_OUTLINE_LIVE_TEST_20261001"
if pgrep -af '[Ww]ow\.exe$'; then
    printf '%s\n' 'WoW is running; close it first.'
    READY=0
else
    PROC_STATUS=$?
    if [ "$PROC_STATUS" -ne 1 ]; then READY=0; fi
fi
if ! command -v python3 >/dev/null 2>&1; then READY=0; fi
if [ "$READY" -eq 1 ]; then
    python3 "$PKG/manage_candidate.py" rollback --client "$CLIENT" --package "$PKG"
fi
```

Restore your normal launch options, or use `NORMAL_LAUNCH_OPTIONS.txt`. Leave
LocalLights=0 until a separate performance candidate is qualified. Neither install
nor rollback writes Wow.exe, d3d9.dll, the companion DLLs or the server checkout.

## Next engineering decision

Correlate the screenshot object against path/placement and captured native caller.
Validate material-to-draw ownership with a narrow native caller trace or frame
capture before treating post-call shader/state observations as the exact culprit.
If the object is WMO/terrain, instrument that identified category rather than
extending this M2 capture blindly. Implement a production fix only after the
responsible stage is proven. Keep R4 distance and alpha baseline intact meanwhile.
