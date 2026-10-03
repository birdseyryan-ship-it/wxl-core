// R6 native-preserving liquid diagnostics. No replacement path; bounded colour snapshot proof only.
// GPL-3.0-or-later. All D3D inspection is confined to the native render thread.
#include "water/WaterDiagCore.hpp"
#include "water/Slot3SnapshotView.hpp"
#include "water/Slot3ProductionRuntime.hpp"
#include "water/Slot3WrathBridge.hpp"
#include "water/Slot3SnapshotPolicy.hpp"
#include "gpu/PerfProbe.hpp"
#include "gpu/GpuPerfRing.hpp"
#include <new>
#include "client/CWorldScene/LiquidDiagnostics.hpp"
#include "client/CWorldScene/RenderModernBridge.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Liquid.hpp"
#include "offsets/engine/Camera.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/events/Event.hpp"
#include "common/Log.hpp"
#include <windows.h>
#include <d3d9.h>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <deque>
#include <vector>
#include <cstdlib>
#include <utility>
#include <cmath>
#include <cstring>

namespace wxl::waterdiag {
namespace {
namespace ev=wxl::events;
namespace gxoff=wxl::offsets::engine::gx;
namespace liqoff=wxl::offsets::engine::liquid;
namespace camoff=wxl::offsets::engine::camera;
namespace perf=wxl::scripts::render_modern::perf;
constexpr size_t kQueueLimit=2*1024*1024,kFlushLimit=256*1024;
constexpr const char* kExeSha="57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da";
template<class T> struct Com {
    T* p=nullptr;~Com(){if(p)p->Release();} T** Out(){return &p;}
    Com()=default;Com(const Com&)=delete;Com& operator=(const Com&)=delete;
};
struct Json {
    std::string s="{";bool first=true;
    void Raw(const char* k,std::string v){if(!first)s+=',';first=false;s+=Quote(k)+':'+v;}
    void Str(const char* k,std::string_view v){Raw(k,Quote(v));}
    template<class T> void Num(const char* k,T v){Raw(k,std::to_string(v));}
    void Bool(const char* k,bool v){Raw(k,v?"true":"false");}
    std::string End(){return s+'}';}
};
std::string Pointer(const void* p){char b[24]{};std::snprintf(b,sizeof(b),"0x%08llx",static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));return b;}
std::string Owner(const void* address){
    HMODULE module=nullptr;Json j;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address),&module)){
        char path[MAX_PATH]{};DWORD n=GetModuleFileNameA(module,path,MAX_PATH);
        if(n&&n<MAX_PATH){const char* name=std::strrchr(path,'\\');j.Str("module",name?name+1:path);}
        j.Num("rva",reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module));
    }else j.Str("module","unresolved_or_trampoline");
    return j.End();
}
Config config;
namespace slot3 = wxl::water::slot3;
// Explicit event/reset ownership; do not destroy native/COM resources under
// the process loader lock. No object is allocated when replacement is off.
slot3::ProductionRuntime* production = nullptr;
slot3::WrathNativeBridge* nativeBridge = nullptr;
bool diagnosticRequested=false, diagnosticFailed=false;
unsigned productionSnapshotRecords=0;
uint64_t replacements = 0, replacementFailures = 0, restoreSuppressions = 0;
unsigned productionRecords = 0;
bool ProductionActive() noexcept { return production && production->Operational(); }
bool InternalRender() noexcept { return production && production->InternalRender(); }
void ResetProduction() noexcept { if (production) production->Reset(); }
IDirect3DDevice9* device=nullptr; // Borrowed only while native callbacks run; never AddRef the device.
DWORD renderThread=0;
bool ready=false,lost=false,quarantined=false,errorLogged=false;
uint64_t frame=0,generation=0,worldEpoch=0,view=0,sequence=0;
uint64_t observed=0,submitted=0,nativeFailures=0,diagTicks=0,submitTicks=0,captureErrors=0;
uint64_t familyCounts[5]{};
const char* phase="before_first_world_event";
LARGE_INTEGER frequency{};
ProfileLimiter profiles,boundaries;
unsigned boundaryRecords=0,nextShaderId=1;
Budget outputBudget;
std::deque<std::string> pending;
size_t pendingBytes=0;
std::ofstream sink;
std::string capturePath;
struct ShaderInfo {Digest hash{};unsigned id=0,major=0;bool valid=false;};
struct ShaderEntry {IUnknown* object=nullptr;ShaderInfo info;};
std::vector<ShaderEntry> shaders;
using DrawPrimitiveFn=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
using DipFn=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using DrawPrimitiveUpFn=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
using DipUpFn=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
using TextureFn=HRESULT (WINAPI*)(IDirect3DDevice9*,DWORD,IDirect3DBaseTexture9*);
using ConstantFn=HRESULT (WINAPI*)(IDirect3DDevice9*,UINT,const float*,UINT);
struct Chain {void** table=nullptr;DrawPrimitiveFn dp=nullptr;DipFn dip=nullptr;DrawPrimitiveUpFn dpup=nullptr;DipUpFn dipup=nullptr;ConstantFn vs=nullptr,ps=nullptr;TextureFn texture=nullptr;};
std::array<Chain,4> chains{};
thread_local bool inspecting=false;

// Temporary R6 lighting provenance probe.
//
// 0x008A38B0 receives the populated native transient-light record.
// The stock function transforms +0x78/+0x7C/+0x80 into native VS c33
// and copies +0x60/+0x64/+0x68 into native VS c35.
//
// This hook is diagnostic-only. It records input and resulting constants,
// then calls/returns through the native function unchanged.
constexpr uintptr_t kLightingConstantBuild = 0x008A38B0;
constexpr unsigned kLightingProbeRecordLimit = 64;

using LightingConstantBuildFn =
    void(__cdecl*)(void* record);

LightingConstantBuildFn
    lightingConstantBuildOriginal = nullptr;

uint64_t lightingProbeCalls = 0;
uint64_t lightingProbeRecords = 0;
uint64_t lightingProbeReadFailures = 0;

// Temporary Step2G P01 plane-source probe.
//
// It never changes GPU state and never replaces a draw.  It attempts
// read-only locks on the exact P01 native index/vertex buffers and inspects
// only POSITION0 of the vertices referenced by that DIP.
//
// Map-liquid vertex positions are already emitted in world coordinates by
// the native Wrath liquid geometry path.  Therefore an exact common Z across
// the actually referenced vertices is direct evidence of a horizontal
// world-space water plane for that draw.
//
// sphere/world comparisons below are diagnostics only.  Numerical equality
// does not promote either field into a production plane source.
constexpr unsigned kP01PlaneProbeRecordLimit = 12;
unsigned p01PlaneProbeRecords = 0;

// R6 snapshot/order-proof state. None of this exists unless the diagnostic is explicitly enabled.
IDirect3DTexture9* snapshotTexture=nullptr;
IDirect3DSurface9* snapshotSurface=nullptr;
D3DSURFACE_DESC snapshotDesc{};
// R6 depth-transport proof owns separate DEFAULT-pool resources. It reuses the
// frozen R3-proven two-stage transfer shape but not R3's resource or timing.
IDirect3DTexture9* snapshotDepthTexture=nullptr;
IDirect3DSurface9* snapshotDepthSurface=nullptr;
IDirect3DSurface9* snapshotDepthPlainSurface=nullptr;
D3DSURFACE_DESC snapshotDepthDesc{},snapshotDepthPlainDesc{};
constexpr D3DFORMAT kIntz=static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z'));
uint64_t worldSceneSerial=0,globalDrawOrdinal=0,sceneBeginOrdinal=0,waterCandidateOrdinal=0;
uint64_t firstLiquidOrdinal=0,lastLiquidOrdinal=0,liquidDrawsScene=0;
uint64_t postWaterNonLiquid=0,postWaterLikelyOpaque=0,postWaterUnclassified=0;
uint64_t drawApiCounts[4]{},drawApiFailures[4]{};
unsigned worldSceneDepth=0,postWaterProbed=0,postWaterRecords=0;
bool waterCandidateSeen=false,snapshotValid=false,depthSnapshotValid=false;
uint64_t snapshotSerial=0,snapshotFrame=0,snapshotProducerOrdinal=0;
uint64_t depthSnapshotSerial=0,depthSnapshotFrame=0,depthSnapshotProducerOrdinal=0;
uintptr_t snapshotSourceRtToken=0;
uint64_t snapshotAttempts=0,snapshotSuccesses=0,depthSnapshotSuccesses=0,snapshotGenerationAttempts=0;
bool snapshotCapReported=false,snapshotGenerationCapReported=false;
constexpr unsigned kSnapshotAttemptLimit=8,kSnapshotAttemptsPerGeneration=4,kPostWaterProbeLimit=128,kPostWaterRecordLimit=16;

Json Record(const char* event){Json j;j.Num("schema",1);j.Str("event",event);j.Num("frame",frame);j.Num("generation",generation);j.Num("sequence",++sequence);j.Num("world_epoch",worldEpoch);j.Num("view",view);j.Num("world_scene_serial",worldSceneSerial);j.Str("phase",phase);return j;}
bool Enqueue(Json j){if(diagnosticFailed||!sink.is_open())return false;auto line=j.End()+'\n';if(line.size()>kQueueLimit-pendingBytes){++outputBudget.dropped;return false;}
    if(!outputBudget.Take(line.size()))return false;pendingBytes+=line.size();pending.push_back(std::move(line));return true;}
void Flush(){if(diagnosticFailed||!sink.is_open())return;size_t n=0;while(!pending.empty()&&n+pending.front().size()<=kFlushLimit){auto& s=pending.front();sink.write(s.data(),std::streamsize(s.size()));n+=s.size();pendingBytes-=s.size();pending.pop_front();}
    sink.flush();if(!sink){diagnosticFailed=true;if(!errorLogged){WLOG_ERROR("r6-water: capture output failed; native rendering continues");errorLogged=true;}}}
