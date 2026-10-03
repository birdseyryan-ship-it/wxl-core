// Explicit D3D9 transaction state; Gx CPU caches are a separate obligation.
// GPL-3.0-or-later.
#pragma once
#ifdef _WIN32
#include <d3d9.h>
#include <array>
#include <algorithm>

namespace wxl::water::slot3 {
class GpuState final {
public:
    GpuState() = default;
    GpuState(const GpuState&) = delete;
    GpuState& operator=(const GpuState&) = delete;
    ~GpuState() { Reset(); }
    bool Capture(IDirect3DDevice9* device) noexcept {
        Reset();
        D3DCAPS9 caps{};
        if (!device || FAILED(device->GetDeviceCaps(&caps)) ||
            caps.NumSimultaneousRTs == 0 || caps.NumSimultaneousRTs > rt_.size())
            return false;
        count_ = caps.NumSimultaneousRTs;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &block_)) || !block_ ||
            FAILED(block_->Capture())) { Reset(); return false; }
        for (unsigned i = 0; i < count_; ++i) {
            const HRESULT hr = device->GetRenderTarget(i, &rt_[i]);
            // D3DERR_NOTFOUND means an unbound optional target, not an API
            // failure. Other query failures cannot be treated as null state.
            if (FAILED(hr) && !(i != 0 && hr == D3DERR_NOTFOUND)) {
                Reset(); return false;
            }
        }
        const HRESULT depthHr = device->GetDepthStencilSurface(&depth_);
        if (!rt_[0] || (FAILED(depthHr) && depthHr != D3DERR_NOTFOUND) ||
            FAILED(device->GetViewport(&viewport_))) { Reset(); return false; }
        captured_ = true;
        return true;
    }
    bool Restore(IDirect3DDevice9* device) noexcept {
        if (!device || !captured_) return false;
        bool ok = true;
        // Remove incompatible secondary surfaces before switching RT0. ALL
        // does not include render targets/depth. Attempt every restoration
        // even if an earlier call fails, then quarantine at the caller.
        if (FAILED(device->SetDepthStencilSurface(nullptr))) ok = false;
        for (unsigned i = 1; i < count_; ++i)
            if (FAILED(device->SetRenderTarget(i, nullptr))) ok = false;
        if (FAILED(device->SetRenderTarget(0, rt_[0]))) ok = false;
        for (unsigned i = 1; i < count_; ++i)
            if (FAILED(device->SetRenderTarget(i, rt_[i]))) ok = false;
        if (FAILED(device->SetDepthStencilSurface(depth_))) ok = false;
        if (FAILED(block_->Apply())) ok = false;
        if (FAILED(device->SetViewport(&viewport_))) ok = false;
        return ok;
    }
    void Reset() noexcept {
        if (block_) { block_->Release(); block_ = nullptr; }
        if (depth_) { depth_->Release(); depth_ = nullptr; }
        for (auto*& rt : rt_) if (rt) { rt->Release(); rt = nullptr; }
        count_ = 0; captured_ = false; viewport_ = {};
    }
private:
    IDirect3DStateBlock9* block_ = nullptr;
    std::array<IDirect3DSurface9*, 4> rt_{};
    IDirect3DSurface9* depth_ = nullptr;
    D3DVIEWPORT9 viewport_{};
    unsigned count_ = 0;
    bool captured_ = false;
};
} // namespace wxl::water::slot3
#endif
