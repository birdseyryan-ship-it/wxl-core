// wxl-modern-render: SMAA 1x backend for the Proton/Wine D3D9 path.
//
// This is intentionally separate from the native-Windows D3D12 SMAA backend.
// It uses the same vendored Jimenez et al. SMAA source + lookup tables, but
// compiles the original HLSL3/DX9 path:
//
//     resolved world
//       -> color edge detection
//       -> blending weight calculation
//       -> neighborhood blending
//       -> native WoW backbuffer
//
// The WoW UI is drawn only after this returns.

#include "gpu/D3D9Smaa.hpp"

#include "common/Log.hpp"

#include "../../vendor/smaa/SMAA_embed.hpp"
#include "../../vendor/smaa/AreaTex.h"
#include "../../vendor/smaa/SearchTex.h"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace wxl::scripts::render_modern::d3d9smaa
{
    namespace
    {
        template<class T>
        void SafeRelease(T*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        struct SmaaVertex
        {
            float x, y, z;
            float u, v;
        };

        IDirect3DDevice9* g_device = nullptr;

        IDirect3DVertexDeclaration9* g_decl = nullptr;

        IDirect3DVertexShader9* g_edgeVs = nullptr;
        IDirect3DVertexShader9* g_blendVs[3] = { nullptr, nullptr, nullptr };
        IDirect3DVertexShader9* g_neighborhoodVs = nullptr;

        IDirect3DPixelShader9* g_edgePs[3] = { nullptr, nullptr, nullptr };
        IDirect3DPixelShader9* g_blendPs[3] = { nullptr, nullptr, nullptr };
        IDirect3DPixelShader9* g_neighborhoodPs = nullptr;

        IDirect3DTexture9* g_edgeTex = nullptr;
        IDirect3DSurface9* g_edgeSurface = nullptr;
        IDirect3DTexture9* g_blendTex = nullptr;
        IDirect3DSurface9* g_blendSurface = nullptr;

        IDirect3DTexture9* g_areaTex = nullptr;
        IDirect3DTexture9* g_searchTex = nullptr;

        unsigned g_width = 0;
        unsigned g_height = 0;

        bool g_loggedFrame[3] = { false, false, false };
        bool g_loggedShaders[3] = { false, false, false };

        void ReleaseTargets()
        {
            SafeRelease(g_edgeSurface);
            SafeRelease(g_edgeTex);
            SafeRelease(g_blendSurface);
            SafeRelease(g_blendTex);
            g_width = 0;
            g_height = 0;
        }

        void ReleaseAll()
        {
            ReleaseTargets();

            SafeRelease(g_areaTex);
            SafeRelease(g_searchTex);

            SafeRelease(g_decl);

            SafeRelease(g_edgeVs);
            for (auto*& p : g_blendVs)
                SafeRelease(p);
            SafeRelease(g_neighborhoodVs);

            for (auto*& p : g_edgePs)
                SafeRelease(p);
            for (auto*& p : g_blendPs)
                SafeRelease(p);
            SafeRelease(g_neighborhoodPs);

            g_device = nullptr;

            for (bool& b : g_loggedFrame)
                b = false;

            for (bool& b : g_loggedShaders)
                b = false;
        }

        bool CompileBlob(const std::string& source,
                         const char* target,
                         const char* label,
                         ID3DBlob** out)
        {
            if (!out) return false;
            *out = nullptr;

            ID3DBlob* errors = nullptr;

            const HRESULT hr = D3DCompile(
                source.data(),
                source.size(),
                label,
                nullptr,
                nullptr,
                "main",
                target,
                D3DCOMPILE_ENABLE_STRICTNESS |
                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                out,
                &errors);

            if (FAILED(hr) || !*out)
            {
                if (errors)
                {
                    const int n = static_cast<int>(
                        std::min<size_t>(
                            errors->GetBufferSize(), 1800));

                    WLOG_ERROR(
                        "wxl-modern-d3d9: %s compile failed "
                        "target=%s hr=0x%08X: %.*s",
                        label,
                        target,
                        static_cast<unsigned>(hr),
                        n,
                        static_cast<const char*>(
                            errors->GetBufferPointer()));
                }
                else
                {
                    WLOG_ERROR(
                        "wxl-modern-d3d9: %s compile failed "
                        "target=%s hr=0x%08X",
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

        bool CompileVertexShader(IDirect3DDevice9* dev,
                                 const std::string& source,
                                 const char* label,
                                 IDirect3DVertexShader9** out)
        {
            ID3DBlob* code = nullptr;

            if (!CompileBlob(source, "vs_3_0", label, &code))
                return false;

            const HRESULT hr = dev->CreateVertexShader(
                static_cast<const DWORD*>(code->GetBufferPointer()),
                out);

            SafeRelease(code);

            if (FAILED(hr) || !*out)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: %s CreateVertexShader failed "
                    "hr=0x%08X",
                    label,
                    static_cast<unsigned>(hr));
                return false;
            }

            return true;
        }

        bool CompilePixelShader(IDirect3DDevice9* dev,
                                const std::string& source,
                                const char* label,
                                IDirect3DPixelShader9** out)
        {
            ID3DBlob* code = nullptr;

            if (!CompileBlob(source, "ps_3_0", label, &code))
                return false;

            const HRESULT hr = dev->CreatePixelShader(
                static_cast<const DWORD*>(code->GetBufferPointer()),
                out);

            SafeRelease(code);

            if (FAILED(hr) || !*out)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: %s CreatePixelShader failed "
                    "hr=0x%08X",
                    label,
                    static_cast<unsigned>(hr));
                return false;
            }

            return true;
        }

        const char* PresetDefine(int tier)
        {
            switch (tier)
            {
                case 0:
                    return "#define SMAA_PRESET_LOW 1\n";
                case 2:
                    return "#define SMAA_PRESET_HIGH 1\n";
                default:
                    return "#define SMAA_PRESET_MEDIUM 1\n";
            }
        }

        std::string SmaaBaseSource(int tier)
        {
            tier = std::max(0, std::min(2, tier));

            std::string src;
            src += "#define SMAA_HLSL_3 1\n";
            src += PresetDefine(tier);

            // Runtime rather than compile-time metrics: c0 is set for both
            // the vertex and pixel shaders every frame.
            src +=
                "float4 SMAA_RT_METRICS : register(c0);\n";

            for (int i = 0; i < k_smaa_partCount; ++i)
                src += k_smaa_parts[i];

            return src;
        }

        std::string EdgeVsSource()
        {
            std::string src = SmaaBaseSource(1);
            src += R"HLSL(

void main(inout float4 position : POSITION,
          inout float2 texcoord : TEXCOORD0,
          out float4 offset[3] : TEXCOORD1)
{
    SMAAEdgeDetectionVS(texcoord, offset);
}
)HLSL";
            return src;
        }

        std::string BlendVsSource(int tier)
        {
            std::string src = SmaaBaseSource(tier);
            src += R"HLSL(

void main(inout float4 position : POSITION,
          inout float2 texcoord : TEXCOORD0,
          out float2 pixcoord : TEXCOORD1,
          out float4 offset[3] : TEXCOORD2)
{
    SMAABlendingWeightCalculationVS(
        texcoord, pixcoord, offset);
}
)HLSL";
            return src;
        }

        std::string NeighborhoodVsSource()
        {
            std::string src = SmaaBaseSource(1);
            src += R"HLSL(

void main(inout float4 position : POSITION,
          inout float2 texcoord : TEXCOORD0,
          out float4 offset : TEXCOORD1)
{
    SMAANeighborhoodBlendingVS(texcoord, offset);
}
)HLSL";
            return src;
        }

        std::string EdgePsSource(int tier)
        {
            std::string src = SmaaBaseSource(tier);
            src += R"HLSL(

sampler2D colorGammaTex : register(s0);

float4 main(float2 texcoord : TEXCOORD0,
            float4 offset[3] : TEXCOORD1) : COLOR0
{
    return float4(
        SMAAColorEdgeDetectionPS(
            texcoord, offset, colorGammaTex),
        0.0,
        0.0);
}
)HLSL";
            return src;
        }

        std::string BlendPsSource(int tier)
        {
            std::string src = SmaaBaseSource(tier);
            src += R"HLSL(

sampler2D edgesTex  : register(s0);
sampler2D areaTex   : register(s1);
sampler2D searchTex : register(s2);

float4 main(float2 texcoord : TEXCOORD0,
            float2 pixcoord : TEXCOORD1,
            float4 offset[3] : TEXCOORD2) : COLOR0
{
    return SMAABlendingWeightCalculationPS(
        texcoord,
        pixcoord,
        offset,
        edgesTex,
        areaTex,
        searchTex,
        float4(0.0, 0.0, 0.0, 0.0));
}
)HLSL";
            return src;
        }

        std::string NeighborhoodPsSource()
        {
            std::string src = SmaaBaseSource(1);
            src += R"HLSL(

sampler2D colorTex : register(s0);
sampler2D blendTex : register(s1);

float4 main(float2 texcoord : TEXCOORD0,
            float4 offset : TEXCOORD1) : COLOR0
{
    return SMAANeighborhoodBlendingPS(
        texcoord, offset, colorTex, blendTex);
}
)HLSL";
            return src;
        }

        bool EnsureDeclaration(IDirect3DDevice9* dev)
        {
            if (g_decl)
                return true;

            const D3DVERTEXELEMENT9 elems[] = {
                {
                    0, 0,
                    D3DDECLTYPE_FLOAT3,
                    D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_POSITION,
                    0
                },
                {
                    0, 12,
                    D3DDECLTYPE_FLOAT2,
                    D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_TEXCOORD,
                    0
                },
                D3DDECL_END()
            };

            const HRESULT hr =
                dev->CreateVertexDeclaration(elems, &g_decl);

            if (FAILED(hr) || !g_decl)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA vertex declaration failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                return false;
            }

            return true;
        }

        bool EnsureBaseShaders(IDirect3DDevice9* dev)
        {
            if (!g_edgeVs)
            {
                if (!CompileVertexShader(
                        dev,
                        EdgeVsSource(),
                        "R3C SMAA edge VS",
                        &g_edgeVs))
                    return false;
            }

            if (!g_neighborhoodVs)
            {
                if (!CompileVertexShader(
                        dev,
                        NeighborhoodVsSource(),
                        "R3C SMAA neighborhood VS",
                        &g_neighborhoodVs))
                    return false;
            }

            if (!g_neighborhoodPs)
            {
                if (!CompilePixelShader(
                        dev,
                        NeighborhoodPsSource(),
                        "R3C SMAA neighborhood PS",
                        &g_neighborhoodPs))
                    return false;
            }

            return true;
        }

        bool EnsureTierShaders(IDirect3DDevice9* dev, int tier)
        {
            tier = std::max(0, std::min(2, tier));

            if (!EnsureBaseShaders(dev))
                return false;

            char label[96] = {};

            if (!g_blendVs[tier])
            {
                std::snprintf(
                    label, sizeof(label),
                    "R3C SMAA blend VS tier %d", tier);

                if (!CompileVertexShader(
                        dev,
                        BlendVsSource(tier),
                        label,
                        &g_blendVs[tier]))
                    return false;
            }

            if (!g_edgePs[tier])
            {
                std::snprintf(
                    label, sizeof(label),
                    "R3C SMAA edge PS tier %d", tier);

                if (!CompilePixelShader(
                        dev,
                        EdgePsSource(tier),
                        label,
                        &g_edgePs[tier]))
                    return false;
            }

            if (!g_blendPs[tier])
            {
                std::snprintf(
                    label, sizeof(label),
                    "R3C SMAA blend PS tier %d", tier);

                if (!CompilePixelShader(
                        dev,
                        BlendPsSource(tier),
                        label,
                        &g_blendPs[tier]))
                    return false;
            }

            if (!g_loggedShaders[tier])
            {
                g_loggedShaders[tier] = true;
                WLOG_INFO(
                    "wxl-modern-d3d9: SMAA shaders ready (tier=%d)",
                    tier);
            }

            return true;
        }

        bool EnsureLookupTextures(IDirect3DDevice9* dev)
        {
            if (g_areaTex && g_searchTex)
                return true;

            SafeRelease(g_areaTex);
            SafeRelease(g_searchTex);

            HRESULT hr = dev->CreateTexture(
                AREATEX_WIDTH,
                AREATEX_HEIGHT,
                1,
                D3DUSAGE_DYNAMIC,
                D3DFMT_A8L8,
                D3DPOOL_DEFAULT,
                &g_areaTex,
                nullptr);

            if (FAILED(hr) || !g_areaTex)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA area LUT creation failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                return false;
            }

            D3DLOCKED_RECT areaLock = {};
            hr = g_areaTex->LockRect(
                0, &areaLock, nullptr, D3DLOCK_DISCARD);

            if (FAILED(hr))
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA area LUT lock failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                SafeRelease(g_areaTex);
                return false;
            }

            for (int y = 0; y < AREATEX_HEIGHT; ++y)
            {
                std::memcpy(
                    static_cast<unsigned char*>(areaLock.pBits) +
                        y * areaLock.Pitch,
                    areaTexBytes + y * AREATEX_PITCH,
                    AREATEX_PITCH);
            }

            g_areaTex->UnlockRect(0);

            hr = dev->CreateTexture(
                SEARCHTEX_WIDTH,
                SEARCHTEX_HEIGHT,
                1,
                D3DUSAGE_DYNAMIC,
                D3DFMT_L8,
                D3DPOOL_DEFAULT,
                &g_searchTex,
                nullptr);

            if (FAILED(hr) || !g_searchTex)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA search LUT creation failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                SafeRelease(g_areaTex);
                return false;
            }

            D3DLOCKED_RECT searchLock = {};
            hr = g_searchTex->LockRect(
                0, &searchLock, nullptr, D3DLOCK_DISCARD);

            if (FAILED(hr))
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA search LUT lock failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                SafeRelease(g_areaTex);
                SafeRelease(g_searchTex);
                return false;
            }

            for (int y = 0; y < SEARCHTEX_HEIGHT; ++y)
            {
                std::memcpy(
                    static_cast<unsigned char*>(searchLock.pBits) +
                        y * searchLock.Pitch,
                    searchTexBytes + y * SEARCHTEX_PITCH,
                    SEARCHTEX_PITCH);
            }

            g_searchTex->UnlockRect(0);

            WLOG_INFO(
                "wxl-modern-d3d9: SMAA lookup textures ready");

            return true;
        }

        bool EnsureTargets(IDirect3DDevice9* dev,
                           unsigned width,
                           unsigned height)
        {
            if (g_edgeTex &&
                g_blendTex &&
                g_width == width &&
                g_height == height)
                return true;

            ReleaseTargets();

            HRESULT hr = dev->CreateTexture(
                width,
                height,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                &g_edgeTex,
                nullptr);

            if (FAILED(hr) || !g_edgeTex)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA edge target creation failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                return false;
            }

            hr = g_edgeTex->GetSurfaceLevel(
                0, &g_edgeSurface);

            if (FAILED(hr) || !g_edgeSurface)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA edge surface failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                ReleaseTargets();
                return false;
            }

            hr = dev->CreateTexture(
                width,
                height,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                &g_blendTex,
                nullptr);

            if (FAILED(hr) || !g_blendTex)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA blend target creation failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                ReleaseTargets();
                return false;
            }

            hr = g_blendTex->GetSurfaceLevel(
                0, &g_blendSurface);

            if (FAILED(hr) || !g_blendSurface)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: SMAA blend surface failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                ReleaseTargets();
                return false;
            }

            g_width = width;
            g_height = height;

            WLOG_INFO(
                "wxl-modern-d3d9: SMAA intermediates ready %ux%u",
                width, height);

            return true;
        }

        void SetLinearClamp(IDirect3DDevice9* dev, DWORD sampler)
        {
            dev->SetSamplerState(
                sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(
                sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(
                sampler, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(
                sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(
                sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(
                sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(
                sampler, D3DSAMP_SRGBTEXTURE, FALSE);
        }

        void SetPointClamp(IDirect3DDevice9* dev, DWORD sampler)
        {
            dev->SetSamplerState(
                sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(
                sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(
                sampler, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(
                sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(
                sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(
                sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(
                sampler, D3DSAMP_SRGBTEXTURE, FALSE);
        }

        void SetCommonRenderState(IDirect3DDevice9* dev)
        {
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
            dev->SetRenderState(
                D3DRS_COLORWRITEENABLE,
                D3DCOLORWRITEENABLE_RED |
                D3DCOLORWRITEENABLE_GREEN |
                D3DCOLORWRITEENABLE_BLUE |
                D3DCOLORWRITEENABLE_ALPHA);
        }

        HRESULT DrawFullscreen(IDirect3DDevice9* dev,
                               unsigned width,
                               unsigned height)
        {
            const float px =
                1.0f / static_cast<float>(width);
            const float py =
                1.0f / static_cast<float>(height);

            // Matches SMAA's official D3D9 fullscreen-quad alignment.
            const SmaaVertex quad[4] = {
                { -1.0f - px,  1.0f + py, 0.5f, 0.0f, 0.0f },
                {  1.0f - px,  1.0f + py, 0.5f, 1.0f, 0.0f },
                { -1.0f - px, -1.0f + py, 0.5f, 0.0f, 1.0f },
                {  1.0f - px, -1.0f + py, 0.5f, 1.0f, 1.0f },
            };

            return dev->DrawPrimitiveUP(
                D3DPT_TRIANGLESTRIP,
                2,
                quad,
                sizeof(SmaaVertex));
        }

        bool CheckDraw(HRESULT hr, const char* pass)
        {
            if (SUCCEEDED(hr))
                return true;

            WLOG_ERROR(
                "wxl-modern-d3d9: SMAA %s draw failed hr=0x%08X",
                pass,
                static_cast<unsigned>(hr));

            return false;
        }

        void UnbindTextures(IDirect3DDevice9* dev)
        {
            dev->SetTexture(0, nullptr);
            dev->SetTexture(1, nullptr);
            dev->SetTexture(2, nullptr);
        }
    }

    void PrepareForReset()
    {
        ReleaseAll();
    }

    bool Render(IDirect3DDevice9* device,
                IDirect3DTexture9* scene,
                IDirect3DSurface9* destination,
                unsigned width,
                unsigned height,
                Quality quality)
    {
        if (!device ||
            !scene ||
            !destination ||
            width == 0 ||
            height == 0)
            return false;

        if (g_device != device)
        {
            ReleaseAll();
            g_device = device;
        }

        const int tier = std::max(
            0, std::min(2, static_cast<int>(quality)));

        if (!EnsureDeclaration(device) ||
            !EnsureTierShaders(device, tier) ||
            !EnsureLookupTextures(device) ||
            !EnsureTargets(device, width, height))
            return false;

        const float metrics[4] = {
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height),
            static_cast<float>(width),
            static_cast<float>(height)
        };

        D3DVIEWPORT9 vp = {};
        vp.X = 0;
        vp.Y = 0;
        vp.Width = width;
        vp.Height = height;
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;

        device->SetViewport(&vp);
        device->SetVertexDeclaration(g_decl);
        SetCommonRenderState(device);

        device->SetVertexShaderConstantF(0, metrics, 1);
        device->SetPixelShaderConstantF(0, metrics, 1);

        // --------------------------------------------------------
        // Pass 1: color edge detection.
        // --------------------------------------------------------

        HRESULT hr = device->SetRenderTarget(
            0, g_edgeSurface);

        if (FAILED(hr))
        {
            WLOG_ERROR(
                "wxl-modern-d3d9: SMAA edge SetRenderTarget failed "
                "hr=0x%08X",
                static_cast<unsigned>(hr));
            return false;
        }

        hr = device->Clear(
            0,
            nullptr,
            D3DCLEAR_TARGET,
            D3DCOLOR_ARGB(0, 0, 0, 0),
            1.0f,
            0);

        if (FAILED(hr))
            return false;

        device->SetVertexShader(g_edgeVs);
        device->SetPixelShader(g_edgePs[tier]);
        device->SetTexture(0, scene);
        SetLinearClamp(device, 0);

        if (!CheckDraw(
                DrawFullscreen(device, width, height),
                "edge"))
        {
            UnbindTextures(device);
            return false;
        }

        // --------------------------------------------------------
        // Pass 2: blending-weight calculation.
        // --------------------------------------------------------

        hr = device->SetRenderTarget(
            0, g_blendSurface);

        if (FAILED(hr))
        {
            UnbindTextures(device);
            WLOG_ERROR(
                "wxl-modern-d3d9: SMAA blend SetRenderTarget failed "
                "hr=0x%08X",
                static_cast<unsigned>(hr));
            return false;
        }

        hr = device->Clear(
            0,
            nullptr,
            D3DCLEAR_TARGET,
            D3DCOLOR_ARGB(0, 0, 0, 0),
            1.0f,
            0);

        if (FAILED(hr))
        {
            UnbindTextures(device);
            return false;
        }

        device->SetVertexShader(g_blendVs[tier]);
        device->SetPixelShader(g_blendPs[tier]);

        device->SetTexture(0, g_edgeTex);
        device->SetTexture(1, g_areaTex);
        device->SetTexture(2, g_searchTex);

        SetLinearClamp(device, 0);
        SetLinearClamp(device, 1);
        SetPointClamp(device, 2);

        if (!CheckDraw(
                DrawFullscreen(device, width, height),
                "blend-weight"))
        {
            UnbindTextures(device);
            return false;
        }

        // --------------------------------------------------------
        // Pass 3: neighborhood blending into WoW's native target.
        // --------------------------------------------------------

        hr = device->SetRenderTarget(
            0, destination);

        if (FAILED(hr))
        {
            UnbindTextures(device);
            WLOG_ERROR(
                "wxl-modern-d3d9: SMAA neighborhood SetRenderTarget "
                "failed hr=0x%08X",
                static_cast<unsigned>(hr));
            return false;
        }

        device->SetVertexShader(g_neighborhoodVs);
        device->SetPixelShader(g_neighborhoodPs);

        device->SetTexture(0, scene);
        device->SetTexture(1, g_blendTex);
        device->SetTexture(2, nullptr);

        SetLinearClamp(device, 0);
        SetLinearClamp(device, 1);

        const bool ok = CheckDraw(
            DrawFullscreen(device, width, height),
            "neighborhood");

        UnbindTextures(device);

        if (ok && !g_loggedFrame[tier])
        {
            g_loggedFrame[tier] = true;
            WLOG_INFO(
                "wxl-modern-d3d9: SMAA frame PASS "
                "%ux%u tier=%d",
                width,
                height,
                tier);
        }

        return ok;
    }
}