void Failure() noexcept {diagnosticFailed=true;++captureErrors;if(!errorLogged){WLOG_ERROR("r6-water: logging disabled after capture failure; resource lifecycle remains independent");errorLogged=true;}}
bool CanInspect(IDirect3DDevice9* d) noexcept{return ready&&!lost&&!quarantined&&d==device&&GetCurrentThreadId()==renderThread;}
void ReleaseShaders() noexcept{for(auto& s:shaders)if(s.object)s.object->Release();shaders.clear();}
template<class T> void Drop(T*& p) noexcept{if(p){p->Release();p=nullptr;}}
void ReleaseDepthSnapshot() noexcept{
    Drop(snapshotDepthPlainSurface);Drop(snapshotDepthSurface);Drop(snapshotDepthTexture);
    snapshotDepthDesc={};snapshotDepthPlainDesc={};depthSnapshotValid=false;
    depthSnapshotSerial=0;depthSnapshotFrame=0;depthSnapshotProducerOrdinal=0;
}
void ReleaseSnapshot() noexcept{
    Drop(snapshotSurface);Drop(snapshotTexture);snapshotDesc={};snapshotValid=false;snapshotSerial=0;snapshotFrame=0;
    snapshotProducerOrdinal=0;snapshotSourceRtToken=0;ReleaseDepthSnapshot();
}
struct Probe {
    Com<IDirect3DSurface9> rt,ds;Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
    HRESULT rtHr=D3DERR_INVALIDCALL,dsHr=D3DERR_INVALIDCALL,vpHr=D3DERR_INVALIDCALL,vsHr=D3DERR_INVALIDCALL,psHr=D3DERR_INVALIDCALL;
    D3DVIEWPORT9 viewport{};
    std::array<HRESULT,4> stateHr{{D3DERR_INVALIDCALL,D3DERR_INVALIDCALL,D3DERR_INVALIDCALL,D3DERR_INVALIDCALL}};
    std::array<DWORD,4> state{};
};
void FillProbe(IDirect3DDevice9* d,Probe& p){
    p.rtHr=d->GetRenderTarget(0,p.rt.Out());p.dsHr=d->GetDepthStencilSurface(p.ds.Out());p.vpHr=d->GetViewport(&p.viewport);
    p.vsHr=d->GetVertexShader(p.vs.Out());p.psHr=d->GetPixelShader(p.ps.Out());
    static constexpr D3DRENDERSTATETYPE types[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ALPHABLENDENABLE,D3DRS_COLORWRITEENABLE};
    for(unsigned i=0;i<4;++i)p.stateHr[i]=d->GetRenderState(types[i],&p.state[i]);
}
bool SameViewport(const Probe& a,const Probe& b){return SUCCEEDED(a.vpHr)&&SUCCEEDED(b.vpHr)&&std::memcmp(&a.viewport,&b.viewport,sizeof(a.viewport))==0;}
bool SameProbe(const Probe& a,const Probe& b){
    if(a.rtHr!=b.rtHr||a.dsHr!=b.dsHr||a.vsHr!=b.vsHr||a.psHr!=b.psHr)return false;
    if(a.rt.p!=b.rt.p||a.ds.p!=b.ds.p||a.vs.p!=b.vs.p||a.ps.p!=b.ps.p)return false;
    if(!SameViewport(a,b))return false;
    for(unsigned i=0;i<4;++i)if(a.stateHr[i]!=b.stateHr[i]||(SUCCEEDED(a.stateHr[i])&&a.state[i]!=b.state[i]))return false;
    return true;
}
std::string Surface(IDirect3DSurface9* s,HRESULT query);
std::string ProbeJson(const Probe& p){Json j;j.Str("rt0_token",Pointer(p.rt.p));j.Str("depth_token",Pointer(p.ds.p));
    j.Raw("rt0",Surface(p.rt.p,p.rtHr));j.Raw("depth",Surface(p.ds.p,p.dsHr));j.Num("viewport_hr",int32_t(p.vpHr));
    if(SUCCEEDED(p.vpHr)){j.Num("x",p.viewport.X);j.Num("y",p.viewport.Y);j.Num("width",p.viewport.Width);j.Num("height",p.viewport.Height);j.Str("min_max_z_le_hex",Hex(&p.viewport.MinZ,8));}
    j.Str("vs_token",Pointer(p.vs.p));j.Str("ps_token",Pointer(p.ps.p));
    static constexpr const char* names[]={"zenable","zwrite","alpha_blend","color_write"};
    for(unsigned i=0;i<4;++i){j.Num((std::string(names[i])+"_hr").c_str(),int32_t(p.stateHr[i]));if(SUCCEEDED(p.stateHr[i]))j.Num(names[i],p.state[i]);}
    return j.End();
}
bool EnsureSnapshotTarget(IDirect3DDevice9* d,const D3DSURFACE_DESC& src,HRESULT& createHr,HRESULT& levelHr){
    createHr=S_OK;levelHr=S_OK;
    if(snapshotTexture&&snapshotSurface&&snapshotDesc.Width==src.Width&&snapshotDesc.Height==src.Height&&snapshotDesc.Format==src.Format&&snapshotDesc.MultiSampleType==D3DMULTISAMPLE_NONE)return true;
    ReleaseSnapshot();
    createHr=d->CreateTexture(src.Width,src.Height,1,D3DUSAGE_RENDERTARGET,src.Format,D3DPOOL_DEFAULT,&snapshotTexture,nullptr);
    if(FAILED(createHr)||!snapshotTexture){ReleaseSnapshot();return false;}
    levelHr=snapshotTexture->GetSurfaceLevel(0,&snapshotSurface);
    if(FAILED(levelHr)||!snapshotSurface){ReleaseSnapshot();return false;}
    HRESULT descHr=snapshotSurface->GetDesc(&snapshotDesc);
    if(FAILED(descHr)||snapshotDesc.Width!=src.Width||snapshotDesc.Height!=src.Height||snapshotDesc.Format!=src.Format||snapshotDesc.MultiSampleType!=D3DMULTISAMPLE_NONE){ReleaseSnapshot();return false;}
    return true;
}
struct DepthTargetStatus{
    HRESULT intzCreate=D3DERR_INVALIDCALL,intzLevel=D3DERR_INVALIDCALL,intzDesc=D3DERR_INVALIDCALL;
    HRESULT plainCreate=D3DERR_INVALIDCALL,plainDesc=D3DERR_INVALIDCALL;
};
bool EnsureSnapshotDepthTarget(IDirect3DDevice9* d,const D3DSURFACE_DESC& src,DepthTargetStatus& st){
    if(snapshotDepthTexture&&snapshotDepthSurface&&snapshotDepthPlainSurface&&
       snapshotDepthDesc.Width==src.Width&&snapshotDepthDesc.Height==src.Height&&
       snapshotDepthDesc.Format==kIntz&&snapshotDepthDesc.MultiSampleType==D3DMULTISAMPLE_NONE&&
       snapshotDepthPlainDesc.Width==src.Width&&snapshotDepthPlainDesc.Height==src.Height&&
       snapshotDepthPlainDesc.Format==src.Format&&snapshotDepthPlainDesc.MultiSampleType==D3DMULTISAMPLE_NONE){
        st.intzCreate=st.intzLevel=st.intzDesc=st.plainCreate=st.plainDesc=S_OK;return true;
    }
    ReleaseDepthSnapshot();
    st.intzCreate=d->CreateTexture(src.Width,src.Height,1,D3DUSAGE_DEPTHSTENCIL,kIntz,D3DPOOL_DEFAULT,&snapshotDepthTexture,nullptr);
    if(FAILED(st.intzCreate)||!snapshotDepthTexture){ReleaseDepthSnapshot();return false;}
    st.intzLevel=snapshotDepthTexture->GetSurfaceLevel(0,&snapshotDepthSurface);
    if(FAILED(st.intzLevel)||!snapshotDepthSurface){ReleaseDepthSnapshot();return false;}
    st.intzDesc=snapshotDepthSurface->GetDesc(&snapshotDepthDesc);
    if(FAILED(st.intzDesc)||snapshotDepthDesc.Width!=src.Width||snapshotDepthDesc.Height!=src.Height||
       snapshotDepthDesc.Format!=kIntz||snapshotDepthDesc.MultiSampleType!=D3DMULTISAMPLE_NONE){
        ReleaseDepthSnapshot();return false;
    }
    st.plainCreate=d->CreateDepthStencilSurface(src.Width,src.Height,src.Format,D3DMULTISAMPLE_NONE,0,FALSE,&snapshotDepthPlainSurface,nullptr);
    if(FAILED(st.plainCreate)||!snapshotDepthPlainSurface){ReleaseDepthSnapshot();return false;}
    st.plainDesc=snapshotDepthPlainSurface->GetDesc(&snapshotDepthPlainDesc);
    if(FAILED(st.plainDesc)||snapshotDepthPlainDesc.Width!=src.Width||snapshotDepthPlainDesc.Height!=src.Height||
       snapshotDepthPlainDesc.Format!=src.Format||snapshotDepthPlainDesc.MultiSampleType!=D3DMULTISAMPLE_NONE){
        ReleaseDepthSnapshot();return false;
    }
    return true;
}
std::string KnownShader(IUnknown* object){if(!object)return "null";for(const auto& s:shaders)if(s.object==object&&s.info.valid)return Hex(s.info.hash);return "UNKNOWN";}

