// Bounded R7 observation policy; no renderer decisions. GPL-3.0-or-later.
#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace wxl::outlinediag {
struct Config {
    bool enabled=false, valid=true;
    unsigned start=300, stride=120, windows=3, records=512, minDistance=150;
    std::string model;
    bool Samples(uint64_t frame) const noexcept {
        return enabled && valid && frame>=start && (frame-start)%stride==0 &&
               (frame-start)/stride<windows;
    }
};
inline bool Number(const char* s,unsigned low,unsigned high,unsigned& out) {
    if(!s||!*s) return false;
    unsigned n=0;
    for(;*s;++s) {
        if(*s<'0'||*s>'9') return false;
        unsigned d=unsigned(*s-'0');
        if(n>high/10 || (n==high/10 && d>high%10)) return false;
        n=n*10+d;
    }
    if(n<low) return false;
    out=n; return true;
}
template<class Get> Config Parse(Get get) {
    Config c;
    auto s=get("WXL_R7_OUTLINE_DIAG");
    if(!s || std::strcmp(s,"0")==0) return c;
    if(std::strcmp(s,"1")!=0) { c.valid=false; return c; }
    c.enabled=true;
    struct Option { const char* name; unsigned* value; unsigned lo,hi; };
    for(auto o:{Option{"WXL_R7_OUTLINE_START",&c.start,1,36000},
                Option{"WXL_R7_OUTLINE_STRIDE",&c.stride,1,3600},
                Option{"WXL_R7_OUTLINE_WINDOWS",&c.windows,1,8},
                Option{"WXL_R7_OUTLINE_RECORDS",&c.records,1,2048},
                Option{"WXL_R7_OUTLINE_MIN_DISTANCE",&c.minDistance,0,5000}})
        if((s=get(o.name)) && !Number(s,o.lo,o.hi,*o.value)) c.valid=false;
    if((s=get("WXL_R7_OUTLINE_MODEL"))) {
        c.model=s;
        if(c.model.size()>200) c.valid=false;
    }
    if(!c.valid) c.enabled=false;
    return c;
}
inline bool Matches(const Config& c,uint32_t flags,float distanceSq,std::string_view path) {
    return (flags&0x20)!=0 && std::isfinite(distanceSq) && distanceSq>=0 &&
           std::sqrt(distanceSq)>=c.minDistance &&
           (c.model.empty() || path.find(c.model)!=std::string_view::npos);
}
// A band cutoff comparison is NOT a native selector or a fade equation.
// Keep all five comparisons, and leave the actual selector explicitly unknown.
inline int Compare(float distance,float cutoff) {
    if(!std::isfinite(distance)||!std::isfinite(cutoff)) return -1;
    return distance<=cutoff?1:0;
}
}
