#include "water/Slot3WrathBridge.hpp"
#ifdef _WIN32
#include "client/CWorldScene/LiquidDiagnostics.hpp"
#include "offsets/engine/Camera.hpp"
#include "engine/hook/Hook.hpp"
#include <windows.h>
#include <cstring>

namespace wxl::water::slot3 {
namespace {
namespace camera=wxl::offsets::engine::camera;
struct Memory {
    bool Read(std::uint32_t address,void* output,std::size_t n) noexcept {
        return waterdiag::native::Read(reinterpret_cast<const void*>(address),output,n);
    }
    bool Writable(std::uint32_t address,std::size_t n) noexcept {
        if(!address || n>UINT32_MAX-address) return false;
        auto cursor=std::uintptr_t(address);const auto end=cursor+n;
        while(cursor<end) {
            MEMORY_BASIC_INFORMATION info{};
            if(!VirtualQuery(reinterpret_cast<const void*>(cursor),&info,sizeof(info)) ||
               info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
               !(info.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
            const auto next=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
            if(next<=cursor) return false;
            cursor=next;
        }
        return true;
    }
    bool Write(std::uint32_t address,const void* data,std::size_t n) noexcept {
        SIZE_T written=0;
        return Writable(address,n) && WriteProcessMemory(GetCurrentProcess(),
            reinterpret_cast<void*>(address),data,n,&written) && written==n;
    }
};
using CameraFn=void(__cdecl*)(const float*,const float*);
CameraFn originalCamera=nullptr;
WrathNativeBridge* cameraOwner=nullptr;
void __cdecl CameraObserver(const float* eye,const float* target) {
    // Call the native builder once. Observation cannot alter its inputs.
    originalCamera(eye,target);
    if(cameraOwner) cameraOwner->ObserveCamera(eye,target);
}
template<class T> bool Read(std::uintptr_t address,T& value) noexcept {
    return waterdiag::native::Read(reinterpret_cast<const void*>(address),&value,sizeof(value));
}
bool Entry(std::uint32_t address,const std::array<std::uint8_t,8>& expected) noexcept {
    std::array<std::uint8_t,8> actual{};
    return Read(address,actual) && actual==expected;
}
} // namespace

bool WrathNativeBridge::Initialize(bool identity,bool (*internal)()) noexcept {
    static_assert(sizeof(void*)==4,"Exact native bridge is Win32 only");
    internalRender_=internal;
    entryPoints_=identity &&
        Entry(native_state::kSet,{{0x55,0x8b,0xec,0x57,0x8b,0xf9,0x83,0xbf}}) &&
        Entry(native_state::kForceOne,{{0x55,0x8b,0xec,0x83,0xec,0x10,0x56,0x8b}}) &&
        Entry(native_state::kFlush,{{0x55,0x8b,0xec,0x83,0x7d,0x08,0x00,0x53}}) &&
        Entry(native_state::kSky,{{0x55,0x8b,0xec,0x83,0xec,0x38,0x83,0x3d}});
    // Do not install additional live hooks for an unqualified candidate.
    // Both missing contracts must be closed by evidence, never an env override.
    if(entryPoints_ && native_state::kScreenSamplerContractQualified &&
       native_state::kSecondaryStateContractQualified) {
        cameraOwner=this;
        cameraHook_=wxl::hook::Install("R6.MainCameraInputs",camera::kBuildCameraMatrices,
                                      &CameraObserver,&originalCamera,100);
    }
    return entryPoints_;
}
bool WrathNativeBridge::Qualified() const noexcept {
    return entryPoints_ && cameraHook_ && native_state::kScreenSamplerContractQualified &&
           native_state::kSecondaryStateContractQualified;
}
void WrathNativeBridge::Reset() noexcept {
    state_.Reset();device_=nullptr;thread_=0;binding_=cameraReady_=false;bindings_={};
}
void WrathNativeBridge::ObserveCamera(const float* eye,const float* target) noexcept {
    if(!internalRender_ || internalRender_()) return;
    ReflectionVec3 e{},t{};std::array<float,16> view{};
    cameraReady_=waterdiag::native::Read(eye,&e,sizeof(e)) &&
        waterdiag::native::Read(target,&t,sizeof(t)) && Read(camera::kView,view) && Finite(e) && Finite(t);
    if(cameraReady_) {eye_=e;target_=t;cameraView_=view;}
}
bool WrathNativeBridge::Camera(ReflectionVec3& eye,ReflectionVec3& target) noexcept {
    std::array<float,16> current{};
    if(!Qualified() || !cameraReady_ || !Read(camera::kView,current) || current!=cameraView_) return false;
    eye=eye_;target=target_;return true;
}
bool WrathNativeBridge::Capture(IDirect3DDevice9* d) noexcept {
    if(!Qualified() || !d) return false;
    std::uint32_t gx=0;Memory memory;
    if(!Read(native_state::kSingleton,gx) || !state_.Capture(memory,gx,reinterpret_cast<std::uint32_t>(d))) return false;
    device_=d;thread_=GetCurrentThreadId();return true;
}
bool WrathNativeBridge::Restore(IDirect3DDevice9* d) noexcept {
    binding_=false;
    if(d!=device_ || GetCurrentThreadId()!=thread_) return false;
    Memory memory;return state_.Restore(memory,reinterpret_cast<std::uint32_t>(d));
}
void WrathNativeBridge::ObserveTexture(IDirect3DDevice9* d,unsigned stage,
                                      IDirect3DBaseTexture9* texture,HRESULT hr) noexcept {
    if(!binding_ || d!=device_ || GetCurrentThreadId()!=thread_ || (stage!=5 && stage!=7)) return;
    auto& b=bindings_[stage==5?0:1];b.texture=texture;b.hr=hr;++b.calls;
}
bool WrathNativeBridge::BindMaterialsAndSamplers(IDirect3DDevice9* d,const MaterialView& material) noexcept {
    if(!Qualified() || !state_.Valid() || d!=device_ || GetCurrentThreadId()!=thread_) return false;
    struct Binder {
        WrathNativeBridge& owner; IDirect3DDevice9* device;
        std::uint32_t gx;
        bool Begin() noexcept { owner.bindings_={};owner.binding_=true;return true; }
        void Set(std::uint32_t state,std::uint32_t value) noexcept {
            reinterpret_cast<void(__thiscall*)(void*,unsigned,unsigned)>(native_state::kSet)
                (reinterpret_cast<void*>(gx),state,value);
        }
        void Force(unsigned state) noexcept {
            reinterpret_cast<void(__thiscall*)(void*,unsigned)>(native_state::kForceOne)(reinterpret_cast<void*>(gx),state);
        }
        void Flush() noexcept {
            reinterpret_cast<void(__thiscall*)(void*,int)>(native_state::kFlush)(reinterpret_cast<void*>(gx),0);
        }
        bool Verify(unsigned stage,std::uint32_t expectedNative) noexcept {
            const auto& b=owner.bindings_[stage==5?0:1];
            if(b.calls!=1 || FAILED(b.hr) || !b.texture) return false;
            IDirect3DBaseTexture9* actual=nullptr;
            const auto hr=device->GetTexture(stage,&actual);
            const bool same=SUCCEEDED(hr) && actual && actual==b.texture;
            if(actual) actual->Release();
            if(!same) return false;
            std::uint32_t app=0,value=0;
            if(!Read(gx+0x28F4,app) || !app || !Read(app+(0x15+stage)*24,value) || value!=expectedNative) return false;
            const D3DSAMPLERSTATETYPE types[]={D3DSAMP_MAGFILTER,D3DSAMP_MINFILTER,D3DSAMP_MIPFILTER,
                D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_MAXANISOTROPY};
            const unsigned groups[]={2,0x12,0x22,0x32,0x42,0x5A};
            for(unsigned i=0;i<6;++i) {
                DWORD physical=0;std::uint32_t cached=0;
                if(!Read(gx+0x3BC4+4*(groups[i]+stage),cached) ||
                   FAILED(device->GetSamplerState(stage,types[i],&physical)) || physical!=cached) return false;
            }
            return true;
        }
        bool Drained() noexcept {
            std::uint32_t count=1,app=0,hw=0;
            if(!Read(gx+0x28,count) || count || !Read(gx+0x28F4,app) || !Read(gx+0x2900,hw)) return false;
            for(unsigned i=0;i<native_state::kStates;++i) {
                native_state::AppState a{};std::array<std::uint32_t,4> h{};
                if(!Read(app+i*24,a) || !Read(hw+i*16,h) || a.dirty || a.value!=h) return false;
            }
            return true;
        }
    } binder{*this,d,state_.DeviceObject()};
    const bool materialOk=native_state::RealizeMaterials(binder,reinterpret_cast<std::uint32_t>(material.t5Gx),
                                                         reinterpret_cast<std::uint32_t>(material.t7Gx));
    binding_=false;
    // s0/s1/s6 are deliberately not assigned inferred point/linear/clamp values.
    // This method cannot be enabled until their exact contract is supplied.
    return materialOk && native_state::kScreenSamplerContractQualified;
}
} // namespace
#endif
