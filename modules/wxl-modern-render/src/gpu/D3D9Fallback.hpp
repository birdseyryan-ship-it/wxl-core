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

    // Releases D3DPOOL_DEFAULT resources before a D3D9 device reset.
    void PrepareForReset();

    // Runs the Proton colour post-process.
    // Precedence: ProofTint -> SMAA -> FXAA.
    // FXAA/SMAA are normally mutually exclusive via the overlay.
    bool Frame(IDirect3DDevice9* device,
               bool fxaaEnabled,
               Quality fxaaQuality,
               bool smaaEnabled,
               Quality smaaQuality,
               IDirect3DSurface9* worldDepth);
}
