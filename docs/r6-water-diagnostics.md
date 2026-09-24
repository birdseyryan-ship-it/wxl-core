# R6 native-preserving diagnostic candidate

This candidate is the bounded **pre-Water scene-snapshot and draw-order proof** for R6. It still contains **no Classic replacement shader, refraction, reflection pass, ripple simulation or depth copy**. When explicitly armed, it may perform up to eight colour-only same-frame render-target resolves solely to prove the producer/order seam. Native Wrath water is always rendered. R5 shadows, R4 grass/environment, R3 AO/SMAA, the fixed proxy and Wow.exe remain the baseline. No live validation is claimed before the home test.

## Activation

All R6 work defaults off. Set `WXL_CLASSIC_WATER_DIAG=1` to enable. Invalid settings disable the diagnostic with one warning. The exact supplied Wow.exe SHA256 and preferred image base are checked before seven native entry-byte gates and hook registration.

| Flag | Default | Allowed values / effect |
| --- | --- | --- |
| WXL_CLASSIC_WATER_DIAG | 0 | 0 or 1; no R6 hooks/subscribers/files when off |
| WXL_CLASSIC_WATER_DIAG_MODE | full | full: bytecode, state, textures, samplers, constants; profiles: identities/state only; timing: CPU counters without heavy capture |
| WXL_CLASSIC_WATER_DIAG_SAMPLES | 3 | 1–4 samples per profile, repeated samples at least 180 presented frames apart |
| WXL_CLASSIC_WATER_DIAG_MAX_DRAWS | 256 | 1–512 admitted draw records across the process, including resets |
| WXL_CLASSIC_WATER_DIAG_MAX_MIB | 16 | 1–32 MiB hard output ceiling; 2 MiB pending CPU queue; ≤256 KiB written per Present |
| WXL_CLASSIC_WATER_DIAG_COPY | 0 | 0 observes ordering only; 1 arms the bounded colour-only pre-Water snapshot proof (maximum 8 process-wide, 4 per device generation); native rendering still continues |

No setter/replacement flag exists. COPY=1 is diagnostic only and cannot suppress or replace a native draw. It proves nothing until the emitted snapshot/order evidence passes audit and the live reset/regression checks. Depth resolve remains deliberately out of scope. Heavy capture is unsuitable for performance measurements; use timing and off for those comparisons.

## Hooks and ownership

`src/client/CWorldScene/LiquidDiagnostics.cpp` joins existing MinHook chains at priority 100. A new append-only `OnWorldSceneBegin` event is emitted from the already-hooked world-scene seam immediately before the native world renderer; existing event IDs are unchanged. Native pass, Water, WaterNoSpec, ProcWater, Magma and terrain/WMO provider scopes identify context. The first verified base-Water material entry in an outer world scene is the only snapshot producer candidate, before that material calls its native renderer. The existing liquid-scoped DrawIndexedPrimitive seam remains the final replacement-eligibility/consumer observation point. The existing grass predecessor remains in the draw chain; R6 never wraps SetVertexShader or SetPixelShader.

The four D3D9 draw-entry wrappers (`DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`) and optional float-constant setter wrappers are installed from the native EndScene event, after existing static subscribers such as grass. Prior pointers are preserved; every native draw and float-setter is forwarded exactly once with its original arguments and HRESULT. A global draw ordinal plus bounded post-Water state probes test whether likely opaque colour/depth-writing work still occurs on the same RT0 after the candidate. Capture exceptions quarantine observation. Actual downstream hook ordering still requires live confirmation; this diagnostic never authorises replacement merely from its discovery label.

In full mode, successful VS/PS float writes **during native material scope** are marked. Writes outside that scope and integer/bool write calls are not traced. Final float/int/bool state is separately queried. Static shader reads and relative addressing require offline bytecode analysis. No constant/sampler is declared free.

The source comment mapping selector 2 to slime and 3 to magma is not used: verified shader-class dispatch is 2 Magma, 3 ProcWater. A material scope plus final shader pair plus provider identity is required for the strongest discovery classification. Unknown/fixed-function paths remain unknown and native.

## Capture v1

Output: `Logs/r6-water-<process>-<tick>.ndjson`. Every record contains schema=1, event, frame, generation, sequence, world_epoch, view and phase. Records are deduplicated and capped; dropped counts are explicit. Timing uses QueryPerformanceCounter and means CPU inspection/submission, never GPU time.