std::string Surface(IDirect3DSurface9* s,HRESULT query){Json j;j.Num("get_hr",int32_t(query));j.Bool("bound",s!=nullptr);j.Str("object_token",Pointer(s));
    if(s){D3DSURFACE_DESC d{};HRESULT hr=s->GetDesc(&d);j.Num("desc_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("width",d.Width);j.Num("height",d.Height);j.Num("format",unsigned(d.Format));j.Num("usage",d.Usage);j.Num("pool",unsigned(d.Pool));j.Num("msaa",unsigned(d.MultiSampleType));j.Num("msaa_quality",d.MultiSampleQuality);}}
    return j.End();}
std::string Targets(IDirect3DDevice9* d){Json j;Com<IDirect3DSurface9> rt,ds;
    HRESULT r=d->GetRenderTarget(0,rt.Out()),z=d->GetDepthStencilSurface(ds.Out());j.Raw("rt0",Surface(rt.p,r));j.Raw("depth",Surface(ds.p,z));
    D3DVIEWPORT9 v{};HRESULT hr=d->GetViewport(&v);j.Num("viewport_hr",int32_t(hr));
    if(SUCCEEDED(hr)){j.Num("x",v.X);j.Num("y",v.Y);j.Num("width",v.Width);j.Num("height",v.Height);j.Str("min_max_z_le_hex",Hex(&v.MinZ,8));}
    j.Bool("copy_performed",false);j.Str("depth_sampleability","unproven; a bound surface is not a snapshot");return j.End();}

template<class Shader> ShaderInfo Identify(Shader* s,const char* stage){
    if(!s)return {};
    for(const auto& e:shaders)if(e.object==s)return e.info;
    if(shaders.size()>=128)return {};
    UINT bytes=0;HRESULT a=s->GetFunction(nullptr,&bytes);if(FAILED(a)||bytes<8||bytes>65536||bytes%4)return {};
    std::vector<uint8_t> code(bytes);UINT actual=bytes;HRESULT b=s->GetFunction(code.data(),&actual);
    if(FAILED(b)||actual!=bytes)return {};
    uint32_t version=0;std::memcpy(&version,code.data(),4);
    ShaderInfo i;i.hash=Sha256::Of(code.data(),code.size());i.id=nextShaderId++;i.major=(version>>8)&255;i.valid=true;
    s->AddRef();try{shaders.push_back({s,i});}catch(...){s->Release();throw;}
    auto j=Record("shader");j.Str("stage",stage);j.Num("id",i.id);j.Str("sha256",Hex(i.hash));j.Str("object_token",Pointer(s));
    j.Num("bytes",bytes);j.Num("version_token",version);j.Str("identity_origin","GetFunction at first observed liquid draw; not creation interception");
    if(config.mode==Mode::Full)j.Str("bytecode_le_hex",Hex(code.data(),code.size()));
    j.Str("register_read_occupancy","UNKNOWN until bytecode disassembly, including relative indexing");Enqueue(std::move(j));return i;
}
std::string RenderStates(IDirect3DDevice9* d){
    static constexpr D3DRENDERSTATETYPE types[]={D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_ZFUNC,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAREF,D3DRS_ALPHAFUNC,
        D3DRS_ALPHABLENDENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_BLENDOP,D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,D3DRS_BLENDOPALPHA,
        D3DRS_CULLMODE,D3DRS_COLORWRITEENABLE,D3DRS_SRGBWRITEENABLE,D3DRS_FOGENABLE,D3DRS_FOGCOLOR,D3DRS_FOGTABLEMODE,D3DRS_FOGVERTEXMODE,
        D3DRS_FOGSTART,D3DRS_FOGEND,D3DRS_FOGDENSITY,D3DRS_CLIPPLANEENABLE,D3DRS_SCISSORTESTENABLE,D3DRS_MULTISAMPLEANTIALIAS,D3DRS_MULTISAMPLEMASK,D3DRS_STENCILENABLE};
    std::string a="[";bool first=true;for(auto t:types){DWORD value=0;HRESULT hr=d->GetRenderState(t,&value);Json j;j.Num("state",unsigned(t));j.Num("hr",int32_t(hr));if(SUCCEEDED(hr))j.Num("value",value);if(!first)a+=',';first=false;a+=j.End();}return a+']';
}
std::string TextureDesc(IDirect3DBaseTexture9* t){Json j;j.Bool("bound",t!=nullptr);if(!t)return j.End();
    auto type=t->GetType();j.Num("type",unsigned(type));j.Num("levels",t->GetLevelCount());
    if(type==D3DRTYPE_TEXTURE||type==D3DRTYPE_CUBETEXTURE){D3DSURFACE_DESC d{};
        HRESULT hr=type==D3DRTYPE_TEXTURE?static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0,&d):static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0,&d);
        j.Num("desc_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("width",d.Width);j.Num("height",d.Height);j.Num("format",unsigned(d.Format));j.Num("usage",d.Usage);j.Num("pool",unsigned(d.Pool));}}
    else if(type==D3DRTYPE_VOLUMETEXTURE){D3DVOLUME_DESC d{};HRESULT hr=static_cast<IDirect3DVolumeTexture9*>(t)->GetLevelDesc(0,&d);j.Num("desc_hr",int32_t(hr));
        if(SUCCEEDED(hr)){j.Num("width",d.Width);j.Num("height",d.Height);j.Num("depth",d.Depth);j.Num("format",unsigned(d.Format));j.Num("usage",d.Usage);j.Num("pool",unsigned(d.Pool));}}
    return j.End();}
std::string Textures(IDirect3DDevice9* d){
    static constexpr D3DSAMPLERSTATETYPE types[]={D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_ADDRESSW,D3DSAMP_BORDERCOLOR,D3DSAMP_MAGFILTER,D3DSAMP_MINFILTER,D3DSAMP_MIPFILTER,D3DSAMP_MIPMAPLODBIAS,D3DSAMP_MAXMIPLEVEL,D3DSAMP_MAXANISOTROPY,D3DSAMP_SRGBTEXTURE,D3DSAMP_ELEMENTINDEX,D3DSAMP_DMAPOFFSET};
    std::string a="[";for(unsigned i=0;i<20;++i){DWORD stage=i<16?i:DWORD(D3DVERTEXTEXTURESAMPLER0+i-16);Com<IDirect3DBaseTexture9> t;
        HRESULT hr=d->GetTexture(stage,t.Out());Json j;j.Num("stage",stage);j.Num("get_hr",int32_t(hr));j.Str("object_token",Pointer(t.p));
        auto desc=TextureDesc(t.p);j.Raw("descriptor",desc);j.Str("descriptor_sha256",Hex(Sha256::Of(desc.data(),desc.size())));
        j.Str("identity_limit","descriptor signature is stable; pointer is session correlation only; texel/content hash unknown");
        std::string states="[";bool first=true;for(auto type:types){DWORD value=0;HRESULT q=d->GetSamplerState(stage,type,&value);Json s;s.Num("state",unsigned(type));s.Num("hr",int32_t(q));if(SUCCEEDED(q))s.Num("value",value);if(!first)states+=',';first=false;states+=s.End();}
        j.Raw("sampler_states",states+']');if(i)a+=',';a+=j.End();}return a+']';
}
std::string Streams(IDirect3DDevice9* d){D3DCAPS9 caps{};d->GetDeviceCaps(&caps);unsigned count=std::min(16u,unsigned(caps.MaxStreams));
    std::string a="[";for(unsigned i=0;i<count;++i){Com<IDirect3DVertexBuffer9> vb;UINT offset=0,stride=0,freq=0;HRESULT hr=d->GetStreamSource(i,vb.Out(),&offset,&stride);Json j;j.Num("stream",i);j.Num("hr",int32_t(hr));j.Str("object_token",Pointer(vb.p));
        if(SUCCEEDED(hr)){j.Num("offset",offset);j.Num("stride",stride);}HRESULT f=d->GetStreamSourceFreq(i,&freq);j.Num("frequency_hr",int32_t(f));if(SUCCEEDED(f))j.Num("frequency",freq);
        if(vb.p){D3DVERTEXBUFFER_DESC v{};HRESULT q=vb.p->GetDesc(&v);j.Num("desc_hr",int32_t(q));if(SUCCEEDED(q)){j.Num("bytes",v.Size);j.Num("usage",v.Usage);j.Num("pool",unsigned(v.Pool));}}
        if(i)a+=',';a+=j.End();}return a+']';}
void Constants(Json& j,IDirect3DDevice9* d,const native::Context& c,const ShaderInfo& ps){
    D3DCAPS9 caps{};HRESULT ch=d->GetDeviceCaps(&caps);unsigned vc=SUCCEEDED(ch)?std::min(256u,unsigned(caps.MaxVertexShaderConst)):0;
    unsigned pc=ps.valid?(ps.major>=3?224u:ps.major==2?32u:8u):0;
    std::array<float,256*4> vf{};std::array<float,224*4> pf{};
    HRESULT a=vc?d->GetVertexShaderConstantF(0,vf.data(),vc):D3DERR_INVALIDCALL;
    HRESULT b=pc?d->GetPixelShaderConstantF(0,pf.data(),pc):D3DERR_INVALIDCALL;
    Json o;o.Num("vs_float4_count",vc);o.Num("ps_float4_count",pc);o.Num("vs_float_hr",int32_t(a));o.Num("ps_float_hr",int32_t(b));
    if(SUCCEEDED(a))o.Str("vs_float_le_hex",Hex(vf.data(),vc*16));if(SUCCEEDED(b))o.Str("ps_float_le_hex",Hex(pf.data(),pc*16));
    std::array<int,64> vi{},pi{};std::array<BOOL,16> vb{},pb{};
    a=d->GetVertexShaderConstantI(0,vi.data(),16);b=d->GetPixelShaderConstantI(0,pi.data(),16);o.Num("vs_int_hr",int32_t(a));o.Num("ps_int_hr",int32_t(b));
    if(SUCCEEDED(a))o.Str("vs_int_le_hex",Hex(vi.data(),sizeof(vi)));if(SUCCEEDED(b))o.Str("ps_int_le_hex",Hex(pi.data(),sizeof(pi)));
    a=d->GetVertexShaderConstantB(0,vb.data(),16);b=d->GetPixelShaderConstantB(0,pb.data(),16);o.Num("vs_bool_hr",int32_t(a));o.Num("ps_bool_hr",int32_t(b));
    if(SUCCEEDED(a))o.Str("vs_bool_le_hex",Hex(vb.data(),sizeof(vb)));if(SUCCEEDED(b))o.Str("ps_bool_le_hex",Hex(pb.data(),sizeof(pb)));
    o.Str("vs_float_written_since_material_entry",Hex(c.vsWrites.data(),c.vsWrites.size()));o.Str("ps_float_written_since_material_entry",Hex(c.psWrites.data(),c.psWrites.size()));
    o.Num("vs_float_write_calls",c.vsWriteCalls);o.Num("ps_float_write_calls",c.psWriteCalls);o.Num("out_of_range_writes",c.invalidWriteRanges);
    o.Str("limits","Successful float API writes in material scope only; I/B write calls not traced; reads/relative addressing require bytecode analysis; no spare registers asserted");j.Raw("constants",o.End());
}
void NativeBytes(Json& j,const native::Context& c){
    uintptr_t vtable=0;bool ok=native::Read(c.material,&vtable,sizeof(vtable));j.Bool("material_vtable_read",ok);if(ok)j.Str("material_vtable",Pointer(reinterpret_cast<void*>(vtable)));
    float camera[3]{},world[16]{},sphere[4]{};uint8_t settings[0x60]{};
    ok=native::Read(c.camera,camera,sizeof(camera));j.Bool("camera_read",ok);if(ok)j.Str("camera_raw_le_hex",Hex(camera,sizeof(camera)));
    ok=native::Read(c.world,world,sizeof(world));j.Bool("world_read",ok);if(ok)j.Str("world_raw_le_hex",Hex(world,sizeof(world)));
    ok=native::Read(c.sphere,sphere,sizeof(sphere));j.Bool("sphere_read",ok);if(ok)j.Str("sphere_raw_le_hex",Hex(sphere,sizeof(sphere)));
    ok=c.settings&&native::Read(static_cast<const char*>(c.settings)+0x300,settings,sizeof(settings));j.Bool("settings_read",ok);if(ok)j.Str("settings_300_35f_hex",Hex(settings,sizeof(settings)));
    j.Str("native_bytes_limit","Source-declared fields, raw capture only; readable memory does not prove coordinate/colour semantics");
}
bool FloatBitsEqual(
    float a,
    float b) noexcept
{
    std::uint32_t aa = 0;
    std::uint32_t bb = 0;

    std::memcpy(
        &aa,
        &a,
        sizeof(aa));

    std::memcpy(
        &bb,
        &b,
        sizeof(bb));

    return aa == bb;
}

std::string P01PlaneProbe(
    IDirect3DDevice9* d,
    D3DPRIMITIVETYPE type,
    INT base,
    UINT min,
    UINT vertices,
    UINT start,
    UINT primitives,
    std::uint64_t drawOrdinal,
    const D3DVERTEXELEMENT9* elements,
    UINT elementCount,
    const native::Context& c)
{
    Json j;

    j.Num("draw_ordinal", drawOrdinal);
    j.Num("invocation", c.invocation);
    j.Num("primitive_type", unsigned(type));
    j.Num("base_vertex", base);
    j.Num("min_vertex", min);
    j.Num("vertices", vertices);
    j.Num("start_index", start);
    j.Num("primitives", primitives);

    j.Str(
        "authority",
        "diagnostic only: exact referenced native P01 POSITION0 world-space Z; "
        "sphere/world/camera comparisons do not promote semantics");

    if (!d ||
        type != D3DPT_TRIANGLESTRIP ||
        !vertices ||
        !primitives ||
        !elements ||
        !elementCount)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "unsupported_draw_shape");
        return j.End();
    }

    bool positionFound = false;
    WORD positionOffset = 0;
    BYTE positionType = D3DDECLTYPE_UNUSED;

    for (UINT i = 0;
         i < elementCount;
         ++i)
    {
        const auto& e =
            elements[i];

        if (e.Stream == 0xFF)
            break;

        if (e.Stream == 0 &&
            e.Usage == D3DDECLUSAGE_POSITION &&
            e.UsageIndex == 0)
        {
            positionFound = true;
            positionOffset = e.Offset;
            positionType = e.Type;
            break;
        }
    }

    j.Bool("position0_found", positionFound);
    j.Num("position0_offset", positionOffset);
    j.Num("position0_type", unsigned(positionType));

    if (!positionFound ||
        (positionType != D3DDECLTYPE_FLOAT3 &&
         positionType != D3DDECLTYPE_FLOAT4))
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "position0_not_float3_or_float4");
        return j.End();
    }

    Com<IDirect3DVertexBuffer9>
        vb;

    UINT streamOffset = 0;
    UINT stride = 0;

    const HRESULT streamHr =
        d->GetStreamSource(
            0,
            vb.Out(),
            &streamOffset,
            &stride);

    j.Num("stream0_hr", int32_t(streamHr));
    j.Str("vb_token", Pointer(vb.p));
    j.Num("stream0_offset", streamOffset);
    j.Num("stream0_stride", stride);

    Com<IDirect3DIndexBuffer9>
        ib;

    const HRESULT indicesHr =
        d->GetIndices(
            ib.Out());

    j.Num("indices_hr", int32_t(indicesHr));
    j.Str("ib_token", Pointer(ib.p));

    if (FAILED(streamHr) ||
        !vb.p ||
        FAILED(indicesHr) ||
        !ib.p ||
        stride < UINT(positionOffset) + 12u)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "native_stream_or_index_unavailable");
        return j.End();
    }

    D3DVERTEXBUFFER_DESC
        vbDesc{};

    D3DINDEXBUFFER_DESC
        ibDesc{};

    const HRESULT vbDescHr =
        vb.p->GetDesc(
            &vbDesc);

    const HRESULT ibDescHr =
        ib.p->GetDesc(
            &ibDesc);

    j.Num("vb_desc_hr", int32_t(vbDescHr));
    j.Num("ib_desc_hr", int32_t(ibDescHr));

    if (SUCCEEDED(vbDescHr))
    {
        j.Num("vb_bytes", vbDesc.Size);
        j.Num("vb_usage", vbDesc.Usage);
        j.Num("vb_pool", unsigned(vbDesc.Pool));
    }

    if (SUCCEEDED(ibDescHr))
    {
        j.Num("ib_bytes", ibDesc.Size);
        j.Num("ib_usage", ibDesc.Usage);
        j.Num("ib_pool", unsigned(ibDesc.Pool));
        j.Num("index_format", unsigned(ibDesc.Format));
    }

    if (FAILED(vbDescHr) ||
        FAILED(ibDescHr))
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "buffer_descriptor_unavailable");
        return j.End();
    }

    UINT indexSize = 0;

    if (ibDesc.Format == D3DFMT_INDEX16)
        indexSize = 2;
    else if (ibDesc.Format == D3DFMT_INDEX32)
        indexSize = 4;

    if (!indexSize)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "unsupported_index_format");
        return j.End();
    }

    const std::uint64_t indexCount =
        std::uint64_t(primitives) + 2u;

    j.Num("index_count_expected", indexCount);

    if (!indexCount ||
        indexCount > 131072u)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "index_count_cap");
        return j.End();
    }

    const std::uint64_t indexByteStart =
        std::uint64_t(start) *
        indexSize;

    const std::uint64_t indexByteCount =
        indexCount *
        indexSize;

    if (indexByteStart >
            ibDesc.Size ||
        indexByteCount >
            ibDesc.Size ||
        indexByteStart +
            indexByteCount >
            ibDesc.Size)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "index_range_out_of_bounds");
        return j.End();
    }

    void* lockedIndices = nullptr;

    const HRESULT ibLockHr =
        ib.p->Lock(
            UINT(indexByteStart),
            UINT(indexByteCount),
            &lockedIndices,
            D3DLOCK_READONLY);

    j.Num("ib_lock_readonly_hr", int32_t(ibLockHr));

    if (FAILED(ibLockHr) ||
        !lockedIndices)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "index_readonly_lock_failed");
        return j.End();
    }

    std::vector<std::uint64_t>
        actualIndices;

    actualIndices.reserve(
        size_t(indexCount));

    bool indexRangeValid = true;
    std::uint64_t minActual = 0;
    std::uint64_t maxActual = 0;
    bool haveActual = false;

    const std::uint64_t declaredEnd =
        std::uint64_t(min) +
        std::uint64_t(vertices);

    for (std::uint64_t i = 0;
         i < indexCount;
         ++i)
    {
        std::uint32_t index = 0;

        const auto* src =
            static_cast<const std::uint8_t*>(
                lockedIndices) +
            i * indexSize;

        if (indexSize == 2)
        {
            std::uint16_t value = 0;

            std::memcpy(
                &value,
                src,
                sizeof(value));

            index = value;
        }
        else
        {
            std::memcpy(
                &index,
                src,
                sizeof(index));
        }

        if (std::uint64_t(index) <
                std::uint64_t(min) ||
            std::uint64_t(index) >=
                declaredEnd)
        {
            indexRangeValid = false;
            break;
        }

        const std::int64_t actualSigned =
            std::int64_t(base) +
            std::int64_t(index);

        if (actualSigned < 0)
        {
            indexRangeValid = false;
            break;
        }

        const std::uint64_t actual =
            std::uint64_t(actualSigned);

        actualIndices.push_back(
            actual);

        if (!haveActual)
        {
            minActual = actual;
            maxActual = actual;
            haveActual = true;
        }
        else
        {
            if (actual < minActual)
                minActual = actual;

            if (actual > maxActual)
                maxActual = actual;
        }
    }

    const HRESULT ibUnlockHr =
        ib.p->Unlock();

    j.Num("ib_unlock_hr", int32_t(ibUnlockHr));
    j.Bool("index_range_valid", indexRangeValid);
    j.Num("referenced_index_count", actualIndices.size());

    if (!indexRangeValid ||
        !haveActual ||
        FAILED(ibUnlockHr))
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "referenced_index_validation_failed");
        return j.End();
    }

    j.Num("actual_vertex_min", minActual);
    j.Num("actual_vertex_max", maxActual);

    const std::uint64_t vertexByteStart =
        std::uint64_t(streamOffset) +
        minActual *
        std::uint64_t(stride);

    const std::uint64_t vertexSpanCount =
        maxActual -
        minActual +
        1u;

    const std::uint64_t vertexByteCount =
        vertexSpanCount *
        std::uint64_t(stride);

    if (vertexByteStart >
            vbDesc.Size ||
        vertexByteCount >
            vbDesc.Size ||
        vertexByteStart +
            vertexByteCount >
            vbDesc.Size ||
        vertexByteCount >
            (32u * 1024u * 1024u))
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "vertex_range_out_of_bounds_or_cap");
        return j.End();
    }

    void* lockedVertices = nullptr;

    const HRESULT vbLockHr =
        vb.p->Lock(
            UINT(vertexByteStart),
            UINT(vertexByteCount),
            &lockedVertices,
            D3DLOCK_READONLY);

    j.Num("vb_lock_readonly_hr", int32_t(vbLockHr));

    if (FAILED(vbLockHr) ||
        !lockedVertices)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "vertex_readonly_lock_failed");
        return j.End();
    }

    bool finite = true;
    bool exactFlat = true;
    bool havePosition = false;

    float firstPosition[3]{};
    float minZ = 0.0f;
    float maxZ = 0.0f;

    std::uint32_t firstZBits = 0;

    for (const std::uint64_t actual :
         actualIndices)
    {
        const std::uint64_t relative =
            actual -
            minActual;

        const auto* src =
            static_cast<const std::uint8_t*>(
                lockedVertices) +
            relative *
                std::uint64_t(stride) +
            positionOffset;

        float position[3]{};

        std::memcpy(
            position,
            src,
            sizeof(position));

        if (!std::isfinite(position[0]) ||
            !std::isfinite(position[1]) ||
            !std::isfinite(position[2]))
        {
            finite = false;
            break;
        }

        std::uint32_t zBits = 0;

        std::memcpy(
            &zBits,
            &position[2],
            sizeof(zBits));

        if (!havePosition)
        {
            std::memcpy(
                firstPosition,
                position,
                sizeof(firstPosition));

            minZ = position[2];
            maxZ = position[2];
            firstZBits = zBits;
            havePosition = true;
        }
        else
        {
            if (zBits != firstZBits)
                exactFlat = false;

            if (position[2] < minZ)
                minZ = position[2];

            if (position[2] > maxZ)
                maxZ = position[2];
        }
    }

    const HRESULT vbUnlockHr =
        vb.p->Unlock();

    j.Num("vb_unlock_hr", int32_t(vbUnlockHr));
    j.Bool("position_finite", finite);
    j.Bool("position_samples_present", havePosition);
    j.Bool("exact_horizontal_world_z", exactFlat && finite && havePosition);

    if (havePosition)
    {
        j.Str(
            "first_position_xyz_le_hex",
            Hex(
                firstPosition,
                sizeof(firstPosition)));

        j.Str(
            "candidate_plane_z_le_hex",
            Hex(
                &firstPosition[2],
                sizeof(float)));

        j.Num("candidate_plane_z", firstPosition[2]);
        j.Num("min_z", minZ);
        j.Num("max_z", maxZ);
        j.Num("z_span", maxZ - minZ);
    }

    if (FAILED(vbUnlockHr) ||
        !finite ||
        !havePosition)
    {
        j.Bool("probe_complete", false);
        j.Str("reason", "vertex_position_read_failed");
        return j.End();
    }

    float sphere[4]{};
    float world[16]{};
    float contextCamera[3]{};

    const bool sphereRead =
        c.sphere &&
        native::Read(
            c.sphere,
            sphere,
            sizeof(sphere));

    const bool worldRead =
        c.world &&
        native::Read(
            c.world,
            world,
            sizeof(world));

    const bool contextCameraRead =
        c.camera &&
        native::Read(
            c.camera,
            contextCamera,
            sizeof(contextCamera));

    j.Bool("sphere_read", sphereRead);
    j.Bool("world_read", worldRead);
    j.Bool("context_camera_read", contextCameraRead);

    if (sphereRead)
    {
        j.Str(
            "sphere_le_hex",
            Hex(
                sphere,
                sizeof(sphere)));

        j.Bool(
            "sphere_z_bit_exact_to_geometry_plane",
            exactFlat &&
            FloatBitsEqual(
                sphere[2],
                firstPosition[2]));
    }

    std::uint32_t worldPlaneMatchMask = 0;

    if (worldRead)
    {
        j.Str(
            "world_le_hex",
            Hex(
                world,
                sizeof(world)));

        if (exactFlat)
        {
            for (unsigned i = 0;
                 i < 16u;
                 ++i)
            {
                if (FloatBitsEqual(
                        world[i],
                        firstPosition[2]))
                {
                    worldPlaneMatchMask |=
                        (1u << i);
                }
            }
        }
    }

    j.Num(
        "world_plane_bit_exact_match_mask",
        worldPlaneMatchMask);

    auto getActiveCamera =
        reinterpret_cast<
            camoff::GetActiveCameraFn>(
                camoff::kGetActiveCamera);

    void* activeCamera =
        getActiveCamera
            ? getActiveCamera()
            : nullptr;

    j.Str(
        "active_camera_token",
        Pointer(activeCamera));

    float activePosition[3]{};
    float activeForward[3]{};
    float globalCameraPosition[3]{};

    const bool activePositionRead =
        activeCamera &&
        native::Read(
            static_cast<const char*>(
                activeCamera) +
                camoff::kCameraPosition,
            activePosition,
            sizeof(activePosition));

    const bool activeForwardRead =
        activeCamera &&
        native::Read(
            static_cast<const char*>(
                activeCamera) +
                camoff::kCameraForward,
            activeForward,
            sizeof(activeForward));

    const bool globalCameraRead =
        native::Read(
            reinterpret_cast<const void*>(
                camoff::kCameraPos),
            globalCameraPosition,
            sizeof(globalCameraPosition));

    j.Bool(
        "active_camera_position_read",
        activePositionRead);

    j.Bool(
        "active_camera_forward_read",
        activeForwardRead);

    j.Bool(
        "global_camera_position_read",
        globalCameraRead);

    if (activePositionRead)
    {
        j.Str(
            "active_camera_position_le_hex",
            Hex(
                activePosition,
                sizeof(activePosition)));
    }

    if (activeForwardRead)
    {
        j.Str(
            "active_camera_forward_le_hex",
            Hex(
                activeForward,
                sizeof(activeForward)));

        if (activePositionRead)
        {
            float target[3]{
                activePosition[0] +
                    activeForward[0],
                activePosition[1] +
                    activeForward[1],
                activePosition[2] +
                    activeForward[2]
            };

            j.Str(
                "active_camera_target_position_plus_forward_le_hex",
                Hex(
                    target,
                    sizeof(target)));
        }
    }

    if (globalCameraRead)
    {
        j.Str(
            "global_camera_position_le_hex",
            Hex(
                globalCameraPosition,
                sizeof(globalCameraPosition)));
    }

    j.Bool(
        "context_camera_bit_exact_to_active_position",
        contextCameraRead &&
        activePositionRead &&
        std::memcmp(
            contextCamera,
            activePosition,
            sizeof(contextCamera)) == 0);

    j.Bool(
        "context_camera_bit_exact_to_global_position",
        contextCameraRead &&
        globalCameraRead &&
        std::memcmp(
            contextCamera,
            globalCameraPosition,
            sizeof(contextCamera)) == 0);

    j.Bool(
        "active_position_bit_exact_to_global_position",
        activePositionRead &&
        globalCameraRead &&
        std::memcmp(
            activePosition,
            globalCameraPosition,
            sizeof(activePosition)) == 0);

    j.Bool("probe_complete", true);

    j.Str(
        "result",
        exactFlat
            ? "exact_flat_world_z_candidate"
            : "referenced_geometry_not_one_exact_z_plane");

    return j.End();
}

