# R7 Performance Diagnosis 01 — 2026-10-01

## Live evidence

Target system: 3840x2160, requested 8x MSAA.

The live log confirms:
- 8x MSAA retained;
- WXL two-stage StretchRect depth copy passes;
- water remains stable at about 2.02–2.03 ms GPU median;
- fog GPU cost is strongly scene-dependent, ranging from about 3.85 ms to about 21.13 ms.

GodRays was hot-enabled at 22:39:18.148. The same high-cost fog plateaus
(roughly 12–21 ms) already occurred before GodRays was enabled, so GodRays
cannot be the sole cause of the Elwynn performance collapse.

Immediately after enabling GodRays, fog remained about 3.85–4.58 ms before
later returning to scene-dependent 12–21 ms plateaus. At night the ray path can
also be gated by celestial visibility/screen position, so this run does not
establish a standalone GodRays cost.

## Strong current suspect: local-light fog path

Pinned upstream renderer behaviour:
- LocalLights=1 uploads up to eight point lights.
- If at least one is uploaded, the renderer switches from the normal march to a
  dedicated lit march shader and lit composite path.
- The lit path processes the local-light data during fog integration.

The discrete live plateaus (~3.9 ms, ~12.9 ms, ~20.5 ms) are consistent with a
scene-dependent branch such as local-light capture, but this is not yet proven.

## Next discriminator

At the exact bad Elwynn camera:
1. keep all current settings, including GodRays, unchanged;
2. record current FPS;
3. hot-toggle only LocalLights=1 -> 0;
4. wait at least 60 seconds;
5. record FPS and fog GPU median;
6. restore LocalLights=1 after the test unless explicitly accepting a change.

Also inspect the log's `local lights:` lines around the high-cost intervals.

If LocalLights=0 collapses fog cost back toward the ~3.9 ms baseline and returns
FPS toward 60, R7B requires optimisation/re-design rather than simple intensity
tuning: LocalLightIntensity changes appearance, not the expensive lit-shader
branch itself.

## Silhouette blocker

Treat separately. The outline predates LocalLights/ForeverGlow/ColorGrading.
After the performance discriminator, test Temporal=0 at the same silhouette
before changing Quality or compositor code.