| Event | Payload / limits |
| --- | --- |
| session | Executable authority, effective mode/limits, `copy_supported=true`, `replacement_supported=false`, the eight-attempt process ceiling and four-attempt per-device-generation ceiling; Classic target controls are labelled target metadata, not live Wrath CVar values |
| device | First-observed/reset device, prior draw-owner module/RVA, caps, MSAA and present parameters; the diagnostic snapshot target is allocated lazily only after a verified Water candidate |
| boundary | Before/after one native liquid invocation, pass, instance count, RT0/depth descriptors, viewport, phase; ≤128 records process-wide |
| shader | Final observed VS/PS object, process-unique ID, SHA256, size/version and full-mode exact little-endian bytecode hex; at most 128 simultaneously retained shader objects; ≤64 KiB each |
| draw | Liquid-scoped material/settings/provider/pass; shader SHA256s; declaration/FVF; topology/streams/indices; render states/hash; RT/depth/viewport; full-mode 16 pixel and 4 vertex texture/sampler stages and constant blocks |
| world_scene | Outer world-scene begin/end serial, draw ordinals, target/state envelope, first/last liquid ordinal, post-Water counts and provisional ordering result |
| water_candidate | First verified base-Water material entry for the outer world-scene serial; records producer ordinal and whether earlier liquid draws already occurred |
| snapshot_attempt | At most 8 process-wide and 4 per device generation: direct-original EndScene/StretchRect/BeginScene HRESULTs, CPU bracket ticks, source/destination descriptors and state-preservation result; `replacement_allowed=false` |
| post_water_draw | At most 16 detailed records from a bounded 128-probe window after the candidate, classifying likely late opaque same-RT writes versus unclassified work |
| summary | Counts, failures, profile admissions, queue/output/drop counts and CPU ticks/frequency |
| lost/reset/world_leave/shutdown | Lifecycle evidence; invalidation and final pending/drop counts |

Profiles use class, provider, pass, VS/PS hashes, declaration, source-declared material-settings bytes and topology. Texture descriptor SHA256 is stable for that description, **not a texel hash or guaranteed unique resource ID**. Texture pointer tokens are session correlation only; they may be reused. Native target/texture/stream references from getters are released in the same inspection. Shader cache references are released on lost/world-leave/device-change. Reset invalidates profile keys but retains process-wide ceilings. Ordinary process shutdown closes CPU output without invoking D3D under loader lock. Hot DLL unloading is not supported by this core/hook architecture.

Float constants are raw little-endian bytes, including NaN/Inf bit patterns; they are never emitted as invalid JSON numeric literals. HRESULTs distinguish missing/unsupported/failed queries from proven empty state. Native camera/world/settings captures are source-declared raw bytes; readability does not prove coordinate or colour semantics.

Run `python tools/r6-water-check/audit_capture.py <capture.ndjson> --extract <new-directory>` after the home test. It checks schema envelope, shader hashes/IDs, material mapping, constant lengths, record order and ceilings, and extracts exact bytecode without overwriting different existing data. It makes no spare-register or visual-fidelity claim. Dropped shader records can make the audit fail; keep that failure as evidence of an incomplete capture.

## Safety / remaining live gates

With COPY=0 the module remains observational. With COPY=1, the only mutation is a bounded diagnostic scene bracket: retain explicit COM references to current RT0/depth and state, call the direct-original `EndScene`, `StretchRect` RT0 into an R6-owned single-sample `D3DPOOL_DEFAULT` render-target texture of matching size/format, then call `BeginScene` exactly once when EndScene succeeded. The snapshot texture is never bound or sampled by this candidate. RT0/depth/viewport and relevant render state are re-probed afterward; any BeginScene failure quarantines the diagnostic. There is no target binding, constant substitution, replacement draw, readback, GPU query or depth copy.

Verify in one home session: matching native appearance; the first Water candidate occurs before the first liquid draw; no likely opaque same-RT writes occur after the candidate; all attempted EndScene/StretchRect/BeginScene brackets succeed; state is preserved; reset/lost recovery releases and lazily recreates the DEFAULT-pool snapshot resource; grass/shadow/UI/AO remain intact; and off/timing performance stays unchanged on the same machine. Test shallow lake/river, coast, waterline, optional waterfall/WMO/non-water examples and the Amberpine reference. Do not infer feature absence from a site producing no liquid draw records. The per-generation reservation deliberately leaves four attempts available after one successful Reset so lazy DEFAULT-pool recreation can be proven. A passing diagnostic authorises only the colour-snapshot producer/order seam, not the final Classic-water shader or any depth-dependent design.

CI uses the existing Win32/MSVC workflow, retains R5 receiver checks, and adds portable policy/hash tests plus capture-integrity tests. Deploy only a hash-pinned candidate WarcraftXL.dll. The workflow-built proxy is not the accepted fixed proxy and must not be deployed. The accepted R5 WarcraftXL.dll is the rollback, not the older DIAG9 DLL.