template<class Shader> ShaderInfo ProductionShader(Shader* shader) noexcept {
    if(!shader) return {};
    UINT bytes=0;
    if(FAILED(shader->GetFunction(nullptr,&bytes)) || bytes<8 || bytes>65536 || bytes%4) return {};
    std::array<std::uint8_t,65536> code{};UINT actual=bytes;
    if(FAILED(shader->GetFunction(code.data(),&actual)) || actual!=bytes) return {};
    ShaderInfo result{};result.hash=Sha256::Of(code.data(),bytes);result.valid=true;
    return result;
}

ProfileKey ProductionProfile(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,const native::Context& c) {
    Com<IDirect3DVertexShader9> vs; Com<IDirect3DPixelShader9> ps;
    const auto vi=SUCCEEDED(d->GetVertexShader(vs.Out()))?ProductionShader(vs.p):ShaderInfo{};
    const auto pi=SUCCEEDED(d->GetPixelShader(ps.Out()))?ProductionShader(ps.p):ShaderInfo{};
    ProfileKey key{};
    if(!vi.valid || !pi.valid) return key;
    Com<IDirect3DVertexDeclaration9> declaration;
    std::array<D3DVERTEXELEMENT9,65> elements{}; UINT count=UINT(elements.size());
    if(FAILED(d->GetVertexDeclaration(declaration.Out())) || !declaration.p ||
       FAILED(declaration.p->GetDeclaration(elements.data(),&count)) || count==0 || count>elements.size()) return key;
    for(UINT i=0;i<count && elements[i].Stream!=0xff;++i)
        if(elements[i].Stream!=0) return key;
    const auto encoded=Hex(elements.data(),count*sizeof(elements[0]));
    std::array<uint8_t,0x60> material{};
    if(!c.settings || !native::Read(static_cast<const char*>(c.settings)+0x300,material.data(),material.size())) return key;
    key.family=c.family;key.provider=c.provider;key.pass=c.pass;
    key.vs=vi.hash;key.ps=pi.hash;key.topology=unsigned(type);
    key.declaration=Sha256::Of(encoded.data(),encoded.size());
    key.material=Sha256::Of(material.data(),material.size());
    return key;
}

void Capture(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT min,UINT vertices,UINT start,UINT primitives,uint64_t drawOrdinal,const native::Context& c){
    Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;HRESULT vhr=d->GetVertexShader(vs.Out()),phr=d->GetPixelShader(ps.Out());
    auto vi=SUCCEEDED(vhr)?Identify(vs.p,"vs"):ShaderInfo{};
    auto pi=SUCCEEDED(phr)?Identify(ps.p,"ps"):ShaderInfo{};
    Com<IDirect3DVertexDeclaration9> decl;HRESULT dh=d->GetVertexDeclaration(decl.Out());std::array<D3DVERTEXELEMENT9,65> elements{};UINT n=65;
    HRESULT de=decl.p?decl.p->GetDeclaration(elements.data(),&n):D3DERR_INVALIDCALL;
    DWORD fvf=0;HRESULT fh=d->GetFVF(&fvf);std::string declaration;
    if(SUCCEEDED(de)&&n<=65)declaration=Hex(elements.data(),n*sizeof(elements[0]));else declaration="fvf:"+std::to_string(fvf)+":hr:"+std::to_string(int32_t(fh));
    ProfileKey key;key.family=c.family;key.provider=c.provider;key.pass=c.pass;key.vs=vi.hash;key.ps=pi.hash;key.declaration=Sha256::Of(declaration.data(),declaration.size());
    std::array<uint8_t,0x60> materialData{};
    if(c.settings&&native::Read(static_cast<const char*>(c.settings)+0x300,materialData.data(),materialData.size()))
        key.material=Sha256::Of(materialData.data(),materialData.size());
    key.topology=unsigned(type);
    if(!profiles.Admit(key,frame,config.samples,config.maxDraws))return;
    const auto futureProfile=AllowlistedReplacementProfile(key);
    Com<IDirect3DSurface9> eligibilityRt;const HRESULT eligibilityRtHr=d->GetRenderTarget(0,eligibilityRt.Out());
    const bool sameSnapshotRt=SUCCEEDED(eligibilityRtHr)&&snapshotSourceRtToken!=0&&reinterpret_cast<uintptr_t>(eligibilityRt.p)==snapshotSourceRtToken;
    const bool snapshotSameScene=snapshotSerial==worldSceneSerial&&snapshotSerial!=0;
    const bool snapshotSameFrame=snapshotFrame==frame&&snapshotFrame!=0;
    const bool producerBeforeConsumer=snapshotProducerOrdinal!=0&&snapshotProducerOrdinal<drawOrdinal;
    const bool futureTransportReady=ReplacementTransportReady(snapshotValid,snapshotSameScene,snapshotSameFrame,producerBeforeConsumer,sameSnapshotRt);
    auto j=Record("draw");j.Num("invocation",c.invocation);j.Num("scope_depth",c.depth);j.Num("pass",c.pass);j.Num("instances",c.instances);
    j.Str("material_class",Name(c.family));j.Num("selector_by_verified_class",Selector(c.family));j.Str("provider",Name(c.provider));
    j.Str("material_settings_sha256",Hex(key.material));
    j.Str("classification",Classification(true,c.family,c.provider,vi.valid,pi.valid));
    j.Str("future_replacement_profile",Name(futureProfile));j.Bool("future_replacement_profile_allowlisted",futureProfile!=ReplacementProfile::None);
    j.Bool("future_replacement_transport_ready",futureTransportReady);j.Bool("future_snapshot_same_scene",snapshotSameScene);j.Bool("future_snapshot_same_frame",snapshotSameFrame);
    j.Bool("future_snapshot_before_consumer",producerBeforeConsumer);j.Bool("future_snapshot_same_rt",sameSnapshotRt);
    j.Bool("replacement_allowed",false);
    j.Str("material_token",Pointer(c.material));j.Str("settings_token",Pointer(c.settings));j.Str("geometry_token",Pointer(c.geometry));j.Str("animation_token",Pointer(c.animation));
    j.Num("vs_get_hr",int32_t(vhr));j.Num("ps_get_hr",int32_t(phr));j.Num("vs_id",vi.id);j.Num("ps_id",pi.id);
    j.Str("vs_sha256",vi.valid?Hex(vi.hash):"UNKNOWN");j.Str("ps_sha256",pi.valid?Hex(pi.hash):"UNKNOWN");
    j.Num("declaration_get_hr",int32_t(dh));j.Num("declaration_data_hr",int32_t(de));j.Str("declaration",declaration);j.Str("declaration_sha256",Hex(key.declaration));
    j.Num("primitive_type",unsigned(type));j.Num("base_vertex",base);j.Num("min_vertex",min);j.Num("vertices",vertices);j.Num("start_index",start);j.Num("primitives",primitives);
    auto states=RenderStates(d);j.Raw("render_states",states);j.Str("render_state_sha256",Hex(Sha256::Of(states.data(),states.size())));
    j.Raw("targets",Targets(d));j.Raw("streams",Streams(d));Com<IDirect3DIndexBuffer9> ib;HRESULT ih=d->GetIndices(ib.Out());j.Num("indices_hr",int32_t(ih));j.Str("indices_token",Pointer(ib.p));
    if(ib.p){D3DINDEXBUFFER_DESC id{};HRESULT q=ib.p->GetDesc(&id);j.Num("index_desc_hr",int32_t(q));if(SUCCEEDED(q)){j.Num("index_format",unsigned(id.Format));j.Num("index_bytes",id.Size);}}
    if(config.mode==Mode::Full){
        j.Raw("textures",Textures(d));
        Constants(j,d,c,pi);
        NativeBytes(j,c);

        if(futureProfile==ReplacementProfile::P01 &&
           p01PlaneProbeRecords<kP01PlaneProbeRecordLimit)
        {
            ++p01PlaneProbeRecords;

            j.Raw(
                "p01_plane_probe",
                P01PlaneProbe(
                    d,
                    type,
                    base,
                    min,
                    vertices,
                    start,
                    primitives,
                    drawOrdinal,
                    elements.data(),
                    n,
                    c));
        }
    }
    j.Str("final_boundary","chained DIP before forwarding; no R6 state setters; downstream owners recorded at install");Enqueue(std::move(j));
}


