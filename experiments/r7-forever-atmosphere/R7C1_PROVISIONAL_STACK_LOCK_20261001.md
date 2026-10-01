# R7C1 Provisional Stack Lock — Forever Glow — 2026-10-01

## Status

ForeverGlow=1 is provisionally retained in the active R7 stack.

This is a feature-presence lock, not final tuning. The combined stack will be
optimised after the intended additive features have all been qualified.

## R7C1 delta

- ForeverGlow: 0 -> 1

Preserved:
- LocalLights=1
- ColorGrading=0
- GodRays=0
- FarClipMax=0
- WaterQuality=3
- WaterWaves=1.0
- WaterWind=1.5
- WaterReflections=0.75

## Visual evidence

A near-identical Goldshire exterior before/after pair showed essentially no
global exposure or colour shift, which is a healthy result because ForeverGlow
uses authored modern glow amounts and many continent lights have zero glow.

The supplied inn interior remained visually coherent, with no obvious scene-wide
wash, fog blowout or destructive haloing. Because that camera was not an exact
pixel-matched A/B, the magnitude of the interior glow contribution is not
treated as independently proven.

## Next tranche

R7C2 changes only:
- ColorGrading=0 -> ColorGrading=1

ForeverGlow=1 and LocalLights=1 remain enabled so the look controls can be
qualified in the intended combined stack before final tuning.
