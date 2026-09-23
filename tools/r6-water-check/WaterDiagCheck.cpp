#include "water/WaterDiagCore.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <limits>
#include <vector>
using namespace wxl::waterdiag;
static unsigned tests=0;
#define CHECK(x) do {++tests;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
    std::map<std::string,std::string> env;
    auto get=[&](const char* k)->const char*{auto p=env.find(k);return p==env.end()?nullptr:p->second.c_str();};
    CHECK(!ParseConfig(get).enabled);env["WXL_CLASSIC_WATER_DIAG"]="yes";CHECK(!ParseConfig(get).valid);
    env["WXL_CLASSIC_WATER_DIAG"]="1";auto cfg=ParseConfig(get);CHECK(cfg.enabled&&cfg.samples==3&&cfg.maxDraws==256&&cfg.maxMiB==16);
    env["WXL_CLASSIC_WATER_DIAG_MAX_DRAWS"]="429496729600";CHECK(!ParseConfig(get).enabled);
    env["WXL_CLASSIC_WATER_DIAG_MAX_DRAWS"]="0";CHECK(!ParseConfig(get).valid);
    env["WXL_CLASSIC_WATER_DIAG_MAX_DRAWS"]="512";CHECK(ParseConfig(get).enabled);
    env["WXL_CLASSIC_WATER_DIAG_MODE"]="replacement";CHECK(!ParseConfig(get).enabled);
    env["WXL_CLASSIC_WATER_DIAG_MODE"]="timing";CHECK(ParseConfig(get).mode==Mode::Timing);
    env["WXL_CLASSIC_WATER_DIAG_COPY"]="1";CHECK(ParseConfig(get).copyRequested);
    env["WXL_CLASSIC_WATER_DIAG"]="0";CHECK(!ParseConfig(get).enabled);
    CHECK(Hex(Sha256::Of("",0))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Hex(Sha256::Of("abc",3))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string million(1000000,'a');CHECK(Hex(Sha256::Of(million.data(),million.size()))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    for(size_t n:{55u,56u,63u,64u,65u,127u,4096u}){std::string s(n,'z');Sha256 chunked;
        for(size_t i=0;i<n;i+=7)chunked.Update(s.data()+i,std::min(size_t(7),n-i));
        CHECK(chunked.Finish()==Sha256::Of(s.data(),n));}
    CHECK(Quote("a\"\\\n")=="\"a\\\"\\\\\\u000a\"");
    Budget b{10};CHECK(b.Take(10));CHECK(!b.Take(1));CHECK(!b.Take(std::numeric_limits<size_t>::max()));CHECK(b.used==10&&b.dropped==2);
    std::array<uint8_t,58> mask{};CHECK(MarkRange(mask,46,12));CHECK(mask[45]==0&&mask[46]==1&&mask[57]==1);
    CHECK(!MarkRange(mask,57,2));CHECK(!MarkRange(mask,std::numeric_limits<unsigned>::max(),2));
    CHECK(Selector(Family::Magma)==2&&Selector(Family::ProcWater)==3);
    CHECK(std::string(Classification(false,Family::Water,Provider::Terrain,true,true))=="outside_liquid");
    CHECK(std::string(Classification(true,Family::Water,Provider::Unknown,true,true))=="context_only_unknown_provider");
    CHECK(std::string(Classification(true,Family::ProcWater,Provider::Terrain,true,false))=="context_only_fixed_or_query_failed");
    CHECK(std::string(Classification(true,Family::Magma,Provider::Wmo,true,true))=="observed_material_and_final_shader_pair");
    ProfileKey k;k.family=Family::ProcWater;k.provider=Provider::Terrain;k.vs=Sha256::Of("vs",2);k.ps=Sha256::Of("ps",2);
    ProfileLimiter l;CHECK(l.Admit(k,0,3,256));CHECK(!l.Admit(k,0,3,256));CHECK(!l.Admit(k,179,3,256));
    CHECK(l.Admit(k,180,3,256));CHECK(!l.Admit(k,1,3,256));CHECK(l.Admit(k,360,3,256));CHECK(!l.Admit(k,540,3,256));
    k.family=Family::Magma;CHECK(l.Admit(k,540,3,256));k.provider=Provider::Wmo;CHECK(l.Admit(k,540,3,256));
    ProfileLimiter full;for(unsigned i=0;i<128;++i){k.pass=int(i);CHECK(full.Admit(k,0,1,512));}k.pass=129;CHECK(!full.Admit(k,0,1,512));
    ProfileLimiter cap;CHECK(cap.Admit(k,0,4,1));k.pass=130;CHECK(!cap.Admit(k,1000,4,1));
    cap.NewGeneration();CHECK(cap.size==0&&cap.total==1);CHECK(!cap.Admit(k,2000,4,1));
    unsigned calls=0,failures=0;bool busy=false;
    int hr=ObserveThenForward(true,[&]{InspectionScope scope(busy);throw 7;},[&]{++calls;return -123;},[&]{++failures;});
    CHECK(hr==-123&&calls==1&&failures==1&&!busy);
    hr=ObserveThenForward(false,[&]{throw 8;},[&]{++calls;return 17;},[&]{++failures;});
    CHECK(hr==17&&calls==2&&failures==1);
    std::printf("PASS %u R6 checks: SHA256, flags/default-off, bounds, material+shader classification, dedup/sample ceilings\n",tests);return 0;
}
