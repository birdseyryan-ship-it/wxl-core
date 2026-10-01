# R7 Performance Diagnosis 02 — LocalLights proven culprit — 2026-10-01

## Result

At the exact low-FPS Elwynn scene, hot-toggling only:

LocalLights=1 -> LocalLights=0

restored observed frame rate from about 40 FPS to about 60 FPS.

The live GPU timing confirms the causal change:

Before:
- sustained fog GPU medians around 19.65–21.81 ms;
- water remained ~2.02 ms.

After the LocalLights toggle:
- first summary still contained pre-toggle frames (~20.85 ms);
- subsequent settled fog medians dropped to 4.36, 4.66, 4.31, 4.29, 4.29 ms;
- water remained ~2.02 ms.

This is a decisive live A/B. LocalLights is the dominant R7 performance blocker in the affected scene.

## Interpretation

Pinned upstream behavior switches to a dedicated lit fog march/composite whenever at least one local point light is uploaded. The active shader integrates local-light data through the fog march. At 3840x2160, this path is too expensive on the target RTX 5060 when active, despite the unlit fog path being acceptable.

R7B visual intent remains accepted, but the current upstream implementation is not acceptable for the 60-FPS target.

## Operational state

Until the implementation is optimised:
- keep LocalLights=0 in the live performance baseline;
- retain LocalLightIntensity=1.0 and LocalLightPhase=0.3 as target tuning values;
- do not discard the R7B feature goal.

Other current candidate features may remain enabled for testing:
- ForeverGlow=1
- ColorGrading=1
- GodRays=1.0 (subject to daylight visual/performance qualification)

## Next engineering task

Optimise the local-light fog path rather than reducing unrelated water or atmosphere quality.

Candidate directions, to be benchmarked rather than guessed:
1. reduce local-light work spatially to pixels/rays whose march intersects a light sphere;
2. cap/cluster the active light set more aggressively;
3. separate local-light scattering into a lower-cost additive pass rather than evaluating all lights inside every fog march sample;
4. exploit per-light screen/scissor bounds or low-resolution accumulation;
5. retain the unlit march for most pixels and composite local-light scattering only where needed.

Do not simply lower LocalLightIntensity; intensity changes appearance but does not remove the expensive lit shader branch.

## Separate blocker

The distant silhouette/outline artifact remains and predates LocalLights. With LocalLights=0 now restoring performance headroom, test Temporal=0 next at the same artifact location.
