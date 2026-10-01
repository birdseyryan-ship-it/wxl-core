# R7A Locked Visual Baseline — 2026-10-01

## Status

R7A core atmosphere is visually accepted as the baseline for the next additive
tranche.

This is a visual/configuration lock, not a replacement for the existing frozen
R0-R6 binary authority.

## Locked R7A core atmosphere

- Quality=2
- Density=1.0
- Haze=1.0
- GroundFog=0.6
- FarFog=1.0
- NoiseAmount=0.15
- NoiseScale=0.025
- NoiseWindSpeed=0.5
- ClassicNoise=1
- StockFog=1
- TransparentFog=0
- DataMode=1
- ColorSpace=1
- SunScatter=1.0
- Ambient=1.0
- Exposure=1.0
- ClassicExposure=1.0
- ClassicPhase=0
- InteriorAware=1
- InteriorDensity=0.15
- Temporal=0.85

## Features deliberately still off

- LocalLights=0
- GodRays=0
- ForeverGlow=0
- ColorGrading=0
- FarClipMax=0

R4 remains the sole far-clip owner.

## Locked water baseline carried into R7B

- WaterQuality=3
- WaterWaves=1.0
- WaterWind=1.5
- WaterFoam=1.0
- WaterReflections=0.75
- WaterSpecular=1.0
- WaterClarity=1.0
- WaterZoneColors=0.5
- WaterRipples=0.5
- WaterClientSplashes=0

WaterWind=1.5 is the accepted visual micro-tune relative to the earlier R6
WaterWind=2.0 baseline. It was chosen from a controlled Bloodhoof Village lake
A/B at approximately 12:17 server time because it reduced small-scale surface
busyness while preserving reflection strength and the modern-water look.

## Visual evidence summary

Elwynn Forest:
- foreground remained readable;
- mid/far trees gained progressive atmospheric depth;
- no obvious hard fog edges or gross haloing were observed in the supplied
  screenshots;
- modern water remained visually coherent inside the fogged scene.

Bloodhoof Village:
- a temporary server TZ override was used only to align local Azeroth time with
  a daylight WoW Forever reference;
- the daylight comparison showed the R7A atmosphere in the same broad visual
  family as the Forever reference;
- WaterReflections=0.75 remained appropriate;
- WaterWind=1.5 was preferred over 2.0 for a calmer lake surface.

## Technical acceptance boundary

The existing R6 live gate already proved:
- requested 8x MSAA -> used 8x;
- WXL two-stage StretchRect MSAA depth transport -> INTZ;
- modern water shading and FFT waves;
- 0 skipped water frames in the recorded R6 gate.

A dedicated post-R7A runtime log should still be retained before final R7
freeze so the active fog GPU median and continued MSAA/depth transport are
recorded under the atmosphere-on configuration.

## Next tranche

R7B changes one functional switch only:

LocalLights=0 -> LocalLights=1

All other R7A and water values remain fixed during the R7B A/B.
