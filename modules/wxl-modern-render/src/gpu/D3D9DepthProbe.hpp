#pragma once

struct IDirect3DDevice9;

namespace wxl::scripts::render_modern::d3d9depthprobe
{
    // One-shot diagnostic of the live Proton D3D9 device's readable-depth
    // capabilities. This does not modify the render path.
    void ProbeOnce(IDirect3DDevice9* device);

    // Allows a fresh probe after a device reset/recreation.
    void Reset();
}
