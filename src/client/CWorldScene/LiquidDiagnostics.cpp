// R6: balanced liquid contexts in the existing MinHook chain. GPL-3.0-or-later.
#include "client/CWorldScene/LiquidDiagnostics.hpp"
#include "engine/hook/Hook.hpp"
#include "offsets/engine/Liquid.hpp"
#include "offsets/engine/Gx.hpp"
#include "common/Log.hpp"
#include <windows.h>
#include <atomic>

namespace wxl::waterdiag::native {
namespace {
namespace liq=wxl::offsets::engine::liquid;
namespace gx=wxl::offsets::engine::gx;
thread_local Context* current=nullptr;
std::atomic<uint64_t> sequence{0};
Boundary boundary=nullptr;
gx::LiquidRenderPassFn passOriginal=nullptr;
liq::MaterialWaterRenderFn waterOriginal=nullptr,noSpecOriginal=nullptr,procOriginal=nullptr,magmaOriginal=nullptr;
liq::GeomGetBuffersFn terrainOriginal=nullptr,wmoOriginal=nullptr;
struct Scope {Context* previous;Scope(Context& c):previous(current){current=&c;}~Scope(){current=previous;}};
void __fastcall Pass(void* bank,void* edx,void* transform,int pass) {
    Context c;c.invocation=++sequence;c.depth=current?current->depth+1:1;c.pass=pass;c.bank=bank;c.transform=transform;
    if(bank&&pass>=0&&pass<2)Read(static_cast<const char*>(bank)+size_t(pass)*16+4,&c.instances,sizeof(c.instances));
    Scope scope(c);if(boundary)boundary(true,c);
    struct End {const Context& c;~End(){if(boundary)boundary(false,c);}} end{c};
    passOriginal(bank,edx,transform,pass);
}
void Material(Family family,liq::MaterialWaterRenderFn original,void* self,void* edx,void* env,void* geom,
              void* anim,const float* camera,const float* world,const float* sphere,void* settings) {
    if(!current) {original(self,edx,env,geom,anim,camera,world,sphere,settings);return;}
    Context c=*current;c.family=family;c.provider=Provider::Unknown;c.material=self;c.geometry=geom;
    c.animation=anim;c.camera=camera;c.world=world;c.sphere=sphere;c.settings=settings;
    c.vsWrites.fill(0);c.psWrites.fill(0);c.vsWriteCalls=c.psWriteCalls=c.invalidWriteRanges=0;
    Scope scope(c);original(self,edx,env,geom,anim,camera,world,sphere,settings);
}
#define R6_MATERIAL(name,family,original) \
void __fastcall name(void* self,void* edx,void* env,void* geom,void* anim,const float* camera,const float* world,const float* sphere,void* settings) { \
    Material(Family::family,original,self,edx,env,geom,anim,camera,world,sphere,settings); }
R6_MATERIAL(Water,Water,waterOriginal)
R6_MATERIAL(NoSpec,WaterNoSpec,noSpecOriginal)
R6_MATERIAL(Proc,ProcWater,procOriginal)
R6_MATERIAL(Magma,Magma,magmaOriginal)
#undef R6_MATERIAL
int __fastcall Terrain(void* self,void* edx,int fmt,void** vb,void** ib,void* desc) {
    if(current&&current->geometry==self)current->provider=Provider::Terrain;
    return terrainOriginal(self,edx,fmt,vb,ib,desc);
}
int __fastcall Wmo(void* self,void* edx,int fmt,void** vb,void** ib,void* desc) {
    if(current&&current->geometry==self)current->provider=Provider::Wmo;
    return wmoOriginal(self,edx,fmt,vb,ib,desc);
}
}
bool Read(const void* address,void* output,size_t bytes) noexcept {
    SIZE_T n=0;return address&&ReadProcessMemory(GetCurrentProcess(),address,output,bytes,&n)&&n==bytes;
}
const Context* Current() noexcept {return current;}
void FloatWrite(bool pixel,unsigned first,unsigned count) noexcept {
    if(!current || current->family==Family::Unknown)return;
    if(pixel){++current->psWriteCalls;if(!MarkRange(current->psWrites,first,count))++current->invalidWriteRanges;}
    else {++current->vsWriteCalls;if(!MarkRange(current->vsWrites,first,count))++current->invalidWriteRanges;}
}
bool Install(Boundary cb) {
    struct Gate {uintptr_t address;std::array<uint8_t,16> bytes;};
    static const Gate gates[]={
        {gx::kLiquidRenderPass, {0x55, 0x8b, 0xec, 0x81, 0xec, 0x18, 0x01, 0x00, 0x00, 0x53, 0x56, 0x57, 0x8b, 0xd9, 0x8d, 0xb5}},
        {liq::kMaterialWaterRender, {0x55, 0x8b, 0xec, 0x81, 0xec, 0x7c, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8b, 0x75, 0x20, 0x6a, 0x01}},
        {liq::kMaterialWaterNoSpecRender, {0x55, 0x8b, 0xec, 0x81, 0xec, 0x7c, 0x01, 0x00, 0x00, 0x53, 0x56, 0x8b, 0x75, 0x20, 0x6a, 0x01}},
        {liq::kMaterialProcWaterRender, {0x55, 0x8b, 0xec, 0x81, 0xec, 0xc0, 0x01, 0x00, 0x00, 0x57, 0x8b, 0x7d, 0x20, 0x68, 0xe2, 0x04}},
        {liq::kMaterialMagmaRender, {0x55, 0x8b, 0xec, 0x81, 0xec, 0x74, 0x01, 0x00, 0x00, 0x53, 0x57, 0x8b, 0x7d, 0x20, 0x68, 0xe2}},
        {liq::kChunkGeomGetBuffers, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x74, 0x53, 0x8b, 0xd9, 0x8b, 0x4b, 0x1c, 0x57, 0x33, 0xff, 0x3b}},
        {liq::kMeshGeomGetBuffers, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x70, 0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x46, 0x0c, 0x0f, 0xb7, 0x90}},
    };
    for(const auto& g:gates) {std::array<uint8_t,16> actual{};
        if(!Read(reinterpret_cast<const void*>(g.address),actual.data(),actual.size())||actual!=g.bytes){
            WLOG_ERROR("r6-water: native hook byte gate failed at %08X; diagnostics disabled",unsigned(g.address));return false;}}
    boundary=cb;bool ok=true;
    ok &= wxl::hook::Install("R6.LiquidScope",gx::kLiquidRenderPass,&Pass,&passOriginal,100);
    ok &= wxl::hook::Install("R6.WaterContext",liq::kMaterialWaterRender,&Water,&waterOriginal,100);
    ok &= wxl::hook::Install("R6.NoSpecContext",liq::kMaterialWaterNoSpecRender,&NoSpec,&noSpecOriginal,100);
    ok &= wxl::hook::Install("R6.ProcWaterContext",liq::kMaterialProcWaterRender,&Proc,&procOriginal,100);
    ok &= wxl::hook::Install("R6.MagmaContext",liq::kMaterialMagmaRender,&Magma,&magmaOriginal,100);
    ok &= wxl::hook::Install("R6.TerrainLiquid",liq::kChunkGeomGetBuffers,&Terrain,&terrainOriginal,100);
    ok &= wxl::hook::Install("R6.WmoLiquid",liq::kMeshGeomGetBuffers,&Wmo,&wmoOriginal,100);
    if(!ok)boundary=nullptr;return ok;
}
}
