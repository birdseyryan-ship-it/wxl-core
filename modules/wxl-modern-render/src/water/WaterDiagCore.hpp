// R6 diagnostics: portable policy, hashing and bounded capture. GPL-3.0-or-later.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <algorithm>

namespace wxl::waterdiag
{
enum class Family : uint8_t { Unknown, Water, WaterNoSpec, ProcWater, Magma };
enum class Provider : uint8_t { Unknown, Terrain, Wmo };
enum class Mode : uint8_t { Full, Profiles, Timing };
inline const char* Name(Family v) {
    switch(v) { case Family::Water:return "Water"; case Family::WaterNoSpec:return "WaterNoSpec";
    case Family::ProcWater:return "ProcWater";case Family::Magma:return "Magma";default:return "Unknown"; }
}
inline const char* Name(Provider v) { return v==Provider::Terrain?"terrain":v==Provider::Wmo?"wmo":"unknown"; }
inline unsigned Selector(Family f) { return f==Family::Magma?2:f==Family::ProcWater?3:(f==Family::Water||f==Family::WaterNoSpec)?1:0; }
struct Config {
    bool enabled=false, valid=true, copyRequested=false;
    Mode mode=Mode::Full;
    unsigned samples=3, maxDraws=256, maxMiB=16;
};
inline bool Decimal(const char* s, unsigned lo, unsigned hi, unsigned& value) {
    if(!s || !*s) return false;
    unsigned n=0;
    for(;*s;++s) { if(*s<'0'||*s>'9')return false; unsigned d=unsigned(*s-'0');
        if(n>hi/10 || (n==hi/10 && d>hi%10))return false;
        n=n*10+d; }
    if(n<lo||n>hi)return false;
    value=n;return true;
}
template<class Get> Config ParseConfig(Get get) {
    Config c;const char* s=get("WXL_CLASSIC_WATER_DIAG");
    if(!s || std::strcmp(s,"0")==0)return c;
    if(std::strcmp(s,"1")!=0) {c.valid=false;return c;} c.enabled=true;
    s=get("WXL_CLASSIC_WATER_DIAG_MODE");
    if(s) {if(std::strcmp(s,"full")==0)c.mode=Mode::Full;else if(std::strcmp(s,"profiles")==0)c.mode=Mode::Profiles;
        else if(std::strcmp(s,"timing")==0)c.mode=Mode::Timing;else c.valid=false;}
    struct Option {const char* name;unsigned* dst;unsigned lo,hi;};
    for(auto o:{Option{"WXL_CLASSIC_WATER_DIAG_SAMPLES",&c.samples,1,4},
                Option{"WXL_CLASSIC_WATER_DIAG_MAX_DRAWS",&c.maxDraws,1,512},
                Option{"WXL_CLASSIC_WATER_DIAG_MAX_MIB",&c.maxMiB,1,32}})
        if((s=get(o.name)) && !Decimal(s,o.lo,o.hi,*o.dst))c.valid=false;
    s=get("WXL_CLASSIC_WATER_DIAG_COPY");
    if(s) {if(std::strcmp(s,"1")==0)c.copyRequested=true;else if(std::strcmp(s,"0")!=0)c.valid=false;}
    if(!c.valid)c.enabled=false;
    return c;
}
using Digest=std::array<uint8_t,32>;
class Sha256 {
    std::array<uint32_t,8> h_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::array<uint8_t,64> block_{};uint64_t bytes_=0;size_t used_=0;
    static uint32_t R(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
    void Block() {
        static constexpr uint32_t k[64]={
          0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
          0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
          0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
          0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
          0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
          0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
          0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
          0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];for(unsigned i=0;i<16;++i)w[i]=(uint32_t(block_[4*i])<<24)|(uint32_t(block_[4*i+1])<<16)|(uint32_t(block_[4*i+2])<<8)|block_[4*i+3];
        for(unsigned i=16;i<64;++i) {auto a=w[i-15],b=w[i-2];w[i]=w[i-16]+(R(a,7)^R(a,18)^(a>>3))+w[i-7]+(R(b,17)^R(b,19)^(b>>10));}
        auto a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
        for(unsigned i=0;i<64;++i){uint32_t t=h+(R(e,6)^R(e,11)^R(e,25))+((e&f)^((~e)&g))+k[i]+w[i];
            uint32_t u=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
        h_[0]+=a;h_[1]+=b;h_[2]+=c;h_[3]+=d;h_[4]+=e;h_[5]+=f;h_[6]+=g;h_[7]+=h;
    }
public:
    void Update(const void* p,size_t n){auto b=static_cast<const uint8_t*>(p);bytes_+=n;while(n){size_t take=std::min(n,64-used_);std::memcpy(block_.data()+used_,b,take);used_+=take;b+=take;n-=take;if(used_==64){Block();used_=0;}}}
    Digest Finish(){uint64_t bits=bytes_*8;uint8_t one=0x80,zero=0;Update(&one,1);while(used_!=56)Update(&zero,1);
        uint8_t tail[8];for(unsigned i=0;i<8;++i)tail[7-i]=uint8_t(bits>>(i*8));Update(tail,8);
        Digest d{};for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)d[4*i+j]=uint8_t(h_[i]>>(24-8*j));return d;}
    static Digest Of(const void* p,size_t n){Sha256 s;s.Update(p,n);return s.Finish();}
};
inline std::string Hex(const void* p,size_t n){static constexpr char h[]="0123456789abcdef";auto b=static_cast<const uint8_t*>(p);std::string s(n*2,'0');for(size_t i=0;i<n;++i){s[2*i]=h[b[i]>>4];s[2*i+1]=h[b[i]&15];}return s;}
inline std::string Hex(const Digest& d){return Hex(d.data(),d.size());}
inline std::string Quote(std::string_view s){std::string o="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){o+='\\';o+=char(c);}else if(c<32){static constexpr char h[]="0123456789abcdef";o+="\\u00";o+=h[c>>4];o+=h[c&15];}else o+=char(c);}return o+'"';}
struct Budget {size_t limit=0,used=0;uint64_t dropped=0;
    bool Take(size_t n){if(used>limit||n>limit-used){++dropped;return false;}used+=n;return true;}};
struct ProfileKey {
    Family family=Family::Unknown;Provider provider=Provider::Unknown;int pass=-1;
    Digest vs{},ps{},declaration{},material{};
    unsigned topology=0;
    bool operator==(const ProfileKey&)const=default;
};
struct ProfileLimiter {
    struct Item {ProfileKey key;unsigned samples=0;uint64_t lastFrame=0;};
    std::array<Item,128> items{};size_t size=0;unsigned total=0;
    void NewGeneration(){size=0;items={};} // Keep the process-wide admitted count.
    // Does not mutate when rejected; identities are full hashes, never just addresses.
    bool Admit(const ProfileKey& key,uint64_t frame,unsigned samples,unsigned ceiling){
        if(total>=ceiling)return false;
        size_t i=0;for(;i<size;++i)if(items[i].key==key)break;
        if(i==size){if(size==items.size())return false;items[size++].key=key;}
        auto& e=items[i];if(e.samples>=samples||(e.samples && (frame<e.lastFrame||frame-e.lastFrame<180)))return false;
        ++e.samples;e.lastFrame=frame;++total;return true;
    }
};
struct InspectionScope {
    bool& flag;bool previous;
    explicit InspectionScope(bool& f):flag(f),previous(f){flag=true;}
    ~InspectionScope(){flag=previous;}
};
template<class Inspect,class Forward,class Failed>
auto ObserveThenForward(bool enabled,Inspect inspect,Forward forward,Failed failed)->decltype(forward()) {
    if(enabled) {try{inspect();}catch(...){failed();}}
    return forward(); // Native result and exactly one submission, including capture failure.
}
inline const char* Classification(bool scope,Family family,Provider provider,bool vs,bool ps){
    if(!scope)return "outside_liquid";
    if(family==Family::Unknown)return "unknown_material";
    if(provider==Provider::Unknown)return "context_only_unknown_provider";
    if(!vs||!ps)return "context_only_fixed_or_query_failed";
    return "observed_material_and_final_shader_pair"; // Discovery only; NEVER replacement permission.
}
enum class ReplacementProfile : uint8_t { None, P01, P02, P03 };
inline const char* Name(ReplacementProfile p) {
    switch(p){case ReplacementProfile::P01:return "P01";case ReplacementProfile::P02:return "P02";case ReplacementProfile::P03:return "P03";default:return "none";}
}
inline constexpr Digest kR6BaseWaterVs{{0xb3,0xc5,0x87,0x75,0xb7,0x64,0xf2,0xb5,0x88,0xa2,0x20,0x25,0x2d,0x25,0x0f,0x54,0x09,0xf9,0x67,0x51,0x02,0x70,0x4c,0xd2,0x53,0x4c,0x8e,0xdb,0x34,0x4e,0x86,0xd2}};
inline constexpr Digest kR6BaseWaterPs{{0x55,0x1e,0xff,0xcf,0xd8,0x2f,0xb9,0xf8,0xcc,0x2a,0x91,0x00,0x2f,0x76,0xed,0x91,0x44,0x1f,0x64,0x6f,0x5f,0x77,0x42,0xe4,0xdb,0x9d,0xc7,0x3a,0x9a,0x75,0xf1,0x3d}};
inline constexpr Digest kR6BaseWaterDecl{{0x96,0xf7,0x2c,0x74,0x36,0x05,0x8f,0x1a,0xb9,0x8d,0x30,0xdb,0x59,0x58,0xb7,0x1d,0xc0,0xc1,0x8e,0xe5,0xe6,0xa6,0xcc,0xed,0x95,0xe3,0xdb,0xa9,0x90,0x15,0x66,0xdd}};
inline constexpr Digest kR6WaterMaterialP01{{0xf6,0xac,0xea,0x57,0x86,0xe7,0x27,0x8b,0x89,0xcd,0x18,0xe8,0xab,0xe9,0x8b,0x84,0x57,0x2c,0x31,0x77,0xde,0xaf,0x08,0xc4,0xcc,0x08,0xf3,0x05,0x8c,0x04,0xec,0xb4}};
inline constexpr Digest kR6WaterMaterialP02P03{{0x8b,0xb9,0xab,0x6d,0x47,0x2b,0xf5,0x97,0x38,0x65,0x15,0xa7,0xca,0x53,0x16,0x11,0x86,0x29,0xd4,0x73,0x15,0xd2,0x14,0xc6,0xea,0x09,0x05,0x9b,0x9e,0x72,0xdb,0x31}};
inline ReplacementProfile AllowlistedReplacementProfile(const ProfileKey& k) {
    if(k.family!=Family::Water||k.pass!=1||k.topology!=5||k.vs!=kR6BaseWaterVs||k.ps!=kR6BaseWaterPs||k.declaration!=kR6BaseWaterDecl)
        return ReplacementProfile::None;
    if(k.provider==Provider::Terrain&&k.material==kR6WaterMaterialP01)return ReplacementProfile::P01;
    if(k.provider==Provider::Terrain&&k.material==kR6WaterMaterialP02P03)return ReplacementProfile::P02;
    if(k.provider==Provider::Wmo&&k.material==kR6WaterMaterialP02P03)return ReplacementProfile::P03;
    return ReplacementProfile::None;
}
inline bool ReplacementTransportReady(bool snapshotValid,bool sameScene,bool sameFrame,bool producerBeforeConsumer,bool sameSourceRt) {
    return snapshotValid&&sameScene&&sameFrame&&producerBeforeConsumer&&sameSourceRt;
}
template<size_t N> inline bool MarkRange(std::array<uint8_t,N>& bits,unsigned start,unsigned count){
    if(start>N||count>N-start)return false;
    std::fill(bits.begin()+start,bits.begin()+start+count,uint8_t(1));return true;
}
}
