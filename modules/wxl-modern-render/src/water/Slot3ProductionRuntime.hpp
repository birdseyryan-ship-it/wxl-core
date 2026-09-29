// P01 production coordinator. GPL-3.0-or-later.
#pragma once
#include "water/Slot3ProductionPolicy.hpp"
#include "water/Slot3DepthRuntime.hpp"
#include "water/Slot3MaterialRuntime.hpp"
#include "water/Slot3ReflectionWin32.hpp"
#include "water/Slot3ShaderRuntime.hpp"

#ifdef _WIN32
#include <d3d9.h>
namespace wxl::water::slot3 {

// Exact-build adapter boundary. No implementation may claim Qualified until
// both deferred sampler realization and native CPU/hardware cache restoration
// have been established for the hash-pinned Wrath client. D3DSBT_ALL alone is
// insufficient. No runtime flag can bypass this gate.
class ProductionNativeBridge {
public:
    virtual ~ProductionNativeBridge() = default;
    virtual bool Qualified() const noexcept = 0;
    virtual void Reset() noexcept = 0;
    // Read-only: actual inputs to the already-closed native camera builder.
    virtual bool Camera(ReflectionVec3& eye, ReflectionVec3& target) noexcept = 0;
    virtual bool VolumeFogDisabledOrUnsupported() const noexcept = 0;
    // Capture is read-only and must be atomic on failure.
    virtual bool Capture(IDirect3DDevice9* device) noexcept = 0;
    // Canonical Gx setter 0x00685F50, states 0x1A/0x1C, plus a VERIFIED
    // realization step; no texture-object-to-COM pointer arithmetic.
    // This also owns the qualified native/Classic sampler-state bridge.
    virtual bool BindMaterialsAndSamplers(IDirect3DDevice9* device,
                                          const MaterialView& material) noexcept = 0;
    // Restore every CPU cache/geometry/constant mutation made by the reflected
    // native render or material binder, including partially failed operations.
    // The outer coordinator then restores the original D3D state. Restore is
    // repeatable: called before replacement binding and again on final exit;
    // it must retain the original capture until the next successful Capture.
    virtual bool Restore(IDirect3DDevice9* device) noexcept = 0;
};

using NativeDipFn = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                    INT, UINT, UINT, UINT, UINT);
struct ProductionDraw {
    IDirect3DDevice9* device = nullptr;
    NativeDipFn native = nullptr;
    D3DPRIMITIVETYPE type = D3DPT_FORCE_DWORD;
    INT base = 0;
    UINT min = 0, vertices = 0, start = 0, primitives = 0;
    waterdiag::ProfileKey profile{};
    const void* settings = nullptr;
    const void* world = nullptr;
    std::uint64_t invocation = 0;
    ProductionOrdinals ordinals{};
};
struct ProductionTrace {
    ProductionStage stage = ProductionStage::Disabled;
    std::uint32_t liquidTypeId = 0;
    float plane = 0;
    std::uint64_t colourOrdinal = 0, depthOrdinal = 0;
    std::uint64_t reflectionOrdinal = 0, consumerOrdinal = 0;
    bool resourcesReady = false;
};

class ProductionRuntime final {
public:
    ProductionRuntime() = default;
    ProductionRuntime(const ProductionRuntime&) = delete;
    ProductionRuntime& operator=(const ProductionRuntime&) = delete;
    void Configure(const Config& config) noexcept { config_ = config; }
    bool Requested() const noexcept { return config_.valid && config_.enabled; }
    bool Operational() const noexcept {
        return Requested() && bridge_ && bridge_->Qualified() && !quarantined_;
    }
    // A concrete adapter still needs every qualification prerequisite.
    void SetNativeBridge(ProductionNativeBridge* bridge) noexcept { bridge_ = bridge; }
    bool InternalRender() const noexcept { return internal_; }
    bool Quarantined() const noexcept { return quarantined_; }
    unsigned ReflectionMode() const noexcept { return config_.reflectionMode; }
    DrawResult Draw(const ProductionDraw& draw, ProductionTrace& trace) noexcept;
    void Reset() noexcept;
private:
    struct Transaction;
    bool EnsureZeroVolume(IDirect3DDevice9* device) noexcept;
    Config config_{};
    ProductionNativeBridge* bridge_ = nullptr; // non-owning, render-thread only
    ShaderRuntime shaders_{};
    MaterialRuntime materials_{};
    DepthRuntime depth_{};
    Win32ReflectionRuntime reflection_{};
    IDirect3DVolumeTexture9* zeroVolume_ = nullptr;
    IDirect3DDevice9* zeroDevice_ = nullptr;
    bool internal_ = false, reflectionActive_ = false, quarantined_ = false;
};
} // namespace wxl::water::slot3
#endif
