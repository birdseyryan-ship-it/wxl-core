// R7 residual R4 outline: opt-in READ-ONLY observation. GPL-3.0-or-later.
// No visibility overrides, batch splitting, alpha changes or D3D setters.
#include "client/CWorldScene/OutlineDiagnosticPolicy.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Camera.hpp"
#include "offsets/engine/Shader.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/WMO.hpp"
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
#include <cstring>
#include <type_traits>

namespace wxl::outlinediag {
namespace {
namespace m2=wxl::offsets::game::m2;
namespace wmo=wxl::offsets::game::wmo;
namespace gx=wxl::offsets::engine::gx;
namespace sh=wxl::offsets::engine::shader;
namespace ws=wxl::offsets::game::worldscene;
namespace cam=wxl::offsets::engine::camera;
namespace ev=wxl::events;
namespace fmt=wxl::structure::m2;
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
std::array<unsigned,6> counts{}; // M2 material/direct-batch/readiness/batch, WMO material/draw
std::array<unsigned,6> windowCounts{};
uint64_t rawGxDrawCallsWindow=0;
uint64_t rawGxDrawM2TokenWindow=0;
uint64_t rawGxDrawWmoTokenWindow=0;
DWORD renderThread=0;
bool ready=false;

thread_local uint64_t pendingM2Material=0;
thread_local uint64_t pendingWmoMaterial=0;
thread_local uintptr_t pendingWmoBatch=0;
thread_local uintptr_t currentWmoRoot=0;
thread_local uintptr_t currentWmoGroup=0;
thread_local const char* currentWmoLeaf="NONE";
thread_local int currentWmoLeafFlag=0;

m2::M2_SetupMaterialFn originalMaterial=nullptr;
m2::M2_IsDrawableFn originalReady=nullptr;
m2::M2_IsBatchDoodadCompatibleFn originalBatch=nullptr;
gx::GxDeviceDrawFn originalDraw=nullptr;

using M2TriangleBatchFn=void(__fastcall*)(void* ctx,void* edx);
using M2DoodadBatchFn=void(__fastcall*)(void* ctx,void* edx,void* elements,void* indices);
M2TriangleBatchFn originalM2TriangleBatch=nullptr;
M2DoodadBatchFn originalM2DoodadBatch=nullptr;

wmo::Wmo_CullBatchFn originalWmoCull=nullptr;
wmo::Wmo_RenderLeafFn originalWmoExt=nullptr;
wmo::Wmo_RenderLeafFn originalWmoInt=nullptr;
sh::EffectBindFn originalWmoEffectBind=nullptr;

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

std::string WmoPath(uintptr_t root) {
    std::array<char,260> name{};
    if(!ReadBytes(root+wmo::kOffNameInline,name.data(),name.size())) return {};
    auto end=std::find(name.begin(),name.end(),'\0');
    return end==name.end()?std::string{}:std::string(name.begin(),end);
}

const char* WmoFamily(int index) {
    static constexpr const char* names[7]={
        "Diffuse","Specular","Metal","Env","Opaque","EnvMetal","Composite"
    };
    return index>=0 && index<7 ? names[index] : "UNKNOWN";
}

struct WmoSelection {
    int exterior=-1;
    int alternate=-1;
};

WmoSelection SelectedWmoEffect(uint32_t active) {
    WmoSelection out;
    for(int i=0;i<7;++i) {
        uint32_t p=0;
        if(Read<uint32_t>(sh::kExteriorEffectTable,size_t(i)*4,p) && p==active)
            out.exterior=i;
        p=0;
        if(Read<uint32_t>(sh::kAltEffectTable,size_t(i)*4,p) && p==active)
            out.alternate=i;
    }
    return out;
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
IDirect3DDevice9* LiveD3D();
void Hardware(Json& j,IDirect3DDevice9* d,const char* phase);

void Material(void* ctx,uintptr_t caller) {
    pendingM2Material=0;
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
            j.Num("skin_batch_flags",b.flags);
            j.Num("skin_priority_plane",b.priorityPlane);
            j.Num("skin_shader_id",b.shaderId);
            j.Num("skin_shader_id_highbit_8000",(b.shaderId&0x8000u)?1:0);
            j.Num("skin_material_index",b.materialIndex);
            j.Num("skin_section_index",b.skinSectionIndex);
            j.Num("skin_geoset_index",b.geosetIndex);
            j.Num("skin_color_index",b.colorIndex);
            j.Num("skin_material_layer",b.materialLayer);
            j.Num("skin_texture_count",b.textureCount);
            j.Num("skin_texture_combo_index",b.textureComboIndex);
            j.Num("skin_texture_coord_combo_index",b.textureCoordComboIndex);
            j.Num("skin_texture_weight_combo_index",b.textureWeightComboIndex);
            j.Num("skin_texture_transform_combo_index",b.textureTransformComboIndex);
        }
    }

