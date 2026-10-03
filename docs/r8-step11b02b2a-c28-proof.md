# R8 Step 11B-02B2A — read-only UDiffuse constant proof

Starting authority: `537790002f54ad54e84024fe2973f8bbcaff78e2`, tree
`3d4df55ff91ff34b5ec0f917b379c4b5c45c6a90`, repository `birdseyryan-ship-it/wxl-core`.
The seed SHA256 is `0ef53c3950670b6c22b3721504403ff4443a3ffc840f10a94f904de09f3c4563`.
This tranche implements offline instrumentation. It does not claim a live timing pass.

## Proven source and shader facts

- The supplied 11B-02A evidence has 35 valid direct collection observations; the supplied
  B1 qualification records three exact substitutions, 14 safe fallback identities,
  no build/structural errors and accepted rollback. These are existing evidence, not new tests.
- Both UDiffuse bytecode hashes were recomputed from the supplied corpus. The exact
  `MapObjUDiffuse_T1` slots are 31/41/51 (`b5bcd972...`) and 61/71/81 (`3eb44db2...`).
  The selected shaders normalize the transformed normal, calculate saturated `NdotL`,
  saturate the c10/c11 lighting term, then execute `mad r1.xyz, c28, r1, v2` and
  `add_sat o1.xyz, r1, c29`. No local-light constant set occurs in these two shaders.
- Fresh selected/raw/PS hashes, versions, family and exact slot pairs fence admission.
  Only Diffuse/Opaque, pix5 with vtx31/41/51 or pix7 with vtx61/71/81 are included.
  Local-light variants, Specular, pix15 and every mismatch stay unobserved/native.
- Exact PS fixtures do not reference or define c31. This establishes only that these
  four shader programs do not consume it; it does not establish global ownership.
- At the starting source, `src/offsets/engine/Shader.hpp` documents EffectBind
  (`0x00873060`) writing shader wrappers into the deferred GxState cache.
  `src/offsets/engine/Gx.hpp` documents the constant setter (`0x0069E970`, device vtable
  +0x118), VS cache `0x00C5EFE8`, PS cache `0x00C5DFE0` and dirty ranges. Constants are
  process-wide, value-deduplicated, then uploaded by the pre-draw flush.
  Consequently EffectBind's completion does not establish hardware-constant currency.
  This tranche uses those existing source authorities; it does not invent a new native offset.

## Timing contract and its limits

1. Enter a directly scoped WMO ExtRender/IntRender boundary. Every scope entry/exit
   invalidates pending attribution; nested scopes cannot leak an earlier observation.
2. Invalidate before **every** EffectBind. Let the full native/downstream bind chain run.
3. Read active collection wrappers and exact hashes after the bind. On an exact match,
   take `post_bind` D3D9 getters plus native cache/dirty-range snapshots.
4. Observe the later standard D3D9 draw dispatch (all four DP/DIP/UP signatures are
   covered). Require the same scope, device, generation, active collection and exact
   bound VS/PS bytecode hashes. A mismatch produces no attributed constant sample.
5. Take `pre_draw`; forward the original arguments exactly once; take `post_draw`;
   return the original HRESULT. Capture/logging exceptions quarantine observation
   without catching or retrying the native call.

This later seam is the narrowest **already specified, ABI-known** dispatch boundary
that follows native constant/shader flushing without acquiring the prohibited engine
draw seam. It uses no guessed native callsite. The capture can directly distinguish
stale post-bind device values, later cache changes, and stable constants at the draw.
The exact native c28 producer instruction and each internal upload call's order are
not traced by this diagnostic; the live paired observations determine the boundary
contract needed for B2B. The ordering of individual dirty uploads vs individual shader
binds inside the native flush remains unasserted and is not needed for this read seam.

The draw observer is an entry/return bracket around the **existing downstream D3D9
wrapper chain**, not a claim of being the last instruction before the driver.
The audit requires shader and constant stability across that bracket. This does not
exclude an unseen downstream temporary write-and-restore; any later B2B implementation
must audit that chain and its placement before claiming a safe mutation window.
Readback at post-bind matching a later draw does not prove post-bind is universally safe.

