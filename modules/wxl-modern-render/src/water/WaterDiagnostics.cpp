// R6 native-preserving liquid diagnostics. No replacement or scene-copy path.
// GPL-3.0-or-later. All D3D inspection is confined to the native render thread.
#include "water/WaterDiagCore.hpp"
#include "client/CWorldScene/LiquidDiagnostics.hpp"
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

namespace wxl::waterdiag {
namespace {
namespace ev=wxl::events;
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
using DipFn=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using ConstantFn=HRESULT (WINAPI*)(IDirect3DDevice9*,UINT,const float*,UINT);
struct Chain {void** table=nullptr;DipFn dip=nullptr;ConstantFn vs=nullptr,ps=nullptr;};
std::array<Chain,4> chains{};
thread_local bool inspecting=false;

Json Record(const char* event){Json j;j.Num("schema",1);j.Str("event",event);j.Num("frame",frame);j.Num("generation",generation);j.Num("sequence",++sequence);j.Num("world_epoch",worldEpoch);j.Num("view",view);j.Str("phase",phase);return j;}
bool Enqueue(Json j){auto line=j.End()+'\n';if(line.size()>kQueueLimit-pendingBytes){++outputBudget.dropped;return false;}
    if(!outputBudget.Take(line.size()))return false;pendingBytes+=line.size();pending.push_back(std::move(line));return true;}
void Flush(){size_t n=0;while(!pending.empty()&&n+pending.front().size()<=kFlushLimit){auto& s=pending.front();sink.write(s.data(),std::streamsize(s.size()));n+=s.size();pendingBytes-=s.size();pending.pop_front();}
    sink.flush();if(!sink){quarantined=true;if(!errorLogged){WLOG_ERROR("r6-water: capture output failed; native rendering continues");errorLogged=true;}}}
void Failure() noexcept {quarantined=true;++captureErrors;if(!errorLogged){WLOG_ERROR("r6-water: diagnostics quarantined after capture failure; native calls remain unchanged");errorLogged=true;}}
bool CanInspect(IDirect3DDevice9* d) noexcept{return ready&&!lost&&!quarantined&&d==device&&GetCurrentThreadId()==renderThread;}
void ReleaseShaders() noexcept{for(auto& s:shaders)if(s.object)s.object->Release();shaders.clear();}

std::string Surface(IDirect3DSurface9* s,HRESULT query){Json j;j.Num("get_hr",int32_t(query));j.Bool("bound",s!=nullptr);
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
void Capture(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT min,UINT vertices,UINT start,UINT primitives,const native::Context& c){
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
    auto j=Record("draw");j.Num("invocation",c.invocation);j.Num("scope_depth",c.depth);j.Num("pass",c.pass);j.Num("instances",c.instances);
    j.Str("material_class",Name(c.family));j.Num("selector_by_verified_class",Selector(c.family));j.Str("provider",Name(c.provider));
    j.Str("material_settings_sha256",Hex(key.material));
    j.Str("classification",Classification(true,c.family,c.provider,vi.valid,pi.valid));j.Bool("replacement_allowed",false);
    j.Str("material_token",Pointer(c.material));j.Str("settings_token",Pointer(c.settings));j.Str("geometry_token",Pointer(c.geometry));j.Str("animation_token",Pointer(c.animation));
    j.Num("vs_get_hr",int32_t(vhr));j.Num("ps_get_hr",int32_t(phr));j.Num("vs_id",vi.id);j.Num("ps_id",pi.id);
    j.Str("vs_sha256",vi.valid?Hex(vi.hash):"UNKNOWN");j.Str("ps_sha256",pi.valid?Hex(pi.hash):"UNKNOWN");
    j.Num("declaration_get_hr",int32_t(dh));j.Num("declaration_data_hr",int32_t(de));j.Str("declaration",declaration);j.Str("declaration_sha256",Hex(key.declaration));
    j.Num("primitive_type",unsigned(type));j.Num("base_vertex",base);j.Num("min_vertex",min);j.Num("vertices",vertices);j.Num("start_index",start);j.Num("primitives",primitives);
    auto states=RenderStates(d);j.Raw("render_states",states);j.Str("render_state_sha256",Hex(Sha256::Of(states.data(),states.size())));
    j.Raw("targets",Targets(d));j.Raw("streams",Streams(d));Com<IDirect3DIndexBuffer9> ib;HRESULT ih=d->GetIndices(ib.Out());j.Num("indices_hr",int32_t(ih));j.Str("indices_token",Pointer(ib.p));
    if(ib.p){D3DINDEXBUFFER_DESC id{};HRESULT q=ib.p->GetDesc(&id);j.Num("index_desc_hr",int32_t(q));if(SUCCEEDED(q)){j.Num("index_format",unsigned(id.Format));j.Num("index_bytes",id.Size);}}
    if(config.mode==Mode::Full){j.Raw("textures",Textures(d));Constants(j,d,c,pi);NativeBytes(j,c);}
    j.Str("final_boundary","chained DIP before forwarding; no R6 state setters; downstream owners recorded at install");Enqueue(std::move(j));
}

Chain* Find(IDirect3DDevice9* d) noexcept{void** v=*reinterpret_cast<void***>(d);for(auto& c:chains)if(c.table==v)return &c;return nullptr;}
HRESULT WINAPI Dip(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT min,UINT vertices,UINT start,UINT primitives){
    Chain* chain=Find(d);if(!chain||!chain->dip)return D3DERR_INVALIDCALL; // Installed only on retained table entries.
    const auto* c=native::Current();bool eligible=c&&!inspecting&&CanInspect(d);
    LARGE_INTEGER a{},b{};
    // Capture exceptions cannot suppress or duplicate the original submission.
    return ObserveThenForward(eligible,[&]{
        ++observed;++familyCounts[unsigned(c->family)];QueryPerformanceCounter(&a);
        InspectionScope guard(inspecting);
        if(config.mode!=Mode::Timing&&profiles.total<config.maxDraws&&outputBudget.used<outputBudget.limit)
            Capture(d,type,base,min,vertices,start,primitives,*c);
        QueryPerformanceCounter(&b);diagTicks+=uint64_t(b.QuadPart-a.QuadPart);
    },[&]{
    if(eligible&&config.mode==Mode::Timing)QueryPerformanceCounter(&a);
    const HRESULT hr=chain->dip(d,type,base,min,vertices,start,primitives);
    if(eligible){++submitted;if(FAILED(hr))++nativeFailures;if(config.mode==Mode::Timing){QueryPerformanceCounter(&b);submitTicks+=uint64_t(b.QuadPart-a.QuadPart);}}
    return hr;
    },[]{Failure();});
}
HRESULT WINAPI VSFloat(IDirect3DDevice9* d,UINT first,const float* data,UINT count){auto* c=Find(d);if(!c||!c->vs)return D3DERR_INVALIDCALL;
    HRESULT hr=c->vs(d,first,data,count);if(SUCCEEDED(hr)&&native::Current()&&CanInspect(d))native::FloatWrite(false,first,count);return hr;}
HRESULT WINAPI PSFloat(IDirect3DDevice9* d,UINT first,const float* data,UINT count){auto* c=Find(d);if(!c||!c->ps)return D3DERR_INVALIDCALL;
    HRESULT hr=c->ps(d,first,data,count);if(SUCCEEDED(hr)&&native::Current()&&CanInspect(d))native::FloatWrite(true,first,count);return hr;}
bool Swap(void** slot,void* hook,void** original){if(*slot==hook)return true;DWORD p=0;if(!VirtualProtect(slot,sizeof(void*),PAGE_EXECUTE_READWRITE,&p))return false;
    *original=*slot;InterlockedExchangePointer(slot,hook);DWORD ignored=0;return VirtualProtect(slot,sizeof(void*),p,&ignored)!=FALSE;}
void Device(IDirect3DDevice9* d){
    if(!d)return;if(device==d)return;
    if(device)ReleaseShaders();device=d;renderThread=GetCurrentThreadId();lost=false;++generation;
    void** v=*reinterpret_cast<void***>(d);Chain* c=Find(d);if(!c){for(auto& x:chains)if(!x.table){c=&x;break;}}
    if(!c){quarantined=true;return;}
    if(!c->table){c->table=v;
        bool ok=Swap(&v[82],reinterpret_cast<void*>(&Dip),reinterpret_cast<void**>(&c->dip));
        if(config.mode==Mode::Full){ok &= Swap(&v[94],reinterpret_cast<void*>(&VSFloat),reinterpret_cast<void**>(&c->vs));ok &= Swap(&v[109],reinterpret_cast<void*>(&PSFloat),reinterpret_cast<void**>(&c->ps));}
        if(!ok){quarantined=true;WLOG_ERROR("r6-water: device tap incomplete; observation disabled, installed wrappers retain original forwards");}}
    auto j=Record("device");j.Str("reason","first observed device or successful reset");j.Bool("dip_slot_is_r6",v[82]==reinterpret_cast<void*>(&Dip));j.Str("device_token",Pointer(d));j.Str("previous_dip",Pointer(reinterpret_cast<void*>(c->dip)));j.Raw("previous_dip_owner",Owner(reinterpret_cast<void*>(c->dip)));
    j.Str("previous_vs_float",Pointer(reinterpret_cast<void*>(c->vs)));j.Str("previous_ps_float",Pointer(reinterpret_cast<void*>(c->ps)));j.Bool("quarantined",quarantined);
    D3DCAPS9 caps{};HRESULT hr=d->GetDeviceCaps(&caps);j.Num("caps_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("vertex_shader_version",caps.VertexShaderVersion);j.Num("pixel_shader_version",caps.PixelShaderVersion);j.Num("max_vs_float4",caps.MaxVertexShaderConst);j.Num("max_streams",caps.MaxStreams);j.Num("max_textures",caps.MaxSimultaneousTextures);}
    D3DDEVICE_CREATION_PARAMETERS creation{};hr=d->GetCreationParameters(&creation);j.Num("creation_parameters_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("behavior_flags",creation.BehaviorFlags);j.Num("adapter",creation.AdapterOrdinal);}
    Com<IDirect3DSwapChain9> sc;hr=d->GetSwapChain(0,sc.Out());if(SUCCEEDED(hr)&&sc.p){D3DPRESENT_PARAMETERS pp{};hr=sc.p->GetPresentParameters(&pp);j.Num("present_parameters_hr",int32_t(hr));if(SUCCEEDED(hr)){j.Num("backbuffer_width",pp.BackBufferWidth);j.Num("backbuffer_height",pp.BackBufferHeight);j.Num("msaa",unsigned(pp.MultiSampleType));j.Num("msaa_quality",pp.MultiSampleQuality);j.Bool("windowed",pp.Windowed!=FALSE);}}
    j.Bool("owned_render_targets",false);j.Bool("real_scene_copy_supported",false);Enqueue(std::move(j));
    WLOG_INFO("r6-water: read-only DIP tap active generation=%llu mode=%u; no replacement/copy",static_cast<unsigned long long>(generation),unsigned(config.mode));
}
void Boundary(bool begin,const native::Context& c) noexcept {if(!CanInspect(device)||config.mode==Mode::Timing||boundaryRecords>=128)return;
    try{auto targets=Targets(device);ProfileKey k;k.pass=c.pass;k.vs=Sha256::Of(targets.data(),targets.size());std::string discriminator=std::to_string(worldEpoch)+':'+(begin?"begin":"end");k.ps=Sha256::Of(discriminator.data(),discriminator.size());
        if(!boundaries.Admit(k,frame,4,128))return;
        auto j=Record("boundary");j.Str("point",begin?"liquid_begin":"liquid_end");j.Num("invocation",c.invocation);j.Num("pass",c.pass);j.Num("instances",c.instances);j.Num("scope_depth",c.depth);j.Raw("targets",targets);
        j.Str("ordering_limit","Before/after this native queue only; opaque completion and prior liquid writes unresolved");Enqueue(std::move(j));++boundaryRecords;
    }catch(...){Failure();}}
void Summary(){auto j=Record("summary");j.Num("liquid_dips_observed",observed);j.Num("native_dips_forwarded",submitted);j.Num("native_failures",nativeFailures);
    j.Num("profiles",profiles.size);j.Num("draw_records_admitted",profiles.total);j.Num("shader_objects_held",shaders.size());j.Num("output_bytes_admitted",outputBudget.used);j.Num("dropped_records",outputBudget.dropped);
    j.Num("pending_bytes",pendingBytes);j.Num("diagnostic_cpu_ticks",diagTicks);j.Num("native_submission_cpu_ticks",submitTicks);j.Num("qpc_frequency",frequency.QuadPart);j.Num("capture_errors",captureErrors);
    j.Num("unknown_draws",familyCounts[0]);j.Num("water_draws",familyCounts[1]);j.Num("nospec_draws",familyCounts[2]);j.Num("procwater_draws",familyCounts[3]);j.Num("magma_draws",familyCounts[4]);
    j.Bool("quarantined",quarantined);j.Str("timing_limit","CPU submission/inspection only; no GPU timing or live performance claim");Enqueue(std::move(j));}
void OnEndScene(void*,const void* a){if(!ready||lost||quarantined)return;try{Device(static_cast<IDirect3DDevice9*>(static_cast<const ev::EndSceneArgs*>(a)->device));}catch(...){Failure();}}
void OnFrame(void*,const void*){if(!ready||GetCurrentThreadId()!=renderThread)return;try{if(frame%300==0)Summary();Flush();++frame;}catch(...){Failure();}}
void OnLost(void*,const void*){if(!ready)return;lost=true;try{auto j=Record("lost");Enqueue(std::move(j));Summary();Flush();}catch(...){Failure();}ReleaseShaders();device=nullptr;}
void OnReset(void*,const void* a){if(!ready)return;try{lost=false;profiles.NewGeneration();boundaries.NewGeneration();
    // Capture ceilings are process-wide: keep total admitted count across resets.
    Device(static_cast<IDirect3DDevice9*>(static_cast<const ev::DeviceResetArgs*>(a)->device));auto j=Record("reset");j.Bool("success",true);Enqueue(std::move(j));}catch(...){Failure();}}
void OnWorldLeave(void*,const void*){if(!ready)return;try{auto j=Record("world_leave");Enqueue(std::move(j));Summary();Flush();++worldEpoch;}catch(...){Failure();}ReleaseShaders();}
void OnWorld(void*,const void*){if(!ready)return;++view;phase="world_begin";}
void OnSceneEnd(void*,const void*){if(ready)phase="world_scene_end_after_prior_subscribers";}
void OnWorldEnd(void*,const void*){if(ready)phase="world_end_after_prior_subscribers";}
bool Identity(){wchar_t path[MAX_PATH]{};DWORD n=GetModuleFileNameW(nullptr,path,MAX_PATH);if(!n||n>=MAX_PATH)return false;
    std::ifstream f(std::filesystem::path(path),std::ios::binary);if(!f)return false;Sha256 hash;std::array<char,65536> b{};while(f){f.read(b.data(),b.size());auto count=f.gcount();if(count>0)hash.Update(b.data(),size_t(count));}
    return f.eof()&&Hex(hash.Finish())==kExeSha&&reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))==0x400000;
}
bool Install(){
    config=ParseConfig([](const char* n){return std::getenv(n);});if(!config.enabled){if(!config.valid)WLOG_WARN("r6-water: invalid diagnostic flags; OFF");return true;}
    try{if(!Identity()){WLOG_ERROR("r6-water: supplied Wow.exe SHA256/base gate failed; OFF");return true;}
        CreateDirectoryA("Logs",nullptr);capturePath="Logs\\r6-water-"+std::to_string(GetCurrentProcessId())+'-'+std::to_string(GetTickCount64())+".ndjson";
        sink.open(capturePath,std::ios::binary|std::ios::out|std::ios::trunc);if(!sink){WLOG_ERROR("r6-water: output unavailable; OFF");return true;}
        outputBudget.limit=size_t(config.maxMiB)*1024*1024-4096;QueryPerformanceFrequency(&frequency);shaders.reserve(128);
        if(!native::Install(&Boundary)){sink.close();return true;}
        ev::Subscribe(ev::Event::OnEndScene,&OnEndScene,nullptr);ev::Subscribe(ev::Event::OnFrame,&OnFrame,nullptr);
        ev::Subscribe(ev::Event::OnDeviceLost,&OnLost,nullptr);ev::Subscribe(ev::Event::OnDeviceReset,&OnReset,nullptr);
        ev::Subscribe(ev::Event::OnWorldLeave,&OnWorldLeave,nullptr);ev::Subscribe(ev::Event::OnWorldRender,&OnWorld,nullptr);
        ev::Subscribe(ev::Event::OnWorldSceneEnd,&OnSceneEnd,nullptr);ev::Subscribe(ev::Event::OnWorldRenderEnd,&OnWorldEnd,nullptr);
        ready=true;auto j=Record("session");j.Str("exe_sha256",kExeSha);j.Num("mode",unsigned(config.mode));j.Num("samples_per_profile",config.samples);j.Num("max_draws",config.maxDraws);j.Num("max_mib",config.maxMiB);
        j.Bool("copy_requested",config.copyRequested);j.Bool("copy_supported",false);j.Bool("replacement_supported",false);
        j.Str("copy_gate","Native active-scene bracketing not proven; no EndScene/BeginScene/StretchRect called");
        j.Str("classic_target_controls","water 0..3; ripple 0..2; reflection 0 screen,1 sky,2 sky+terrain,3 sky+terrain+WMO; not Wrath live setting values");
        Enqueue(std::move(j));Flush();WLOG_INFO("r6-water: native-preserving diagnostic enabled; output=%s",capturePath.c_str());
        if(config.copyRequested)WLOG_WARN("r6-water: COPY requested but unsupported; scene-boundary evidence only; native rendering unchanged");
    }catch(...){ready=false;Failure();}return true;
}
// All pending records are normally flushed at Present/world leave. At process shutdown,
// close CPU output only: never call D3D or mutate hook tables while the loader lock is held.
struct Shutdown {
    ~Shutdown(){if(!ready||!sink.is_open())return;try{auto j=Record("shutdown");j.Num("pending_records_not_flushed",pending.size());j.Num("dropped_records",outputBudget.dropped);auto s=j.End()+'\n';sink.write(s.data(),std::streamsize(s.size()));sink.flush();}catch(...){}}
} shutdown;
}
}
WXL_REGISTER_FEATURE("r6-water-diagnostics",true,wxl::waterdiag::Install)