void RecordPostWater(IDirect3DDevice9* d,const char* api,uint64_t ordinal){
    ++postWaterNonLiquid;
    if(postWaterProbed>=kPostWaterProbeLimit){++postWaterUnclassified;return;}
    ++postWaterProbed;
    Com<IDirect3DSurface9> rt;HRESULT rtHr=d->GetRenderTarget(0,rt.Out());
    DWORD z=0,zw=0,blend=0,color=0;
    HRESULT zHr=d->GetRenderState(D3DRS_ZENABLE,&z),zwHr=d->GetRenderState(D3DRS_ZWRITEENABLE,&zw),
            blendHr=d->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend),colorHr=d->GetRenderState(D3DRS_COLORWRITEENABLE,&color);
    const bool sameRt=SUCCEEDED(rtHr)&&reinterpret_cast<uintptr_t>(rt.p)==snapshotSourceRtToken&&snapshotSourceRtToken!=0;
    const bool classified=SUCCEEDED(zHr)&&SUCCEEDED(zwHr)&&SUCCEEDED(blendHr)&&SUCCEEDED(colorHr);
    const bool likelyOpaque=sameRt&&classified&&z!=0&&zw!=0&&blend==FALSE&&color!=0;
    if(likelyOpaque)++postWaterLikelyOpaque;
    if(postWaterRecords>=kPostWaterRecordLimit)return;
    ++postWaterRecords;
    Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
    HRESULT vsHr=d->GetVertexShader(vs.Out()),psHr=d->GetPixelShader(ps.Out());
    auto j=Record("post_water_draw");j.Num("ordinal",ordinal);j.Str("api",api);j.Str("rt0_token",Pointer(rt.p));j.Bool("same_snapshot_source_rt",sameRt);
    j.Num("rt0_hr",int32_t(rtHr));j.Num("zenable_hr",int32_t(zHr));j.Num("zwrite_hr",int32_t(zwHr));j.Num("alpha_blend_hr",int32_t(blendHr));j.Num("color_write_hr",int32_t(colorHr));
    if(SUCCEEDED(zHr))j.Num("zenable",z);if(SUCCEEDED(zwHr))j.Num("zwrite",zw);if(SUCCEEDED(blendHr))j.Num("alpha_blend",blend);if(SUCCEEDED(colorHr))j.Num("color_write",color);
    j.Bool("likely_late_opaque_write",likelyOpaque);j.Num("vs_hr",int32_t(vsHr));j.Num("ps_hr",int32_t(psHr));j.Str("vs_token",Pointer(vs.p));j.Str("ps_token",Pointer(ps.p));
    j.Str("vs_sha256_if_cached",KnownShader(vs.p));j.Str("ps_sha256_if_cached",KnownShader(ps.p));
    Enqueue(std::move(j));
}
Json SnapshotRecord(){auto j=Record("snapshot_attempt");j.Bool("replacement_allowed",false);return j;}
uint64_t TrackDraw(IDirect3DDevice9* d,bool liquidScope,unsigned apiIndex,const char* api){
    const uint64_t ordinal=++globalDrawOrdinal;if(apiIndex<4)++drawApiCounts[apiIndex];
    if(worldSceneDepth!=1||!CanInspect(d))return ordinal;
    if(liquidScope){++liquidDrawsScene;if(!firstLiquidOrdinal)firstLiquidOrdinal=ordinal;lastLiquidOrdinal=ordinal;}
    else if(waterCandidateSeen&&diagnosticRequested&&!diagnosticFailed)RecordPostWater(d,api,ordinal);
    return ordinal;
}
void CopyPreWaterAfterEnd(const Probe& before,HRESULT endHr,bool depth,
                         HRESULT& colour,HRESULT& depth1,HRESULT& depth2,HRESULT& begin) noexcept {
    if(SUCCEEDED(endHr)) {
        const auto colourStart=perf::CpuBegin();
        {
            perf::GpuScope gpuColour(
                perf::CpuRegion::WaterColourCopy,
                device);
            colour=device->StretchRect(before.rt.p,nullptr,snapshotSurface,nullptr,D3DTEXF_NONE);
        }
        perf::CpuEnd(perf::CpuRegion::WaterColourCopy,colourStart);

        if(depth) {
            const auto depth1Start=perf::CpuBegin();
            {
                perf::GpuScope gpuDepth1(
                    perf::CpuRegion::WaterDepthStage1,
                    device);
                depth1=device->StretchRect(before.ds.p,nullptr,snapshotDepthPlainSurface,nullptr,D3DTEXF_NONE);
            }
            perf::CpuEnd(perf::CpuRegion::WaterDepthStage1,depth1Start);

            if(SUCCEEDED(depth1)) {
                const auto depth2Start=perf::CpuBegin();
                {
                    perf::GpuScope gpuDepth2(
                        perf::CpuRegion::WaterDepthStage2,
                        device);
                    depth2=device->StretchRect(snapshotDepthPlainSurface,nullptr,snapshotDepthSurface,nullptr,D3DTEXF_NONE);
                }
                perf::CpuEnd(perf::CpuRegion::WaterDepthStage2,depth2Start);
            }
        }

        // Exactly one BeginScene after every successful direct-original EndScene.
        const auto beginStart=perf::CpuBegin();
        begin=device->BeginScene();
        perf::CpuEnd(perf::CpuRegion::WaterBeginScene,beginStart);
    }
}
// Continuous path performs all resource work before allocating any log text.
// It reuses the same target owners, transport bracket and freshness stamps.
void ProductionSnapshotAtWater() noexcept {
    perf::CpuScope snapshotPerf(perf::CpuRegion::WaterSnapshotTotal);
    waterCandidateSeen=true;snapshotValid=depthSnapshotValid=false;
    phase="first_water_material_begin";waterCandidateOrdinal=globalDrawOrdinal;
    if(liquidDrawsScene || !slot3::SnapshotAttemptPermitted(ProductionActive(),false,snapshotAttempts,snapshotGenerationAttempts)) return;
    Probe before;FillProbe(device,before);
    D3DSURFACE_DESC colour{},depth{};
    if(FAILED(before.rtHr)||!before.rt.p||FAILED(before.dsHr)||!before.ds.p||
       FAILED(before.rt.p->GetDesc(&colour))||FAILED(before.ds.p->GetDesc(&depth))) return;
    HRESULT create=S_OK,level=S_OK;DepthTargetStatus depthTarget{};
    if(!EnsureSnapshotTarget(device,colour,create,level) || snapshotSurface==before.rt.p ||
       snapshotDesc.Width!=colour.Width || snapshotDesc.Height!=colour.Height ||
       snapshotDesc.Format!=colour.Format || snapshotDesc.MultiSampleType!=D3DMULTISAMPLE_NONE ||
       !EnsureSnapshotDepthTarget(device,depth,depthTarget)) return;
    snapshotSourceRtToken=reinterpret_cast<std::uintptr_t>(before.rt.p);
    ++snapshotAttempts;++snapshotGenerationAttempts;

    const auto endStart=perf::CpuBegin();
    const HRESULT end=static_cast<HRESULT>(wxl::runtime::render::EndSceneForPostProcess(device));
    perf::CpuEnd(perf::CpuRegion::WaterEndScene,endStart);

    HRESULT copy=D3DERR_INVALIDCALL,depth1=D3DERR_INVALIDCALL,depth2=D3DERR_INVALIDCALL,begin=D3DERR_INVALIDCALL;
    {
        perf::GpuFrameScope gpuFrame(
            perf::GpuOwner::WaterSnapshot,
            SUCCEEDED(end) ? device : nullptr);
        CopyPreWaterAfterEnd(before,end,true,copy,depth1,depth2,begin);
    }
    Probe after;bool preserved=false;
    if(SUCCEEDED(begin)) {FillProbe(device,after);preserved=SameProbe(before,after);}
    snapshotValid=SUCCEEDED(end)&&SUCCEEDED(copy)&&SUCCEEDED(begin)&&preserved;
    depthSnapshotValid=snapshotValid&&SUCCEEDED(depth1)&&SUCCEEDED(depth2);
    if(snapshotValid) {snapshotSerial=worldSceneSerial;snapshotFrame=frame;snapshotProducerOrdinal=waterCandidateOrdinal;++snapshotSuccesses;}
    if(depthSnapshotValid) {depthSnapshotSerial=worldSceneSerial;depthSnapshotFrame=frame;depthSnapshotProducerOrdinal=waterCandidateOrdinal;++depthSnapshotSuccesses;}
    if(SUCCEEDED(end)&&FAILED(begin)) {quarantined=true;++captureErrors;}
    // Logging has its own cap. Copying continues after this cap and DIAG=0.
    if(!diagnosticFailed && sink.is_open() && productionSnapshotRecords<64) try {
        ++productionSnapshotRecords;auto j=Record("production_snapshot");
        j.Num("attempt_index",snapshotAttempts);j.Num("generation_attempt_index",snapshotGenerationAttempts);
        j.Num("producer_draw_ordinal",waterCandidateOrdinal);j.Num("end_scene_hr",int32_t(end));
        j.Num("stretch_rect_hr",int32_t(copy));j.Num("depth_stage1_hr",int32_t(depth1));
        j.Num("depth_stage2_hr",int32_t(depth2));j.Num("begin_scene_hr",int32_t(begin));
        j.Bool("state_preserved",preserved);j.Bool("snapshot_valid",snapshotValid);j.Bool("depth_snapshot_valid",depthSnapshotValid);
        Enqueue(std::move(j));
    } catch(...) {Failure();}
}
void SnapshotAtWater(const native::Context& c) noexcept{
    if(c.family!=Family::Water||InternalRender()||!ready||!device||!CanInspect(device)||worldSceneDepth!=1||waterCandidateSeen)return;
    if(ProductionActive()) {ProductionSnapshotAtWater();return;}
    if(!diagnosticRequested || diagnosticFailed) return;
    try{
        waterCandidateSeen=true;snapshotValid=false;depthSnapshotValid=false;phase="first_water_material_begin";
        const uint64_t candidateOrdinal=globalDrawOrdinal;waterCandidateOrdinal=candidateOrdinal;
        Probe before;FillProbe(device,before);
        D3DSURFACE_DESC srcDesc{},depthSrcDesc{};
        HRESULT srcDescHr=before.rt.p?before.rt.p->GetDesc(&srcDesc):D3DERR_INVALIDCALL;
        HRESULT depthSrcDescHr=before.ds.p?before.ds.p->GetDesc(&depthSrcDesc):D3DERR_INVALIDCALL;
        snapshotSourceRtToken=reinterpret_cast<uintptr_t>(before.rt.p);
        auto candidate=Record("water_candidate");candidate.Num("invocation",c.invocation);candidate.Num("pass",c.pass);candidate.Num("scope_depth",c.depth);
        candidate.Num("producer_draw_ordinal",candidateOrdinal);candidate.Num("scene_begin_draw_ordinal",sceneBeginOrdinal);candidate.Num("liquid_draws_before_candidate",liquidDrawsScene);
        candidate.Raw("state_before",ProbeJson(before));candidate.Bool("copy_requested",config.copyRequested);candidate.Bool("depth_copy_requested",config.depthCopyRequested);
        candidate.Num("snapshot_attempts_before",snapshotAttempts);Enqueue(std::move(candidate));

        if(!config.copyRequested)return;
        if(liquidDrawsScene!=0){auto j=SnapshotRecord();j.Bool("attempted",false);j.Str("reason","prior_liquid_draw_seen");j.Num("liquid_draws_before_candidate",liquidDrawsScene);Enqueue(std::move(j));return;}
        if(!ProductionActive()&&snapshotAttempts>=kSnapshotAttemptLimit){if(!snapshotCapReported){auto j=SnapshotRecord();j.Bool("attempted",false);j.Str("reason","process_attempt_cap_reached");j.Num("cap",kSnapshotAttemptLimit);Enqueue(std::move(j));snapshotCapReported=true;}return;}
        if(!ProductionActive()&&snapshotGenerationAttempts>=kSnapshotAttemptsPerGeneration){if(!snapshotGenerationCapReported){auto j=SnapshotRecord();j.Bool("attempted",false);j.Str("reason","generation_attempt_cap_reached");j.Num("cap",kSnapshotAttemptsPerGeneration);Enqueue(std::move(j));snapshotGenerationCapReported=true;}return;}
        if(FAILED(before.rtHr)||!before.rt.p||FAILED(srcDescHr)){auto j=SnapshotRecord();j.Bool("attempted",false);j.Str("reason","source_rt_unavailable");j.Num("rt_hr",int32_t(before.rtHr));j.Num("desc_hr",int32_t(srcDescHr));Enqueue(std::move(j));return;}

        HRESULT createHr=S_OK,levelHr=S_OK;const bool targetOk=EnsureSnapshotTarget(device,srcDesc,createHr,levelHr);
        // EnsureSnapshotTarget may release owned targets and clear stamps; restore this candidate token afterwards.
        snapshotSourceRtToken=reinterpret_cast<uintptr_t>(before.rt.p);
        const bool distinct=targetOk&&snapshotSurface!=before.rt.p;
        const bool compatible=targetOk&&snapshotDesc.Width==srcDesc.Width&&snapshotDesc.Height==srcDesc.Height&&snapshotDesc.Format==srcDesc.Format&&snapshotDesc.MultiSampleType==D3DMULTISAMPLE_NONE;
        if(!targetOk||!distinct||!compatible){auto j=SnapshotRecord();j.Bool("attempted",false);j.Str("reason","owned_target_unavailable_or_incompatible");j.Num("create_hr",int32_t(createHr));j.Num("surface_level_hr",int32_t(levelHr));j.Bool("distinct",distinct);j.Bool("compatible",compatible);j.Raw("source",Surface(before.rt.p,before.rtHr));j.Raw("destination",Surface(snapshotSurface,targetOk?S_OK:D3DERR_INVALIDCALL));Enqueue(std::move(j));return;}

        DepthTargetStatus depthTarget{};
        bool depthTargetOk=!config.depthCopyRequested;
        if(config.depthCopyRequested){
            depthTargetOk=SUCCEEDED(before.dsHr)&&before.ds.p&&SUCCEEDED(depthSrcDescHr)&&EnsureSnapshotDepthTarget(device,depthSrcDesc,depthTarget);
            if(!depthTargetOk){
                auto j=SnapshotRecord();j.Bool("attempted",false);j.Str("reason","depth_source_or_target_unavailable");
                j.Num("depth_get_hr",int32_t(before.dsHr));j.Num("depth_desc_hr",int32_t(depthSrcDescHr));
                j.Num("depth_intz_create_hr",int32_t(depthTarget.intzCreate));j.Num("depth_intz_level_hr",int32_t(depthTarget.intzLevel));
                j.Num("depth_intz_desc_hr",int32_t(depthTarget.intzDesc));j.Num("depth_plain_create_hr",int32_t(depthTarget.plainCreate));
                j.Num("depth_plain_desc_hr",int32_t(depthTarget.plainDesc));j.Raw("depth_source",Surface(before.ds.p,before.dsHr));
                Enqueue(std::move(j));return;
            }
        }

        ++snapshotAttempts;++snapshotGenerationAttempts;LARGE_INTEGER qa{},qb{};QueryPerformanceCounter(&qa);
        const HRESULT endHr=static_cast<HRESULT>(wxl::runtime::render::EndSceneForPostProcess(device));
        HRESULT stretchHr=D3DERR_INVALIDCALL,depthStage1Hr=D3DERR_INVALIDCALL,depthStage2Hr=D3DERR_INVALIDCALL,beginHr=D3DERR_INVALIDCALL;
        CopyPreWaterAfterEnd(before,endHr,config.depthCopyRequested,stretchHr,depthStage1Hr,depthStage2Hr,beginHr);
        QueryPerformanceCounter(&qb);
        Probe after;bool statePreserved=false;
        if(SUCCEEDED(beginHr)){FillProbe(device,after);statePreserved=SameProbe(before,after);}
        const bool success=SUCCEEDED(endHr)&&SUCCEEDED(stretchHr)&&SUCCEEDED(beginHr)&&statePreserved;
        const bool depthSuccess=config.depthCopyRequested&&success&&SUCCEEDED(depthStage1Hr)&&SUCCEEDED(depthStage2Hr);
        if(success){snapshotValid=true;snapshotSerial=worldSceneSerial;snapshotFrame=frame;snapshotProducerOrdinal=candidateOrdinal;++snapshotSuccesses;}
        else{snapshotValid=false;snapshotSerial=0;snapshotFrame=0;snapshotProducerOrdinal=0;}
        if(depthSuccess){depthSnapshotValid=true;depthSnapshotSerial=worldSceneSerial;depthSnapshotFrame=frame;depthSnapshotProducerOrdinal=candidateOrdinal;++depthSnapshotSuccesses;}
        else{depthSnapshotValid=false;depthSnapshotSerial=0;depthSnapshotFrame=0;depthSnapshotProducerOrdinal=0;}

        auto j=SnapshotRecord();j.Bool("attempted",true);j.Num("attempt_index",snapshotAttempts);j.Num("generation_attempt_index",snapshotGenerationAttempts);j.Num("producer_draw_ordinal",candidateOrdinal);
        j.Num("end_scene_hr",int32_t(endHr));j.Num("stretch_rect_hr",int32_t(stretchHr));j.Num("begin_scene_hr",int32_t(beginHr));j.Num("bracket_cpu_ticks",uint64_t(qb.QuadPart-qa.QuadPart));j.Num("qpc_frequency",frequency.QuadPart);
        j.Bool("source_destination_distinct",distinct);j.Bool("descriptor_compatible",compatible);j.Bool("state_preserved",statePreserved);j.Bool("snapshot_valid",success);
        j.Bool("depth_copy_requested",config.depthCopyRequested);j.Bool("depth_snapshot_valid",depthSuccess);
        j.Num("depth_stage1_hr",int32_t(depthStage1Hr));j.Num("depth_stage2_hr",int32_t(depthStage2Hr));
        j.Raw("source",Surface(before.rt.p,before.rtHr));j.Raw("destination",Surface(snapshotSurface,S_OK));
        j.Raw("depth_source",Surface(before.ds.p,before.dsHr));
        j.Raw("depth_plain_destination",Surface(snapshotDepthPlainSurface,config.depthCopyRequested?S_OK:D3DERR_INVALIDCALL));
        j.Raw("depth_intz_destination",Surface(snapshotDepthSurface,config.depthCopyRequested?S_OK:D3DERR_INVALIDCALL));
        j.Raw("state_before",ProbeJson(before));if(SUCCEEDED(beginHr))j.Raw("state_after",ProbeJson(after));
        j.Num("source_msaa",unsigned(srcDesc.MultiSampleType));j.Num("destination_msaa",unsigned(snapshotDesc.MultiSampleType));
        if(config.depthCopyRequested){j.Num("depth_source_msaa",unsigned(depthSrcDesc.MultiSampleType));j.Num("depth_plain_msaa",unsigned(snapshotDepthPlainDesc.MultiSampleType));j.Num("depth_intz_msaa",unsigned(snapshotDepthDesc.MultiSampleType));}
        Enqueue(std::move(j));
        if(FAILED(beginHr)&&SUCCEEDED(endHr)){
            quarantined=true;++captureErrors;if(!errorLogged){WLOG_ERROR("r6-water: snapshot BeginScene balance failed hr=0x%08lX; diagnostic quarantined",static_cast<unsigned long>(beginHr));errorLogged=true;}
        }
    }catch(...){Failure();}
}

