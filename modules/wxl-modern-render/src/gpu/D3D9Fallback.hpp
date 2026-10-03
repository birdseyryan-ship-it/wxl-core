#pragma once

#include "gpu/Effect.hpp"

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace wxl::scripts::render_modern::d3d9fallback
{
    // True under Wine / Proton. Native Windows keeps the D3D9On12/D3D12 path.
    bool Available();

    // Deliberately obvious diagnostic pass used only to prove that the fallback
    // owns and rewrites the completed world image before the WoW UI is drawn.
    bool ProofTint();
    void SetProofTint(bool enabled);

    // Classic Enhanced Proton AO is a real production effect from R3E4 onward.
    // It defaults ON and stacks before the selected anti-aliasing pass.
    bool AmbientOcclusion();
    void SetAmbientOcclusion(bool enabled);

    // Releases D3DPOOL_DEFAULT resources before a D3D9 device reset.
    void PrepareForReset();

    // Runs the Proton world post-process.
    // Diagnostic depth/AO proof modes take precedence over production FX.
    // Normal CE order is AO -> selected AA (SMAA or FXAA).
    bool Frame(IDirect3DDevice9* device,
               bool fxaaEnabled,
               Quality fxaaQuality,
               bool smaaEnabled,
               Quality smaaQuality,
               IDirect3DSurface9* worldDepth,
               const float* worldProjection);
}
