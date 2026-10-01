# R7C2 Provisional Stack Lock — Colour Grading — 2026-10-01

## Status

ColorGrading=1 is provisionally retained in the active R7 stack.

This is a feature-presence lock, not final tuning. The combined R7 stack will be
optimised only after the intended additive features have been enabled and
qualified together.

## R7C2 delta

- ColorGrading: 0 -> 1

Preserved:
- LocalLights=1
- LocalLightIntensity=1.0
- LocalLightPhase=0.3
- ForeverGlow=1
- GodRays=0
- FarClipMax=0
- WaterQuality=3
- WaterWaves=1.0
- WaterWind=1.5
- WaterReflections=0.75

## Visual assessment

The supplied Goldshire exterior and Lion's Pride interior screenshots show a
clearer, brighter modern midtone/highlight treatment than the preceding stack,
without an obvious whole-scene colour cast or destructive loss of shadow depth.

The interior is notably punchier around fire/candle highlights and warm wood.
The exterior road/ground highlights are also brighter, while the distant fog
layer remains readable.

Because the screenshots are not exact pixel-matched A/B captures, the precise
numeric contribution is not treated as independently proven. The feature is
therefore retained at the full baseline strength for combined-stack evaluation,
not yet considered finally tuned.

## Next tranche

R7D changes only:
- GodRays=0 -> GodRays=1.0

The upstream control is a continuous strength in the range 0..4. A value of 1.0
is used as the first neutral enabled baseline; it is not assumed to be final.
