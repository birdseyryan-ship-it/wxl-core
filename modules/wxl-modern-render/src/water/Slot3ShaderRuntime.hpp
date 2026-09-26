// R6 P01/A0/B1/Q0 Classic-water shader runtime. GPL-3.0-or-later.
//
// Resource creation only. This class does not bind shaders, alter a draw, or
// activate replacement. The final DIP integration remains separately gated by
// Slot3Core.hpp and the governing R6 slot-3 translation contract.
#pragma once

struct IDirect3DDevice9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;

namespace wxl::water::slot3
{
class ShaderRuntime
{
public:
    ShaderRuntime() = default;
    ~ShaderRuntime();

    ShaderRuntime(const ShaderRuntime&) = delete;
    ShaderRuntime& operator=(const ShaderRuntime&) = delete;

    // Compile/create both exact-equation SM3 translations for this device.
    // Fail closed: a partial pair is never reported ready.
    bool Ensure(IDirect3DDevice9* device) noexcept;

    // Release device-owned shader objects. Safe to call repeatedly and from
    // lost/reset shutdown paths.
    void Reset() noexcept;

    bool ReadyFor(IDirect3DDevice9* device) const noexcept;
    IDirect3DVertexShader9* Vertex() const noexcept { return vertex_; }
    IDirect3DPixelShader9* Pixel() const noexcept { return pixel_; }

private:
    IDirect3DDevice9* device_ = nullptr; // non-owning, matching renderer lifetime
    IDirect3DVertexShader9* vertex_ = nullptr;
    IDirect3DPixelShader9* pixel_ = nullptr;
};
} // namespace wxl::water::slot3