Chain* Find(IDirect3DDevice9* d) noexcept{void** v=*reinterpret_cast<void***>(d);for(auto& c:chains)if(c.table==v)return &c;return nullptr;}
HRESULT WINAPI DrawPrimitive(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT start,UINT primitives){
    Chain* chain=Find(d);if(!chain||!chain->dp)return D3DERR_INVALIDCALL;
    if(!inspecting&&!InternalRender()&&CanInspect(d))TrackDraw(d,native::Current()!=nullptr,0,"DrawPrimitive");
    HRESULT hr=chain->dp(d,type,start,primitives);if(FAILED(hr))++drawApiFailures[0];return hr;
}
HRESULT WINAPI Dip(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT min,UINT vertices,UINT start,UINT primitives){
    Chain* chain=Find(d);if(!chain||!chain->dip)return D3DERR_INVALIDCALL; // Installed only on retained table entries.
    const auto* c=native::Current();bool eligible=c&&!inspecting&&!InternalRender()&&CanInspect(d);
    const uint64_t drawOrdinal=!inspecting&&!InternalRender()&&CanInspect(d)?TrackDraw(d,c!=nullptr,1,"DrawIndexedPrimitive"):0;
    LARGE_INTEGER a{},b{};
    ProfileKey productionProfile{}; bool productionProfileReady=false;
    // Capture exceptions cannot suppress or duplicate the original submission.
    return ObserveThenForward(eligible,[&]{
        ++observed;++familyCounts[unsigned(c->family)];QueryPerformanceCounter(&a);
        InspectionScope guard(inspecting);
        if(production && production->Requested()) {
            if(ProductionActive()) productionProfile=ProductionProfile(d,type,*c);
            productionProfileReady=true;
        }
        if(diagnosticRequested&&!diagnosticFailed&&config.mode!=Mode::Timing&&profiles.total<config.maxDraws&&outputBudget.used<outputBudget.limit)
            Capture(d,type,base,min,vertices,start,primitives,drawOrdinal,*c);
        QueryPerformanceCounter(&b);diagTicks+=uint64_t(b.QuadPart-a.QuadPart);
    },[&]{
    if(eligible&&config.mode==Mode::Timing)QueryPerformanceCounter(&a);
    HRESULT hr=D3DERR_INVALIDCALL;
    bool nativeSubmission=true;
    if(eligible && productionProfileReady && production && production->Requested()) {
        slot3::ProductionOrdinals ordinals{};
        if(slot3::ReserveProductionOrdinals(globalDrawOrdinal,ordinals)) {
            slot3::ProductionDraw draw{};
            draw.device=d;draw.native=chain->dip;draw.type=type;draw.base=base;
            draw.min=min;draw.vertices=vertices;draw.start=start;draw.primitives=primitives;
            draw.profile=productionProfile;draw.settings=c->settings;draw.world=c->world;
            draw.invocation=c->invocation;draw.ordinals=ordinals;
            slot3::ProductionTrace trace{};
            // This call OWNS submission. Never forward again after it returns.
            const auto result=production->Draw(draw,trace);
            hr=HRESULT(result.result);
            nativeSubmission=result.submission==slot3::Submission::Native;
            if(result.submission==slot3::Submission::Replacement) {++replacements;if(FAILED(hr))++replacementFailures;}
            if(result.submission==slot3::Submission::SuppressedAfterRestoreFailure) ++restoreSuppressions;
            // Logging is after the exactly-once decision; an exception here
            // cannot cause a native retry of submitted replacement geometry.
            if(productionRecords<64) try {
                ++productionRecords;auto j=Record("slot3_transaction");
                j.Str("stage",slot3::Name(trace.stage));j.Num("liquid_type_id",trace.liquidTypeId);
                j.Num("plane",trace.plane);j.Num("reflection_mode",production->ReflectionMode());
                j.Num("downsample_shift",slot3::kFirstCandidateDownsampleShift);
                j.Num("colour_ordinal",trace.colourOrdinal);j.Num("depth_ordinal",trace.depthOrdinal);
                j.Num("reflection_ordinal",trace.reflectionOrdinal);j.Num("consumer_ordinal",trace.consumerOrdinal);
                j.Num("submission",unsigned(result.submission));j.Bool("restore_ok",result.restoreOk);
                j.Num("hr",int32_t(hr));j.Bool("resources_ready",trace.resourcesReady);
                j.Bool("bridge_qualified",ProductionActive());Enqueue(std::move(j));
            } catch(...) {Failure();}
        } else hr=chain->dip(d,type,base,min,vertices,start,primitives);
    } else hr=chain->dip(d,type,base,min,vertices,start,primitives);
    if(FAILED(hr))++drawApiFailures[1];
    if(eligible){if(nativeSubmission){++submitted;if(FAILED(hr))++nativeFailures;}if(config.mode==Mode::Timing){QueryPerformanceCounter(&b);submitTicks+=uint64_t(b.QuadPart-a.QuadPart);}}
    return hr;
    },[]{Failure();});
}
HRESULT WINAPI DrawPrimitiveUp(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT primitives,const void* data,UINT stride){
    Chain* chain=Find(d);if(!chain||!chain->dpup)return D3DERR_INVALIDCALL;
    if(!inspecting&&!InternalRender()&&CanInspect(d))TrackDraw(d,native::Current()!=nullptr,2,"DrawPrimitiveUP");
    HRESULT hr=chain->dpup(d,type,primitives,data,stride);if(FAILED(hr))++drawApiFailures[2];return hr;
}
HRESULT WINAPI DipUp(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT min,UINT vertices,UINT primitives,const void* indices,D3DFORMAT fmt,const void* data,UINT stride){
    Chain* chain=Find(d);if(!chain||!chain->dipup)return D3DERR_INVALIDCALL;
    if(!inspecting&&!InternalRender()&&CanInspect(d))TrackDraw(d,native::Current()!=nullptr,3,"DrawIndexedPrimitiveUP");
    HRESULT hr=chain->dipup(d,type,min,vertices,primitives,indices,fmt,data,stride);if(FAILED(hr))++drawApiFailures[3];return hr;
}
HRESULT WINAPI VSFloat(IDirect3DDevice9* d,UINT first,const float* data,UINT count){auto* c=Find(d);if(!c||!c->vs)return D3DERR_INVALIDCALL;
    HRESULT hr=c->vs(d,first,data,count);if(SUCCEEDED(hr)&&native::Current()&&!InternalRender()&&CanInspect(d))native::FloatWrite(false,first,count);return hr;}
HRESULT WINAPI PSFloat(IDirect3DDevice9* d,UINT first,const float* data,UINT count){auto* c=Find(d);if(!c||!c->ps)return D3DERR_INVALIDCALL;
    HRESULT hr=c->ps(d,first,data,count);if(SUCCEEDED(hr)&&native::Current()&&!InternalRender()&&CanInspect(d))native::FloatWrite(true,first,count);return hr;}
HRESULT WINAPI SetTextureObserved(IDirect3DDevice9* d,DWORD stage,IDirect3DBaseTexture9* texture) {
    auto* c=Find(d);if(!c||!c->texture)return D3DERR_INVALIDCALL;
    const HRESULT hr=c->texture(d,stage,texture);
    if(nativeBridge)nativeBridge->ObserveTexture(d,stage,texture,hr);
    return hr;
}
bool Swap(void** slot,void* hook,void** original){if(*slot==hook)return true;DWORD p=0;if(!VirtualProtect(slot,sizeof(void*),PAGE_EXECUTE_READWRITE,&p))return false;
    *original=*slot;InterlockedExchangePointer(slot,hook);DWORD ignored=0;return VirtualProtect(slot,sizeof(void*),p,&ignored)!=FALSE;}
