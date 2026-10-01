// R7 residual R4 outline: opt-in READ-ONLY observation. GPL-3.0-or-later.
// No visibility overrides, batch splitting, alpha changes or D3D setters.
#include "client/CWorldScene/OutlineDiagnosticPolicy.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Camera.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/WorldScene.hpp"
#include "game/M2.hpp"
#include "water/WaterDiagCore.hpp"
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace wxl::outlinediag {
namespace {
namespace m2=wxl::offsets::game::m2;
namespace gx=wxl::offsets::engine::gx;
namespace ws=wxl::offsets::game::worldscene;
namespace cam=wxl::offsets::engine::camera;
namespace ev=wxl::events;
using wxl::waterdiag::Quote;
using wxl::waterdiag::Hex;
using wxl::waterdiag::Sha256;
constexpr const char* kExeSha="57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da";
constexpr size_t kByteLimit=16*1024*1024;
Config config;
std::ofstream sink;
uint64_t frame=0,sequence=0;
unsigned sceneDepth=0,windows=0,errors=0;
size_t bytes=0;
std::array<unsigned,4> counts{}; // material, draw, readiness, batch compatibility
std::array<unsigned,4> windowCounts{};
DWORD renderThread=0;
bool ready=false;
thread_local uint64_t pendingMaterial=0;
m2::M2_SetupMaterialFn originalMaterial=nullptr;
m2::M2_IsDrawableFn originalReady=nullptr;
m2::M2_IsBatchDoodadCompatibleFn originalBatch=nullptr;
gx::GxDeviceDrawFn originalDraw=nullptr;

struct Json {
    std::string text="{";
    bool first=true;
    void Raw(const char* key,std::string value) { if(!first)text+=',';first=false;text+=Quote(key)+':'+value; }
    void Str(const char* key,std::string_view value) { Raw(key,Quote(value)); }
    template<class T> void Num(const char* key,T value) { Raw(key,std::to_string(value)); }
    void Float(const char* key,float value) {
        char b[32]{};
        if(std::isfinite(value)) {std::snprintf(b,sizeof(b),"%.9g",double(value));Raw(key,b);}
        else Raw(key,"null");
    }
    std::string End() { return text+'}'; }
};
template<class T> struct Com {
    T* p=nullptr;
    ~Com() { if(p)p->Release(); }
    Com()=default;
    Com(const Com&)=delete;
    Com& operator=(const Com&)=delete;
};
std::string Pointer(uintptr_t p) { char b[24]{};std::snprintf(b,sizeof(b),"0x%08llx",static_cast<unsigned long long>(p));return b; }
bool ReadBytes(uintptr_t p,void* out,size_t n) {
    SIZE_T copied=0;
    return p && n<=UINTPTR_MAX-p && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&copied) && copied==n;
}
template<class T> bool Read(uintptr_t p,size_t offset,T& out) {
    return offset<=UINTPTR_MAX-p && ReadBytes(p+offset,&out,sizeof(out));
}
template<class T> void Field(Json& j,const char* name,uintptr_t p,size_t offset) {
    T value{};
    if(!Read(p,offset,value)) { j.Raw(name,"null"); return; }
    if constexpr(std::is_floating_point_v<T>) j.Float(name,value); else j.Num(name,value);
}
std::string Floats(const float* p,unsigned n) {
    std::string s="[";
    for(unsigned i=0;i<n;++i) {
        if(i)s+=',';char b[32]{};
        if(std::isfinite(p[i])) {std::snprintf(b,sizeof(b),"%.9g",double(p[i]));s+=b;}else s+="null";
    }
    return s+']';
}
std::string Path(uintptr_t model) {
    std::array<char,m2::kOffModelHeader-m2::kOffModelPathStem> name{};
    if(!ReadBytes(model+m2::kOffModelPathStem,name.data(),name.size())) return {};
    auto end=std::find(name.begin(),name.end(),'\0');
    return end==name.end()?std::string{}:std::string(name.begin(),end);
}
bool Active(unsigned stage) {
    return ready && sceneDepth && GetCurrentThreadId()==renderThread && config.Samples(frame) &&
           windowCounts[stage]<config.records && bytes<kByteLimit;
}
uint64_t Emit(Json& j) {
    j.Num("sequence",++sequence);j.Num("world_frame",frame);
    auto s=j.End();
    if(s.size()+1>kByteLimit-bytes) return 0;
    sink<<s<<'\n';
    if(!sink) { ready=false; ++errors; WLOG_ERROR("r7-outline: capture write failed; observation OFF"); return 0; }
    bytes+=s.size()+1;return sequence;
}
bool Instance(Json& j,uintptr_t instance) {
    uint32_t flags=0,model=0;
    float distanceSq=0;
    if(!Read(instance,m2::kOffInstOwnerFlags,flags) || !(flags&0x20) ||
       !Read(instance,m2::kOffInstModel,model) || !model ||
       !Read(instance,m2::kOffInstViewDistSq,distanceSq)) return false;
    auto name=Path(model);
    if(!Matches(config,flags,distanceSq,name)) return false;
    j.Str("category","placed_M2_ADT_or_WMO_unknown");
    j.Str("instance",Pointer(instance));j.Str("model",Pointer(model));j.Str("path",name);
    j.Num("owner_flags",flags);j.Float("view_distance_sq",distanceSq);j.Float("view_distance",std::sqrt(distanceSq));
    j.Str("native_selected_band","UNKNOWN");
    j.Str("distance_skin_LOD","not_documented_in_target_build");
    Field<uint32_t>(j,"init_flags",instance,m2::kOffInstInitFlags);
    Field<float>(j,"alpha_base",instance,m2::kOffInstAlphaBase);
    Field<float>(j,"alpha_stage",instance,m2::kOffInstAlphaStage);
    float placement[16]{};
    if(ReadBytes(instance+m2::kOffInstPlacement,placement,sizeof(placement))) j.Raw("placement",Floats(placement,16));
    else j.Raw("placement","null");
    uint32_t skin=0;
    if(Read(model,m2::kOffModelSkin,skin)) j.Str("skin",Pointer(skin));else j.Raw("skin","null");
    return true;
}
void Bands(Json& j) {
    float band[5]{},fade[5]{},camera[3]{},viewproj[16]{};
    if(ReadBytes(ws::kDistanceBand1Live,band,sizeof(band))) j.Raw("live_band_cutoffs",Floats(band,5));
    else j.Raw("live_band_cutoffs","null");
    if(ReadBytes(ws::kDistanceBand1FadeStart,fade,sizeof(fade))) j.Raw("live_fade_starts",Floats(fade,5));
    else j.Raw("live_fade_starts","null");
    if(ReadBytes(cam::kCameraPos,camera,sizeof(camera))) j.Raw("camera",Floats(camera,3));
    if(ReadBytes(cam::kViewProj,viewproj,sizeof(viewproj))) j.Raw("engine_viewproj",Floats(viewproj,16));
}
void Material(void* ctx,uintptr_t caller) {
    pendingMaterial=0;
    if(!Active(0)) return;
    const auto p=reinterpret_cast<uintptr_t>(ctx);
    uint32_t instance=0,element=0,material=0,model=0,skin=0;
    if(!Read(p,m2::kOffRenderCtxInstance,instance) || !Read(p,m2::kOffRenderCtxElement,element) ||
       !Read(p,m2::kOffDrawCtxMaterial,material)) return;
    Json j;j.Str("event","material_after_native_and_R4");
    if(!Instance(j,instance)) return;
    j.Str("material_caller",Pointer(caller));j.Str("element",Pointer(element));j.Str("material",Pointer(material));
    Field<float>(j,"element_alpha",element,m2::kOffElementAlpha);
    Field<uint16_t>(j,"blend_mode",material,m2::kOffMaterialBlend);
    Field<uint16_t>(j,"raw_material_word0",material,0);
    Field<uint32_t>(j,"requested_co_instance_run",element,gx::kM2ElementRunLengthField);
    uint32_t index=0,section=0;
    if(Read(element,m2::kOffElementBatchIndex,index)) j.Num("batch_index",index);else j.Raw("batch_index","null");
    // Read current section off the element, NEVER the reused/stale ctx+0x90.
    if(Read(element,gx::kM2ElementSectionField,section)) {
        wxl::structure::m2::M2SkinSection s{};
        if(ReadBytes(section,&s,sizeof(s))) {
            j.Str("section",Pointer(section));j.Num("section_id",s.skinSectionId);j.Num("section_level_raw",s.level);
            j.Num("section_vertices",s.vertexCount);j.Num("section_indices",s.indexCount);
            j.Num("section_bones",s.boneCount);
        }
    }
    uint32_t count=0,batches=0;
    using Skin=wxl::game::m2::M2SkinProfile;
    if(Read(instance,m2::kOffInstModel,model) && Read(model,m2::kOffModelSkin,skin) &&
       Read(skin,offsetof(Skin,batchCount),count) && count<=4096 && index<count &&
       Read(skin,offsetof(Skin,batches),batches) && batches) {
        wxl::structure::m2::M2Batch b{};
        if(Read(batches,size_t(index)*sizeof(b),b)) {
            j.Num("skin_shader_id",b.shaderId);j.Num("skin_material_index",b.materialIndex);
            j.Num("skin_section_index",b.skinSectionIndex);j.Num("skin_texture_count",b.textureCount);
        }
    }
    Bands(j);++counts[0];++windowCounts[0];pendingMaterial=Emit(j);
}
void __fastcall HookMaterial(void* ctx,void* edx) {
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    originalMaterial(ctx,edx);
    try { Material(ctx,caller); } catch(...) { pendingMaterial=0;++errors; }
}
template<class Shader> std::string ShaderHash(Shader* s) {
    if(!s) return "null";
    UINT n=0;
    if(FAILED(s->GetFunction(nullptr,&n)) || !n || n>65536) return "UNAVAILABLE";
    std::vector<uint8_t> code(n);
    if(FAILED(s->GetFunction(code.data(),&n)) || n>code.size()) return "UNAVAILABLE";
    return Hex(Sha256::Of(code.data(),n));
}
void Hardware(Json& j,IDirect3DDevice9* d) {
    Json rs;
    struct State { const char* name;D3DRENDERSTATETYPE state; };
    for(auto s:{State{"alpha_ref",D3DRS_ALPHAREF},State{"alpha_test",D3DRS_ALPHATESTENABLE},
                State{"alpha_func",D3DRS_ALPHAFUNC},State{"alpha_blend",D3DRS_ALPHABLENDENABLE},
                State{"src_blend",D3DRS_SRCBLEND},State{"dest_blend",D3DRS_DESTBLEND},
                State{"z_enable",D3DRS_ZENABLE},State{"z_write",D3DRS_ZWRITEENABLE},
                State{"z_func",D3DRS_ZFUNC},State{"cull",D3DRS_CULLMODE},
                State{"fog",D3DRS_FOGENABLE},State{"color_write",D3DRS_COLORWRITEENABLE}}) {
        DWORD v=0;if(SUCCEEDED(d->GetRenderState(s.state,&v)))rs.Num(s.name,v);else rs.Raw(s.name,"null");
    }
    j.Raw("hardware_after_native_draw",rs.End());
    Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
    if(SUCCEEDED(d->GetVertexShader(&vs.p))) j.Str("vs_sha256",ShaderHash(vs.p));
    if(SUCCEEDED(d->GetPixelShader(&ps.p))) j.Str("ps_sha256",ShaderHash(ps.p));
    D3DVIEWPORT9 vp{};
    if(SUCCEEDED(d->GetViewport(&vp))) {
        j.Num("viewport_width",vp.Width);j.Num("viewport_height",vp.Height);
        j.Float("viewport_min_z",vp.MinZ);j.Float("viewport_max_z",vp.MaxZ);
    }
    for(DWORD slot=0;slot<4;++slot) {
        Json t;t.Num("slot",slot);
        for(auto type:{D3DSAMP_MINFILTER,D3DSAMP_MIPFILTER,D3DSAMP_MIPMAPLODBIAS,D3DSAMP_MAXMIPLEVEL}) {
            DWORD value=0;if(SUCCEEDED(d->GetSamplerState(slot,type,&value))) {
                auto key="sampler_"+std::to_string(unsigned(type));t.Num(key.c_str(),value);
            }
        }
        Com<IDirect3DBaseTexture9> tex;
        if(SUCCEEDED(d->GetTexture(slot,&tex.p)) && tex.p) {
            t.Num("levels",tex.p->GetLevelCount());t.Num("resource_min_LOD",tex.p->GetLOD());
            if(tex.p->GetType()==D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC desc{};
                if(SUCCEEDED(static_cast<IDirect3DTexture9*>(tex.p)->GetLevelDesc(0,&desc))) {
                    t.Num("width",desc.Width);t.Num("height",desc.Height);t.Num("format",unsigned(desc.Format));
                }
            }
        }
        t.Str("actual_sampled_mip","UNKNOWN");auto key="texture_"+std::to_string(slot);j.Raw(key.c_str(),t.End());
    }
}
void __fastcall HookDraw(void* device,void* edx,uint32_t* batch,int indexed) {
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    // Consume only the first Gx draw after this material setter. This is an
    // ORDERING CORRELATION, not a statically proven one-to-one object/draw bind.
    uint64_t token=pendingMaterial;pendingMaterial=0;
    originalDraw(device,edx,batch,indexed);
    if(!token || !Active(1)) return;
    try {
        Json j;j.Str("event","first_Gx_draw_after_material");j.Num("material_sequence",token);
        j.Str("correlation","ordering_only_not_proven_object_binding");j.Str("native_draw_caller",Pointer(caller));
        j.Str("route","UNKNOWN_use_native_caller_and_run_record");j.Num("indexed",indexed);
        Field<uint32_t>(j,"primitive_type",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchPrimType);
        Field<uint32_t>(j,"start_index",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchStartIndex);
        Field<uint32_t>(j,"index_count",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchIndexCount);
        uint32_t raw=0;
        if(Read(reinterpret_cast<uintptr_t>(device),gx::kD3DDeviceField,raw) && raw) Hardware(j,reinterpret_cast<IDirect3DDevice9*>(raw));
        ++counts[1];++windowCounts[1];Emit(j);
    } catch(...) { ++errors; }
}
int __fastcall HookReady(void* instance,void* edx,int a,int b) {
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    int native=originalReady(instance,edx,a,b);
    if(!Active(2) || (caller!=m2::kDoodadDrainRetA && caller!=m2::kDoodadDrainRetB)) return native;
    try {
        Json j;j.Str("event","drawable_readiness_not_distance_cull");
        if(Instance(j,reinterpret_cast<uintptr_t>(instance))) {j.Num("native_result",native);j.Str("caller",Pointer(caller));++counts[2];++windowCounts[2];Emit(j);}
    } catch(...) { ++errors; }
    return native;
}
int __fastcall HookBatch(void* instance,void* edx,uint8_t* flags) {
    int native=originalBatch(instance,edx,flags);
    if(!Active(3)) return native;
    try {
        Json j;j.Str("event","batch_compatibility");
        if(Instance(j,reinterpret_cast<uintptr_t>(instance))) {j.Num("native_result",native);++counts[3];++windowCounts[3];Emit(j);}
    } catch(...) { ++errors; }
    return native;
}
void Begin(void*,const void*) {
    if(!ready) return;
    if(!sceneDepth) { renderThread=GetCurrentThreadId();++frame;pendingMaterial=0;windowCounts={}; }
    ++sceneDepth;
    if(config.Samples(frame) && sceneDepth==1) {
        try {Json j;j.Str("event","sample_begin");Bands(j);Emit(j);++windows;}catch(...){++errors;}
    }
}
void End(void*,const void*) {
    if(!sceneDepth) return;
    --sceneDepth;
    if(sceneDepth) return;
    pendingMaterial=0;
    if(config.Samples(frame)) {
        try {Json j;j.Str("event","sample_end");j.Num("windows",windows);j.Num("capture_exceptions",errors);
            for(unsigned i=0;i<4;++i) {auto key="records_"+std::to_string(i);j.Num(key.c_str(),counts[i]);}
            Emit(j);sink.flush();}catch(...){++errors;}
    }
}
bool Identity() {
    wchar_t path[MAX_PATH]{};DWORD n=GetModuleFileNameW(nullptr,path,MAX_PATH);
    if(!n||n>=MAX_PATH || reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))!=0x400000) return false;
    std::ifstream f(std::filesystem::path(path),std::ios::binary);if(!f)return false;
    Sha256 hash;std::array<char,65536> b{};
    while(f) {f.read(b.data(),b.size());if(f.gcount()>0)hash.Update(b.data(),size_t(f.gcount()));}
    return f.eof() && Hex(hash.Finish())==kExeSha;
}
bool Install() {
    try {
        config=Parse([](const char* n){return std::getenv(n);});
        if(!config.enabled) {if(!config.valid)WLOG_WARN("r7-outline: invalid flags; OFF");return true;}
        if(!Identity()) {WLOG_ERROR("r7-outline: canonical Wow.exe SHA/base gate failed; OFF");return true;}
        std::filesystem::create_directories("Logs");
        auto file="Logs/r7-outline-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".ndjson";
        sink.open(file,std::ios::binary);if(!sink) {WLOG_ERROR("r7-outline: cannot open capture; OFF");return true;}
        // Outer material observer: priority -1000 calls through ALL normal-
        // priority native/R4 setters before reading; no duplicated R4 logic.
        bool ok=wxl::hook::Install("R7OutlineMaterial",m2::kSetupMaterial,&HookMaterial,&originalMaterial,-1000);
        ok &= wxl::hook::Install("R7OutlineDraw",gx::kGxDeviceDraw,&HookDraw,&originalDraw,-1000);
        ok &= wxl::hook::Install("R7OutlineReadiness",m2::kIsDrawable,&HookReady,&originalReady,-1000);
        ok &= wxl::hook::Install("R7OutlineBatch",m2::kIsBatchDoodadCompatible,&HookBatch,&originalBatch,-1000);
        if(!ok) return false;
        ev::Subscribe(ev::Event::OnWorldSceneBegin,&Begin,nullptr);
        ev::Subscribe(ev::Event::OnWorldSceneEnd,&End,nullptr);
        ready=true;
        Json j;j.Str("event","identity");j.Str("wow_sha256",kExeSha);j.Str("mode","read_only");
        j.Str("object_draw_binding","ordering_correlation_requires_validation");
        j.Num("start",config.start);j.Num("stride",config.stride);j.Num("windows",config.windows);Emit(j);sink.flush();
        WLOG_INFO("r7-outline: read-only capture %s start=%u stride=%u windows=%u",file.c_str(),config.start,config.stride,config.windows);
        return true;
    } catch(...) {ready=false;WLOG_ERROR("r7-outline: setup exception; OFF");return true;}
}
WXL_REGISTER_FEATURE("r7-distant-outline-diagnostics",true,Install);
}
}
