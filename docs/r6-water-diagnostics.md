# R6 native-preserving diagnostic candidate

This candidate observes Wrath liquid rendering. It contains **no Classic replacement shader, refraction, reflection pass, ripple simulation or scene copy**. R5 shadows, R4 grass/environment, R3 AO/SMAA, the fixed proxy and Wow.exe remain the baseline. No live validation is claimed.

## Activation

All R6 work defaults off. Set `WXL_CLASSIC_WATER_DIAG=1` to enable. Invalid settings disable the diagnostic with one warning. The exact supplied Wow.exe SHA256 and preferred image base are checked before seven native entry-byte gates and hook registration.

| Flag | Default | Allowed values / effect |
| --- | --- | --- |
| WXL_CLASSIC_WATER_DIAG | 0 | 0 or 1; no R6 hooks/subscribers/files when off |
| WXL_CLASSIC_WATER_DIAG_MODE | full | full: bytecode, state, textures, samplers, constants; profiles: identities/state only; timing: CPU counters without heavy capture |
| WXL_CLASSIC_WATER_DIAG_SAMPLES | 3 | 1–4 samples per profile, repeated samples at least 180 presented frames apart |
| WXL_CLASSIC_WATER_DIAG_MAX_DRAWS | 256 | 1–512 admitted draw records across the process, including resets |
| WXL_CLASSIC_WATER_DIAG_MAX_MIB | 16 | 1–32 MiB hard output ceiling; 2 MiB pending CPU queue; ≤256 KiB written per Present |
| WXL_CLASSIC_WATER_DIAG_COPY | 0 | A value of 1 is explicitly rejected as unsupported; native rendering continues |

No setter/replacement flag exists. Do not interpret COPY=1 as a completed proof. Safe scene bracketing, opaque ordering and depth resolve remain unresolved. Heavy capture is unsuitable for performance measurements; use timing and off for those comparisons.

## Hooks and ownership

`src/client/CWorldScene/LiquidDiagnostics.cpp` joins existing MinHook chains at priority 100. The original event ABI and Render.cpp are unchanged. Native pass, Water, WaterNoSpec, ProcWater, Magma and terrain/WMO provider scopes identify context before a separate chained D3D9 DrawIndexedPrimitive observer captures final state. The existing grass predecessor remains in the draw chain; R6 never wraps SetVertexShader or SetPixelShader.

The DIP and optional float-constant setter wrappers are installed from the native EndScene event, after existing static subscribers such as grass. The prior DIP pointer and module/RVA are recorded. Every native draw and float-setter is forwarded once with its original arguments and HRESULT. Capture exceptions quarantine observation. Actual downstream hook ordering still requires live confirmation; this diagnostic never authorises replacement merely from its discovery label.

In full mode, successful VS/PS float writes **during native material scope** are marked. Writes outside that scope and integer/bool write calls are not traced. Final float/int/bool state is separately queried. Static shader reads and relative addressing require offline bytecode analysis. No constant/sampler is declared free.

The source comment mapping selector 2 to slime and 3 to magma is not used: verified shader-class dispatch is 2 Magma, 3 ProcWater. A material scope plus final shader pair plus provider identity is required for the strongest discovery classification. Unknown/fixed-function paths remain unknown and native.

## Capture v1

Output: `Logs/r6-water-<process>-<tick>.ndjson`. Every record contains schema=1, event, frame, generation, sequence, world_epoch, view and phase. Records are deduplicated and capped; dropped counts are explicit. Timing uses QueryPerformanceCounter and means CPU inspection/submission, never GPU time.

| Event | Payload / limits |
| --- | --- |
| session | Executable authority, effective mode/limits, unsupported-copy/replacement declarations; Classic target controls are labelled target metadata, not live Wrath CVar values |
| device | First-observed/reset device, prior draw-owner module/RVA, caps, MSAA and present parameters; no GPU targets allocated |
| boundary | Before/after one native liquid invocation, pass, instance count, RT0/depth descriptors, viewport, phase; ≤128 records process-wide |
| shader | Final observed VS/PS object, process-unique ID, SHA256, size/version and full-mode exact little-endian bytecode hex; at most 128 simultaneously retained shader objects; ≤64 KiB each |
| draw | Material/settings/provider/pass; shader SHA256s; declaration/FVF; topology/streams/indices; render states/hash; RT/depth/viewport; full-mode 16 pixel and 4 vertex texture/sampler stages and constant blocks |
| summary | Counts, failures, profile admissions, queue/output/drop counts and CPU ticks/frequency |
| lost/reset/world_leave/shutdown | Lifecycle evidence; invalidation and final pending/drop counts |

Profiles use class, provider, pass, VS/PS hashes, declaration, source-declared material-settings bytes and topology. Texture descriptor SHA256 is stable for that description, **not a texel hash or guaranteed unique resource ID**. Texture pointer tokens are session correlation only; they may be reused. Native target/texture/stream references from getters are released in the same inspection. Shader cache references are released on lost/world-leave/device-change. Reset invalidates profile keys but retains process-wide ceilings. Ordinary process shutdown closes CPU output without invoking D3D under loader lock. Hot DLL unloading is not supported by this core/hook architecture.

Float constants are raw little-endian bytes, including NaN/Inf bit patterns; they are never emitted as invalid JSON numeric literals. HRESULTs distinguish missing/unsupported/failed queries from proven empty state. Native camera/world/settings captures are source-declared raw bytes; readability does not prove coordinate or colour semantics.

Run `python tools/r6-water-check/audit_capture.py <capture.ndjson> --extract <new-directory>` after the home test. It checks schema envelope, shader hashes/IDs, material mapping, constant lengths, record order and ceilings, and extracts exact bytecode without overwriting different existing data. It makes no spare-register or visual-fidelity claim. Dropped shader records can make the audit fail; keep that failure as evidence of an incomplete capture.

## Safety / remaining live gates

The module makes no D3D render-state changes: no scene splitting, target binding, constants substitution, replacement draw or render-target allocation. Therefore it does not introduce a state-restoration transaction. A future copy/replacement must supply and test that transaction separately. Readback and GPU queries are not used.

Verify in one eventual home session: matching native appearance; material/provider coverage; final shader identities; constants written versus read; target/MSAA descriptors and pass order; reset recovery; grass/shadow/UI/AO preservation; off/timing performance on the unchanged machine. Test shallow lake/river, coast, waterline, optional waterfall/WMO/non-water examples and the Amberpine reference. Do not infer feature absence from a site producing no liquid draw records.

CI uses the existing Win32/MSVC workflow, retains R5 receiver checks, and adds portable policy/hash tests plus capture-integrity tests. Deploy only a hash-pinned candidate WarcraftXL.dll. The workflow-built proxy is not the accepted fixed proxy and must not be deployed. The accepted R5 WarcraftXL.dll is the rollback, not the older DIAG9 DLL.
