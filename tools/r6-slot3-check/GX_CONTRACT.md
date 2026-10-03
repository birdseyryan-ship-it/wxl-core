# R6 canonical Wrath bridge — qualification boundary

Canonical executable SHA256: `57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da`.

The source contains a concrete adapter, but `Qualified()` remains FALSE. There is no environment override. This is source/build qualification work, not a live-test candidate. Raw exact-build evidence is preserved in the downloadable checkpoint, not published with proprietary binaries to this repository.

## Proven realization path

`0x00685F50` (`thiscall`, state/value, ret 8) changes deferred app state; `0x00685970` records dirty/undo state. `0x006859E0` forces one state dirty and complements its hardware mirror. `0x00685B50(false)` (ret 4) dispatches differences via D3D backend `0x006A4C30`; texture states `0x15..0x24` call `0x006A4900`, which calls physical SetTexture then native sampler-cache updater `0x006A3C40`. The real draw `0x006A3620` calls synchronization `0x006A5940` before physical DIP at `0x006A36CF`.

The adapter sets/forces native states 0x1A/0x1C, flushes without drawing, and verifies observed physical t5/t7 and six sampler values against the exact native backend cache. It never reads a COM pointer from a Gx texture object. Native void functions ignore HRESULT; there is no invented flush return status.

## Captured value layout and ownership

Application/HW arrays: 108 records of 24/16 bytes through Gx +0x28F4/+0x2900. App record +0x10 is stack tag; +0x14 is dirty. Capture requires drained dirty queue, no dirty records and matching app/HW values.

Undo, stack-offset and dirty growable headers are at +4/+0x14/+0x24 (capacity/count/data/grow DWORDs). Capture reads their live prefix only. Restore retains current allocation pointers/capacities/growth, restores original prefix/count, verifies every write, and continues best-effort restoration after individual write failures. Array counts, not their ownership, are rollback values. Limits of 4096 undo entries/64 stack entries are fail-closed adapter bounds, not claimed engine maxima.

Known value ranges are listed explicitly in `Slot3NativeState.hpp`. Constructor 0x00688690 and matrix constructor 0x00683B90 establish 0x118-byte matrix stacks with four INLINE matrices, level/dirty and four flag DWORDs. The six owning lists at +0x2668 are excluded; their headers and engine render-target references are read-only invariants. No whole-object memcpy or allocation ownership rollback occurs. D3D stream/declaration/index caches and state cache are non-owning values; physical COM lifetime/restoration remains the enclosing GpuState owner's obligation.

Known native constant banks/dirty bounds and existing Camera/Shader.hpp globals are included. These captures are tested as a bounded memory transaction, but are not represented as a complete proof of every secondary-render mutation.

## Still unqualified

1. Exact Classic 1.13.2.31650 screen sampler descriptors for s0 (colour), s1 (reflection) and s6 (linear depth): filtering, addressing, mip/LOD and sRGB state. Shader `dcl_sampler mode_default` and HLSL sampler declarations do not encode those states. The supplied source/handoff/supplemental checkpoint does not supply their concrete descriptors. Do not assign conventional point/linear/clamp values from inference or reuse unrelated stale Wrath slots. Required authority: the exact descriptor construction/bind evidence, or bounded read-only inspection of the already-known Classic executable SHA256 `2253e8d3449580a484e7fa403cab3c1a61f25a06ef593e15d8824fc34d490d40`. No playable Classic client is required.
2. Complete secondary-render CPU cache closure. The original reflection backend rebuilds camera/viewer/cull state using the supplied closed authority. The new capture covers grounded Gx value ranges, but completeness across the nested native sky/material/terrain/WMO renderer and shader-selection state has not been established. In particular, the native sky path calls 0x007ECF20 / 0x007F08C0 and nested shader selection; owning-list header equality alone does not establish unchanged node contents or all dependent caches. Do not flip the secondary-state qualification constant merely because these portable tests or a Windows build pass. Any further analysis must stay on these concrete restoration edges, not repeat reflection-producer selection.

The proven sky ABI error IS fixed: canonical caller 0x0079ACB7 pushes rectangle 0x00ADF570 for cdecl sky function 0x007F09B0; 0x0079ACC1 cleans four bytes. It is not the portal rectangle at 0x00ADF58C.

## Lifecycle and testing

The qualified production path performs copies before building log records, reuses CP09's exact colour/depth transfer bracket and resource owners, and ignores diagnostic copy ceilings. DIAG=0, record/byte caps and log-write failures do not gate production. Diagnostic-only copies retain their historical caps. Shader classification has a separate bounded bytecode read and does not depend on the diagnostic identity-cache ceiling. Frame freshness advances even if log output fails. All this remains unexercised in game while qualification is false.

R6NativeBridgeCheck injects capture and restoration failures, validates partial writes/ownership preservation/repeatability, canonical t5/t7 policy, failed observable realization, dual CPU/GPU restore outcomes, recursion, subsequent native-cache use, reset, exactly-once/failing replacement submission, quarantine and 20,000-frame copy policy. These are portable tests, not an in-game native ABI proof. Baseline 528/1043/190 suites are retained, as are capture auditing and frozen R5 receiver checks. The capture auditor intentionally rejects a claimed operational build until the missing contracts are closed.