    uint32_t headerPtr=0;
    if(model && Read(model,m2::kOffModelHeader,headerPtr) && headerPtr) {
        uint32_t version=0;
        uint32_t globalFlags=0;

        uint32_t lookupCount=0;
        uint32_t lookupRef=0;

        uint32_t comboCount=0;
        uint32_t comboRef=0;

        const bool haveVersion=
            Read(
                headerPtr,
                offsetof(fmt::M2Header,version),
                version
            );

        const bool haveFlags=
            Read(
                headerPtr,
                offsetof(fmt::M2Header,globalFlags),
                globalFlags
            );

        const bool haveLookupCount=
            Read(
                headerPtr,
                offsetof(fmt::M2Header,textureUnitLookup)
                    + offsetof(fmt::M2Array,count),
                lookupCount
            );

        const bool haveLookupRef=
            Read(
                headerPtr,
                offsetof(fmt::M2Header,textureUnitLookup)
                    + offsetof(fmt::M2Array,offset),
                lookupRef
            );

        const bool hasCombiner=
            haveFlags
            && ((globalFlags&fmt::kFlagUseTextureCombinerCombos)!=0);

        bool haveComboCount=false;
        bool haveComboRef=false;

        if(hasCombiner) {
            haveComboCount=
                Read(
                    headerPtr,
                    offsetof(fmt::M2Header,textureCombinerCombos)
                        + offsetof(fmt::M2Array,count),
                    comboCount
                );

            haveComboRef=
                Read(
                    headerPtr,
                    offsetof(fmt::M2Header,textureCombinerCombos)
                        + offsetof(fmt::M2Array,offset),
                    comboRef
                );
        }

        if(haveVersion)
            j.Num("model_version",version);
        else
            j.Raw("model_version","null");

        if(haveFlags)
            j.Num("model_global_flags",globalFlags);
        else
            j.Raw("model_global_flags","null");

        if(haveLookupCount)
            j.Num("texture_unit_lookup_count",lookupCount);
        else
            j.Raw("texture_unit_lookup_count","null");

        if(haveLookupRef)
            j.Num("texture_unit_lookup_ref_raw_u32",lookupRef);
        else
            j.Raw("texture_unit_lookup_ref_raw_u32","null");

        if(haveFlags)
            j.Num(
                "texture_combiner_combo_contract",
                hasCombiner?1:0
            );
        else
            j.Raw(
                "texture_combiner_combo_contract",
                "null"
            );

        if(!hasCombiner) {
            j.Num("texture_combiner_combo_count",0);
            j.Num("texture_combiner_combo_ref_raw_u32",0);
        } else {
            if(haveComboCount)
                j.Num(
                    "texture_combiner_combo_count",
                    comboCount
                );
            else
                j.Raw(
                    "texture_combiner_combo_count",
                    "null"
                );

            if(haveComboRef)
                j.Num(
                    "texture_combiner_combo_ref_raw_u32",
                    comboRef
                );
            else
                j.Raw(
                    "texture_combiner_combo_ref_raw_u32",
                    "null"
                );
        }
    }

