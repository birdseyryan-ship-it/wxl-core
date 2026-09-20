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
        IDirect3DPixelShader9* g_aoProofShader = nullptr;
        IDirect3DPixelShader9* g_aoDenoiseShader = nullptr;
        IDirect3DPixelShader9* g_fxaaShader[3] = { nullptr, nullptr, nullptr };

        // R3E2+: raw AO is evaluated at half resolution, then depth-aware
        // denoised/upsampled while compositing at full resolution.
        IDirect3DTexture9*     g_aoTexture = nullptr;
        IDirect3DSurface9*     g_aoSurface = nullptr;

        // R3E4: full-resolution AO-composited world image. When an AA method
        // is active, AO writes here first and SMAA/FXAA consumes this texture.
        IDirect3DTexture9*     g_aoCompositeTexture = nullptr;
        IDirect3DSurface9*     g_aoCompositeSurface = nullptr;
        D3DFORMAT              g_aoCompositeFormat = D3DFMT_UNKNOWN;

        UINT      g_width = 0;
        UINT      g_height = 0;
        D3DFORMAT g_format = D3DFMT_UNKNOWN;

        UINT      g_depthWidth = 0;
        UINT      g_depthHeight = 0;

        UINT      g_aoWidth = 0;
        UINT      g_aoHeight = 0;

        bool g_proofTint = false;

        // Accepted Classic Enhanced R3E3 Preset 2 is production-default ON.
        bool g_ambientOcclusion = true;

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
        bool g_loggedAoMode = false;
        bool g_loggedAoProjection = false;
        bool g_loggedAoProduction = false;

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
                    modeValue[0] <= '9')
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

        int AoProofMode()
        {
            static const int mode = []()
            {
                char raw[16] = {};

                const DWORD n =
                    GetEnvironmentVariableA(
                        "WXL_AO_PROOF_MODE",
                        raw,
                        sizeof(raw));

                if (n > 0 &&
                    n < sizeof(raw) &&
                    raw[1] == '\0' &&
                    (raw[0] == '1' || raw[0] == '2'))
                {
                    return static_cast<int>(raw[0] - '0');
                }

                return 0;
            }();

            return mode;
        }

        int AoTuneMode()
        {
            static const int mode = []()
            {
                char raw[16] = {};

                const DWORD n =
                    GetEnvironmentVariableA(
                        "WXL_AO_TUNE",
                        raw,
                        sizeof(raw));

                if (n > 0 &&
                    n < sizeof(raw) &&
                    raw[1] == '\0' &&
                    raw[0] >= '1' &&
                    raw[0] <= '3')
                {
                    return static_cast<int>(
                        raw[0] - '0');
                }

                // 0 is the exact R3E2 visual baseline.
                return 0;
            }();

            return mode;
        }

        void ReleaseTarget()
        {
            SafeRelease(g_sceneSurface);
            SafeRelease(g_sceneTexture);

            SafeRelease(g_aoSurface);
            SafeRelease(g_aoTexture);

            SafeRelease(g_aoCompositeSurface);
            SafeRelease(g_aoCompositeTexture);
            g_aoCompositeFormat = D3DFMT_UNKNOWN;

            SafeRelease(g_depthPlainSurface);
            SafeRelease(g_depthSurface);
            SafeRelease(g_depthTexture);

            g_width = 0;
            g_height = 0;
            g_format = D3DFMT_UNKNOWN;

            g_depthWidth = 0;
            g_depthHeight = 0;

            g_aoWidth = 0;
            g_aoHeight = 0;
        }

        void ReleaseRuntime()
        {
            ReleaseTarget();
            d3d9smaa::PrepareForReset();

            SafeRelease(g_proofShader);
            SafeRelease(g_depthProofShader);
            SafeRelease(g_aoProofShader);
            SafeRelease(g_aoDenoiseShader);

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
            g_loggedAoMode = false;
            g_loggedAoProjection = false;
            g_loggedAoProduction = false;
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
            // mode 8 = reconstructed view-Z equivalence bands
            // mode 9 = raw stored-depth threshold bands, no reconstruction
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
    else if (mode >= 8.5)
    {
        // R3D3C: raw sampled depth buckets. No projection or viewport
        // assumptions are applied here. Under ordinary D3D 0..1 depth
        // these roughly correspond to progressively farther geometry.
        if      (d < 0.6000) v = 1.000;
        else if (d < 0.8000) v = 0.875;
        else if (d < 0.9000) v = 0.750;
        else if (d < 0.9500) v = 0.625;
        else if (d < 0.9750) v = 0.500;
        else if (d < 0.9900) v = 0.375;
        else if (d < 0.9950) v = 0.250;
        else if (d < 0.9990) v = 0.125;
        else                 v = 0.000;
    }
    else if (mode >= 7.5)
    {
        // R3D3D reconstruction-equivalence proof.
        //
        // Reconstruct actual view-space Z using the engine projection,
        // then quantise it at the real-distance boundaries corresponding
        // to R3D3C Mode 9's raw depth thresholds.
        //
        // If projection/depth interpretation is correct, this image should
        // reproduce Mode 9's distance layering despite taking the entirely
        // different route through reconstructed viewZ.
        float denom = d - depthProjection.x;
        float safeDenom =
            abs(denom) > 1.0e-7 ? denom : -1.0e-7;

        float viewZ =
            max(depthProjection.y / safeDenom, 0.0);

        if      (viewZ <   0.9989904) v = 1.000;
        else if (viewZ <   1.9954624) v = 0.875;
        else if (viewZ <   3.9808896) v = 0.750;
        else if (viewZ <   7.9219390) v = 0.625;
        else if (viewZ <  15.6868860) v = 0.500;
        else if (viewZ <  38.0850980) v = 0.375;
        else if (viewZ <  72.6736400) v = 0.250;
        else if (viewZ < 265.7684400) v = 0.125;
        else                          v = 0.000;
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

        bool EnsureAoProofShader(IDirect3DDevice9* dev)
        {
            if (g_aoProofShader)
                return true;

            static const char* kAoRawPs = R"HLSL(
sampler2D depthTex : register(s0);

float4 rcpAo     : register(c0);
float4 proj      : register(c1);
float4 aoParams  : register(c2);
float4 aoControl : register(c3);

float linearZ(float d)
{
    float denom = d - proj.z;
    float safeDenom =
        abs(denom) > 1.0e-7 ? denom : -1.0e-7;

    return max(proj.w / safeDenom, 0.0);
}

float3 viewPos(float2 uv, float d)
{
    float z = linearZ(d);

    float2 n = uv * 2.0 - 1.0;
    n.y = -n.y;

    return float3(
        n.x * z / proj.x,
        n.y * z / proj.y,
        z);
}

float hash12(float2 p)
{
    return frac(
        sin(dot(p, float2(12.9898, 78.233))) *
        43758.5453);
}

float2 rotate2(float2 v, float2 cs)
{
    return float2(
        v.x * cs.x - v.y * cs.y,
        v.x * cs.y + v.y * cs.x);
}

float aoSample(
    float3 P,
    float3 N,
    float2 uv,
    float2 direction,
    float uvRadius,
    float scale)
{
    float2 suv =
        uv + direction * uvRadius * scale;

    float sd =
        tex2D(depthTex, suv).r;

    if (sd >= 0.9995)
        return 0.0;

    float3 Q =
        viewPos(suv, sd);

    float3 V =
        Q - P;

    float dist =
        length(V);

    if (dist <= 1.0e-4 ||
        dist >= aoParams.x)
        return 0.0;

    float dz =
        abs(Q.z - P.z);

    if (dz > aoParams.x * 0.55)
        return 0.0;

    float hemi =
        saturate(
            (dot(N, V) - aoParams.z) /
            max(dist, 1.0e-4));

    float falloff =
        saturate(
            1.0 -
            dist / aoParams.x);

    // R3E3: explicit radial shaping. 2.0 exactly reproduces the
    // R3E2 squared falloff; higher values progressively tighten AO
    // around true contact regions without changing the sampling topology.
    falloff =
        pow(
            max(falloff, 0.0001),
            max(aoControl.w, 1.0));

    float depthConfidence =
        saturate(
            1.0 -
            dz /
            max(
                aoParams.x * 0.55,
                1.0e-4));

    return
        hemi *
        falloff *
        depthConfidence;
}

float horizonPair(
    float3 P,
    float3 N,
    float2 uv,
    float2 direction,
    float uvRadius)
{
    float h0 =
        aoSample(
            P, N, uv,
            direction,
            uvRadius,
            0.32);

    float h1 =
        aoSample(
            P, N, uv,
            direction,
            uvRadius,
            0.72);

    return max(h0, h1);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float d =
        tex2D(depthTex, uv).r;

    if (d >= 0.9995)
        return float4(1,1,1,1);

    float3 P =
        viewPos(uv, d);

    float3 rawN =
        cross(
            ddx(P),
            ddy(P));

    float3 N =
        rawN /
        max(length(rawN), 1.0e-5);

    if (dot(N, -P) < 0.0)
        N = -N;

    float uvRadius =
        min(
            0.5 *
            aoParams.x *
            proj.y /
            max(P.z, 0.01),
            aoParams.w);

    float2 pixel =
        floor(
            uv /
            rcpAo.xy);

    float angle =
        hash12(pixel) *
        6.28318530718;

    float2 cs =
        float2(
            cos(angle),
            sin(angle));

    float2 d0 = rotate2(float2( 1.0,       0.0),       cs);
    float2 d1 = rotate2(float2( 0.5,       0.8660254), cs);
    float2 d2 = rotate2(float2(-0.5,       0.8660254), cs);
    float2 d3 = rotate2(float2(-1.0,       0.0),       cs);
    float2 d4 = rotate2(float2(-0.5,      -0.8660254), cs);
    float2 d5 = rotate2(float2( 0.5,      -0.8660254), cs);

    float occ = 0.0;

    occ += horizonPair(P, N, uv, d0, uvRadius);
    occ += horizonPair(P, N, uv, d1, uvRadius);
    occ += horizonPair(P, N, uv, d2, uvRadius);
    occ += horizonPair(P, N, uv, d3, uvRadius);
    occ += horizonPair(P, N, uv, d4, uvRadius);
    occ += horizonPair(P, N, uv, d5, uvRadius);

    occ /= 6.0;

    float ao =
        saturate(
            1.0 -
            occ * aoParams.y);

    ao =
        pow(
            max(ao, 0.0001),
            aoControl.x);

    if (aoControl.z > aoControl.y)
    {
        float fade =
            saturate(
                (P.z - aoControl.y) /
                (aoControl.z - aoControl.y));

        ao =
            lerp(
                ao,
                1.0,
                fade);
    }

    return float4(ao, ao, ao, 1.0);
}
)HLSL";

            if (!CompilePixelShader(
                    dev,
                    kAoRawPs,
                    "R3E2 half-res raw AO",
                    &g_aoProofShader))
                return false;

            WLOG_INFO(
                "wxl-modern-d3d9: R3E2 raw AO shader ready "
                "(half-res 12-tap rotated horizon)");

            return true;
        }

        bool EnsureAoDenoiseShader(IDirect3DDevice9* dev)
        {
            if (g_aoDenoiseShader)
                return true;

            static const char* kAoDenoisePs = R"HLSL(
sampler2D sceneTex : register(s0);
sampler2D aoTex    : register(s1);
sampler2D depthTex : register(s2);

float4 metrics : register(c0);
// x=A, y=B, z=depth sharpness, w=mode
float4 denoise : register(c1);

float linearZ(float d)
{
    float denom = d - denoise.x;
    float safeDenom =
        abs(denom) > 1.0e-7 ? denom : -1.0e-7;

    return max(
        denoise.y / safeDenom,
        0.0);
}

float2 aoTap(
    float2 uv,
    float2 offset,
    float centerZ,
    float spatialWeight)
{
    float2 suv =
        uv +
        offset * metrics.xy;

    float sd =
        tex2D(depthTex, suv).r;

    if (sd >= 0.9995)
        return float2(0.0, 0.0);

    float sampleZ =
        linearZ(sd);

    float depthWeight =
        exp(
            -abs(sampleZ - centerZ) *
            denoise.z);

    float w =
        spatialWeight *
        depthWeight;

    float a =
        tex2D(aoTex, suv).r;

    return float2(
        a * w,
        w);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene =
        tex2D(sceneTex, uv);

    float dc =
        tex2D(depthTex, uv).r;

    if (dc >= 0.9995)
    {
        if (denoise.w < 1.5)
            return float4(1,1,1,1);

        return float4(scene.rgb, 1.0);
    }

    float centerZ =
        linearZ(dc);

    float2 total =
        float2(0.0, 0.0);

    total += aoTap(uv, float2(-1,-1), centerZ, 1.0);
    total += aoTap(uv, float2( 0,-1), centerZ, 2.0);
    total += aoTap(uv, float2( 1,-1), centerZ, 1.0);

    total += aoTap(uv, float2(-1, 0), centerZ, 2.0);
    total += aoTap(uv, float2( 0, 0), centerZ, 4.0);
    total += aoTap(uv, float2( 1, 0), centerZ, 2.0);

    total += aoTap(uv, float2(-1, 1), centerZ, 1.0);
    total += aoTap(uv, float2( 0, 1), centerZ, 2.0);
    total += aoTap(uv, float2( 1, 1), centerZ, 1.0);

    float ao =
        total.y > 1.0e-5
            ? total.x / total.y
            : tex2D(aoTex, uv).r;

    ao =
        saturate(ao);

    if (denoise.w < 1.5)
        return float4(ao, ao, ao, 1.0);

    return float4(
        scene.rgb * ao,
        1.0);
}
)HLSL";

            if (!CompilePixelShader(
                    dev,
                    kAoDenoisePs,
                    "R3E2 depth-aware AO denoise",
                    &g_aoDenoiseShader))
                return false;

            WLOG_INFO(
                "wxl-modern-d3d9: R3E2 AO denoise shader ready "
                "(half-res -> full-res bilateral)");

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

        bool EnsureAoTarget(
            IDirect3DDevice9* dev,
            UINT fullW,
            UINT fullH,
            D3DFORMAT fullFmt)
        {
            if (!dev ||
                !fullW ||
                !fullH ||
                fullFmt == D3DFMT_UNKNOWN)
                return false;

            const UINT aoW =
                fullW > 1 ? fullW / 2 : 1;

            const UINT aoH =
                fullH > 1 ? fullH / 2 : 1;

            if (g_aoTexture &&
                g_aoSurface &&
                g_aoCompositeTexture &&
                g_aoCompositeSurface &&
                g_aoWidth == aoW &&
                g_aoHeight == aoH &&
                g_aoCompositeFormat == fullFmt)
                return true;

            SafeRelease(g_aoSurface);
            SafeRelease(g_aoTexture);
            SafeRelease(g_aoCompositeSurface);
            SafeRelease(g_aoCompositeTexture);

            g_aoWidth = 0;
            g_aoHeight = 0;
            g_aoCompositeFormat = D3DFMT_UNKNOWN;

            HRESULT hr =
                dev->CreateTexture(
                    aoW,
                    aoH,
                    1,
                    D3DUSAGE_RENDERTARGET,
                    D3DFMT_A8R8G8B8,
                    D3DPOOL_DEFAULT,
                    &g_aoTexture,
                    nullptr);

            if (FAILED(hr) ||
                !g_aoTexture)
            {
                WLOG_ERROR(
                    "wxl-modern-r3e4: half-res AO texture "
                    "creation failed %ux%u hr=0x%08X",
                    aoW,
                    aoH,
                    static_cast<unsigned>(hr));

                SafeRelease(g_aoTexture);
                return false;
            }

            hr =
                g_aoTexture->GetSurfaceLevel(
                    0,
                    &g_aoSurface);

            if (FAILED(hr) ||
                !g_aoSurface)
            {
                WLOG_ERROR(
                    "wxl-modern-r3e4: half-res AO surface "
                    "acquisition failed hr=0x%08X",
                    static_cast<unsigned>(hr));

                SafeRelease(g_aoSurface);
                SafeRelease(g_aoTexture);
                return false;
            }

            hr =
                dev->CreateTexture(
                    fullW,
                    fullH,
                    1,
                    D3DUSAGE_RENDERTARGET,
                    fullFmt,
                    D3DPOOL_DEFAULT,
                    &g_aoCompositeTexture,
                    nullptr);

            if (FAILED(hr) ||
                !g_aoCompositeTexture)
            {
                WLOG_ERROR(
                    "wxl-modern-r3e4: AO composite texture "
                    "creation failed %ux%u fmt=%u hr=0x%08X",
                    fullW,
                    fullH,
                    static_cast<unsigned>(fullFmt),
                    static_cast<unsigned>(hr));

                SafeRelease(g_aoSurface);
                SafeRelease(g_aoTexture);
                SafeRelease(g_aoCompositeTexture);
                return false;
            }

            hr =
                g_aoCompositeTexture->GetSurfaceLevel(
                    0,
                    &g_aoCompositeSurface);

            if (FAILED(hr) ||
                !g_aoCompositeSurface)
            {
                WLOG_ERROR(
                    "wxl-modern-r3e4: AO composite surface "
                    "acquisition failed hr=0x%08X",
                    static_cast<unsigned>(hr));

                SafeRelease(g_aoSurface);
                SafeRelease(g_aoTexture);
                SafeRelease(g_aoCompositeSurface);
                SafeRelease(g_aoCompositeTexture);
                return false;
            }

            g_aoWidth = aoW;
            g_aoHeight = aoH;
            g_aoCompositeFormat = fullFmt;

            WLOG_INFO(
                "wxl-modern-r3e4: AO targets ready "
                "raw=%ux%u composite=%ux%u fmt=%u",
                g_aoWidth,
                g_aoHeight,
                fullW,
                fullH,
                static_cast<unsigned>(fullFmt));

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

    bool AmbientOcclusion()
    {
        return g_ambientOcclusion;
    }

    void SetAmbientOcclusion(bool enabled)
    {
        g_ambientOcclusion = enabled;

        WLOG_INFO(
            "wxl-modern-r3e4: Classic Enhanced AO %s",
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

        const int requestedAoProofMode = AoProofMode();

        // Existing depth diagnostics deliberately take precedence.
        const bool aoProof =
            !depthProof &&
            requestedAoProofMode > 0;

        const int aoProofMode =
            aoProof ? requestedAoProofMode : 0;

        const int aoTuneMode =
            AoTuneMode();

        // R3E4 production AO is independent of the diagnostic environment
        // switches and defaults ON for Classic Enhanced.
        bool productionAo =
            !depthProof &&
            !aoProof &&
            !g_proofTint &&
            g_ambientOcclusion;

        // Production should degrade gracefully to colour-only AA if a single
        // frame ever arrives without the retained world depth/projection.
        if (productionAo &&
            (!worldDepth || !worldProjection))
        {
            static bool loggedProductionAoFallback = false;

            if (!loggedProductionAoFallback)
            {
                loggedProductionAoFallback = true;

                WLOG_WARN(
                    "wxl-modern-r3e4: production AO unavailable for "
                    "frame; falling back to colour-only AA");
            }

            productionAo = false;
        }

        const bool aoActive =
            aoProof || productionAo;

        const bool needReadableDepth =
            depthProof || aoActive;

        // Diagnostic proof modes remain strict: missing authority is a failed
        // proof rather than a silent fallback.
        if (((depthMode == 7 || depthMode == 8) || aoProof) &&
            !worldProjection)
        {
            static bool loggedMissingProjection = false;

            if (!loggedMissingProjection)
            {
                loggedMissingProjection = true;

                WLOG_ERROR(
                    "wxl-modern-d3d9: retained world projection "
                    "required for linear-depth/AO processing");
            }

            return false;
        }

        if (aoProof && !worldDepth)
        {
            static bool loggedMissingAoDepth = false;

            if (!loggedMissingAoDepth)
            {
                loggedMissingAoDepth = true;

                WLOG_ERROR(
                    "wxl-modern-r3e4: AO proof requires the captured "
                    "world-depth surface");
            }

            return false;
        }

        if (aoProof && !g_loggedAoMode)
        {
            g_loggedAoMode = true;

            WLOG_INFO(
                "wxl-modern-r3e4: AO proof mode=%d "
                "(1=mask 2=composite)",
                aoProofMode);
        }

        if (productionAo &&
            !g_loggedAoProduction)
        {
            g_loggedAoProduction = true;

            WLOG_INFO(
                "wxl-modern-r3e4: production CE AO enabled "
                "preset=2 chain-before-AA");
        }

        if (depthProof && !g_loggedDepthMode)
        {
            g_loggedDepthMode = true;

            WLOG_INFO(
                "wxl-modern-d3d9: R3D2 depth-proof mode=%d",
                depthMode);
        }

        if (!depthProof &&
            !aoActive &&
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

        // The panel makes the AA methods mutually exclusive. If an external
        // caller enables both, SMAA still wins.
        const bool useSmaa =
            !depthProof &&
            !aoProof &&
            !g_proofTint &&
            smaaEnabled;

        IDirect3DPixelShader9* shader = nullptr;

        if (depthProof)
        {
            if (!EnsureDepthProofShader(device))
                return false;

            shader = g_depthProofShader;
        }
        else if (aoActive)
        {
            if (!EnsureAoProofShader(device) ||
                !EnsureAoDenoiseShader(device))
                return false;

            shader = g_aoProofShader;
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

        if (aoActive &&
            !EnsureAoTarget(
                device,
                bbDesc.Width,
                bbDesc.Height,
                bbDesc.Format))
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

        if (needReadableDepth &&
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

        if (needReadableDepth &&
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

        if (needReadableDepth &&
            !(depthProof && depthMode == 1))
        {
            if (aoActive ||
                depthMode == 5 ||
                depthMode == 6 ||
                depthMode == 7 ||
                depthMode == 8 ||
                depthMode == 9)
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

        if (needReadableDepth && FAILED(depthStretchHr))
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
                mode == 10 ? "depth-viewz-equivalence" :
                mode == 11 ? "depth-raw-thresholds" :
                mode == 12 ? "ao-proof-mask" :
                mode == 13 ? "ao-proof-composite" :
                mode == 14 ? "ao-denoised-mask" :
                mode == 15 ? "ao-denoised-composite" :
                mode == 16 ? "ao-production" :
                mode == 17 ? "ao-production-smaa" :
                mode == 18 ? "ao-production-fxaa" :
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
        // R3E2 AO:
        // pass 1 = half-resolution raw AO
        // pass 2 = full-resolution bilateral denoise/upsample
        // ------------------------------------------------------------

        if (aoActive)
        {
            const float projection[4] = {
                worldProjection[0],
                worldProjection[5],
                worldProjection[10],
                worldProjection[14]
            };

            // Diagnostic R3E3 sweep remains available, but normal R3E4
            // production always resolves to the accepted Preset 2.
            const int effectiveTuneMode =
                aoProof ? aoTuneMode : 2;

            const int aoOutputMode =
                aoProof ? aoProofMode : 2;

            // R3E3 controlled contact-shaping sweep.
            //
            // Preset 0 exactly reproduces R3E2.
            // The other presets progressively reduce the world-space
            // footprint and steepen radial attenuation. This targets the
            // broad "blob" visible around character feet without hiding
            // the problem by simply lowering global intensity.
            float aoRadius       = 0.65f;
            float aoIntensity    = 1.70f;
            float aoBias         = 0.018f;
            float aoMaxUvRadius  = 0.040f;
            float aoPower        = 1.15f;
            float radialFalloff  = 2.00f;
            float denoiseSharp   = 8.00f;

            if (effectiveTuneMode == 1)
            {
                // Balanced: preserve architectural depth but tighten
                // character/object contact lobes.
                aoRadius       = 0.52f;
                aoIntensity    = 1.62f;
                aoBias         = 0.020f;
                aoMaxUvRadius  = 0.032f;
                aoPower        = 1.12f;
                radialFalloff  = 2.50f;
                denoiseSharp   = 10.0f;
            }
            else if (effectiveTuneMode == 2)
            {
                // Contact focused: likely CE target territory.
                aoRadius       = 0.42f;
                aoIntensity    = 1.58f;
                aoBias         = 0.022f;
                aoMaxUvRadius  = 0.026f;
                aoPower        = 1.10f;
                radialFalloff  = 3.00f;
                denoiseSharp   = 10.0f;
            }
            else if (effectiveTuneMode == 3)
            {
                // Tight/subtle: useful lower-bound comparison.
                aoRadius       = 0.34f;
                aoIntensity    = 1.48f;
                aoBias         = 0.024f;
                aoMaxUvRadius  = 0.021f;
                aoPower        = 1.08f;
                radialFalloff  = 3.40f;
                denoiseSharp   = 12.0f;
            }

            const float aoParams[4] = {
                aoRadius,
                aoIntensity,
                aoBias,
                aoMaxUvRadius
            };

            const float rawControl[4] = {
                aoPower,
                18.0f,
                55.0f,
                radialFalloff
            };

            device->SetDepthStencilSurface(nullptr);
            device->SetVertexShader(nullptr);
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

            // Pass 1
            D3DVIEWPORT9 aoVp = {};
            aoVp.X = 0;
            aoVp.Y = 0;
            aoVp.Width = g_aoWidth;
            aoVp.Height = g_aoHeight;
            aoVp.MinZ = 0.0f;
            aoVp.MaxZ = 1.0f;

            device->SetRenderTarget(
                0,
                g_aoSurface);

            device->SetViewport(
                &aoVp);

            device->SetPixelShader(
                g_aoProofShader);

            device->SetTexture(
                0,
                static_cast<IDirect3DBaseTexture9*>(
                    g_depthTexture));

            device->SetSamplerState(
                0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(
                0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(
                0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(
                0, D3DSAMP_SRGBTEXTURE, FALSE);

            const float rcpAo[4] = {
                1.0f / static_cast<float>(g_aoWidth),
                1.0f / static_cast<float>(g_aoHeight),
                0.0f,
                0.0f
            };

            device->SetPixelShaderConstantF(
                0, rcpAo, 1);

            device->SetPixelShaderConstantF(
                1, projection, 1);

            device->SetPixelShaderConstantF(
                2, aoParams, 1);

            device->SetPixelShaderConstantF(
                3, rawControl, 1);

            const float aoW =
                static_cast<float>(g_aoWidth);

            const float aoH =
                static_cast<float>(g_aoHeight);

            const FsVertex aoQuad[4] = {
                { -0.5f,      -0.5f,      0.0f, 1.0f, 0.0f, 0.0f },
                { aoW - 0.5f, -0.5f,      0.0f, 1.0f, 1.0f, 0.0f },
                { -0.5f,       aoH - 0.5f,0.0f, 1.0f, 0.0f, 1.0f },
                { aoW - 0.5f,  aoH - 0.5f,0.0f, 1.0f, 1.0f, 1.0f },
            };

            const HRESULT aoDrawHr =
                device->DrawPrimitiveUP(
                    D3DPT_TRIANGLESTRIP,
                    2,
                    aoQuad,
                    sizeof(FsVertex));

            device->SetTexture(0, nullptr);

            if (FAILED(aoDrawHr))
            {
                WLOG_ERROR(
                    "wxl-modern-r3e2: half-res AO draw failed "
                    "hr=0x%08X",
                    static_cast<unsigned>(aoDrawHr));

                RestoreDeviceState(
                    device, state, oldRt, oldDepth, oldViewport);

                SafeRelease(state);
                SafeRelease(oldRt);
                SafeRelease(oldDepth);
                backbuffer->Release();
                return false;
            }

            // Pass 2: denoise/upscale + composite. Production AO writes
            // to an intermediate texture whenever a later AA pass is active.
            D3DVIEWPORT9 fullVp = {};
            fullVp.X = 0;
            fullVp.Y = 0;
            fullVp.Width = bbDesc.Width;
            fullVp.Height = bbDesc.Height;
            fullVp.MinZ = 0.0f;
            fullVp.MaxZ = 1.0f;

            const bool aoNeedsAa =
                productionAo &&
                (useSmaa || fxaaEnabled);

            IDirect3DSurface9* aoOutputSurface =
                aoNeedsAa
                    ? g_aoCompositeSurface
                    : backbuffer;

            device->SetRenderTarget(
                0,
                aoOutputSurface);

            device->SetViewport(
                &fullVp);

            device->SetPixelShader(
                g_aoDenoiseShader);

            device->SetTexture(
                0,
                static_cast<IDirect3DBaseTexture9*>(
                    g_sceneTexture));

            device->SetTexture(
                1,
                static_cast<IDirect3DBaseTexture9*>(
                    g_aoTexture));

            device->SetTexture(
                2,
                static_cast<IDirect3DBaseTexture9*>(
                    g_depthTexture));

            device->SetSamplerState(
                0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(
                0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(
                0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(
                0, D3DSAMP_SRGBTEXTURE, FALSE);

            device->SetSamplerState(
                1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(
                1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(
                1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(
                1, D3DSAMP_SRGBTEXTURE, FALSE);

            device->SetSamplerState(
                2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(
                2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(
                2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(
                2, D3DSAMP_SRGBTEXTURE, FALSE);

            const float metrics[4] = {
                1.0f / static_cast<float>(g_aoWidth),
                1.0f / static_cast<float>(g_aoHeight),
                1.0f / static_cast<float>(bbDesc.Width),
                1.0f / static_cast<float>(bbDesc.Height)
            };

            const float denoiseParams[4] = {
                projection[2],
                projection[3],
                denoiseSharp,
                static_cast<float>(aoOutputMode)
            };

            device->SetPixelShaderConstantF(
                0, metrics, 1);

            device->SetPixelShaderConstantF(
                1, denoiseParams, 1);

            const float w =
                static_cast<float>(bbDesc.Width);

            const float h =
                static_cast<float>(bbDesc.Height);

            const FsVertex fullQuad[4] = {
                { -0.5f,    -0.5f,    0.0f, 1.0f, 0.0f, 0.0f },
                { w - 0.5f, -0.5f,    0.0f, 1.0f, 1.0f, 0.0f },
                { -0.5f,     h - 0.5f,0.0f, 1.0f, 0.0f, 1.0f },
                { w - 0.5f,  h - 0.5f,0.0f, 1.0f, 1.0f, 1.0f },
            };

            const HRESULT denoiseDrawHr =
                device->DrawPrimitiveUP(
                    D3DPT_TRIANGLESTRIP,
                    2,
                    fullQuad,
                    sizeof(FsVertex));

            device->SetTexture(0, nullptr);
            device->SetTexture(1, nullptr);
            device->SetTexture(2, nullptr);

            if (!g_loggedAoProjection)
            {
                g_loggedAoProjection = true;

                WLOG_INFO(
                    "wxl-modern-r3e4: AO path production=%d preset=%d "
                    "xScale=%.9g yScale=%.9g A=%.9g B=%.9g "
                    "radius=%.3g intensity=%.3g bias=%.3g "
                    "maxUv=%.3g power=%.3g radial=%.3g "
                    "fade=%.3g..%.3g ao=%ux%u denoiseSharp=%.3g",
                    productionAo ? 1 : 0,
                    effectiveTuneMode,
                    projection[0],
                    projection[1],
                    projection[2],
                    projection[3],
                    aoParams[0],
                    aoParams[1],
                    aoParams[2],
                    aoParams[3],
                    rawControl[0],
                    rawControl[3],
                    rawControl[1],
                    rawControl[2],
                    g_aoWidth,
                    g_aoHeight,
                    denoiseParams[2]);
            }

            if (FAILED(denoiseDrawHr))
            {
                RestoreDeviceState(
                    device, state, oldRt, oldDepth, oldViewport);

                WLOG_ERROR(
                    "wxl-modern-r3e4: AO denoise/composite draw "
                    "failed hr=0x%08X",
                    static_cast<unsigned>(denoiseDrawHr));

                SafeRelease(state);
                SafeRelease(oldRt);
                SafeRelease(oldDepth);
                backbuffer->Release();
                return false;
            }

            // Production CE ordering:
            //
            //   resolved world -> AO -> AA -> UI
            //
            // Diagnostic AO proof modes deliberately stop before AA so the
            // mask/composite remains directly inspectable.
            if (productionAo &&
                aoNeedsAa &&
                useSmaa)
            {
                const bool ok =
                    d3d9smaa::Render(
                        device,
                        g_aoCompositeTexture,
                        backbuffer,
                        bbDesc.Width,
                        bbDesc.Height,
                        smaaQuality);

                RestoreDeviceState(
                    device, state, oldRt, oldDepth, oldViewport);

                if (ok)
                    logPass(17, smaaTier);

                SafeRelease(state);
                SafeRelease(oldRt);
                SafeRelease(oldDepth);
                backbuffer->Release();

                return ok;
            }

            if (productionAo &&
                aoNeedsAa &&
                !useSmaa &&
                fxaaEnabled)
            {
                if (!EnsureFxaaShader(
                        device,
                        fxaaTier))
                {
                    RestoreDeviceState(
                        device, state, oldRt, oldDepth, oldViewport);

                    SafeRelease(state);
                    SafeRelease(oldRt);
                    SafeRelease(oldDepth);
                    backbuffer->Release();
                    return false;
                }

                device->SetRenderTarget(
                    0,
                    backbuffer);

                device->SetDepthStencilSurface(
                    nullptr);

                device->SetViewport(
                    &fullVp);

                device->SetVertexShader(nullptr);
                device->SetPixelShader(
                    g_fxaaShader[fxaaTier]);
                device->SetFVF(kFsFvf);

                device->SetTexture(
                    0,
                    static_cast<IDirect3DBaseTexture9*>(
                        g_aoCompositeTexture));

                device->SetSamplerState(
                    0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                device->SetSamplerState(
                    0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                device->SetSamplerState(
                    0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                device->SetSamplerState(
                    0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                device->SetSamplerState(
                    0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                device->SetSamplerState(
                    0, D3DSAMP_SRGBTEXTURE, FALSE);

                const float rcpFrame[4] = {
                    1.0f / static_cast<float>(bbDesc.Width),
                    1.0f / static_cast<float>(bbDesc.Height),
                    0.0f,
                    0.0f
                };

                device->SetPixelShaderConstantF(
                    0,
                    rcpFrame,
                    1);

                const HRESULT fxaaDrawHr =
                    device->DrawPrimitiveUP(
                        D3DPT_TRIANGLESTRIP,
                        2,
                        fullQuad,
                        sizeof(FsVertex));

                device->SetTexture(0, nullptr);

                RestoreDeviceState(
                    device, state, oldRt, oldDepth, oldViewport);

                if (SUCCEEDED(fxaaDrawHr))
                    logPass(18, fxaaTier);
                else
                    WLOG_ERROR(
                        "wxl-modern-r3e4: AO -> FXAA draw failed "
                        "hr=0x%08X",
                        static_cast<unsigned>(fxaaDrawHr));

                SafeRelease(state);
                SafeRelease(oldRt);
                SafeRelease(oldDepth);
                backbuffer->Release();

                return SUCCEEDED(fxaaDrawHr);
            }

            RestoreDeviceState(
                device, state, oldRt, oldDepth, oldViewport);

            if (aoProof)
            {
                logPass(
                    aoProofMode == 1 ? 14 : 15,
                    aoProofMode);
            }
            else
            {
                logPass(16, 2);
            }

            SafeRelease(state);
            SafeRelease(oldRt);
            SafeRelease(oldDepth);
            backbuffer->Release();

            return true;
        }

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

        if (aoProof)
        {
            device->SetTexture(
                1,
                static_cast<IDirect3DBaseTexture9*>(g_depthTexture));
        }

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

        if (aoProof)
        {
            device->SetSamplerState(
                1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            device->SetSamplerState(
                1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            device->SetSamplerState(
                1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            device->SetSamplerState(
                1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(
                1, D3DSAMP_SRGBTEXTURE, FALSE);
        }

        const float rcpFrame[4] = {
            1.0f / static_cast<float>(bbDesc.Width),
            1.0f / static_cast<float>(bbDesc.Height),
            0.0f,
            0.0f
        };

        device->SetPixelShaderConstantF(
            0, rcpFrame, 1);

        if (aoProof && worldProjection)
        {
            const float projection[4] = {
                worldProjection[0],
                worldProjection[5],
                worldProjection[10],
                worldProjection[14]
            };

            // R3E1 correctness values. These are still diagnostic rather
            // than accepted Classic Enhanced tuning: the key change here is
            // spatial stability / discontinuity behavior, not final strength.
            const float aoParams[4] = {
                0.65f,   // local world-space radius
                1.70f,   // visible proof intensity
                0.018f,  // self-occlusion bias
                0.040f   // max near-camera UV footprint
            };

            const float aoControl[4] = {
                static_cast<float>(aoProofMode),
                1.15f,   // contrast/power
                18.0f,   // full-strength distance
                55.0f    // fully faded distance
            };

            device->SetPixelShaderConstantF(
                1, projection, 1);

            device->SetPixelShaderConstantF(
                2, aoParams, 1);

            device->SetPixelShaderConstantF(
                3, aoControl, 1);

            if (!g_loggedAoProjection)
            {
                g_loggedAoProjection = true;

                WLOG_INFO(
                    "wxl-modern-r3e1: AO projection "
                    "xScale=%.9g yScale=%.9g A=%.9g B=%.9g "
                    "radius=%.3g intensity=%.3g bias=%.3g "
                    "fade=%.3g..%.3g",
                    projection[0],
                    projection[1],
                    projection[2],
                    projection[3],
                    aoParams[0],
                    aoParams[1],
                    aoParams[2],
                    aoControl[2],
                    aoControl[3]);
            }
        }

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

        if (aoProof)
            device->SetTexture(1, nullptr);

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
            const int passMode =
                depthProof ? (2 + depthMode) :
                aoProof ? (aoProofMode == 1 ? 12 : 13) :
                g_proofTint ? 0 : 1;

            const int passTier =
                depthProof ? depthMode :
                aoProof ? aoProofMode :
                fxaaTier;

            logPass(passMode, passTier);
        }

        SafeRelease(state);
        SafeRelease(oldRt);
        SafeRelease(oldDepth);
        backbuffer->Release();

        return SUCCEEDED(drawHr);
    }
}