void Device(IDirect3DDevice9* d){
    if(!d)return;if(device==d)return;
    if(device){ResetProduction();ReleaseShaders();ReleaseSnapshot();}device=d;renderThread=GetCurrentThreadId();lost=false;++generation;snapshotGenerationAttempts=0;snapshotGenerationCapReported=false;
    void** v=*reinterpret_cast<void***>(d);Chain* c=Find(d);if(!c){for(auto& x:chains)if(!x.table){c=&x;break;}}
    if(!c){quarantined=true;return;}
    if(!c->table){c->table=v;
        bool ok=Swap(&v[gxoff::vt::kDrawPrimitive],reinterpret_cast<void*>(&DrawPrimitive),reinterpret_cast<void**>(&c->dp));
        ok &= Swap(&v[gxoff::vt::kDrawIndexedPrimitive],reinterpret_cast<void*>(&Dip),reinterpret_cast<void**>(&c->dip));
        ok &= Swap(&v[gxoff::vt::kDrawPrimitiveUP],reinterpret_cast<void*>(&DrawPrimitiveUp),reinterpret_cast<void**>(&c->dpup));
        ok &= Swap(&v[gxoff::vt::kDrawIndexedPrimitiveUP],reinterpret_cast<void*>(&DipUp),reinterpret_cast<void**>(&c->dipup));
        if(diagnosticRequested&&config.mode==Mode::Full){ok &= Swap(&v[gxoff::vt::kSetVertexShaderConstantF],reinterpret_cast<void*>(&VSFloat),reinterpret_cast<void**>(&c->vs));ok &= Swap(&v[gxoff::vt::kSetPixelShaderConstantF],reinterpret_cast<void*>(&PSFloat),reinterpret_cast<void**>(&c->ps));}
        if(ProductionActive()) ok &= Swap(&v[gxoff::vt::kSetTexture],reinterpret_cast<void*>(&SetTextureObserved),reinterpret_cast<void**>(&c->texture));
        if(!ok){quarantined=true;WLOG_ERROR("r6-water: device tap incomplete; observation disabled, installed wrappers retain original forwards");}}
    auto j=Record("device");j.Str("reason","first observed device or successful reset");j.Bool("dip_slot_is_r6",v[gxoff::vt::kDrawIndexedPrimitive]==reinterpret_cast<void*>(&Dip));j.Str("device_token",Pointer(d));j.Str("previous_dip",Pointer(reinterpret_cast<void*>(c->dip)));j.Raw("previous_dip_owner",Owner(reinterpret_cast<void*>(c->dip)));
    j.Str("previous_draw_primitive",Pointer(reinterpret_cast<void*>(c->dp)));j.Str("previous_draw_primitive_up",Pointer(reinterpret_cast<void*>(c->dpup)));j.Str("previous_dip_up",Pointer(reinterpret_cast<void*>(c->dipup)));
    j.Str("previous_vs_float",Pointer(reinterpret_cast<void*>(c->vs)));j.Str("previous_ps_float",Pointer(reinterpret_cast<void*>(c->ps)));j.Bool("quarantined",quarantined);
    D3DCAPS9 caps{};HRESULT hr=d->GetDeviceCaps(&caps);j.Num("caps_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("vertex_shader_version",caps.VertexShaderVersion);j.Num("pixel_shader_version",caps.PixelShaderVersion);j.Num("max_vs_float4",caps.MaxVertexShaderConst);j.Num("max_streams",caps.MaxStreams);j.Num("max_textures",caps.MaxSimultaneousTextures);}
    D3DDEVICE_CREATION_PARAMETERS creation{};hr=d->GetCreationParameters(&creation);j.Num("creation_parameters_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("behavior_flags",creation.BehaviorFlags);j.Num("adapter",creation.AdapterOrdinal);}
    Com<IDirect3DSwapChain9> sc;hr=d->GetSwapChain(0,sc.Out());if(SUCCEEDED(hr)&&sc.p){D3DPRESENT_PARAMETERS pp{};hr=sc.p->GetPresentParameters(&pp);j.Num("present_parameters_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("backbuffer_width",pp.BackBufferWidth);j.Num("backbuffer_height",pp.BackBufferHeight);j.Num("msaa",unsigned(pp.MultiSampleType));j.Num("msaa_quality",pp.MultiSampleQuality);j.Bool("windowed",pp.Windowed!=FALSE);}}
    j.Bool("owned_render_targets",config.copyRequested);j.Bool("real_scene_copy_supported",true);Enqueue(std::move(j));
    WLOG_INFO("r6-water: draw-order tap active generation=%llu mode=%u copy=%u; native draws retained",static_cast<unsigned long long>(generation),unsigned(config.mode),config.copyRequested?1u:0u);
}
void Boundary(bool begin,const native::Context& c) noexcept {if(!diagnosticRequested||diagnosticFailed||!CanInspect(device)||config.mode==Mode::Timing||boundaryRecords>=128)return;
    try{auto targets=Targets(device);ProfileKey k;k.pass=c.pass;k.vs=Sha256::Of(targets.data(),targets.size());std::string discriminator=std::to_string(worldEpoch)+':'+(begin?"begin":"end");k.ps=Sha256::Of(discriminator.data(),discriminator.size());
        if(!boundaries.Admit(k,frame,4,128))return;
        auto j=Record("boundary");j.Str("point",begin?"liquid_begin":"liquid_end");j.Num("invocation",c.invocation);j.Num("pass",c.pass);j.Num("instances",c.instances);j.Num("scope_depth",c.depth);j.Raw("targets",targets);
        j.Str("ordering_limit","Before/after this native queue only; opaque completion and prior liquid writes unresolved");Enqueue(std::move(j));++boundaryRecords;
    }catch(...){Failure();}}
void __cdecl LightingConstantBuildProbe(void* record)
{
    const native::Context* c =
        native::Current();

    const bool scoped =
        ready &&
        !lost &&
        !quarantined &&
        config.mode == Mode::Full &&
        c &&
        c->family == Family::Water &&
        c->pass == 1 &&
        record != nullptr;

    if (scoped)
        ++lightingProbeCalls;

    const bool capture =
        scoped &&
        lightingProbeRecords <
            kLightingProbeRecordLimit;

    std::array<float, 12> raw54To80{};
    bool rawRead = false;

    if (capture)
    {
        rawRead =
            native::Read(
                static_cast<const char*>(record) + 0x54,
                raw54To80.data(),
                sizeof(raw54To80));
    }

    if (!lightingConstantBuildOriginal)
    {
        Failure();
        return;
    }

    lightingConstantBuildOriginal(record);

    if (!capture)
        return;

    std::array<float, 16> nativeC33ToC36{};

    const bool nativeRowsRead =
        native::Read(
            reinterpret_cast<const void*>(
                liqoff::kVsConstBlock +
                33u * 16u),
            nativeC33ToC36.data(),
            sizeof(nativeC33ToC36));

    try
    {
        auto j =
            Record("lighting_probe");

        j.Num("invocation", c->invocation);
        j.Num("pass", c->pass);
        j.Num("scope_depth", c->depth);

        j.Str(
            "material_class",
            Name(c->family));

        j.Str(
            "provider",
            Name(c->provider));

        j.Str(
            "material_token",
            Pointer(c->material));

        j.Str(
            "settings_token",
            Pointer(c->settings));

        j.Str(
            "record_token",
            Pointer(record));

        j.Bool(
            "raw_54_80_read",
            rawRead);

        if (rawRead)
        {
            j.Str(
                "raw_54_80_le_hex",
                Hex(
                    raw54To80.data(),
                    sizeof(raw54To80)));

            j.Num("raw_ambient_x", raw54To80[0]);
            j.Num("raw_ambient_y", raw54To80[1]);
            j.Num("raw_ambient_z", raw54To80[2]);

            j.Num("raw_diffuse_x", raw54To80[3]);
            j.Num("raw_diffuse_y", raw54To80[4]);
            j.Num("raw_diffuse_z", raw54To80[5]);

            j.Num("raw_specular_x", raw54To80[6]);
            j.Num("raw_specular_y", raw54To80[7]);
            j.Num("raw_specular_z", raw54To80[8]);

            j.Num("raw_direction_x", raw54To80[9]);
            j.Num("raw_direction_y", raw54To80[10]);
            j.Num("raw_direction_z", raw54To80[11]);
        }

        j.Bool(
            "native_c33_c36_read",
            nativeRowsRead);

        if (nativeRowsRead)
        {
            j.Str(
                "native_c33_c36_le_hex",
                Hex(
                    nativeC33ToC36.data(),
                    sizeof(nativeC33ToC36)));

            j.Num("native_c33_x", nativeC33ToC36[0]);
            j.Num("native_c33_y", nativeC33ToC36[1]);
            j.Num("native_c33_z", nativeC33ToC36[2]);
            j.Num("native_c33_w", nativeC33ToC36[3]);

            j.Num("native_c35_x", nativeC33ToC36[8]);
            j.Num("native_c35_y", nativeC33ToC36[9]);
            j.Num("native_c35_z", nativeC33ToC36[10]);
            j.Num("native_c35_w", nativeC33ToC36[11]);
        }

        j.Str(
            "contract",
            "same-call raw +0x54..+0x80 before "
            "0x008A38B0 and native c33..c36 after; "
            "join to P01 by liquid invocation");

        Enqueue(std::move(j));

        ++lightingProbeRecords;

        if (!rawRead ||
            !nativeRowsRead)
        {
            ++lightingProbeReadFailures;
        }
    }
    catch (...)
    {
        Failure();
    }
}

void Summary(){auto j=Record("summary");j.Num("slot3_replacements",replacements);j.Num("slot3_replacement_failures",replacementFailures);j.Num("slot3_restore_suppressions",restoreSuppressions);j.Num("liquid_dips_observed",observed);j.Num("native_dips_forwarded",submitted);j.Num("native_failures",nativeFailures);
    j.Num("profiles",profiles.size);j.Num("draw_records_admitted",profiles.total);j.Num("shader_objects_held",shaders.size());j.Num("output_bytes_admitted",outputBudget.used);j.Num("dropped_records",outputBudget.dropped);
    j.Num("pending_bytes",pendingBytes);j.Num("diagnostic_cpu_ticks",diagTicks);j.Num("native_submission_cpu_ticks",submitTicks);j.Num("qpc_frequency",frequency.QuadPart);j.Num("capture_errors",captureErrors);
    j.Num("unknown_draws",familyCounts[0]);j.Num("water_draws",familyCounts[1]);j.Num("nospec_draws",familyCounts[2]);j.Num("procwater_draws",familyCounts[3]);j.Num("magma_draws",familyCounts[4]);
    j.Num("draw_primitive_calls",drawApiCounts[0]);j.Num("draw_indexed_primitive_calls",drawApiCounts[1]);j.Num("draw_primitive_up_calls",drawApiCounts[2]);j.Num("draw_indexed_primitive_up_calls",drawApiCounts[3]);
    j.Num("draw_primitive_failures",drawApiFailures[0]);j.Num("draw_indexed_primitive_failures",drawApiFailures[1]);j.Num("draw_primitive_up_failures",drawApiFailures[2]);j.Num("draw_indexed_primitive_up_failures",drawApiFailures[3]);
    j.Num("snapshot_attempts",snapshotAttempts);j.Num("snapshot_successes",snapshotSuccesses);j.Num("depth_snapshot_successes",depthSnapshotSuccesses);j.Num("snapshot_generation_attempts",snapshotGenerationAttempts);j.Num("snapshot_attempt_cap",kSnapshotAttemptLimit);j.Num("snapshot_generation_attempt_cap",kSnapshotAttemptsPerGeneration);
    j.Num("lighting_probe_calls",lightingProbeCalls);j.Num("lighting_probe_records",lightingProbeRecords);j.Num("lighting_probe_read_failures",lightingProbeReadFailures);
    j.Bool("quarantined",quarantined);j.Str("timing_limit","CPU submission/inspection and snapshot bracket only; no GPU timing claim");Enqueue(std::move(j));}
void OnEndScene(void*,const void* a){if(!ready||lost||quarantined)return;try{Device(static_cast<IDirect3DDevice9*>(static_cast<const ev::EndSceneArgs*>(a)->device));}catch(...){Failure();}}
void OnFrame(void*,const void*){if(!ready||GetCurrentThreadId()!=renderThread)return;try{if(frame%300==0)Summary();Flush();}catch(...){Failure();}++frame;if(nativeBridge)nativeBridge->NextFrame();}
void OnLost(void*,const void*){if(!ready)return;ResetProduction();lost=true;worldSceneDepth=0;try{auto j=Record("lost");j.Bool("snapshot_resource_was_live",snapshotSurface!=nullptr);j.Bool("depth_snapshot_resource_was_live",snapshotDepthSurface!=nullptr);Enqueue(std::move(j));Summary();Flush();}catch(...){Failure();}ReleaseSnapshot();ReleaseShaders();device=nullptr;}
void OnReset(void*,const void* a){if(!ready)return;try{lost=false;profiles.NewGeneration();boundaries.NewGeneration();worldSceneDepth=0;waterCandidateSeen=false;
    // Capture ceilings and the eight snapshot-attempt ceiling are process-wide across resets.
    Device(static_cast<IDirect3DDevice9*>(static_cast<const ev::DeviceResetArgs*>(a)->device));auto j=Record("reset");j.Bool("success",true);j.Str("snapshot_recreate_policy","lazy_on_next_water_candidate");Enqueue(std::move(j));}catch(...){Failure();}}
void OnWorldLeave(void*,const void*){if(!ready)return;ResetProduction();try{auto j=Record("world_leave");Enqueue(std::move(j));Summary();Flush();++worldEpoch;}catch(...){Failure();}worldSceneDepth=0;waterCandidateSeen=false;ReleaseSnapshot();ReleaseShaders();}
void OnSceneBegin(void*,const void* a){
    if(!ready||lost||quarantined||InternalRender())return;
    try{
        auto* d=static_cast<IDirect3DDevice9*>(static_cast<const ev::WorldSceneBeginArgs*>(a)->device);
        if(!CanInspect(d))return;
        ++worldSceneDepth;
        if(worldSceneDepth!=1){auto j=Record("world_scene");j.Str("point","nested_begin");j.Num("depth",worldSceneDepth);Enqueue(std::move(j));return;}
        ++worldSceneSerial;view=worldSceneSerial;phase="world_scene_begin";sceneBeginOrdinal=globalDrawOrdinal;
        firstLiquidOrdinal=lastLiquidOrdinal=liquidDrawsScene=0;postWaterNonLiquid=postWaterLikelyOpaque=postWaterUnclassified=0;postWaterProbed=postWaterRecords=0;
        waterCandidateSeen=false;waterCandidateOrdinal=0;snapshotValid=false;snapshotSerial=snapshotFrame=snapshotProducerOrdinal=0;snapshotSourceRtToken=0;
        depthSnapshotValid=false;depthSnapshotSerial=depthSnapshotFrame=depthSnapshotProducerOrdinal=0;
        Probe p;FillProbe(d,p);auto j=Record("world_scene");j.Str("point","begin");j.Num("depth",worldSceneDepth);j.Num("present_frame",frame);j.Num("draw_ordinal",sceneBeginOrdinal);j.Str("device_token",Pointer(d));j.Raw("state",ProbeJson(p));Enqueue(std::move(j));
    }catch(...){Failure();}
}
void OnSceneEnd(void*,const void* a){
    if(!ready||worldSceneDepth==0||InternalRender())return;
    try{
        if(worldSceneDepth>1){auto j=Record("world_scene");j.Str("point","nested_end");j.Num("depth",worldSceneDepth);Enqueue(std::move(j));--worldSceneDepth;return;}
        phase="world_scene_end";auto* e=static_cast<const ev::WorldSceneEndArgs*>(a);auto* d=static_cast<IDirect3DDevice9*>(e->device);
        auto j=Record("world_scene");j.Str("point","end");j.Num("depth",worldSceneDepth);j.Num("present_frame",frame);j.Num("draw_ordinal",globalDrawOrdinal);j.Str("device_token",Pointer(d));
        j.Num("scene_begin_draw_ordinal",sceneBeginOrdinal);j.Bool("water_candidate_seen",waterCandidateSeen);j.Num("liquid_draws",liquidDrawsScene);j.Num("first_liquid_ordinal",firstLiquidOrdinal);j.Num("last_liquid_ordinal",lastLiquidOrdinal);
        j.Num("post_water_non_liquid_draws",postWaterNonLiquid);j.Num("post_water_probed",postWaterProbed);j.Num("post_water_likely_opaque",postWaterLikelyOpaque);j.Num("post_water_unclassified_due_cap",postWaterUnclassified);
        j.Bool("snapshot_valid",snapshotValid&&snapshotSerial==worldSceneSerial);j.Num("snapshot_producer_ordinal",snapshotProducerOrdinal);j.Str("snapshot_source_rt_token",Pointer(reinterpret_cast<void*>(snapshotSourceRtToken)));
        j.Bool("depth_snapshot_valid",depthSnapshotValid&&depthSnapshotSerial==worldSceneSerial);j.Num("depth_snapshot_producer_ordinal",depthSnapshotProducerOrdinal);
        if(CanInspect(d))j.Raw("targets_end",Targets(d));
        j.Num("water_candidate_ordinal",waterCandidateOrdinal);j.Bool("ordering_candidate_pass",waterCandidateSeen&&firstLiquidOrdinal>waterCandidateOrdinal&&postWaterLikelyOpaque==0&&postWaterUnclassified==0);
        Enqueue(std::move(j));--worldSceneDepth;
    }catch(...){worldSceneDepth=0;Failure();}
}
void OnWorldEnd(void*,const void*){if(ready)phase="world_end_after_prior_subscribers";}
bool Identity(){wchar_t path[MAX_PATH]{};DWORD n=GetModuleFileNameW(nullptr,path,MAX_PATH);if(!n||n>=MAX_PATH)return false;
    std::ifstream f(std::filesystem::path(path),std::ios::binary);if(!f)return false;Sha256 hash;std::array<char,65536> b{};while(f){f.read(b.data(),b.size());auto count=f.gcount();if(count>0)hash.Update(b.data(),size_t(count));}
    return f.eof()&&Hex(hash.Finish())==kExeSha&&reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))==0x400000;
}
bool Install(){
    const auto replacement=slot3::ParseConfig([](const char* n){return std::getenv(n);});
    config=ParseConfig([](const char* n){return std::getenv(n);});
    diagnosticRequested=config.enabled && config.valid;
    if(replacement.enabled && replacement.valid) {
        production=new(std::nothrow) slot3::ProductionRuntime();
        if(!production) {WLOG_ERROR("r6-slot3: coordinator allocation failed; OFF");return true;}
        production->Configure(replacement);
        config.enabled=true;
        nativeBridge=new(std::nothrow) slot3::WrathNativeBridge();
        if(!nativeBridge) {production->Configure({});WLOG_ERROR("r6-slot3: adapter allocation failed; OFF");}
        else production->SetNativeBridge(nativeBridge);
    }
    if(!replacement.valid) WLOG_WARN("r6-slot3: invalid production flags; OFF");
    if(!config.enabled){if(!config.valid)WLOG_WARN("r6-water: invalid diagnostic flags; OFF");return true;}
    try{if(!Identity()){WLOG_ERROR("r6-water: supplied Wow.exe SHA256/base gate failed; OFF");return true;}
        if(nativeBridge) {
            nativeBridge->Initialize(true,&InternalRender);
            if(!nativeBridge->Qualified()) WLOG_WARN("r6-slot3: %s; replacement OFF",nativeBridge->Blocker());
        }
        if(ProductionActive()) config.copyRequested=config.depthCopyRequested=true;
        CreateDirectoryA("Logs",nullptr);capturePath="Logs\\r6-water-"+std::to_string(GetCurrentProcessId())+'-'+std::to_string(GetTickCount64())+".ndjson";
        sink.open(capturePath,std::ios::binary|std::ios::out|std::ios::trunc);if(!sink){diagnosticFailed=true;WLOG_ERROR("r6-water: output unavailable; logging OFF");if(!production || !production->Requested())return true;}
        outputBudget.limit=size_t(config.maxMiB)*1024*1024-4096;QueryPerformanceFrequency(&frequency);shaders.reserve(128);

        if(diagnosticRequested && config.mode==Mode::Full && !production)
        {
            static const std::array<uint8_t,16>
                expectedLightingBuild{{
                    0x55,0x8b,0xec,0x81,
                    0xec,0xa4,0x00,0x00,
                    0x00,0xd9,0xe8,0xa1,
                    0x88,0xdf,0xc5,0x00
                }};

            std::array<uint8_t,16>
                actualLightingBuild{};

            if(!native::Read(
                    reinterpret_cast<const void*>(
                        kLightingConstantBuild),
                    actualLightingBuild.data(),
                    actualLightingBuild.size()) ||
               actualLightingBuild !=
                    expectedLightingBuild)
            {
                WLOG_ERROR(
                    "r6-water: lighting probe byte gate failed at %08X; diagnostics OFF",
                    unsigned(kLightingConstantBuild));

                sink.close();
                return true;
            }

            if(!wxl::hook::Install(
                    "R6.LightingConstantBuildProbe",
                    kLightingConstantBuild,
                    &LightingConstantBuildProbe,
                    &lightingConstantBuildOriginal,
                    100))
            {
                WLOG_ERROR(
                    "r6-water: lighting probe hook registration failed; diagnostics OFF");

                sink.close();
                return true;
            }
        }

        if(!native::Install(&Boundary,&SnapshotAtWater)){sink.close();return true;}
        if(production && !slot3::InstallFirstCandidateLightingCapture()) {
            WLOG_ERROR("r6-slot3: lighting capture unavailable; candidate OFF");
            production->Configure({});
        }
        ev::Subscribe(ev::Event::OnEndScene,&OnEndScene,nullptr);ev::Subscribe(ev::Event::OnFrame,&OnFrame,nullptr);
        ev::Subscribe(ev::Event::OnDeviceLost,&OnLost,nullptr);ev::Subscribe(ev::Event::OnDeviceReset,&OnReset,nullptr);
        ev::Subscribe(ev::Event::OnWorldLeave,&OnWorldLeave,nullptr);ev::Subscribe(ev::Event::OnWorldSceneBegin,&OnSceneBegin,nullptr);
        ev::Subscribe(ev::Event::OnWorldSceneEnd,&OnSceneEnd,nullptr);ev::Subscribe(ev::Event::OnWorldRenderEnd,&OnWorldEnd,nullptr);
        ready=true;auto j=Record("session");j.Str("exe_sha256",kExeSha);j.Num("mode",unsigned(config.mode));j.Num("samples_per_profile",config.samples);j.Num("max_draws",config.maxDraws);j.Num("max_mib",config.maxMiB);
        j.Bool("diagnostics_requested",diagnosticRequested);j.Bool("continuous_production_snapshots",ProductionActive());
        j.Bool("copy_requested",config.copyRequested);j.Bool("copy_supported",true);j.Bool("depth_copy_requested",config.depthCopyRequested);j.Bool("depth_copy_supported",true);j.Bool("replacement_supported",ProductionActive());j.Bool("slot3_requested",production&&production->Requested());j.Bool("slot3_bridge_qualified",ProductionActive());j.Bool("replacement_eligibility_supported",true);
        j.Bool("lighting_probe_8a38b0",diagnosticRequested&&config.mode==Mode::Full&&!production);
        if(nativeBridge) {j.Bool("native_entrypoints_verified",nativeBridge->EntryPointsVerified());j.Str("qualification_blocker",nativeBridge->Blocker());}j.Num("lighting_probe_record_cap",kLightingProbeRecordLimit);
        j.Bool("p01_plane_probe",diagnosticRequested&&config.mode==Mode::Full);j.Num("p01_plane_probe_record_cap",kP01PlaneProbeRecordLimit);
        j.Str("replacement_allowlist","P01/P02/P03 only: base Water pass1 + exact final VS/PS + declaration + material + terrain/WMO provider; still native-forwarded");
        j.Num("copy_attempt_cap",kSnapshotAttemptLimit);j.Num("copy_attempts_per_generation",kSnapshotAttemptsPerGeneration);j.Str("copy_gate",ProductionActive()?"continuous qualified production; diagnostic record limits do not cap copies":"diagnostic copy: process/generation caps apply");
        j.Str("classic_target_controls","water 0..3; ripple 0..2; reflection 0 screen,1 sky,2 sky+terrain,3 sky+terrain+WMO; not Wrath live setting values");
        Enqueue(std::move(j));Flush();WLOG_INFO("r6-water: native-preserving diagnostic enabled; output=%s",capturePath.c_str());
        if(config.copyRequested)WLOG_INFO("r6-water: bounded pre-Water colour snapshot proof armed; max attempts=%u; no replacement",kSnapshotAttemptLimit);
        if(config.depthCopyRequested)WLOG_INFO("r6-water: bounded pre-Water two-stage depth transport proof armed; no replacement");
    }catch(...){ready=false;Failure();}return true;
}
// All pending records are normally flushed at Present/world leave. At process shutdown,
// close CPU output only: never call D3D or mutate hook tables while the loader lock is held.
struct Shutdown {
    ~Shutdown(){if(!ready||!sink.is_open())return;try{auto j=Record("shutdown");j.Num("pending_records_not_flushed",pending.size());j.Num("dropped_records",outputBudget.dropped);auto s=j.End()+'\n';sink.write(s.data(),std::streamsize(s.size()));sink.flush();}catch(...){}}
} shutdown;
} // namespace

bool BorrowPreWaterSnapshot(IDirect3DDevice9* expectedDevice,
                            uintptr_t expectedSourceRt,
                            uint64_t consumerOrdinal,
                            PreWaterSnapshotView& output) noexcept
{
    output = {};
    if (!expectedDevice || !expectedSourceRt || !consumerOrdinal ||
        !CanInspect(expectedDevice) || worldSceneDepth != 1 ||
        !snapshotValid || !depthSnapshotValid ||
        snapshotSerial != worldSceneSerial || depthSnapshotSerial != worldSceneSerial ||
        snapshotFrame != frame || depthSnapshotFrame != frame ||
        !snapshotProducerOrdinal || !depthSnapshotProducerOrdinal ||
        snapshotProducerOrdinal >= consumerOrdinal || depthSnapshotProducerOrdinal >= consumerOrdinal ||
        snapshotSourceRtToken != expectedSourceRt ||
        !snapshotTexture || !snapshotDepthTexture ||
        snapshotDesc.Width == 0 || snapshotDesc.Height == 0 ||
        snapshotDepthDesc.Width != snapshotDesc.Width ||
        snapshotDepthDesc.Height != snapshotDesc.Height ||
        snapshotDepthDesc.Format != kIntz)
        return false;

    PreWaterSnapshotView candidate{};
    candidate.colour = snapshotTexture;
    candidate.rawDepthIntz = snapshotDepthTexture;
    candidate.device = reinterpret_cast<uintptr_t>(expectedDevice);
    candidate.sourceRt = snapshotSourceRtToken;
    candidate.generation = generation;
    candidate.scene = worldSceneSerial;
    candidate.frame = frame;
    candidate.colourProducerOrdinal = snapshotProducerOrdinal;
    candidate.depthProducerOrdinal = depthSnapshotProducerOrdinal;
    candidate.width = snapshotDesc.Width;
    candidate.height = snapshotDesc.Height;
    candidate.colourFormat = unsigned(snapshotDesc.Format);
    candidate.depthFormat = unsigned(snapshotDepthDesc.Format);

    if (!SnapshotMetadataCompatible(candidate,
                                    reinterpret_cast<uintptr_t>(expectedDevice),
                                    expectedSourceRt,
                                    consumerOrdinal))
        return false;

    output = candidate;
    return true;
}
} // namespace wxl::waterdiag
WXL_REGISTER_FEATURE("r6-water-diagnostics",true,wxl::waterdiag::Install)