    j.Str("edgefade_family_classification","DEFER_TO_OFFLINE_SHADER_HASH_SELECTOR_PROOF");

    if(auto* d=LiveD3D()) {
        Hardware(j,d,"after_native_M2_material_setup");
    } else {
        j.Str("hardware_observation_phase","after_native_M2_material_setup");
        j.Raw("hardware_state","null");
    }

    Bands(j);++counts[0];++windowCounts[0];pendingM2Material=Emit(j);
}
void __fastcall HookMaterial(void* ctx,void* edx) {
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    originalMaterial(ctx,edx);
    try { Material(ctx,caller); } catch(...) { pendingM2Material=0;++errors; }
}
template<class Shader> std::string ShaderHash(Shader* s) {
    if(!s) return "null";
    UINT n=0;
    if(FAILED(s->GetFunction(nullptr,&n)) || !n || n>65536) return "UNAVAILABLE";
    std::vector<uint8_t> code(n);
    if(FAILED(s->GetFunction(code.data(),&n)) || n>code.size()) return "UNAVAILABLE";
    return Hex(Sha256::Of(code.data(),n));
}
void Hardware(Json& j,IDirect3DDevice9* d,const char* phase) {
    j.Str("hardware_observation_phase",phase);
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
    j.Raw("hardware_state",rs.End());
    Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;
    if(SUCCEEDED(d->GetVertexShader(&vs.p))) j.Str("vs_sha256",ShaderHash(vs.p));
    if(SUCCEEDED(d->GetPixelShader(&ps.p))) j.Str("ps_sha256",ShaderHash(ps.p));
    D3DVIEWPORT9 vp{};
    if(SUCCEEDED(d->GetViewport(&vp))) {
        j.Num("viewport_width",vp.Width);j.Num("viewport_height",vp.Height);
        j.Float("viewport_min_z",vp.MinZ);j.Float("viewport_max_z",vp.MaxZ);
    }
    struct Samp { const char* name; D3DSAMPLERSTATETYPE type; bool asFloat; };
    for(DWORD slot=0;slot<4;++slot) {
        Json t;t.Num("slot",slot);

        for(auto s:{
                Samp{"min_filter",D3DSAMP_MINFILTER,false},
                Samp{"mag_filter",D3DSAMP_MAGFILTER,false},
                Samp{"mip_filter",D3DSAMP_MIPFILTER,false},
                Samp{"mip_lod_bias",D3DSAMP_MIPMAPLODBIAS,true},
                Samp{"max_mip_level",D3DSAMP_MAXMIPLEVEL,false},
                Samp{"max_anisotropy",D3DSAMP_MAXANISOTROPY,false},
                Samp{"address_u",D3DSAMP_ADDRESSU,false},
                Samp{"address_v",D3DSAMP_ADDRESSV,false}
            }) {
            DWORD value=0;
            if(SUCCEEDED(d->GetSamplerState(slot,s.type,&value))) {
                if(s.asFloat) {
                    float f=0.0f;
                    static_assert(sizeof(f)==sizeof(value));
                    std::memcpy(&f,&value,sizeof(f));
                    t.Float(s.name,f);
                } else {
                    t.Num(s.name,value);
                }
            } else {
                t.Raw(s.name,"null");
            }
        }

        Com<IDirect3DBaseTexture9> tex;
        if(SUCCEEDED(d->GetTexture(slot,&tex.p)) && tex.p) {
            t.Str("texture_object",Pointer(reinterpret_cast<uintptr_t>(tex.p)));
            t.Num("levels",tex.p->GetLevelCount());
            t.Num("resource_min_LOD",tex.p->GetLOD());
            if(tex.p->GetType()==D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC desc{};
                if(SUCCEEDED(static_cast<IDirect3DTexture9*>(tex.p)->GetLevelDesc(0,&desc))) {
                    t.Num("width",desc.Width);
                    t.Num("height",desc.Height);
                    t.Num("format",unsigned(desc.Format));
                }
            }
        }

        t.Str("actual_sampled_mip","UNKNOWN");
        auto key="texture_"+std::to_string(slot);
        j.Raw(key.c_str(),t.End());
    }
}

IDirect3DDevice9* LiveD3D() {
    uint32_t gxDevice=0;
    uint32_t d3d=0;

    if(!ReadBytes(gx::kGxDevicePtr,&gxDevice,sizeof(gxDevice)) || !gxDevice)
        return nullptr;

    if(!Read(uintptr_t(gxDevice),gx::kD3DDeviceField,d3d) || !d3d)
        return nullptr;

    return reinterpret_cast<IDirect3DDevice9*>(uintptr_t(d3d));
}

void ObserveWmoBind(uintptr_t batch,uint32_t vtxIdx,uint32_t pixIdx) {
    Json j;
    j.Str("event","wmo_material_after_native_effect_bind");
    j.Str("correlation","ordering_only_not_proven_object_binding");
    j.Str("source_shader_id_pre_remap","UNAVAILABLE_at_runtime_render_seam");
    j.Str("root",Pointer(currentWmoRoot));
    j.Str("group",Pointer(currentWmoGroup));
    j.Str("moba",Pointer(batch));
    j.Str("path",WmoPath(currentWmoRoot));
    j.Str("wmo_leaf",currentWmoLeaf);
    j.Num("wmo_leaf_flag",currentWmoLeafFlag);
    j.Num("effect_vtx_index",vtxIdx);
    j.Num("effect_pix_index",pixIdx);

    uint8_t mobaFlags=0;
    if(Read(batch,wmo::kOffMobaFlags,mobaFlags))
        j.Num("moba_flags",mobaFlags);
    else
        j.Raw("moba_flags","null");

    Field<uint32_t>(j,"moba_start_index",batch,wmo::kOffMobaStartIndex);
    Field<uint16_t>(j,"moba_index_count",batch,wmo::kOffMobaCount);
    Field<uint16_t>(j,"moba_min_index",batch,wmo::kOffMobaMinIndex);
    Field<uint16_t>(j,"moba_max_index",batch,wmo::kOffMobaMaxIndex);

    const bool modernIndex=(mobaFlags&wmo::kMobaFlagMaterialModern)!=0;
    j.Str("moba_material_index_encoding",modernIndex?"modern_u16_at_0x0A":"stock_u8_at_0x17");

    uint32_t materialIndex=0;
    bool haveMaterialIndex=false;
    if(modernIndex) {
        uint16_t value=0;
        if(Read(batch,wmo::kOffMobaMaterialModern,value)) {
            materialIndex=value;
            haveMaterialIndex=true;
        }
    } else {
        uint8_t value=0;
        if(Read(batch,wmo::kOffMobaMaterial,value)) {
            materialIndex=value;
            haveMaterialIndex=true;
        }
    }

    if(haveMaterialIndex)
        j.Num("runtime_material_index",materialIndex);
    else
        j.Raw("runtime_material_index","null");

    uint32_t groupFlags=0;
    if(Read(currentWmoGroup,wmo::kOffGroupFormatFlags,groupFlags)) {
        j.Num("group_format_flags",groupFlags);
        j.Num("group_has_two_uv",(groupFlags&wmo::kGroupFlagTwoUv)?1:0);
    } else {
        j.Raw("group_format_flags","null");
        j.Raw("group_has_two_uv","null");
    }

    uint32_t materialBase=0,materialCount=0;
    const bool haveBase=Read(currentWmoRoot,wmo::kOffMaterialBase,materialBase) && materialBase;
    const bool haveCount=Read(currentWmoRoot,wmo::kOffMaterialCount,materialCount);

    if(haveCount)
        j.Num("root_material_count",materialCount);
    else
        j.Raw("root_material_count","null");

    if(haveBase && haveCount && haveMaterialIndex && materialIndex<materialCount) {
        const uintptr_t material=uintptr_t(materialBase)+size_t(materialIndex)*wmo::kMomtStride;
        j.Str("runtime_momt",Pointer(material));

        uint32_t flags=0,shaderId=0,blend=0,tex1=0,tex2=0,diffuse=0,h1=0,h2=0;

        if(Read(material,wmo::kOffMomtFlags,flags)) j.Num("momt_flags",flags);
        else j.Raw("momt_flags","null");

        if(Read(material,wmo::kOffMomtShader,shaderId)) j.Num("momt_shader_runtime_id",shaderId);
        else j.Raw("momt_shader_runtime_id","null");

        if(Read(material,wmo::kOffMomtBlend,blend)) j.Num("momt_blend_mode",blend);
        else j.Raw("momt_blend_mode","null");

        if(Read(material,wmo::kOffMomtTexture1,tex1)) j.Num("momt_texture1_ref",tex1);
        else j.Raw("momt_texture1_ref","null");

        if(Read(material,wmo::kOffMomtTexture2,tex2)) j.Num("momt_texture2_ref",tex2);
        else j.Raw("momt_texture2_ref","null");

        if(Read(material,wmo::kOffMomtDiffColor,diffuse)) j.Num("momt_diffuse_bgra",diffuse);
        else j.Raw("momt_diffuse_bgra","null");

        if(Read(material,wmo::kOffMomtHandle1,h1)) j.Str("momt_texture1_handle",Pointer(h1));
        else j.Raw("momt_texture1_handle","null");

        if(Read(material,wmo::kOffMomtHandle2,h2)) j.Str("momt_texture2_handle",Pointer(h2));
        else j.Raw("momt_texture2_handle","null");
    } else {
        j.Raw("runtime_momt","null");
    }

    uint32_t active=0;
    if(ReadBytes(sh::kActiveCollection,&active,sizeof(active)) && active) {
        j.Str("active_effect_collection",Pointer(active));
        const auto selected=SelectedWmoEffect(active);

        j.Num("selected_effect_exterior_index",selected.exterior);
        j.Num("selected_effect_alternate_index",selected.alternate);

        const int familyIndex=selected.exterior>=0 ? selected.exterior : selected.alternate;
        j.Str("selected_effect_family",WmoFamily(familyIndex));

        if(selected.exterior>=0 && selected.alternate>=0)
            j.Str("selected_effect_table","exterior_and_alternate_same_pointer");
        else if(selected.exterior>=0)
            j.Str("selected_effect_table","exterior");
        else if(selected.alternate>=0)
            j.Str("selected_effect_table","alternate");
        else
            j.Str("selected_effect_table","UNKNOWN");
    } else {
        j.Raw("active_effect_collection","null");
        j.Num("selected_effect_exterior_index",-1);
        j.Num("selected_effect_alternate_index",-1);
        j.Str("selected_effect_family","UNKNOWN");
        j.Str("selected_effect_table","UNKNOWN");
    }

    if(auto* d=LiveD3D()) {
        Hardware(j,d,"after_native_WMO_effect_bind");
    } else {
        j.Str("hardware_observation_phase","after_native_WMO_effect_bind");
        j.Raw("hardware_state","null");
    }

    ++counts[4];
    ++windowCounts[4];
    pendingWmoMaterial=Emit(j);
}

char __cdecl HookWmoCull(void* mobaRecord) {
    const char native=originalWmoCull(mobaRecord);
    pendingWmoBatch=0;

    if(native==0 && mobaRecord && currentWmoRoot && currentWmoGroup && Active(4))
        pendingWmoBatch=reinterpret_cast<uintptr_t>(mobaRecord);

    return native;
}

void __cdecl HookWmoEffectBind(uint32_t vtxIdx,uint32_t pixIdx) {
    const uintptr_t batch=pendingWmoBatch;
    pendingWmoBatch=0;

    originalWmoEffectBind(vtxIdx,pixIdx);

    pendingWmoMaterial=0;
    if(!batch || !currentWmoRoot || !currentWmoGroup || !Active(4))
        return;

    try {
        ObserveWmoBind(batch,vtxIdx,pixIdx);
    } catch(...) {
        pendingWmoMaterial=0;
        ++errors;
    }
}

void __fastcall HookWmoExt(void* root,void* edx,void* group,int flag) {
    const auto prevRoot=currentWmoRoot;
    const auto prevGroup=currentWmoGroup;
    const auto prevLeaf=currentWmoLeaf;
    const auto prevFlag=currentWmoLeafFlag;
    const auto prevBatch=pendingWmoBatch;
    const auto prevMaterial=pendingWmoMaterial;

    currentWmoRoot=reinterpret_cast<uintptr_t>(root);
    currentWmoGroup=reinterpret_cast<uintptr_t>(group);
    currentWmoLeaf="exterior";
    currentWmoLeafFlag=flag;
    pendingWmoBatch=0;
    pendingWmoMaterial=0;

    originalWmoExt(root,edx,group,flag);

    pendingWmoBatch=prevBatch;
    pendingWmoMaterial=prevMaterial;
    currentWmoRoot=prevRoot;
    currentWmoGroup=prevGroup;
    currentWmoLeaf=prevLeaf;
    currentWmoLeafFlag=prevFlag;
}

void __fastcall HookWmoInt(void* root,void* edx,void* group,int flag) {
    const auto prevRoot=currentWmoRoot;
    const auto prevGroup=currentWmoGroup;
    const auto prevLeaf=currentWmoLeaf;
    const auto prevFlag=currentWmoLeafFlag;
    const auto prevBatch=pendingWmoBatch;
    const auto prevMaterial=pendingWmoMaterial;

    currentWmoRoot=reinterpret_cast<uintptr_t>(root);
    currentWmoGroup=reinterpret_cast<uintptr_t>(group);
    currentWmoLeaf="interior_or_segmented";
    currentWmoLeafFlag=flag;
    pendingWmoBatch=0;
    pendingWmoMaterial=0;

    originalWmoInt(root,edx,group,flag);

    pendingWmoBatch=prevBatch;
    pendingWmoMaterial=prevMaterial;
    currentWmoRoot=prevRoot;
    currentWmoGroup=prevGroup;
    currentWmoLeaf=prevLeaf;
    currentWmoLeafFlag=prevFlag;
}


void ObserveM2Batch(void* ctx,const char* route) {
    if(!ctx || !Active(1))
        return;

    const uintptr_t p=reinterpret_cast<uintptr_t>(ctx);
    uint32_t instance=0;
    uint32_t element=0;

    if(!Read(p,gx::kDrawBatchCtxModelField,instance) ||
       !Read(p,gx::kDrawBatchCtxElementField,element) ||
       !instance || !element)
        return;

    Json j;
    j.Str("event","m2_batch_after_native_draw");
    j.Str(
        "correlation",
        "direct_native_M2_batch_context_post_call_not_final_device_ownership"
    );
    j.Str("route",route);

    if(!Instance(j,instance))
        return;

    j.Str("element",Pointer(element));

    Field<uint32_t>(
        j,
        "requested_co_instance_run",
        element,
        gx::kM2ElementRunLengthField
    );

    Field<uint32_t>(
        j,
        "batch_index",
        element,
        m2::kOffElementBatchIndex
    );

    uint32_t section=0;
    if(Read(element,gx::kM2ElementSectionField,section) && section) {
        j.Str("section",Pointer(section));

        wxl::structure::m2::M2SkinSection sec{};
        if(ReadBytes(section,&sec,sizeof(sec))) {
            j.Num("section_id",sec.skinSectionId);
            j.Num("section_level_raw",sec.level);
            j.Num("section_vertices",sec.vertexCount);
            j.Num("section_indices",sec.indexCount);
            j.Num("section_bones",sec.boneCount);
        }
    } else {
        j.Raw("section","null");
    }

    if(auto* d=LiveD3D()) {
        Hardware(j,d,"after_native_M2_batch");
    } else {
        j.Str("hardware_observation_phase","after_native_M2_batch");
        j.Raw("hardware_state","null");
    }

    ++counts[1];
    ++windowCounts[1];
    Emit(j);
}

void __fastcall HookM2TriangleBatch(void* ctx,void* edx) {
    originalM2TriangleBatch(ctx,edx);

    try {
        ObserveM2Batch(ctx,"triangle_batch");
    } catch(...) {
        ++errors;
    }
}

void __fastcall HookM2DoodadBatch(
    void* ctx,
    void* edx,
    void* elements,
    void* indices
) {
    originalM2DoodadBatch(ctx,edx,elements,indices);

    try {
        ObserveM2Batch(ctx,"batched_doodad");
    } catch(...) {
        ++errors;
    }
}

void __fastcall HookDraw(void* device,void* edx,uint32_t* batch,int indexed) {
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());

