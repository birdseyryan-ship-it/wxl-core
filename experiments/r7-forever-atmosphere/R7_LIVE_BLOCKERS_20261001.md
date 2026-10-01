# R7 Live Blockers — 2026-10-01

## Current accepted/provisional stack

R7A atmosphere: visually accepted baseline.
R7B LocalLights=1: provisionally retained.
R7C1 ForeverGlow=1: provisionally retained.
R7C2 ColorGrading=1: provisionally retained.
R7D GodRays: PAUSED pending blocker resolution.

Water baseline retained:
- WaterQuality=3
- WaterWaves=1.0
- WaterWind=1.5
- WaterReflections=0.75

FarClipMax remains 0; R4 remains the far-clip owner.

## Blocker 1 — distant-object outline / silhouette artifact

Live symptom:
- distant objects/trees in heavy fog can acquire a strange visible contour/outline;
- user reports this predates the latest R7B/C changes, so it is not treated as a new local-lights/glow/grading regression.

Relevant upstream architecture:
- fog march is quarter/half resolution by Quality;
- full-resolution composite uses depth-validated upsample taps;
- temporal history rejects different surface depth/depth class;
- upstream has explicit thin-silhouette fallback/tests.

Hypotheses to discriminate, not yet proven:
1. temporal-history rejection/accumulation artifact;
2. low-resolution composite/upsample edge failure in a live geometry/depth case not covered by harness tests;
3. depth-classification or resolved-depth discontinuity at distant geometry.

Required live discriminators:
- Temporal 0 vs 0.85;
- Quality 1/2/3 behaviour;
- DebugView=3 linear-depth view around the same silhouette.

Do not patch the compositor until the live discriminator identifies the failing stage.

## Blocker 2 — 4K performance below 60 FPS target

Live symptom:
- ~40 FPS in some Elwynn locations with current R7 stack.
- 40 FPS implies ~25.0 ms/frame; 60 FPS target permits ~16.67 ms/frame, so approximately 8.3 ms/frame must be recovered in the affected scene if GPU-bound.

Known prior R6 water cost:
- water GPU median 2.07 ms over 4171 frames, 0 skipped in the accepted live gate.

Required live evidence before tuning:
- current `fog gpu ...` median;
- current `water gpu ...` median;
- confirmation of 8x MSAA retained;
- note whether GodRays remained off during the measurement;
- approximate GPU utilisation if readily available.

Performance work must preserve visual ownership and first isolate which R7 pass consumes the missing frame budget.

## R7D gate

Do not enable GodRays until both:
- the distant-outline artifact is understood well enough to avoid masking it; and
- the current combined R7A-C2 GPU cost is measured.

GodRays can be resumed after these measurements.
