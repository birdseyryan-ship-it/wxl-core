// Executes the same compiler, entrypoint, targets and flags as ShaderRuntime.
// This qualifies SM3 bytecode compilation only, not GPU execution or fidelity.
#include "water/Slot3Shaders.hpp"
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>

static bool Compile(const char* source, const char* target, const char* name) {
    ID3DBlob* code=nullptr;
    ID3DBlob* errors=nullptr;
    const HRESULT hr=D3DCompile(source,std::strlen(source),name,nullptr,nullptr,
        "main",target,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,&code,&errors);
    if (errors) {
        std::fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stdout);
        errors->Release();
    }
    if (FAILED(hr)||!code) {
        std::printf("FAIL %s hr=%08lx\n",name,static_cast<unsigned long>(hr));
        if(code)code->Release();
        return false;
    }
    ID3DBlob* assembly=nullptr;
    const HRESULT disasm=D3DDisassemble(code->GetBufferPointer(),code->GetBufferSize(),0,nullptr,&assembly);
    bool valid=SUCCEEDED(disasm)&&assembly;
    if (valid) {
        const std::string text(static_cast<const char*>(assembly->GetBufferPointer()),assembly->GetBufferSize());
        valid=text.find(target)!=std::string::npos;
        if (std::strcmp(target,"ps_3_0")==0) {
            // Verify the exact resource slots survive actual compilation.
            for (const char* slot : {"dcl_2d s0","dcl_2d s1","dcl_2d s5","dcl_2d s6","dcl_2d s7"})
                valid=valid && text.find(slot)!=std::string::npos;
        }
        const std::string filename=std::string(name)+".asm";
        if (FILE* file=std::fopen(filename.c_str(),"wb")) {
            std::fwrite(text.data(),1,text.size(),file);std::fclose(file);
        } else valid=false;
    }
    std::printf("%s %s %s: %zu bytes\n",valid?"PASS":"FAIL",name,target,code->GetBufferSize());
    if(assembly)assembly->Release();
    code->Release();
    return valid;
}
int main() {
    const bool vs=Compile(wxl::water::slot3::shaders::kVertexHlsl,"vs_3_0","R6_ProcWater_VS0");
    const bool ps=Compile(wxl::water::slot3::shaders::kPixelHlsl,"ps_3_0","R6_ProcWaterAbove_PS3");
    return vs&&ps ? 0:1;
}
