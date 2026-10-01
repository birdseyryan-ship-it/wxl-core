# R6 Forever Water Companion Qualification

This directory defines the R6 production-compatibility candidate based on
`jealous-sound/coa-vfog`, layered additively over the frozen R0-R5 runtime.

## Frozen authority

The branch starts from WarcraftXL qualification commit:

`1f03309da4639771d74c8957c85bd9653069b7c4`

That commit's tree is the qualified R6 baseline whose commit message records
that R0-R5 are untouched. This tranche does not edit WarcraftXL, the accepted
`d3d9.dll`, or `Wow.exe`.

## Pinned upstream

CoAVolFog is built from exactly:

`4e31ddf53a326ba9c0ad03488b4c30f7ce23c969`

The upstream water implementation is GPL-3.0-only. The packaged artifact keeps
the upstream LICENSE and exact commit identity.

## Scope

The supplied INI intentionally enables modern water while disabling or
neutralising CoAVolFog's fog, Classic fog data use, local-light fog, god rays,
Forever glow, colour grading and far-clip override. The visually qualified
reflection multiplier is locked at `WaterReflections=0.75`.

The intentional R6 visual delta is therefore water only:

- Forever-derived lake/river/ocean/interior presets;
- GPU FFT waves;
- refraction and depth absorption;
- crest/shore/shallow foam;
- scene SSR with sky fallback;
- sun/moon glint;
- local unit ripples/wakes.

Magma, slime and unsupported liquids remain native.

## R0-R5 compatibility delta

The accepted `Wow.exe`, `d3d9.dll` and `WarcraftXL.dll` remain byte-for-byte
untouched. The CI build applies a small patch only to the pinned CoAVolFog
source before compiling the companion:

- on DXVK/another non-system D3D9 implementation, keep the game's requested
  multisampling and use a runtime-self-tested two-stage depth transport:
  multisampled native depth -> single-sample same-format depth -> INTZ;
- the transport deliberately uses the same two-stage StretchRect shape already
  live-proven by WarcraftXL R6 diagnostics;
- when the exact R6 water-only profile has all fog density sources at zero,
  skip the fog preparation/composite path entirely while retaining the readable
  depth substrate needed by water.

The existing CoAVolFog depth-copy self-test remains authoritative. If the
two-stage route fails on the live device, the candidate is not accepted.

Rollback is deletion of only the companion files added by the candidate.
The accepted WarcraftXL.dll, d3d9.dll and Wow.exe must not be replaced.

## Candidate order

1. Build the pinned upstream plus the reviewed WarcraftXL compatibility delta.
2. Preserve and hash the current DEV client.
3. Replace only the five R6 companion files; never replace frozen R0-R5 files.
4. Launch with the existing accepted R0-R5 options plus `version=n,b`.
5. Require the live log to report 8x multisampling retained by the WXL
   two-stage StretchRect path and a passing depth-copy self-test.
6. Verify water remains visually equivalent to the qualified build, with
   reflections at 0.75.
7. Require the water-only profile to skip fog rendering rather than spending
   GPU time on a zero-effect composite.
8. Requalify the frozen visual stack before freezing R6.
