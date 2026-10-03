// Exact Wrath deferred-state transaction. GPL-3.0-or-later.
// The byte layouts below are from canonical 57dd8955... Wow.exe, not a
// reference client's C++ layout. See tools/r6-slot3-check/GX_CONTRACT.md.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wxl::water::slot3 {
namespace native_state {
inline constexpr std::uint32_t kStates = 108;
inline constexpr std::uint32_t kVtable = 0x00A2E718;
inline constexpr std::uint32_t kSingleton = 0x00C5DF88;
inline constexpr std::uint32_t kSet = 0x00685F50;
inline constexpr std::uint32_t kForceOne = 0x006859E0;
inline constexpr std::uint32_t kFlush = 0x00685B50;
inline constexpr std::uint32_t kSky = 0x007F09B0;
inline constexpr std::uint32_t kSkyRect = 0x00ADF570;
struct AppState { std::array<std::uint32_t,4> value{}; std::uint32_t stack=0, dirty=0; };
struct Array { std::uint32_t capacity=0, count=0, data=0, grow=0; };
static_assert(sizeof(AppState)==24 && sizeof(Array)==16);
struct Range { std::uint32_t offset, bytes; };
// No ownership headers are copied. Matrix stacks are four inline matrices,
// level/dirty and four flags (0x118 bytes), not growable arrays. Stop before
// the six owning lists at +0x2668. Geometry references are non-owning.
inline constexpr Range kValues[] = {
    {0x174,16}, {0xF6C,0x2668-0xF6C}, {0x2758,8},
    {0x2790,0x28C4-0x2790}, {0x3B2C,4}, {0x3B5C,0x3EA4-0x3B5C}
};
// Existing Camera/Shader.hpp authorities; constant banks include dirty bounds.
inline constexpr Range kGlobals[] = {
    {0x00C5DFE0,0x2010}, {0x00AD8F88,4},
    {0x00D43010,8}, {0x00D43020,8}, {0x00CFBEAC,4}, {0x00CFBEB4,4},
    {0x00ADF460,64}, {0x00ADF5E8,128}, {0x00CD8F5C,12}
};
// Read-only invariants: never restore an engine allocation/list header or an
// engine-owned render-target reference. A change is unqualified and fails.
inline constexpr Range kInvariants[] = {{0x2668,0xF0}, {0x2910,0x18}};
inline constexpr unsigned kUndoLimit=4096, kStackLimit=64; // adapter bounds

inline bool ValidArray(const Array& a, unsigned limit, unsigned stride) noexcept {
    return a.count<=a.capacity && a.count<=limit &&
           (a.count==0 || (a.data && a.count<= (UINT32_MAX-a.data)/stride));
}
struct Bytes { std::uint32_t address=0; std::vector<std::uint8_t> data; };

// Memory supplies Read(address,out,n), Write(address,data,n), Writable(address,n).
// No engine call occurs during Capture. It either commits an entire snapshot
// or leaves this object invalid, without mutating native memory.
class Snapshot {
    std::uint32_t gx_=0, device_=0, app_=0, hardware_=0;
    Array undo_{}, stack_{}, dirty_{};
    std::vector<Bytes> values_, invariants_;
    std::vector<std::uint8_t> undoBytes_, stackBytes_;
    std::array<AppState,kStates> appStates_{};
    std::array<std::array<std::uint32_t,4>,kStates> hardwareStates_{};
    bool valid_=false;
    template<class M, class T> static bool Read(M& m, std::uint32_t a,T& v) noexcept {
        return m.Read(a,&v,sizeof(v));
    }
    template<class M> static bool Save(M& m,std::uint32_t a,unsigned n,std::vector<Bytes>& out) {
        if(!a || n>UINT32_MAX-a || !m.Writable(a,n)) return false;
        Bytes b{a,std::vector<std::uint8_t>(n)};
        if(!m.Read(a,b.data.data(),n)) return false;
        out.push_back(std::move(b)); return true;
    }
    template<class M> static bool Equal(M& m,const Bytes& b) noexcept {
        std::array<std::uint8_t,256> check{};
        for(std::size_t p=0;p<b.data.size();p+=check.size()) {
            const auto n=(b.data.size()-p<check.size())?b.data.size()-p:check.size();
            if(!m.Read(b.address+std::uint32_t(p),check.data(),n) ||
               std::memcmp(check.data(),b.data.data()+p,n)) return false;
        }
        return true;
    }
    template<class M> static bool Put(M& m,std::uint32_t a,const void* p,std::size_t n) noexcept {
        if(!n) return true;
        if(!m.Writable(a,n) || !m.Write(a,p,n)) return false;
        std::array<std::uint8_t,256> check{};
        for(std::size_t i=0;i<n;i+=check.size()) {
            const auto bytes=(n-i<check.size())?n-i:check.size();
            if(!m.Read(a+std::uint32_t(i),check.data(),bytes) ||
               std::memcmp(check.data(),static_cast<const std::uint8_t*>(p)+i,bytes)) return false;
        }
        return true;
    }
public:
    void Reset() noexcept { valid_=false; gx_=device_=app_=hardware_=0; values_.clear(); invariants_.clear(); }
    bool Valid() const noexcept { return valid_; }
    std::uint32_t DeviceObject() const noexcept { return gx_; }
    template<class M> bool Capture(M& m,std::uint32_t gx,std::uint32_t device) noexcept {
        Reset();
        try {
            std::uint32_t singleton=0,vt=0,physical=0,active=0,blocked=0;
            if(!gx || gx>UINT32_MAX-0x3EA4 || !device ||
               !Read(m,kSingleton,singleton) || singleton!=gx ||
               !Read(m,gx,vt) || vt!=kVtable ||
               !Read(m,gx+0x397C,physical) || physical!=device ||
               !Read(m,gx+0xF58,active) || !active ||
               !Read(m,gx+0xF5C,blocked) || blocked ||
               !Read(m,gx+0x28F4,app_) || !app_ ||
               !Read(m,gx+0x2900,hardware_) || !hardware_ ||
               !Read(m,gx+4,undo_) || !ValidArray(undo_,kUndoLimit,24) ||
               !Read(m,gx+0x14,stack_) || !ValidArray(stack_,kStackLimit,4) ||
               !Read(m,gx+0x24,dirty_) || !ValidArray(dirty_,kStates,4) || dirty_.count ||
               !Read(m,app_,appStates_) || !Read(m,hardware_,hardwareStates_)) return false;
            for(unsigned i=0;i<kStates;++i)
                if(appStates_[i].dirty || appStates_[i].value!=hardwareStates_[i] ||
                   appStates_[i].stack>stack_.count) return false;
            undoBytes_.resize(undo_.count*24u); stackBytes_.resize(stack_.count*4u);
            if((!undoBytes_.empty() && !m.Read(undo_.data,undoBytes_.data(),undoBytes_.size())) ||
               (!stackBytes_.empty() && !m.Read(stack_.data,stackBytes_.data(),stackBytes_.size()))) return false;
            if(!m.Writable(app_,sizeof(appStates_)) || !m.Writable(hardware_,sizeof(hardwareStates_))) return false;
            for(const auto r:kValues) if(!Save(m,gx+r.offset,r.bytes,values_)) return false;
            for(const auto r:kGlobals) if(!Save(m,r.offset,r.bytes,values_)) return false;
            for(const auto r:kInvariants) if(!Save(m,gx+r.offset,r.bytes,invariants_)) return false;
            gx_=gx;device_=device;valid_=true;return true;
        } catch(...) { Reset(); return false; }
    }
    // Read current backing allocations, not saved pointers. Native growth is
    // retained; only the saved live prefix and count are restored. Repeatable.
    template<class M> bool Restore(M& m,std::uint32_t device) noexcept {
        if(!valid_ || device!=device_) return false;
        std::uint32_t singleton=0,vt=0,physical=0,app=0,hw=0;
        if(!Read(m,kSingleton,singleton) || singleton!=gx_ || !Read(m,gx_,vt) || vt!=kVtable ||
           !Read(m,gx_+0x397C,physical) || physical!=device_) return false;
        bool ok=true;
        for(const auto& b:invariants_) if(!Equal(m,b)) ok=false;
        for(const auto& b:values_) if(!Put(m,b.address,b.data.data(),b.data.size())) ok=false;
        if(!Read(m,gx_+0x28F4,app) || app!=app_ ||
           !Put(m,app_,&appStates_,sizeof(appStates_))) ok=false;
        if(!Read(m,gx_+0x2900,hw) || hw!=hardware_ ||
           !Put(m,hardware_,&hardwareStates_,sizeof(hardwareStates_))) ok=false;
        const Array saved[]={undo_,stack_,dirty_};
        const unsigned offsets[]={4,0x14,0x24};
        const unsigned strides[]={24,4,4};
        for(unsigned i=0;i<3;++i) {
            Array now{}; const unsigned limit=i==0?kUndoLimit:i==1?kStackLimit:kStates*4;
            if(!Read(m,gx_+offsets[i],now)) {ok=false;continue;}
            // The live count is itself a value we restore and may have been
            // partially written by a failed attempt. Validate the destination
            // allocation against the SAVED prefix, not that discarded count.
            Array destination=now;destination.count=saved[i].count;
            if(!ValidArray(destination,limit,strides[i])) {ok=false;continue;}
            const auto& prefix=i==0?undoBytes_:stackBytes_;
            if(i<2 && !Put(m,now.data,prefix.data(),prefix.size())) ok=false;
            if(!Put(m,gx_+offsets[i]+4,&saved[i].count,4)) ok=false;
        }
        return ok;
    }
};

// Canonical bind policy shared by Win32 and the fault suite. ForceOne makes
// repeated material bindings observable even when app/hardware values match.
// Native void functions do not return HRESULT; Verify observes actual D3D.
template<class B> bool RealizeMaterials(B& b,std::uint32_t t5,std::uint32_t t7) noexcept {
    if(!t5 || !t7 || !b.Begin()) return false;
    b.Set(0x1A,t5); b.Force(0x1A);
    b.Set(0x1C,t7); b.Force(0x1C);
    b.Flush();
    return b.Verify(5,t5) && b.Verify(7,t7) && b.Drained();
}
// Required qualification is explicit and cannot be enabled with an env flag.
// The supplied shader declarations do not encode sampler filtering/addressing.
inline constexpr bool kScreenSamplerContractQualified=false;
inline constexpr bool kSecondaryStateContractQualified=false;
inline constexpr const char* kBlocker="Classic s0/s1/s6 sampler descriptors and complete secondary-render state closure are unqualified";
} // namespace native_state
} // namespace wxl::water::slot3
