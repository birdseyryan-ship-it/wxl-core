"""Compile/test the actual production helpers without loading the WoW client.

No client shaders/assets are embedded. The assembler smoke fixture is synthetic.
An optional files mode lets a local operator validate their own shader corpus.
"""
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[2]
source=(root/'modules/wxl-modern-render/src/shadows/ClassicShadows.cpp').read_text()
affine=source[source.index('        bool BuildClassicReceiverProjectionRows('):source.index('        // End R5 receiver projection helper.')]
start=source.index('        std::string TrimShaderText(')
helpers=source[start:source.index('        void* MakeClassicReceiverShaderWrapper(',start)]
prefix=r'''
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <d3dcompiler.h>
#endif
int mode = 0;
bool ClassicShadowReceiverStockOuterS8AsS9DiagnosticEnabled() { return mode == 1; }
bool ClassicShadowReceiverPackedOuterDiagnosticEnabled() { return mode == 2; }
#define WLOG_INFO(...) ((void)0)
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
'''
tests=r'''
std::string syntheticShader()
{
    std::string text = "ps_3_0\n"
        "def c0, 1, 0, 0.5, 0.2\n"
        "def c11, 0.2, -11.111111, 11, -1\n"
        "dcl_texcoord4 v5\ndcl_texcoord5 v6\ndcl_texcoord6 v7\n"
        "dcl_2d s6\ndcl_2d s7\ndcl_2d s8\n";
    auto sample = [](const std::string& sampler)
    {
        std::string s = "texldl r3, r2, " + sampler + "\nmov r4.x, r3.x\n";
        for (int offset : {3,5,7,9})
            s += "add r3.xy, r2, c" + std::to_string(offset) + "\n"
                "mov r3.zw, r2\ntexldl r3, r3, " + sampler + "\nadd r4.x, r4.x, r3.x\n";
        return s;
    };
    for (int band : {6,7})
    {
        const std::string v="v"+std::to_string(band);
        text += "abs r1.xy, " + v + "\nmax r1.x, r1.x, r1.y\nif_lt r1.x, c0.x\n"
            "mad r2.xy, " + v + ", c0.z, c0.z\nmov r2.z, " + v + ".z\nmov r2.w, c0.y\n";
        text += sample("s"+std::to_string(band));
        text += "mul r0.x, r4.x, c0.w\nelse\n";
    }
    text += "mov r5.x, v5.w\nmov r5.y, v6.w\n"
        "mad r2.x, v5.w, c0.z, c0.z\nmad r2.y, v6.w, c0.z, c0.z\n"
        "mov r2.z, v7.w\nmov r2.w, c0.y\n";
    text += sample("s8");
    text += "abs r5.xy, r5\nmax r5.x, r5.x, r5.y\n"
        "mad_sat r5.x, r5.x, c11.y, c11.z\n"
        "mad r4.x, r4.x, c11.x, c11.w\nmad r0.x, r5.x, r4.x, c0.x\n"
        "endif\nendif\nmov oC0, r0.x\n";
    return text;
}

void affineTests()
{
    std::mt19937 random(514);
    std::uniform_real_distribution<float> dist(-1.0f,1.0f);
    for (int test=0;test<1000;++test)
    {
        float n[16]={},s[16]={},rows[12]={};n[15]=s[15]=1;
        // Rotated orthographic axes, large world translations and independently
        // recentered sidecar. Compare world->side against native->side.
        const float angle=dist(random)*3.14f, c=std::cos(angle),sn=std::sin(angle);
        n[0]=c/540;n[4]=-sn/540;n[1]=sn/540;n[5]=c/540;n[10]=0.002f;
        for(int i=0;i<12;++i) s[i]=n[i]*((i%4)<2 ? 3.0f : 1.0f);
        for(int i=0;i<3;++i) {n[12+i]=dist(random)*30;s[12+i]=dist(random)*50;}
        require(BuildClassicReceiverProjectionRows(n,s,rows),"valid affine rejected");
        const double world[3]={dist(random)*17000,dist(random)*17000,dist(random)*2000};
        double q[3]={};
        for(int r=0;r<3;++r) {q[r]=n[12+r];for(int k=0;k<3;++k)q[r]+=n[r+4*k]*world[k];}
        for(int r=0;r<3;++r)
        {
            double expected=s[12+r],actual=rows[4*r+3];
            for(int k=0;k<3;++k) {expected+=s[r+4*k]*world[k];actual+=rows[4*r+k]*q[k];}
            require(std::abs(expected-actual)<0.0001,"affine reprojection mismatch");
        }
        require(BuildClassicReceiverProjectionRows(n,n,rows),"identity rejected");
        for(int i=0;i<12;++i)require(rows[i]==((i==0||i==5||i==10)?1.0f:0.0f),"identity not exact");
    }
    float n[16]={},s[16]={},rows[12];n[0]=n[5]=n[10]=n[15]=1;
    std::memcpy(s,n,sizeof(s));for(float& v:rows)v=17;
    n[0]=0;require(!BuildClassicReceiverProjectionRows(n,s,rows),"singular accepted");
    for(float v:rows)require(v==17,"failed inverse overwrote output");
    n[0]=1;n[3]=0.1f;require(!BuildClassicReceiverProjectionRows(n,s,rows),"perspective accepted");
    n[3]=0;n[0]=std::numeric_limits<float>::quiet_NaN();
    require(!BuildClassicReceiverProjectionRows(n,s,rows),"NaN accepted");
}

void assemble(const std::string& text)
{
#ifdef _WIN32
    using Assemble = HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,
        ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
    static HMODULE dll=LoadLibraryA("d3dcompiler_47.dll");
    require(dll!=nullptr,"d3dcompiler_47 not found");
    static auto fn=reinterpret_cast<Assemble>(GetProcAddress(dll,"D3DAssemble"));
    require(fn!=nullptr,"D3DAssemble unavailable");
    ID3DBlob *code=nullptr,*errors=nullptr;
    HRESULT hr=fn(text.data(),text.size(),nullptr,nullptr,nullptr,0,&code,&errors);
    if(errors) {std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());errors->Release();}
    if(code)code->Release();
    require(SUCCEEDED(hr),"shader assembly failed");
#endif
}

int main(int argc,char** argv)
{
    try
    {
        if(argc>=5 && std::string(argv[1])=="files")
        {
            mode=std::stoi(argv[2]);std::filesystem::create_directories(argv[3]);
            for(int i=4;i<argc;++i)
            {
                std::ifstream in(argv[i]);std::stringstream ss;ss<<in.rdbuf();
                const std::string output=InjectClassicCascade4Receiver(ss.str());
                if(!output.empty())
                {
                    assemble(output);
                    std::ofstream(std::filesystem::path(argv[3])/std::filesystem::path(argv[i]).filename())<<output;
                }
            }
            return 0;
        }
        affineTests();const std::string stock=syntheticShader();assemble(stock);
        for(mode=0;mode<3;++mode)
        {
            const std::string patched=InjectClassicCascade4Receiver(stock);
            require(!patched.empty(),"synthetic stock branch not recognized");assemble(patched);
            require(patched.find("dcl_texcoord3")==std::string::npos,"unproven interpolator added");
            require(patched.find("c35")==std::string::npos,"blend constants remain");
        }
        mode=0;
        require(InjectClassicCascade4Receiver(ReplaceShaderRegisterToken(stock,"s8","s10")).empty(),"unknown shader accepted");
        require(InjectClassicCascade4Receiver("ps_3_0\nmov oC0, c0\n").empty(),"non-CSM shader accepted");
#ifdef _WIN32
        std::cout<<"PASS: 1000 affine pairs, exact identity, invalid inputs; stock and all 3 modes assembled with D3DAssemble\n";
#else
        std::cout<<"PASS: 1000 affine pairs, exact identity, invalid inputs; all 3 modes transformed (D3DAssemble requires Windows)\n";
#endif
        return 0;
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
'''
Path(sys.argv[1]).write_text(prefix+affine+helpers+tests)
