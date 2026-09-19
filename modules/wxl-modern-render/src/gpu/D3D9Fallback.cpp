// Proton/Wine D3D9 post-process fallback for wxl-modern-render.
//
// Wine exposes the D3D9On12 interface surface needed for capability discovery,
// but the live Proton path cannot unwrap the engine backbuffer for our D3D12
// post-process. On Wine/Proton we therefore stay inside D3D9:
//
//   MSAA world backbuffer
//       -> StretchRect resolve into a single-sample RT texture
//       -> pixel-shader post-process
//       -> write back into the engine backbuffer
//       -> WoW draws its UI normally on top
//
// Native Windows continues to use the D3D12 modern-render implementation.

#include "gpu/D3D9Fallback.hpp"

#include "client/CWorldScene/RenderModernBridge.hpp"
#include "common/Log.hpp"

#include "../../vendor/fxaa/Fxaa3_11_embed.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace wxl::scripts::render_modern::d3d9fallback
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

        IDirect3DDevice9*      g_device = nullptr;
        IDirect3DTexture9*     g_sceneTexture = nullptr;
        IDirect3DSurface9*     g_sceneSurface = nullptr;
        IDirect3DPixelShader9* g_proofShader = nullptr;
        IDirect3DPixelShader9* g_fxaaShader[3] = { nullptr, nullptr, nullptr };

        UINT      g_width = 0;
        UINT      g_height = 0;
        D3DFORMAT g_format = D3DFMT_UNKNOWN;

        bool g_proofTint = false;
        bool g_loggedActive = false;
        bool g_loggedEndFail = false;
        bool g_loggedStretchFail = false;
        bool g_loggedBeginFail = false;
        bool g_loggedDrawFail = false;

        struct FsVertex
        {
            float x, y, z, rhw;
            float u, v;
        };

        constexpr DWORD kFsFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

        void ReleaseTarget()
        {
            SafeRelease(g_sceneSurface);
            SafeRelease(g_sceneTexture);
            g_width = 0;
            g_height = 0;
            g_format = D3DFMT_UNKNOWN;
        }

        void ReleaseRuntime()
        {
            ReleaseTarget();
            SafeRelease(g_proofShader);
            for (auto*& p : g_fxaaShader)
                SafeRelease(p);
            g_device = nullptr;

            g_loggedActive = false;
            g_loggedEndFail = false;
            g_loggedStretchFail = false;
            g_loggedBeginFail = false;
            g_loggedDrawFail = false;
        }

        bool CompilePixelShader(IDirect3DDevice9* dev,
                                const std::string& src,
                                const char* label,
                                IDirect3DPixelShader9** out)
        {
            if (!dev || !out) return false;

            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;

            const HRESULT chr = D3DCompile(
                src.data(),
                src.size(),
                label,
                nullptr,
                nullptr,
                "main",
                "ps_3_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                &code,
                &errors);

            if (FAILED(chr) || !code)
            {
                if (errors)
                {
                    const int n = static_cast<int>(
                        std::min<size_t>(errors->GetBufferSize(), 1200));
                    WLOG_ERROR("wxl-modern-d3d9: %s compile failed hr=0x%08X: %.*s",
                               label, static_cast<unsigned>(chr), n,
                               static_cast<const char*>(errors->GetBufferPointer()));
                }
                else
                {
                    WLOG_ERROR("wxl-modern-d3d9: %s compile failed hr=0x%08X",
                               label, static_cast<unsigned>(chr));
                }

                SafeRelease(errors);
                SafeRelease(code);
                return false;
            }

            SafeRelease(errors);

            const HRESULT shr = dev->CreatePixelShader(
                static_cast<const DWORD*>(code->GetBufferPointer()), out);

            SafeRelease(code);

            if (FAILED(shr) || !*out)
            {
                WLOG_ERROR("wxl-modern-d3d9: %s CreatePixelShader failed hr=0x%08X",
                           label, static_cast<unsigned>(shr));
                return false;
            }

            return true;
        }

        bool EnsureProofShader(IDirect3DDevice9* dev)
        {
            if (g_proofShader) return true;

            // Intentionally conspicuous warm-magenta cast. This is NOT a
            // production visual effect; it proves that the world image is
            // genuinely passing through the Proton D3D9 backend.
            static const char* kProofPs = R"HLSL(
sampler2D scene : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = tex2D(scene, uv);
    float3 tinted;
    tinted.r = saturate(c.r * 1.15 + 0.18);
    tinted.g = saturate(c.g * 0.62);
    tinted.b = saturate(c.b * 0.88 + 0.08);
    return float4(tinted, 1.0);
}
)HLSL";

            if (!CompilePixelShader(dev, kProofPs,
                                    "R3B proof tint", &g_proofShader))
                return false;

            WLOG_INFO("wxl-modern-d3d9: proof-tint shader ready");
            return true;
        }

        std::string BuildFxaaSource(int tier)
        {
            struct Tier
            {
                int preset;
                const char* subpix;
                const char* edge;
                const char* edgeMin;
            };

            static const Tier kTiers[3] = {
                { 12, "0.50", "0.250", "0.0833" },
                { 29, "0.75", "0.166", "0.0625" },
                { 39, "1.00", "0.125", "0.0312" },
            };

            tier = std::max(0, std::min(2, tier));
            const Tier& t = kTiers[tier];

            std::string src;
            src += "#define FXAA_PC 1\n";
            src += "#define FXAA_HLSL_3 1\n";
            src += "#define FXAA_GREEN_AS_LUMA 1\n";
            src += "#define FXAA_QUALITY__PRESET " + std::to_string(t.preset) + "\n";
            src += std::string("#define WX_FXAA_SUBPIX ")  + t.subpix  + "\n";
            src += std::string("#define WX_FXAA_EDGE ")    + t.edge    + "\n";
            src += std::string("#define WX_FXAA_EDGEMIN ") + t.edgeMin + "\n";

            for (int i = 0; i < k_fxaa3_11_partCount; ++i)
                src += k_fxaa3_11_parts[i];

            src += R"HLSL(

sampler2D scene : register(s0);
float4 wxRcpFrame : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    return FxaaPixelShader(
        uv,
        float4(0,0,0,0),
        scene,
        scene,
        scene,
        scene,
        wxRcpFrame.xy,
        float4(0,0,0,0),
        float4(0,0,0,0),
        float4(0,0,0,0),
        WX_FXAA_SUBPIX,
        WX_FXAA_EDGE,
        WX_FXAA_EDGEMIN,
        8.0,
        0.125,
        0.05,
        float4(1.0, -1.0, 0.25, -0.25));
}
)HLSL";

            return src;
        }

        bool EnsureFxaaShader(IDirect3DDevice9* dev, int tier)
        {
            tier = std::max(0, std::min(2, tier));

            if (g_fxaaShader[tier])
                return true;

            const std::string src = BuildFxaaSource(tier);

            char label[64] = {};
            std::snprintf(label, sizeof(label),
                          "R3B FXAA tier %d", tier);

            if (!CompilePixelShader(dev, src, label,
                                    &g_fxaaShader[tier]))
                return false;

            WLOG_INFO("wxl-modern-d3d9: FXAA shader ready (tier=%d)",
                      tier);
            return true;
        }

        bool EnsureTarget(IDirect3DDevice9* dev,
                          IDirect3DSurface9* backbuffer,
                          D3DSURFACE_DESC& desc)
        {
            if (!dev || !backbuffer) return false;

            if (FAILED(backbuffer->GetDesc(&desc)))
                return false;

            if (g_sceneTexture &&
                g_width == desc.Width &&
                g_height == desc.Height &&
                g_format == desc.Format)
                return true;

            ReleaseTarget();

            HRESULT hr = dev->CreateTexture(
                desc.Width,
                desc.Height,
                1,
                D3DUSAGE_RENDERTARGET,
                desc.Format,
                D3DPOOL_DEFAULT,
                &g_sceneTexture,
                nullptr);

            if (FAILED(hr) || !g_sceneTexture)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: resolve texture creation failed "
                    "%ux%u fmt=%u hr=0x%08X",
                    desc.Width, desc.Height,
                    static_cast<unsigned>(desc.Format),
                    static_cast<unsigned>(hr));
                return false;
            }

            hr = g_sceneTexture->GetSurfaceLevel(0, &g_sceneSurface);
            if (FAILED(hr) || !g_sceneSurface)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: resolve surface acquisition failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
                ReleaseTarget();
                return false;
            }

            g_width = desc.Width;
            g_height = desc.Height;
            g_format = desc.Format;

            WLOG_INFO(
                "wxl-modern-d3d9: resolve target ready "
                "%ux%u fmt=%u sourceMSAA=%u",
                g_width, g_height,
                static_cast<unsigned>(g_format),
                static_cast<unsigned>(desc.MultiSampleType));

            return true;
        }

        void RestoreDeviceState(IDirect3DDevice9* dev,
                                IDirect3DStateBlock9* state,
                                IDirect3DSurface9* oldRt,
                                IDirect3DSurface9* oldDepth,
                                const D3DVIEWPORT9& oldViewport)
        {
            if (state)
                state->Apply();

            if (oldRt)
                dev->SetRenderTarget(0, oldRt);

            dev->SetDepthStencilSurface(oldDepth);
            dev->SetViewport(&oldViewport);
        }
    }

    bool Available()
    {
        static const bool available = []()
        {
            HMODULE ntdll = GetModuleHandleA("ntdll.dll");
            return ntdll &&
                   GetProcAddress(ntdll, "wine_get_version") != nullptr;
        }();

        return available;
    }

    bool ProofTint()
    {
        return g_proofTint;
    }

    void SetProofTint(bool enabled)
    {
        g_proofTint = enabled;
        WLOG_INFO("wxl-modern-d3d9: proof tint %s",
                  enabled ? "enabled" : "disabled");
    }

    void PrepareForReset()
    {
        if (Available())
            ReleaseRuntime();
    }

    bool Frame(IDirect3DDevice9* device,
               bool fxaaEnabled,
               Quality quality)
    {
        if (!Available() || !device)
            return false;

        if (!g_proofTint && !fxaaEnabled)
            return false;

        if (g_device != device)
        {
            ReleaseRuntime();
            g_device = device;
            WLOG_INFO(
                "wxl-modern-d3d9: Proton/Wine fallback backend active");
        }

        const int tier = std::max(
            0, std::min(2, static_cast<int>(quality)));

        IDirect3DPixelShader9* shader = nullptr;

        if (g_proofTint)
        {
            if (!EnsureProofShader(device))
                return false;
            shader = g_proofShader;
        }
        else
        {
            if (!EnsureFxaaShader(device, tier))
                return false;
            shader = g_fxaaShader[tier];
        }

        IDirect3DSurface9* backbuffer = nullptr;
        HRESULT hr = device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);

        if (FAILED(hr) || !backbuffer)
            return false;

        D3DSURFACE_DESC bbDesc = {};
        if (!EnsureTarget(device, backbuffer, bbDesc))
        {
            backbuffer->Release();
            return false;
        }

        IDirect3DStateBlock9* state = nullptr;
        IDirect3DSurface9* oldRt = nullptr;
        IDirect3DSurface9* oldDepth = nullptr;
        D3DVIEWPORT9 oldViewport = {};

        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state)) ||
            !state ||
            FAILED(device->GetRenderTarget(0, &oldRt)) ||
            !oldRt ||
            FAILED(device->GetViewport(&oldViewport)))
        {
            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();
            return false;
        }

        // No depth surface is a legal state, so failure here simply leaves
        // oldDepth null.
        device->GetDepthStencilSurface(&oldDepth);

        // StretchRect cannot execute inside BeginScene/EndScene. Call the
        // original EndScene entry directly so the core's EndScene hook does
        // not emit OnEndScene/ImGui in the middle of this world-only pass.
        hr = static_cast<HRESULT>(
            wxl::runtime::render::EndSceneForPostProcess(device));

        if (FAILED(hr))
        {
            if (!g_loggedEndFail)
            {
                g_loggedEndFail = true;
                WLOG_ERROR(
                    "wxl-modern-d3d9: native EndScene bracket failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(hr));
            }

            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();
            return false;
        }

        const HRESULT stretchHr = device->StretchRect(
            backbuffer, nullptr,
            g_sceneSurface, nullptr,
            D3DTEXF_NONE);

        // Open a new scene immediately, even if the resolve failed. The WoW
        // render path still expects to draw its UI and issue its normal final
        // EndScene later in the frame.
        const HRESULT beginHr = device->BeginScene();

        if (FAILED(beginHr))
        {
            if (!g_loggedBeginFail)
            {
                g_loggedBeginFail = true;
                WLOG_ERROR(
                    "wxl-modern-d3d9: BeginScene reopen failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(beginHr));
            }

            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();
            return false;
        }

        if (FAILED(stretchHr))
        {
            if (!g_loggedStretchFail)
            {
                g_loggedStretchFail = true;
                WLOG_ERROR(
                    "wxl-modern-d3d9: StretchRect resolve failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(stretchHr));
            }

            RestoreDeviceState(
                device, state, oldRt, oldDepth, oldViewport);

            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();
            return false;
        }

        D3DVIEWPORT9 vp = {};
        vp.X = 0;
        vp.Y = 0;
        vp.Width = bbDesc.Width;
        vp.Height = bbDesc.Height;
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;

        device->SetRenderTarget(0, backbuffer);
        device->SetDepthStencilSurface(nullptr);
        device->SetViewport(&vp);

        device->SetVertexShader(nullptr);
        device->SetPixelShader(shader);
        device->SetFVF(kFsFvf);

        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(
            D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED |
            D3DCOLORWRITEENABLE_GREEN |
            D3DCOLORWRITEENABLE_BLUE |
            D3DCOLORWRITEENABLE_ALPHA);

        device->SetTexture(0, g_sceneTexture);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

        const float rcpFrame[4] = {
            1.0f / static_cast<float>(bbDesc.Width),
            1.0f / static_cast<float>(bbDesc.Height),
            0.0f,
            0.0f
        };
        device->SetPixelShaderConstantF(0, rcpFrame, 1);

        const float w = static_cast<float>(bbDesc.Width);
        const float h = static_cast<float>(bbDesc.Height);

        // D3D9's half-pixel convention: -0.5 aligns the transformed quad
        // exactly with pixel centres for a 1:1 fullscreen pass.
        const FsVertex quad[4] = {
            { -0.5f,     -0.5f,     0.0f, 1.0f, 0.0f, 0.0f },
            { w - 0.5f,  -0.5f,     0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,      h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
            { w - 0.5f,   h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
        };

        const HRESULT drawHr = device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP,
            2,
            quad,
            sizeof(FsVertex));

        device->SetTexture(0, nullptr);

        RestoreDeviceState(
            device, state, oldRt, oldDepth, oldViewport);

        if (FAILED(drawHr) && !g_loggedDrawFail)
        {
            g_loggedDrawFail = true;
            WLOG_ERROR(
                "wxl-modern-d3d9: fullscreen draw failed hr=0x%08X",
                static_cast<unsigned>(drawHr));
        }

        if (SUCCEEDED(drawHr) && !g_loggedActive)
        {
            g_loggedActive = true;
            WLOG_INFO(
                "wxl-modern-d3d9: post-process frame PASS "
                "%ux%u sourceMSAA=%u mode=%s tier=%d",
                bbDesc.Width,
                bbDesc.Height,
                static_cast<unsigned>(bbDesc.MultiSampleType),
                g_proofTint ? "proof-tint" : "FXAA",
                tier);
        }

        SafeRelease(state);
        SafeRelease(oldRt);
        SafeRelease(oldDepth);
        backbuffer->Release();

        return SUCCEEDED(drawHr);
    }
}
