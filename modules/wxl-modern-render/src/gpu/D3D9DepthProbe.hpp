#pragma once

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace wxl::scripts::render_modern::d3d9depthprobe
{
    // Capability-only probe. Does not modify the frame.
    void ProbeOnce(IDirect3DDevice9* device);

    // Runs after the engine scene has been ended, while D3D9 copy operations
    // are legal. All destinations are private temporary resources; the bound
    // world depth buffer is read-only.
    void ProbeTransfersOnce(IDirect3DDevice9* device,
                            IDirect3DSurface9* boundDepth);

    void Reset();
}
