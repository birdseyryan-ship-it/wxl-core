# R7 LocalLights research — source candidate, no GPU performance claim

The outline work was checkpointed as CP02 before this tranche. Its root cause is
still UNKNOWN; the read-only WXL diagnostic needs Win32 CI and an affected-object
capture. This secondary work does not alter that conclusion.

## Source and preserved authority

Pinned CoAVolFog is `4e31ddf53a326ba9c0ad03488b4c30f7ce23c969`.
Apply the existing frozen `experiments/r6-forever-water/patch_coavolfog_for_wxl_r6.py`
first. The R7 patch refuses source whose depth header, depth transport or wrapper
does not exactly match that accepted compatibility patch. It does not write
those files, water code/shaders/data, R4/R5, Wow.exe, d3d9.dll or a client folder.

The current accepted INI remains LocalLights=0, Quality=2, Temporal=.85,
WaterQuality=3, reflections=.75, farclip 2112 and native 8x MSAA.

## What is established

PROVEN as supplied observations: the user's short same-camera LocalLights A/B
improved fog timing from about 20 ms to about 4.3 ms and recovered about 60 FPS.
This run did not repeat that live test.

PROVEN in the supplied longer log: after LocalLights switches off at 22:50:01,
slow medians recur for minutes. The audit excludes summaries spanning a settings
change and the first interval without a known predecessor. Of 34 stable-setting
disabled intervals, 25 exceed 18 ms and 9 are below 8 ms. Camera and target are
end-of-interval observations, not proof they stayed fixed for every frame.

| End-of-interval observations | Logged fog timing | LocalLights |
|---|---:|---:|
| 22:51:00.783; camera -9475.80,57.33,60.37 | 4.29 ms | 0 |
| 22:51:54.737; same camera and target | 21.52 ms | 0 |
| 22:57:52.006; camera -9459.15,49.00,59.76; Temporal=0 | 21.04 ms | 0 |
| 22:58:17.873; same camera, target and Temporal | 4.56 ms | 0 |
| 22:58:54.618; same camera, target and Temporal | 20.96 ms | 0 |

PROVEN in pinned source: zero uploaded lights selects the unlit shader. Fog
timestamps span depth refresh, march, temporal filtering, composite and optional
late GodRays; a split timer subtracts the gap before late GodRays. These timestamps
do not isolate the lit march or guarantee continuous GPU execution. Query results
are asynchronous, with 32 query sets. Delayed results do not plausibly explain
minutes of disabled-light samples by themselves.

HYPOTHESIS: part of the roughly 16–17 ms slow/fast gap may be submission/pacing or
an idle bubble within the timestamps. A difference near one display period is
not proof of that cause. Depth-copy, march, upload, filtering and composite need
separate timings before assigning it. The earlier conclusion that LocalLights
explains every slow interval is not supported by the complete log.

PROVEN in source: the lit specialization uses a dynamic four-layer loop and
does not reuse StepVariation even when a layer evaluates the exact same sample
distance. DensityVariation performs two volume fetches when enabled. Local-light
quadrature, clipped layers and authored noise have their own costs.

INFERRED: camera-only light/scissor culling will have limited value in the logged
bad scene, where the nearest uploaded light's radius is 98.3 yards and the camera
is about 43–56 yards away, inside its sphere. A separate additive scattering pass
needs an attenuation-preserving derivation and visual validation; it is not
assumed equivalent to this integrator.

UNKNOWN: the dominant GPU stage, the real GPU speedup of the candidate, whether
the candidate improves register pressure, and whether it can recover 60 FPS.

## Candidate and comparison

The cached variant extends the existing StepVariation reuse to the lit loop:
reuse only when `sampleDistance == step.distance`; otherwise sample again. No
sample position, fog coefficient, light radius/intensity/phase, step count,
quadrature, texture resolution, depth transport or composite decision changes.
The baseline variant keeps the original lit sampling policy. Both contain the
same harness additions. No new production render hook is introduced.

The CPU operation model counts base-layer density-noise fetches for the supplied
Elwynn layer profile across 16/24/32 steps and varied ray lengths/jitter. It excludes
local-light quadrature, authored-noise fetches and all other GPU work. Its roughly
two-thirds fetch reduction is an operation estimate, not a GPU timing or FPS result.
FXC may already optimise some duplication or may generate a more expensive branch.

The added D3D9 harness compares the cached and original sampling policies using
FP32 targets at all three qualities, lit and authored-noise specializations,
clipped/empty/shadowed layers, finite/unbounded limits, jitter, noise disabled,
sky/horizon rays and two active lights. It also verifies that the fixture actually
shows local-light radiance. This comparison has NOT run here.

The offline performance scene now accepts `--scene performance4k` and uses a real
3840x2160 backbuffer/viewport with `--samples 8`. It retains the original 32 warmup
and 60 measured frames, timestamp method and immediate presentation. It is a
synthetic fog-only benchmark, not the user's whole-game FPS test or full R6 water gate.

The workflow prepares paired baseline/cached research artifacts. Compile and FXC
failure block artifacts. Harness status is recorded even on unsupported CI GPU
drivers; such an artifact remains unqualified. No live installer is supplied.
The workflow has not run because publication is blocked by automatic approval review.

## Reproduce source preparation

From the reviewed WXL checkout, with a separate pinned CoAVolFog checkout:

```bash
python experiments/r6-forever-water/patch_coavolfog_for_wxl_r6.py ../coa-vfog
python experiments/r7-local-lights/patch_coavolfog_step_cache.py ../coa-vfog --variant cached
```

Use a separate pristine checkout for `--variant baseline`; the patch intentionally
refuses reapplication or unexpected source. Build on Windows using the upstream
Win32 CMake/FXC procedure. Compare shader bytecode for unlit and water passes
between variants and retain build hashes and full harness output.

Run each offline research artifact in its own folder under the same Proton/DXVK
configuration: `vfog_harness.exe --scene performance4k --samples 8`. Repeat in
alternating baseline/cached order with GPU clocks warmed and preserve complete
adapter, resolution, MSAA, timing-method, median and p95 output. Do not copy these
research artifacts into the game folder. Live qualification and an exact hashed
install/rollback package come only after build, equivalence and GPU gates pass.

## Local validation

Eight Python checks cover log attribution/settings transitions, fail-closed source
identity, preservation of the accepted depth files, read-only preparation and
refusal to overwrite another payload, and bytecode/data comparison guards. The operation model builds with GCC using
warnings as errors. The patched upstream comment checker and diff whitespace
check pass. Windows DLL/FXC compilation, D3D9 harness, GPU benchmark and live 4K
visual qualification remain NOT RUN.