## Implementation isolation

`WXL_R8_WMO_C28_PROOF=1` requires structural master and WMO enabled, M2 and B1 perpixel
disabled. Conflicts refuse structural-hook installation. Absent/false means no proof
subscriptions, device taps or readbacks. B1's source and the M2 hook/install functions
are unchanged. No custom shader creation, GxState write, constant write or state mutation
is present. The only dispatch writes install original-preserving observation hooks.
Device reset, end of frame, a new bind, scope exit, device/collection mismatch and
hardware mismatch invalidate attribution. No shader pointer hash cache is used.

Capture is bounded to 64 identities including path, eight binds each, separated by
at least 60 EndScene events, and the first two draws per admitted bind. This is a
diagnostic run, not a performance benchmark. JSON records are individually bounded
below the existing 1024-byte logger limit. Failed getters have explicit HRESULTs and
null values; they cannot be mistaken for successful zero constants.

## Later live qualification — NOT performed by this run

Do not install this artifact until a separate guarded live step is requested. The CI
package must contain only WarcraftXL.dll, SHA256SUMS.txt and SOURCE_AUTHORITY.json.
Protected binaries and the server tree remain unchanged.

Use the complete saved OFF/ON launch strings in the recovery handoff. Both retain the
accepted graphics launch settings and keep `WXL_R8_WMO_PERPIXEL=0`, M2=0, selector=0.
The only OFF/ON difference is `WXL_R8_WMO_C28_PROOF=0/1`.

At minimum revisit the same Stormwind/Goldshire routes used for B1 long enough to collect
two admitted binds for each of Diffuse/Opaque × pix5/pix7. Preserve the entire OFF and
ON core logs, startup configuration, pre/post DLL/EXE/proxy hashes, candidate authority,
and rollback evidence. For every attributed draw retain all six constants in each of
post_bind/pre_draw/post_draw, their raw float bits, cache values, dirty ranges, exact
selected/raw/PS hashes, family, path, selectors, scope, frame, generation and HRESULT.
Capture a controlled reset in an additional run if practical; a reset recovery cannot
be inferred from the offline fake-device tests.

Run from this exact recovered source:

```sh
python tools/r8-step11-material-check/audit_c28.py --off wxl-core.OFF.log --on wxl-core.ON.log
```

Required: no proof events in OFF, no B1 substitutions, one valid proof startup in ON,
complete taps, all four previously seen routes repeated, correct hardware hashes,
successful/finite readbacks, VS cache/device equality at draw, unchanged constants and
shaders across each downstream draw, successful native HRESULTs and no quarantine.
Rejected unrelated draws remain native and are counted separately.

`READ_SIDE_PASS` qualifies sampled reads only. B2B still requires an explicitly designed
constant-write/save/restore transaction, downstream-chain audit, native cache coherence,
error/reset restoration and live A/B qualification. A matching post-bind c28 does not
authorize writing PS c31 there. Distant-outline diagnosis is outside this tranche.

## Reproduction and API references

Run `python -m unittest discover -s tools/r8-step11-material-check -p 'test_*.py' -v`.
Configure `tools/r8-step11-material-check` with CMake for Win32, build Release and run
CTest. C28CoreCheck tests pure classification/readback/failure forwarding;
C28RuntimeCheck compiles the actual production translation unit against fake COM
vtables/Win32 memory and exercises scopes, mismatches, resets, all four draw ABIs,
read failures, logging exceptions, original HRESULTs and partial hook installation.
The production WarcraftXL target is built separately with `CLIENT_PATH` empty.

Microsoft documents the getter signatures and vector counts:
[VS constants](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getvertexshaderconstantf),
[PS constants](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getpixelshaderconstantf),
[bound VS](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getvertexshader), and
[shader bytecode](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dvertexshader9-getfunction).
