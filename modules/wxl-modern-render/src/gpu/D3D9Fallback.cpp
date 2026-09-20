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
#include "gpu/D3D9Smaa.hpp"
#include "gpu/D3D9DepthProbe.hpp"

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

        // R3D1: single-sample sampleable copy of the completed world depth.
        IDirect3DTexture9*     g_depthTexture = nullptr;
        IDirect3DSurface9*     g_depthSurface = nullptr;

        // R3D2 mode 5: explicit two-stage resolve path
        // MSAA D24X8 -> single-sample D24X8 -> INTZ.
        IDirect3DSurface9*     g_depthPlainSurface = nullptr;

        IDirect3DPixelShader9* g_proofShader = nullptr;
        IDirect3DPixelShader9* g_depthProofShader = nullptr;
        IDirect3DPixelShader9* g_fxaaShader[3] = { nullptr, nullptr, nullptr };

        UINT      g_width = 0;
        UINT      g_height = 0;
        D3DFORMAT g_format = D3DFMT_UNKNOWN;

        UINT      g_depthWidth = 0;
        UINT      g_depthHeight = 0;

        bool g_proofTint = false;
        int  g_lastLoggedMode = -1;
        int  g_lastLoggedTier = -1;
        bool g_loggedEndFail = false;
        bool g_loggedStretchFail = false;
        bool g_loggedBeginFail = false;
        bool g_loggedDrawFail = false;
        bool g_loggedDepthStretchFail = false;
        bool g_loggedDepthCopyPass = false;
        bool g_loggedCapturedDepth = false;
        bool g_loggedDepthSelfTest = false;
        bool g_loggedDepthMode = false;

        struct FsVertex
        {
            float x, y, z, rhw;
            float u, v;
        };

        constexpr DWORD kFsFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

        constexpr D3DFORMAT kIntz =
            static_cast<D3DFORMAT>(
                MAKEFOURCC('I', 'N', 'T', 'Z'));

        int DepthProofMode()
        {
            static const int mode = []()
            {
                char modeValue[16] = {};

                const DWORD modeLen =
                    GetEnvironmentVariableA(
                        "WXL_DEPTH_PROOF_MODE",
                        modeValue,
                        sizeof(modeValue));

                if (modeLen > 0 &&
                    modeLen < sizeof(modeValue) &&
                    modeValue[0] >= '1' &&
                    modeValue[0] <= '8')
                {
                    return static_cast<int>(
                        modeValue[0] - '0');
                }

                // Preserve the old diagnostic switch as legacy mode 6.
                char legacy[16] = {};

                const DWORD legacyLen =
                    GetEnvironmentVariableA(
                        "WXL_DEPTH_PROOF",
                        legacy,
                        sizeof(legacy));

                if (legacyLen > 0 &&
                    legacyLen < sizeof(legacy))
                {
                    const char c = legacy[0];

                    if (c != '0' &&
                        c != 'n' && c != 'N' &&
                        c != 'f' && c != 'F')
                        return 6;
                }

                return 0;
            }();

            return mode;
        }

        void ReleaseTarget()
        {
            SafeRelease(g_sceneSurface);
            SafeRelease(g_sceneTexture);

            SafeRelease(g_depthPlainSurface);
            SafeRelease(g_depthSurface);
            SafeRelease(g_depthTexture);

            g_width = 0;
            g_height = 0;
            g_format = D3DFMT_UNKNOWN;

            g_depthWidth = 0;
            g_depthHeight = 0;
        }

        void ReleaseRuntime()
        {
            ReleaseTarget();
            d3d9smaa::PrepareForReset();

            SafeRelease(g_proofShader);
            SafeRelease(g_depthProofShader);

            for (auto*& p : g_fxaaShader)
                SafeRelease(p);

            d3d9depthprobe::Reset();
            g_device = nullptr;

            g_lastLoggedMode = -1;
            g_lastLoggedTier = -1;
            g_loggedEndFail = false;
            g_loggedStretchFail = false;
            g_loggedBeginFail = false;
            g_loggedDrawFail = false;
            g_loggedDepthStretchFail = false;
            g_loggedDepthCopyPass = false;
            g_loggedCapturedDepth = false;
            g_loggedDepthSelfTest = false;
            g_loggedDepthMode = false;
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

        bool EnsureDepthProofShader(IDirect3DDevice9* dev)
        {
            if (g_depthProofShader)
                return true;

            // R3D2 INTZ truth-test shader.
            //
            // mode 1 = INTZ self-test, raw value
            // mode 2 = direct world-depth copy, raw value
            // mode 3 = direct world-depth copy, 1-depth
            // mode 4 = direct world-depth copy, expanded far-depth detail
            // mode 5 = two-stage copy, expanded far-depth detail
            // mode 6 = two-stage copy, graded nonlinear depth proof
            // mode 7 = two-stage copy, reconstructed linear view-Z proof
            // mode 8 = reconstructed view-Z in obvious 10-unit bands
            static const char* kDepthProofPs = R"HLSL(
sampler2D depthTex : register(s0);
float4 proofMode : register(c1);
float4 depthProjection : register(c2);
// depthProjection.x = A
// depthProjection.y = B
// depthProjection.z = linear proof range

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float d = tex2D(depthTex, uv).r;
    float mode = proofMode.x;
    float v = d;

    if (mode >= 2.5 && mode < 3.5)
    {
        v = 1.0 - d;
    }
    else if (mode >= 3.5 && mode < 5.5)
    {
        // Standard D3D depth spends most precision close to 1. Expand
        // the tiny (1-depth) range aggressively so real structure cannot
        // hide in an almost-white raw image.
        v = saturate((1.0 - d) * 4096.0);
    }
    else if (mode >= 7.5)
    {
        // Same reconstructed linear view-Z as mode 7, but deliberately
        // quantised into eight 10-unit bands across 0..80 units.
        // This makes monotonic camera-space distance visually unambiguous.
        float denom = d - depthProjection.x;
        float safeDenom =
            abs(denom) > 1.0e-7 ? denom : -1.0e-7;

        float viewZ =
            max(depthProjection.y / safeDenom, 0.0);

        float normalized =
            saturate(viewZ / depthProjection.z);

        float band =
            floor(normalized * 8.0) / 8.0;

        v = 1.0 - band;
    }
    else if (mode >= 6.5)
    {
        // For the validated WoW D3D perspective matrix:
        //
        //     depth = A + B / viewZ
        //     viewZ = B / (depth - A)
        //
        // Display the first 200 view-space units linearly:
        // near = white, 200+ = black.
        float denom = d - depthProjection.x;
        float safeDenom =
            abs(denom) > 1.0e-7 ? denom : -1.0e-7;

        float viewZ =
            depthProjection.y / safeDenom;

        v = saturate(
            1.0 - max(viewZ, 0.0) / depthProjection.z);
    }
    else if (mode >= 5.5)
    {
        v = pow(saturate(1.0 - d), 0.25);
    }

    return float4(v, v, v, 1.0);
}
)HLSL";

            if (!CompilePixelShader(
                    dev,
                    kDepthProofPs,
                    "R3D2 INTZ truth test",
                    &g_depthProofShader))
                return false;

            WLOG_INFO(
                "wxl-modern-d3d9: R3D2 INTZ truth-test shader ready");

            return true;
        }

        bool EnsureDepthTarget(IDirect3DDevice9* dev,
                               IDirect3DSurface9* sourceDepth)
        {
            if (!dev || !sourceDepth)
                return false;

            D3DSURFACE_DESC desc = {};

            if (FAILED(sourceDepth->GetDesc(&desc)))
                return false;

            if (g_depthTexture &&
                g_depthSurface &&
                g_depthPlainSurface &&
                g_depthWidth == desc.Width &&
                g_depthHeight == desc.Height)
                return true;

            SafeRelease(g_depthPlainSurface);
            SafeRelease(g_depthSurface);
            SafeRelease(g_depthTexture);

            g_depthWidth = 0;
            g_depthHeight = 0;

            HRESULT hr = dev->CreateTexture(
                desc.Width,
                desc.Height,
                1,
                D3DUSAGE_DEPTHSTENCIL,
                kIntz,
                D3DPOOL_DEFAULT,
                &g_depthTexture,
                nullptr);

            if (FAILED(hr) || !g_depthTexture)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: R3D1 INTZ texture creation "
                    "failed %ux%u hr=0x%08X",
                    desc.Width,
                    desc.Height,
                    static_cast<unsigned>(hr));

                SafeRelease(g_depthTexture);
                return false;
            }

            hr = g_depthTexture->GetSurfaceLevel(
                0,
                &g_depthSurface);

            if (FAILED(hr) || !g_depthSurface)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: R3D1 INTZ surface acquisition "
                    "failed hr=0x%08X",
                    static_cast<unsigned>(hr));

                SafeRelease(g_depthSurface);
                SafeRelease(g_depthTexture);
                return false;
            }

            hr = dev->CreateDepthStencilSurface(
                desc.Width,
                desc.Height,
                desc.Format,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,
                &g_depthPlainSurface,
                nullptr);

            if (FAILED(hr) || !g_depthPlainSurface)
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: R3D2 plain D24X8 target "
                    "creation failed %ux%u fmt=%u hr=0x%08X",
                    desc.Width,
                    desc.Height,
                    static_cast<unsigned>(desc.Format),
                    static_cast<unsigned>(hr));

                SafeRelease(g_depthPlainSurface);
                SafeRelease(g_depthSurface);
                SafeRelease(g_depthTexture);
                return false;
            }

            g_depthWidth = desc.Width;
            g_depthHeight = desc.Height;

            WLOG_INFO(
                "wxl-modern-d3d9: R3D2 depth targets ready "
                "%ux%u sourceFmt=%u sourceMSAA=%u",
                desc.Width,
                desc.Height,
                static_cast<unsigned>(desc.Format),
                static_cast<unsigned>(desc.MultiSampleType));

            return true;
        }

        bool RunIntzSelfTest(IDirect3DDevice9* dev)
        {
            if (!dev ||
                !g_sceneSurface ||
                !g_depthSurface ||
                !g_depthWidth ||
                !g_depthHeight)
                return false;

            // INTZ must be bound as the depth-stencil surface to populate it.
            // Pair it with our already-existing single-sample colour RT so the
            // target/depth multisample contracts match.
            dev->SetTexture(0, nullptr);

            const HRESULT rtHr =
                dev->SetRenderTarget(
                    0,
                    g_sceneSurface);

            const HRESULT dsHr =
                dev->SetDepthStencilSurface(
                    g_depthSurface);

            if (FAILED(rtHr) || FAILED(dsHr))
            {
                WLOG_ERROR(
                    "wxl-modern-d3d9: R3D2 INTZ self-test bind "
                    "FAIL rt=0x%08X depth=0x%08X",
                    static_cast<unsigned>(rtHr),
                    static_cast<unsigned>(dsHr));
                return false;
            }

            const LONG w =
                static_cast<LONG>(g_depthWidth);

            const LONG h =
                static_cast<LONG>(g_depthHeight);

            const LONG x1 = w / 3;
            const LONG x2 = (w * 2) / 3;

            const D3DRECT left  = { 0,  0, x1, h };
            const D3DRECT mid   = { x1, 0, x2, h };
            const D3DRECT right = { x2, 0, w,  h };

            const HRESULT a =
                dev->Clear(
                    1,
                    &left,
                    D3DCLEAR_ZBUFFER,
                    0,
                    0.10f,
                    0);

            const HRESULT b =
                dev->Clear(
                    1,
                    &mid,
                    D3DCLEAR_ZBUFFER,
                    0,
                    0.50f,
                    0);

            const HRESULT c =
                dev->Clear(
                    1,
                    &right,
                    D3DCLEAR_ZBUFFER,
                    0,
                    0.90f,
                    0);

            const bool ok =
                SUCCEEDED(a) &&
                SUCCEEDED(b) &&
                SUCCEEDED(c);

            if (!g_loggedDepthSelfTest)
            {
                g_loggedDepthSelfTest = true;

                WLOG_INFO(
                    "wxl-modern-d3d9: R3D2 INTZ self-test "
                    "%s z=[0.10,0.50,0.90] "
                    "hr=[0x%08X,0x%08X,0x%08X]",
                    ok ? "PASS" : "FAIL",
                    static_cast<unsigned>(a),
                    static_cast<unsigned>(b),
                    static_cast<unsigned>(c));
            }

            return ok;
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
               Quality fxaaQuality,
               bool smaaEnabled,
               Quality smaaQuality,
               IDirect3DSurface9* worldDepth,
               const float* worldProjection)
    {
        if (!Available() || !device)
            return false;

        if (g_device != device)
        {
            ReleaseRuntime();
            g_device = device;
            WLOG_INFO(
                "wxl-modern-d3d9: Proton/Wine fallback backend active");
        }

        // Keep the earlier capability probe for evidence, now after the
        // device-change reset so it runs only once per live device.
        d3d9depthprobe::ProbeOnce(device);

        const int depthMode = DepthProofMode();
        const bool depthProof = depthMode > 0;

        if ((depthMode == 7 || depthMode == 8) &&
            !worldProjection)
        {
            static bool loggedMissingProjection = false;

            if (!loggedMissingProjection)
            {
                loggedMissingProjection = true;

                WLOG_ERROR(
                    "wxl-modern-r3d3b: linear depth requires "
                    "the retained world projection");
            }

            return false;
        }

        if (depthProof && !g_loggedDepthMode)
        {
            g_loggedDepthMode = true;

            WLOG_INFO(
                "wxl-modern-d3d9: R3D2 depth-proof mode=%d",
                depthMode);
        }

        if (!depthProof &&
            !g_proofTint &&
            !fxaaEnabled &&
            !smaaEnabled)
        {
            g_lastLoggedMode = -1;
            g_lastLoggedTier = -1;
            return false;
        }

        const int fxaaTier = std::max(
            0, std::min(2, static_cast<int>(fxaaQuality)));

        const int smaaTier = std::max(
            0, std::min(2, static_cast<int>(smaaQuality)));

        // The panel makes these mutually exclusive. If an external caller ever
        // enables both, prefer SMAA so there is still exactly one AA pass.
        const bool useSmaa =
            !depthProof &&
            !g_proofTint &&
            smaaEnabled;

        IDirect3DPixelShader9* shader = nullptr;

        if (depthProof)
        {
            if (!EnsureDepthProofShader(device))
                return false;

            shader = g_depthProofShader;
        }
        else if (g_proofTint)
        {
            if (!EnsureProofShader(device))
                return false;
            shader = g_proofShader;
        }
        else if (!useSmaa)
        {
            if (!EnsureFxaaShader(device, fxaaTier))
                return false;
            shader = g_fxaaShader[fxaaTier];
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

        device->GetDepthStencilSurface(&oldDepth);

        // oldDepth is only the state we must restore later. The actual world
        // depth comes from OnWorldSceneEnd, where the core captured it before
        // the world pass began.
        IDirect3DSurface9* depthSource =
            worldDepth ? worldDepth : oldDepth;

        if (depthProof &&
            worldDepth &&
            !g_loggedCapturedDepth)
        {
            D3DSURFACE_DESC d = {};

            if (SUCCEEDED(worldDepth->GetDesc(&d)))
            {
                WLOG_INFO(
                    "wxl-modern-d3d9: R3D1A captured world-depth "
                    "%ux%u fmt=%u sourceMSAA=%u",
                    d.Width,
                    d.Height,
                    static_cast<unsigned>(d.Format),
                    static_cast<unsigned>(d.MultiSampleType));

                g_loggedCapturedDepth = true;
            }
        }

        if (depthProof &&
            (!depthSource ||
             !EnsureDepthTarget(device, depthSource)))
        {
            WLOG_ERROR(
                "wxl-modern-d3d9: R3D1 depth target unavailable");

            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();
            return false;
        }

        // StretchRect cannot execute inside BeginScene/EndScene. Call the
        // original EndScene target directly so the ImGui/UI hook is not emitted
        // in the middle of this world-only post-process.
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

        HRESULT depthStretchHr = S_OK;

        if (depthProof && depthMode != 1)
        {
            if (depthMode == 5 ||
                depthMode == 6 ||
                depthMode == 7 ||
                depthMode == 8)
            {
                const HRESULT stage1 =
                    device->StretchRect(
                        depthSource,
                        nullptr,
                        g_depthPlainSurface,
                        nullptr,
                        D3DTEXF_NONE);

                HRESULT stage2 = D3DERR_INVALIDCALL;

                if (SUCCEEDED(stage1))
                {
                    stage2 =
                        device->StretchRect(
                            g_depthPlainSurface,
                            nullptr,
                            g_depthSurface,
                            nullptr,
                            D3DTEXF_NONE);
                }

                depthStretchHr =
                    FAILED(stage1) ? stage1 : stage2;

                if (!g_loggedDepthCopyPass)
                {
                    g_loggedDepthCopyPass = true;

                    WLOG_INFO(
                        "wxl-modern-d3d9: R3D2 two-stage depth-copy "
                        "%s stage1=0x%08X stage2=0x%08X",
                        SUCCEEDED(depthStretchHr) ? "PASS" : "FAIL",
                        static_cast<unsigned>(stage1),
                        static_cast<unsigned>(stage2));
                }
            }
            else
            {
                depthStretchHr =
                    device->StretchRect(
                        depthSource,
                        nullptr,
                        g_depthSurface,
                        nullptr,
                        D3DTEXF_NONE);

                if (!g_loggedDepthCopyPass)
                {
                    g_loggedDepthCopyPass = true;

                    WLOG_INFO(
                        "wxl-modern-d3d9: R3D2 direct depth-copy "
                        "%s %ux%u sourceMSAA=8 -> INTZ "
                        "hr=0x%08X",
                        SUCCEEDED(depthStretchHr) ? "PASS" : "FAIL",
                        g_depthWidth,
                        g_depthHeight,
                        static_cast<unsigned>(depthStretchHr));
                }
            }
        }

        // WoW still needs an open scene for the remaining UI work this frame.
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

        if (depthProof &&
            depthMode == 1 &&
            !RunIntzSelfTest(device))
        {
            RestoreDeviceState(
                device, state, oldRt, oldDepth, oldViewport);

            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();
            return false;
        }

        if (depthProof && FAILED(depthStretchHr))
        {
            if (!g_loggedDepthStretchFail)
            {
                g_loggedDepthStretchFail = true;

                WLOG_ERROR(
                    "wxl-modern-d3d9: R3D1 depth StretchRect "
                    "failed hr=0x%08X",
                    static_cast<unsigned>(depthStretchHr));
            }

            RestoreDeviceState(
                device, state, oldRt, oldDepth, oldViewport);

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

        auto logPass = [&](int mode, int tier)
        {
            if (g_lastLoggedMode == mode &&
                g_lastLoggedTier == tier)
                return;

            const char* name =
                mode == 0 ? "proof-tint" :
                mode == 2 ? "SMAA" :
                mode == 3 ? "depth-selftest" :
                mode == 4 ? "depth-raw" :
                mode == 5 ? "depth-invert" :
                mode == 6 ? "depth-expand" :
                mode == 7 ? "depth-two-stage" :
                mode == 8 ? "depth-two-stage-graded" :
                mode == 9 ? "depth-linear-viewz" :
                mode == 10 ? "depth-viewz-bands" :
                             "FXAA";

            WLOG_INFO(
                "wxl-modern-d3d9: post-process frame PASS "
                "%ux%u sourceMSAA=%u mode=%s tier=%d",
                bbDesc.Width,
                bbDesc.Height,
                static_cast<unsigned>(bbDesc.MultiSampleType),
                name,
                tier);

            g_lastLoggedMode = mode;
            g_lastLoggedTier = tier;
        };

        // ------------------------------------------------------------
        // SMAA: three-pass native D3D9 implementation.
        // ------------------------------------------------------------

        if (useSmaa)
        {
            const bool ok = d3d9smaa::Render(
                device,
                g_sceneTexture,
                backbuffer,
                bbDesc.Width,
                bbDesc.Height,
                smaaQuality);

            RestoreDeviceState(
                device, state, oldRt, oldDepth, oldViewport);

            if (ok)
                logPass(2, smaaTier);

            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();

            return ok;
        }

        // ------------------------------------------------------------
        // Existing one-pass proof tint / FXAA path.
        // ------------------------------------------------------------

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

        device->SetTexture(
            0,
            depthProof
                ? static_cast<IDirect3DBaseTexture9*>(g_depthTexture)
                : static_cast<IDirect3DBaseTexture9*>(g_sceneTexture));

        device->SetSamplerState(
            0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(
            0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(
            0,
            D3DSAMP_MINFILTER,
            depthProof ? D3DTEXF_POINT : D3DTEXF_LINEAR);

        device->SetSamplerState(
            0,
            D3DSAMP_MAGFILTER,
            depthProof ? D3DTEXF_POINT : D3DTEXF_LINEAR);

        device->SetSamplerState(
            0,
            D3DSAMP_SRGBTEXTURE,
            FALSE);
        device->SetSamplerState(
            0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

        const float rcpFrame[4] = {
            1.0f / static_cast<float>(bbDesc.Width),
            1.0f / static_cast<float>(bbDesc.Height),
            0.0f,
            0.0f
        };

        device->SetPixelShaderConstantF(
            0, rcpFrame, 1);

        if (depthProof)
        {
            const float proofMode[4] = {
                static_cast<float>(depthMode),
                0.0f,
                0.0f,
                0.0f
            };

            device->SetPixelShaderConstantF(
                1,
                proofMode,
                1);

            if ((depthMode == 7 || depthMode == 8) &&
                worldProjection)
            {
                const float aZ = worldProjection[10];
                const float bZ = worldProjection[14];
                const float farDenom = 1.0f - aZ;

                const float nearPlane =
                    aZ != 0.0f
                        ? (-bZ / aZ)
                        : 0.0f;

                const float farPlane =
                    farDenom != 0.0f
                        ? (bZ / farDenom)
                        : 0.0f;

                const float proofRange =
                    depthMode == 8 ? 80.0f : 200.0f;

                const float depthProjection[4] = {
                    aZ,
                    bZ,
                    proofRange,
                    farPlane
                };

                device->SetPixelShaderConstantF(
                    2,
                    depthProjection,
                    1);

                static bool loggedLinearProjection = false;

                if (!loggedLinearProjection)
                {
                    loggedLinearProjection = true;

                    WLOG_INFO(
                        "wxl-modern-r3d3b: linear view-Z "
                        "A=%.9g B=%.9g near=%.9g far=%.9g "
                        "proofRange=%.9g mode=%d",
                        aZ,
                        bZ,
                        nearPlane,
                        farPlane,
                        proofRange,
                        depthMode);
                }
            }
        }

        const float w =
            static_cast<float>(bbDesc.Width);
        const float h =
            static_cast<float>(bbDesc.Height);

        const FsVertex quad[4] = {
            { -0.5f,    -0.5f,    0.0f, 1.0f, 0.0f, 0.0f },
            { w - 0.5f, -0.5f,    0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,     h - 0.5f,0.0f, 1.0f, 0.0f, 1.0f },
            { w - 0.5f,  h - 0.5f,0.0f, 1.0f, 1.0f, 1.0f },
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

        if (SUCCEEDED(drawHr))
        {
            logPass(
                depthProof ? (2 + depthMode) :
                g_proofTint ? 0 : 1,
                depthProof ? depthMode : fxaaTier);
        }

        SafeRelease(state);
        SafeRelease(oldRt);
        SafeRelease(oldDepth);
        backbuffer->Release();

        return SUCCEEDED(drawHr);
    }
}
