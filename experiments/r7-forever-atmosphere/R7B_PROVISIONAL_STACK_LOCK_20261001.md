# R7B Provisional Stack Lock — Local Lights — 2026-10-01

## Status

R7B local-light scattering is provisionally accepted into the active R7 stack.

This is a feature-presence lock, not final tuning. The strategy from this point is
to enable and qualify the intended additive R7 features one at a time at their
baseline values, then optimise/tune the combined stack after interaction effects
are visible.

## R7B delta

- LocalLights: 0 -> 1
- LocalLightIntensity remains 1.0
- LocalLightPhase remains 0.3

No other functional setting changed at R7B entry.

## Preserved authority

- R4 remains sole far-clip owner: FarClipMax=0
- R7A atmosphere baseline remains unchanged
- R6/R7A water baseline remains:
  - WaterQuality=3
  - WaterWaves=1.0
  - WaterWind=1.5
  - WaterFoam=1.0
  - WaterReflections=0.75
- GodRays=0
- ForeverGlow=0
- ColorGrading=0

## Visual assessment

The supplied Goldshire/Lion's Pride screenshots were visually healthy:
- warm interior lighting remained coherent and attractive;
- the exterior retained strong atmospheric depth;
- no obvious local-light haloing, banding, clipping or scene-wide wash was visible.

The stills do not by themselves prove the exact magnitude of local-light
volumetric scattering because there is no same-camera R7A off/on pair. Therefore
LocalLights=1 is retained provisionally at its upstream calibration rather than
retuned now.

## Next tranche

R7C1 changes only:
- ForeverGlow=0 -> ForeverGlow=1

Local lights remain enabled and untuned so interaction can be judged before
global optimisation.
