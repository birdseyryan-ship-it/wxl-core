// Canonical Wrath native adapter; qualification remains fail-closed.
#pragma once
#include "water/Slot3ProductionRuntime.hpp"
#include "water/Slot3NativeState.hpp"
#ifdef _WIN32
namespace wxl::water::slot3 {
class WrathNativeBridge final : public ProductionNativeBridge {
public:
    bool Initialize(bool canonicalIdentity, bool (*internalRender)()) noexcept;
    bool Qualified() const noexcept override;
    bool EntryPointsVerified() const noexcept { return entryPoints_; }
    const char* Blocker() const noexcept { return native_state::kBlocker; }
    bool Camera(ReflectionVec3& eye,ReflectionVec3& target) noexcept override;
    // This port implements the already-closed unsupported Classic VolumeFog
    // branch only; this does not assert that ordinary Wrath fog is disabled.
    bool VolumeFogDisabledOrUnsupported() const noexcept override { return true; }
    bool Capture(IDirect3DDevice9* device) noexcept override;
    bool BindMaterialsAndSamplers(IDirect3DDevice9*,const MaterialView&) noexcept override;
    bool Restore(IDirect3DDevice9*) noexcept override;
    void Reset() noexcept override;
    void NextFrame() noexcept { cameraReady_=false; }
    void ObserveTexture(IDirect3DDevice9*,unsigned,IDirect3DBaseTexture9*,HRESULT) noexcept;
    void ObserveCamera(const float* eye,const float* target) noexcept;
private:
    native_state::Snapshot state_{};
    IDirect3DDevice9* device_=nullptr;
    unsigned thread_=0;
    bool entryPoints_=false,cameraHook_=false,cameraReady_=false,binding_=false;
    bool (*internalRender_)()=nullptr;
    ReflectionVec3 eye_{},target_{};
    std::array<float,16> cameraView_{};
    struct Binding { IDirect3DBaseTexture9* texture=nullptr; HRESULT hr=E_FAIL; unsigned calls=0; };
    std::array<Binding,2> bindings_{};
};
}
#endif
