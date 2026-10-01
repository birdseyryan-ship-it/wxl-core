#include "../../src/client/CWorldScene/OutlineDiagnosticPolicy.hpp"
#include <iostream>
#include <map>
#include <limits>
using namespace wxl::outlinediag;
int main() {
    unsigned tests=0,failures=0;
    auto check=[&](bool ok,const char* what) {++tests;if(!ok){++failures;std::cerr<<what<<'\n';}};
    std::map<std::string,std::string> env;
    auto get=[&](const char* n)->const char* {auto it=env.find(n);return it==env.end()?nullptr:it->second.c_str();};
    check(!Parse(get).enabled,"absent flag must leave hooks disabled");
    env["WXL_R7_OUTLINE_DIAG"]="0";check(!Parse(get).enabled,"zero must leave hooks disabled");
    env["WXL_R7_OUTLINE_DIAG"]="true";check(!Parse(get).valid,"unrecognized enabling flag rejected");
    env["WXL_R7_OUTLINE_DIAG"]="1";auto c=Parse(get);
    check(c.enabled && c.valid,"explicit diagnostic enabled");
    for(uint64_t f=0;f<1000;++f) check(c.Samples(f)==(f==300 || f==420 || f==540),"sampling schedule");
    check(!c.Samples(UINT64_MAX),"frame overflow cannot reopen capture");
    check(!Matches(c,0,40000,"tree"),"live units never enter placed-doodad capture");
    check(!Matches(c,0x20,-1,"tree"),"negative distance rejected");
    check(!Matches(c,0x20,std::numeric_limits<float>::quiet_NaN(),"tree"),"NaN rejected");
    check(!Matches(c,0x20,std::numeric_limits<float>::infinity(),"tree"),"infinite distance rejected");
    check(!Matches(c,0x20,149.f*149.f,"tree"),"near non-target excluded");
    check(Matches(c,0x20,150.f*150.f,"tree"),"distance boundary included");
    c.model="Elwynn";
    check(!Matches(c,0x20,40000,"BarrensTree"),"model filter excludes unrelated objects");
    check(Matches(c,0x20,40000,"ElwynnTree"),"model filter includes requested path");
    for(auto value:{"0","-1","+1","1x"," 1","999999999999999999999"}) {
        env["WXL_R7_OUTLINE_STRIDE"]=value;auto v=Parse(get);
        check(!v.enabled && !v.valid,"invalid stride must fail closed");
    }
    env.erase("WXL_R7_OUTLINE_STRIDE");
    env["WXL_R7_OUTLINE_RECORDS"]="2049";check(!Parse(get).enabled,"record bound enforced");
    env.erase("WXL_R7_OUTLINE_RECORDS");
    env["WXL_R7_OUTLINE_MODEL"]=std::string(201,'x');check(!Parse(get).enabled,"filter bound enforced");
    check(Compare(100,50)==0 && Compare(50,50)==1,"threshold comparison only");
    check(Compare(std::numeric_limits<float>::infinity(),50)==-1,"unknown comparison does not become a band");
    std::cout<<tests<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