    if(ready && sceneDepth &&
       GetCurrentThreadId()==renderThread &&
       config.Samples(frame)) {
        ++rawGxDrawCallsWindow;
        if(pendingM2Material)
            ++rawGxDrawM2TokenWindow;
        if(pendingWmoMaterial)
            ++rawGxDrawWmoTokenWindow;
    }

    // Both tokens are ordering correlations only. Never promote either to
    // one-to-one draw ownership without separate proof.
    const uint64_t m2Token=pendingM2Material;
    const uint64_t wmoToken=pendingWmoMaterial;
    pendingM2Material=0;
    pendingWmoMaterial=0;

    originalDraw(device,edx,batch,indexed);

    if(m2Token && Active(1)) {
        try {
            Json j;
            j.Str("event","first_Gx_draw_after_material");
            j.Num("material_sequence",m2Token);
            j.Str("correlation","ordering_only_not_proven_object_binding");
            j.Str("native_draw_caller",Pointer(caller));
            j.Str("route","UNKNOWN_use_native_caller_and_run_record");
            j.Num("indexed",indexed);
            Field<uint32_t>(j,"primitive_type",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchPrimType);
            Field<uint32_t>(j,"start_index",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchStartIndex);
            Field<uint32_t>(j,"index_count",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchIndexCount);
            uint32_t raw=0;
            if(Read(reinterpret_cast<uintptr_t>(device),gx::kD3DDeviceField,raw) && raw)
                Hardware(
                    j,
                    reinterpret_cast<IDirect3DDevice9*>(raw),
                    "after_native_GxDeviceDraw"
                );
            ++counts[1];
            ++windowCounts[1];
            Emit(j);
        } catch(...) {
            ++errors;
        }
    }

