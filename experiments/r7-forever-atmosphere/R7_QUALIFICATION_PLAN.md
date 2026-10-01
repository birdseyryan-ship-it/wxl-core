# R7 Forever Atmosphere — Qualification Plan

R7 begins from accepted R6 authority. R0-R6 stay frozen.

## Permanent ownership boundaries

- WarcraftXL/R4 remains the sole far-clip owner.
- CoAVolFog FarClipMax remains 0.
- R6 water stays enabled at WaterQuality=3 and WaterReflections=0.75.
- The accepted WXL two-stage StretchRect 8x-MSAA depth path remains required.
- Wow.exe, d3d9.dll, WarcraftXL.dll and the accepted R6 companion DLLs are not
  replaced for configuration-only R7 tranches.

## Tranches

### R7A — core atmosphere

Enable the authored/derived volumetric fog system, Classic fog-data use,
linear-light scattering, temporal filtering and indoor/outdoor transition.

Deliberately keep off:
- local-light scattering;
- Forever glow replacement;
- colour grading;
- god rays;
- CoAVolFog far-clip override.

Goal: prove the fog core alone is visually useful, performant and compatible.

### R7B — local lighting

Enable LocalLights=1 at the upstream calibration. Validate Stormwind/Orgrimmar
and night/artificial-light scenes. No glow replacement or grading yet.

### R7C — Forever look controls

Qualify ForeverGlow=1 and then ColorGrading independently. Do not bundle their
acceptance. Keep only effects that improve the target look without breaking
accepted phase ownership.

### R7D — optional god rays

GodRays is upstream-default off and is not required for R7 acceptance. Treat it
as an optional enhancement after the authored/Forever-derived features are
stable.

## Live gates for every tranche

- requested 8x MSAA must remain used 8x;
- WXL two-stage StretchRect depth self-test must pass;
- R6 water must remain shaded and WaterReflections must remain 0.75;
- no crash, reset-loop or failed hook;
- record fog/water GPU medians;
- compare at least one exterior daylight/sunset scene, one city/artificial
  light scene, and one interior transition;
- rollback is config restoration only unless a code defect is discovered.
