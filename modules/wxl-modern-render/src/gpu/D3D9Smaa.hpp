#pragma once

#include "gpu/Effect.hpp"

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;

namespace wxl::scripts::render_modern::d3d9smaa
{
    // Releases shaders, lookup tables and D3DPOOL_DEFAULT intermediates.
    void PrepareForReset();

    // SMAA 1x three-pass DX9 path:
    //   scene -> edge detection -> blend weights -> neighborhood blend -> dst
    bool Render(IDirect3DDevice9* device,
                IDirect3DTexture9* scene,
                IDirect3DSurface9* destination,
                unsigned width,
                unsigned height,
                Quality quality);
}
