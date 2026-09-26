// R6 P01/A0/B1/Q0 Classic-water shader runtime. GPL-3.0-or-later.
#include "water/Slot3ShaderRuntime.hpp"

#include "water/Slot3Shaders.hpp"
#include "common/Log.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace wxl::water::slot3
{
namespace
{
template<class T>
void SafeRelease(T*& value) noexcept
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}

bool Compile(const char* source,
             const char* target,
             const char* label,
             ID3DBlob** out) noexcept
{
    if (!source || !target || !label || !out)
        return false;

    *out = nullptr;
    ID3DBlob* errors = nullptr;

    const HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        label,
        nullptr,
        nullptr,
        "main",
        target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        out,
        &errors);

    if (FAILED(hr) || !*out)
    {
        if (errors)
        {
            const int length = static_cast<int>(
                std::min<size_t>(errors->GetBufferSize(), 1800));
            WLOG_ERROR(
                "r6-slot3: %s compile failed target=%s hr=0x%08X: %.*s",
                label,
                target,
                static_cast<unsigned>(hr),
                length,
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        else
        {
            WLOG_ERROR(
                "r6-slot3: %s compile failed target=%s hr=0x%08X",
                label,
                target,
                static_cast<unsigned>(hr));
        }

        SafeRelease(errors);
        SafeRelease(*out);
        return false;
    }

    SafeRelease(errors);
    return true;
}

bool CreateVertex(IDirect3DDevice9* device,
                  IDirect3DVertexShader9** out) noexcept
{
    if (!device || !out)
        return false;

    *out = nullptr;
    ID3DBlob* code = nullptr;

    if (!Compile(shaders::kVertexHlsl,
                 "vs_3_0",
                 "R6 Classic ProcWater VS0",
                 &code))
        return false;

    const HRESULT hr = device->CreateVertexShader(
        static_cast<const DWORD*>(code->GetBufferPointer()), out);
    SafeRelease(code);

    if (FAILED(hr) || !*out)
    {
        WLOG_ERROR(
            "r6-slot3: CreateVertexShader failed hr=0x%08X",
            static_cast<unsigned>(hr));
        SafeRelease(*out);
        return false;
    }

    return true;
}

bool CreatePixel(IDirect3DDevice9* device,
                 IDirect3DPixelShader9** out) noexcept
{
    if (!device || !out)
        return false;

    *out = nullptr;
    ID3DBlob* code = nullptr;

    if (!Compile(shaders::kPixelHlsl,
                 "ps_3_0",
                 "R6 Classic ProcWaterAbove PS3",
                 &code))
        return false;

    const HRESULT hr = device->CreatePixelShader(
        static_cast<const DWORD*>(code->GetBufferPointer()), out);
    SafeRelease(code);

    if (FAILED(hr) || !*out)
    {
        WLOG_ERROR(
            "r6-slot3: CreatePixelShader failed hr=0x%08X",
            static_cast<unsigned>(hr));
        SafeRelease(*out);
        return false;
    }

    return true;
}
} // namespace

ShaderRuntime::~ShaderRuntime()
{
    Reset();
}

bool ShaderRuntime::Ensure(IDirect3DDevice9* device) noexcept
{
    if (!device)
        return false;

    if (ReadyFor(device))
        return true;

    // A device change or a partial prior creation can never be reused.
    Reset();
    device_ = device;

    if (!CreateVertex(device, &vertex_) || !CreatePixel(device, &pixel_))
    {
        Reset();
        return false;
    }

    return true;
}

void ShaderRuntime::Reset() noexcept
{
    SafeRelease(pixel_);
    SafeRelease(vertex_);
    device_ = nullptr;
}

bool ShaderRuntime::ReadyFor(IDirect3DDevice9* device) const noexcept
{
    return device && device_ == device && vertex_ && pixel_;
}
} // namespace wxl::water::slot3