    if(wmoToken && Active(5)) {
        try {
            Json j;
            j.Str("event","first_Gx_draw_after_wmo_effect_bind");
            j.Num("wmo_material_sequence",wmoToken);
            j.Str("correlation","ordering_only_not_proven_object_binding");
            j.Str("native_draw_caller",Pointer(caller));
            j.Str("route","WMO_ordering_correlation");
            j.Num("indexed",indexed);
            Field<uint32_t>(j,"primitive_type",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchPrimType);
            Field<uint32_t>(j,"start_index",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchStartIndex);
            Field<uint32_t>(j,"index_count",reinterpret_cast<uintptr_t>(batch),gx::kGxBatchIndexCount);
            uint32_t raw=0;
            if(Read(reinterpret_cast<uintptr_t>(device),gx::kD3DDeviceField,raw) && raw)
                Hardware(
                    j,
                    reinterpret_cast<IDirect3DDevice9*>(raw),
                    "after_native_GxDeviceDraw"
                );
            ++counts[5];
            ++windowCounts[5];
            Emit(j);
        } catch(...) {
            ++errors;
        }
    }
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
    if(!sceneDepth) {
        renderThread=GetCurrentThreadId();
        ++frame;
        pendingM2Material=0;
        pendingWmoMaterial=0;
        pendingWmoBatch=0;
        windowCounts={};
        rawGxDrawCallsWindow=0;
        rawGxDrawM2TokenWindow=0;
        rawGxDrawWmoTokenWindow=0;
    }
    ++sceneDepth;
    if(config.Samples(frame) && sceneDepth==1) {
        try {Json j;j.Str("event","sample_begin");Bands(j);Emit(j);++windows;}catch(...){++errors;}
    }
}
void End(void*,const void*) {
    if(!sceneDepth) return;
    --sceneDepth;
    if(sceneDepth) return;
    pendingM2Material=0;
    pendingWmoMaterial=0;
    pendingWmoBatch=0;
    if(config.Samples(frame)) {
        try {
            Json j;
            j.Str("event","sample_end");
            j.Num("windows",windows);
            j.Num("capture_exceptions",errors);
            j.Num("raw_gx_device_draw_calls",rawGxDrawCallsWindow);
            j.Num("raw_gx_device_draw_m2_token_calls",rawGxDrawM2TokenWindow);
            j.Num("raw_gx_device_draw_wmo_token_calls",rawGxDrawWmoTokenWindow);
            for(unsigned i=0;i<6;++i) {
                auto key="records_"+std::to_string(i);
                j.Num(key.c_str(),counts[i]);
            }
            Emit(j);
            sink.flush();
        } catch(...) {
            ++errors;
        }
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

        // Direct M2 draw-route observers. These are outer read-only wrappers
        // around the native/WXL batch chain. The triangle route already has
        // the production wide-index hook at normal priority; -1000 observes
        // after that complete chain returns without replacing its behaviour.
        ok &= wxl::hook::Install(
            "R8MaterialM2TriangleBatch",
            gx::kDrawTriangleBatch,
            &HookM2TriangleBatch,
            &originalM2TriangleBatch,
            -1000
        );
        ok &= wxl::hook::Install(
            "R8MaterialM2DoodadBatch",
            gx::kDrawBatchDoodad,
            &HookM2DoodadBatch,
            &originalM2DoodadBatch,
            -1000
        );

        ok &= wxl::hook::Install("R7OutlineReadiness",m2::kIsDrawable,&HookReady,&originalReady,-1000);
        ok &= wxl::hook::Install("R7OutlineBatch",m2::kIsBatchDoodadCompatible,&HookBatch,&originalBatch,-1000);

        // Step 10C adds READ-ONLY WMO observation around the native batch chain.
        // Priority -1000 means each observer calls through any normal-priority
        // renderer hook first and inspects the resulting state/result afterwards.
        ok &= wxl::hook::Install("R8MaterialWmoCull",wmo::kCullBatch,&HookWmoCull,&originalWmoCull,-1000);
        ok &= wxl::hook::Install("R8MaterialWmoExt",wmo::kExtRender,&HookWmoExt,&originalWmoExt,-1000);
        ok &= wxl::hook::Install("R8MaterialWmoInt",wmo::kIntRender,&HookWmoInt,&originalWmoInt,-1000);
        ok &= wxl::hook::Install("R8MaterialWmoEffectBind",sh::kEffectBind,&HookWmoEffectBind,&originalWmoEffectBind,-1000);

        if(!ok) return false;
        ev::Subscribe(ev::Event::OnWorldSceneBegin,&Begin,nullptr);
        ev::Subscribe(ev::Event::OnWorldSceneEnd,&End,nullptr);
        ready=true;
        Json j;j.Str("event","identity");j.Str("wow_sha256",kExeSha);j.Str("mode","read_only");
        j.Str("object_draw_binding","ordering_correlation_requires_validation");
        j.Str("wmo_draw_binding","ordering_correlation_requires_validation");
        j.Str("step10c_material_observer","enabled");
        j.Str("step10c04_direct_hardware_bridge","enabled");
        j.Num("start",config.start);j.Num("stride",config.stride);j.Num("windows",config.windows);Emit(j);sink.flush();
        WLOG_INFO("r7-outline: read-only capture %s start=%u stride=%u windows=%u",file.c_str(),config.start,config.stride,config.windows);
        return true;
    } catch(...) {ready=false;WLOG_ERROR("r7-outline: setup exception; OFF");return true;}
}
WXL_REGISTER_FEATURE("r7-distant-outline-diagnostics",true,Install);
}
}
