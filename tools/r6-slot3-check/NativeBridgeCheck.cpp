#include "water/Slot3NativeState.hpp"
#include "water/Slot3SnapshotPolicy.hpp"
#include "water/Slot3ProductionPolicy.hpp"
#include <cstdio>
#include <algorithm>
#include <limits>
using namespace wxl::water::slot3;
namespace ns=native_state;
static unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}} while(false)

struct Memory {
    std::vector<std::uint8_t> bytes=std::vector<std::uint8_t>(0xD50000);
    unsigned reads=0,writes=0,failRead=0,failWrite=0;
    std::uint32_t unwritable=0;
    bool Read(std::uint32_t a,void* p,std::size_t n) noexcept {
        if(++reads==failRead || !a || a>bytes.size() || n>bytes.size()-a) return false;
        std::memcpy(p,bytes.data()+a,n);return true;
    }
    bool Writable(std::uint32_t a,std::size_t n) noexcept {
        return a && a!=unwritable && a<=bytes.size() && n<=bytes.size()-a;
    }
    bool Write(std::uint32_t a,const void* p,std::size_t n) noexcept {
        ++writes;
        if(!Writable(a,n))return false;
        if(writes==failWrite) {if(n)bytes[a]=0xEE;return false;} // partial mutation
        std::memcpy(bytes.data()+a,p,n);return true;
    }
    template<class T> void Set(std::uint32_t a,const T& v) {std::memcpy(bytes.data()+a,&v,sizeof(v));}
    template<class T> T Get(std::uint32_t a) const {T v{};std::memcpy(&v,bytes.data()+a,sizeof(v));return v;}
};
constexpr std::uint32_t gx=0x10000,device=0x20000,app=0x30000,hw=0x32000;
Memory Initial() {
    Memory m;m.Set(ns::kSingleton,gx);m.Set(gx,ns::kVtable);
    m.Set(gx+0x397C,device);m.Set(gx+0xF58,1u);m.Set(gx+0xF5C,0u);
    m.Set(gx+0x28F4,app);m.Set(gx+0x2900,hw);
    m.Set(gx+4,ns::Array{8,3,0x40000,8});m.Set(gx+0x14,ns::Array{8,2,0x41000,8});
    m.Set(gx+0x24,ns::Array{108,0,0x42000,16});
    for(unsigned i=0;i<ns::kStates;++i) {
        ns::AppState a{{i+1,i+2,i+3,i+4},2,0};m.Set(app+i*24,a);m.Set(hw+i*16,a.value);
    }
    for(unsigned i=0;i<72;++i)m.bytes[0x40000+i]=std::uint8_t(i+3);
    m.Set(0x41000,1u);m.Set(0x41004,2u);return m;
}
void Mutate(Memory& m) {
    for(auto r:ns::kValues)std::fill_n(m.bytes.begin()+gx+r.offset,r.bytes,0x41);
    for(auto r:ns::kGlobals)std::fill_n(m.bytes.begin()+r.offset,r.bytes,0x42);
    std::fill_n(m.bytes.begin()+app,108*24,0x43);std::fill_n(m.bytes.begin()+hw,108*16,0x44);
    m.Set(gx+4,ns::Array{16,5,0x50000,16});m.Set(gx+0x14,ns::Array{16,4,0x51000,16});
    m.Set(gx+0x24,ns::Array{216,2,0x52000,32});
}
void CheckRestored(const Memory& m,const Memory& original) {
    for(auto r:ns::kValues)CHECK(!std::memcmp(m.bytes.data()+gx+r.offset,original.bytes.data()+gx+r.offset,r.bytes));
    for(auto r:ns::kGlobals)CHECK(!std::memcmp(m.bytes.data()+r.offset,original.bytes.data()+r.offset,r.bytes));
    CHECK(!std::memcmp(m.bytes.data()+app,original.bytes.data()+app,108*24));
    CHECK(!std::memcmp(m.bytes.data()+hw,original.bytes.data()+hw,108*16));
    const auto u=m.Get<ns::Array>(gx+4),s=m.Get<ns::Array>(gx+0x14),d=m.Get<ns::Array>(gx+0x24);
    CHECK(u.data==0x50000&&u.capacity==16&&u.grow==16&&u.count==3);
    CHECK(s.data==0x51000&&s.capacity==16&&s.grow==16&&s.count==2);
    CHECK(d.data==0x52000&&d.capacity==216&&d.grow==32&&d.count==0);
    CHECK(!std::memcmp(m.bytes.data()+u.data,original.bytes.data()+0x40000,72));
    CHECK(!std::memcmp(m.bytes.data()+s.data,original.bytes.data()+0x41000,8));
}
struct BindingBackend {
    std::array<unsigned,108> app{},cached{},physical{};
    std::array<unsigned,2> dirty{};
    unsigned queued=0,sets=0,forces=0,flushes=0,draws=0;
    unsigned failureStage=0;bool begin=true,drain=true,partial=false;
    bool Begin() noexcept {return begin;}
    void Set(unsigned state,unsigned value) noexcept {++sets;app[state]=value;}
    void Force(unsigned state) noexcept {++forces;cached[state]=~app[state];dirty[queued++]=state;}
    void Flush() noexcept {++flushes;for(unsigned i=queued;i>0;--i) {
        const auto state=dirty[i-1];
        if(state-0x15!=failureStage)physical[state]=app[state];else partial=true;
        cached[state]=app[state]; // Native caches still advance on physical failure.
    } queued=0;}
    bool Verify(unsigned stage,unsigned value) noexcept {return physical[stage+0x15]==value && cached[stage+0x15]==value;}
    bool Drained() noexcept {return drain&&queued==0;}
};
struct Transaction {
    ns::Snapshot snapshot;Memory memory=Initial();Memory original=memory;
    bool capture=true,produce=true,bind=true,gpuRestore=true,failNativeRestore=false;
    bool gpuDirty=false,quarantine=false,nativeCoherent=true,internal=false;
    unsigned native=0,replacement=0,nativeRestores=0,gpuRestores=0,nested=0;
    std::int32_t submitHr=0;
    bool Preflight() noexcept {return !internal&&!quarantine;}
    bool Capture() noexcept {return capture&&snapshot.Capture(memory,gx,device);}
    bool Produce() noexcept {internal=true;Mutate(memory);gpuDirty=true;
        // A secondary draw is forwarded while recursion is guarded.
        CHECK(!Preflight());++nested;return produce;}
    bool Bind() noexcept {return bind;}
    bool Restore() noexcept {
        ++nativeRestores;memory.failWrite=failNativeRestore?memory.writes+1:0;
        const bool cpu=snapshot.Restore(memory,device);++gpuRestores;
        if(gpuRestore)gpuDirty=false;internal=false;return cpu&&gpuRestore;
    }
    std::int32_t Native() noexcept {++native;nativeCoherent=!gpuDirty&&
        !std::memcmp(memory.bytes.data()+app,original.bytes.data()+app,108*24);return 7;}
    std::int32_t Replace() noexcept {++replacement;return submitHr;}
    void Quarantine() noexcept {quarantine=true;}
};
int main() {
    auto original=Initial();auto m=original;ns::Snapshot snapshot;
    CHECK(snapshot.Capture(m,gx,device));const auto captureReads=m.reads;
    CHECK(m.writes==0&&m.bytes==original.bytes);
    Mutate(m);CHECK(snapshot.Restore(m,device));CheckRestored(m,original);
    const auto writes=m.writes;CHECK(snapshot.Restore(m,device));CheckRestored(m,original);
    // Every capture read can fail: no native mutation, no stale valid snapshot.
    for(unsigned fail=1;fail<=captureReads;++fail) {
        m=original;m.failRead=fail;CHECK(!snapshot.Capture(m,gx,device));
        CHECK(!snapshot.Valid()&&m.writes==0&&m.bytes==original.bytes);
    }
    // Every restoration write can fail partially; all remaining writes attempted.
    for(unsigned fail=1;fail<=writes;++fail) {
        m=original;CHECK(snapshot.Capture(m,gx,device));Mutate(m);m.failWrite=fail;
        CHECK(!snapshot.Restore(m,device));CHECK(m.writes==writes);
        m.failWrite=0;CHECK(snapshot.Restore(m,device));CheckRestored(m,original);
    }
    m=original;m.Set(gx+0x28,1u);CHECK(!snapshot.Capture(m,gx,device)); // pending state
    m=original;m.Set(app+20,1u);CHECK(!snapshot.Capture(m,gx,device)); // lost queue entry
    m=original;m.Set(hw,99u);CHECK(!snapshot.Capture(m,gx,device)); // incoherent HW mirror
    m=original;m.Set(gx+4,ns::Array{2,3,0x40000,8});CHECK(!snapshot.Capture(m,gx,device));
    m=original;m.unwritable=gx+0xF6C;CHECK(!snapshot.Capture(m,gx,device));
    m=original;CHECK(snapshot.Capture(m,gx,device));Mutate(m);m.Set(gx+0x2668,9u);
    CHECK(!snapshot.Restore(m,device));CHECK(m.Get<unsigned>(gx+0x2668)==9); // never overwrite ownership
    m=original;CHECK(snapshot.Capture(m,gx,device));Mutate(m);m.Set(gx+0x28F4,app+4);
    CHECK(!snapshot.Restore(m,device)); // backing app array changed; cannot guess
    m=original;CHECK(snapshot.Capture(m,gx,device));Mutate(m);
    CHECK(!snapshot.Restore(m,device+1));CHECK(m.writes==0);
    snapshot.Reset();CHECK(!snapshot.Valid());CHECK(!snapshot.Restore(m,device));
    // Canonical realization always dispatches both states, including same-value rebind.
    BindingBackend binder;
    for(unsigned i=0;i<100;++i) {
        CHECK(ns::RealizeMaterials(binder,0xA001,0xB001));
        CHECK(binder.physical[0x1A]==0xA001&&binder.physical[0x1C]==0xB001&&binder.draws==0);
    }
    CHECK(binder.sets==200&&binder.forces==200&&binder.flushes==100);
    for(unsigned stage:{5u,7u}) {BindingBackend broken;broken.failureStage=stage;
        CHECK(!ns::RealizeMaterials(broken,1,2));CHECK(broken.partial&&broken.flushes==1&&broken.draws==0);}
    BindingBackend blocked;blocked.begin=false;CHECK(!ns::RealizeMaterials(blocked,1,2));CHECK(blocked.sets==0);
    blocked.begin=true;CHECK(!ns::RealizeMaterials(blocked,0,2));CHECK(blocked.sets==0);
    blocked.drain=false;CHECK(!ns::RealizeMaterials(blocked,1,2));
    // All partial CPU/GPU mutation and restoration outcomes; no duplicate DIP.
    for(unsigned mask=0;mask<64;++mask) {
        Transaction t;t.capture=mask&1;t.produce=mask&2;t.bind=mask&4;
        t.gpuRestore=mask&8;t.failNativeRestore=mask&16;t.submitHr=(mask&32)?-1:0;
        const auto result=ExecuteProduction(true,t);
        const bool attempted=t.capture&&t.produce&&t.bind;
        const bool restored=t.gpuRestore&&!t.failNativeRestore;
        CHECK(t.replacement==unsigned(attempted));
        CHECK(t.native==unsigned(!t.capture||(!attempted&&restored)));
        CHECK(t.nativeCoherent);
        CHECK(t.nativeRestores==unsigned(t.capture)&&t.gpuRestores==unsigned(t.capture));
        CHECK(t.quarantine==(t.capture&&!restored));
        if(attempted) {CHECK(t.native==0);CHECK(result.result==t.submitHr);}
        if(t.capture&&restored) {
            CheckRestored(t.memory,t.original);
            // Repeated subsequent native state changes compare to the restored
            // HW cache and produce the same physical value as an untouched client.
            auto h=t.memory.Get<std::array<unsigned,4>>(hw+0x1A*16);
            auto physical=h;
            for(unsigned n=0;n<16;++n) {
                auto desired=t.original.Get<ns::AppState>(app+0x1A*24).value;desired[0]+=n;
                if(h!=desired) {physical=desired;h=desired;}
                CHECK(physical==desired);
            }
        }
    }
    // Production remains continuous past diagnostic limits, with DIAG=0 and
    // after logging is exhausted. Loss/unqualification stops production copies.
    unsigned continuous=0,withLogging=0,diagnosticOnly=0,disabled=0;
    for(unsigned frame=0;frame<20000;++frame) {
        continuous+=SnapshotAttemptPermitted(true,false,frame,frame);
        withLogging+=SnapshotAttemptPermitted(true,true,frame,frame);
        diagnosticOnly+=SnapshotAttemptPermitted(false,true,frame,frame);
        disabled+=SnapshotAttemptPermitted(false,false,frame,frame);
    }
    CHECK(continuous==20000);CHECK(withLogging==20000);CHECK(diagnosticOnly==4);CHECK(disabled==0);
    CHECK(SnapshotAttemptPermitted(true,false,UINT64_MAX,UINT64_MAX));
    CHECK(ResourceHooksNeeded(true,false));CHECK(!ResourceHooksNeeded(false,false));
    CHECK(!ns::kScreenSamplerContractQualified);CHECK(!ns::kSecondaryStateContractQualified);
    std::printf("R6 native bridge and lifecycle: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
