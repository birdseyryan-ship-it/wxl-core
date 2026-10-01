# R6 Forever Water Companion Qualification

This directory defines the first reversible R6 pivot candidate based on
`jealous-sound/coa-vfog`.

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
Forever glow, colour grading and far-clip override.

The intentional R6 visual delta is therefore water only:

- Forever-derived lake/river/ocean/interior presets;
- GPU FFT waves;
- refraction and depth absorption;
- crest/shore/shallow foam;
- scene SSR with sky fallback;
- sun/moon glint;
- local unit ripples/wakes.

Magma, slime and unsupported liquids remain native.

## Important compatibility gate

This is a companion qualification candidate, not an accepted production
install. CoAVolFog wraps Direct3D9 above the existing D3D9 implementation and
has its own readable-depth/MSAA handling. Therefore R3 AO/SMAA, R4 environment
and R5 shadows must be requalified in-game before this path can be accepted.

Rollback is deletion of only the companion files added by the candidate.
The accepted WarcraftXL.dll, d3d9.dll and Wow.exe must not be replaced.

## Candidate order

1. Build and pass the upstream harness.
2. Preserve and hash the current DEV client.
3. Add only version.dll, CoAVolFog.dll, CoAVolFog.ini, waterdata.bin and
   fogdata.bin.
4. Launch with the existing accepted R0-R5/R6-off options; do not add the old
   Classic-water replacement flag.
5. Verify startup log and modern water.
6. Requalify R3 AO/SMAA, R4 grass/environment and R5 shadows.
7. If any frozen phase regresses, remove the companion files and return to the
   accepted runtime before changing source.
