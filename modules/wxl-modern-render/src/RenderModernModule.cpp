// wxl-render-modern: modern graphics pipeline (post-process FX) for the Client.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "common/Log.hpp"
#include "engine/events/EventScript.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "game/Camera.hpp"
#include "engine/gpu/Proxy.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/World.hpp"
#include "offsets/game/WorldScene.hpp"
#include "client/CWorldScene/RenderModernBridge.hpp"

#include "gpu/Pipeline.hpp"
#include "gpu/D3D9Fallback.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

// The live-engine half of the graphics module. The proxy (d3d9.dll) owns the shared D3D12 device + queue and
// runs On12; this module drives a D3D12 post-process pass on its own queue. It runs at the world -> UI
// boundary (OnWorldRenderEnd): the 3D scene is done but the UI has not drawn. The core hands it the finished
// world (in the backbuffer, or in a render-size offscreen surface when supersampling is on) plus a readable
// depth when a depth-using effect asked for one; the module runs the enabled effects and writes the result
// onto the native backbuffer, and the UI then draws crisp on top.
namespace wxl::scripts::render_modern
{
    namespace ev    = wxl::events;
    namespace cam   = wxl::game::camera;
    namespace m2off = wxl::offsets::game::m2;
    namespace woff  = wxl::offsets::game::world;
    namespace wsoff = wxl::offsets::game::worldscene;

    // R4B2-A: controlled QA extension of the native world far-clip validator.
    //
    // Stock 3.3.5a remains authoritative unless WXL_EXTENDED_FARCLIP is
    // explicitly enabled. Requests at or below the native 1583.3334-yard
    // ceiling still use the engine validator unchanged. Only requests above
    // that ceiling are extended, and only as far as the 2112-yard QA target.
    woff::World_ValidateFarClipFn g_origValidateFarClip = nullptr;

    bool ExtendedFarClipEnabled()
    {
        static const bool enabled = []()
        {
            char raw[16] = {};

            const DWORD n =
                GetEnvironmentVariableA(
                    "WXL_EXTENDED_FARCLIP",
                    raw,
                    sizeof(raw));

            if (n == 0 || n >= sizeof(raw))
                return false;

            const char c = raw[0];

            return c != '0' &&
                   c != 'n' && c != 'N' &&
                   c != 'f' && c != 'F';
        }();

        return enabled;
    }

    float __cdecl hkValidateFarClip(float requested, int32_t mapId)
    {
        const float stock =
            g_origValidateFarClip
                ? g_origValidateFarClip(requested, mapId)
                : requested;

        if (!ExtendedFarClipEnabled())
            return stock;

        constexpr float kNativeCeiling = 1583.3334f;
        constexpr float kQaCeiling = 2112.0f;

        float result = stock;

        if (requested > kNativeCeiling)
            result =
                requested < kQaCeiling
                    ? requested
                    : kQaCeiling;

        static unsigned logged = 0;

        if (logged < 8)
        {
            ++logged;

            WLOG_INFO(
                "wxl-modern-r4b2a: farclip validator "
                "requested=%.9g map=%d stock=%.9g result=%.9g "
                "extended=%u",
                requested,
                static_cast<int>(mapId),
                stock,
                result,
                ExtendedFarClipEnabled() ? 1u : 0u);
        }

        return result;
    }

    bool InstallExtendedFarClipQa()
    {
        const bool installed =
            wxl::hook::Install(
                "R4ExtendedFarClipValidator",
                woff::kValidateFarClip,
                &hkValidateFarClip,
                &g_origValidateFarClip);

        if (installed)
        {
            WLOG_INFO(
                "wxl-modern-r4b2a: extended farclip QA hook installed "
                "(native=1583.3334 qa=2112)");
        }

        return installed;
    }

    WXL_REGISTER_FEATURE(
        "render-modern-r4-extended-farclip",
        true,
        InstallExtendedFarClipQa);

    // R4B2-D: opt-in proportional extension of the complete native
    // five-band object-distance hierarchy to the 2112-yard world horizon.
    //
    // The stock maximum live range is 1250 yards, so every seed is
    // multiplied by 2112/1250 = 1.6896. The native builder remains
    // authoritative for environmentDetail scaling, fade starts and squared
    // culling thresholds.
    wsoff::FadeDistanceScaleFn g_origFadeDistanceScale = nullptr;

    bool ExtendedObjectDistanceEnabled()
    {
        static const bool enabled = []()
        {
            char raw[16] = {};

            const DWORD n =
                GetEnvironmentVariableA(
                    "WXL_EXTENDED_OBJECT_DISTANCE",
                    raw,
                    sizeof(raw));

            if (n == 0 || n >= sizeof(raw))
                return false;

            const char c = raw[0];

            return c != '0' &&
                   c != 'n' && c != 'N' &&
                   c != 'f' && c != 'F';
        }();

        return enabled;
    }

    void __cdecl hkFadeDistanceScale(float scale)
    {
        if (!g_origFadeDistanceScale)
            return;

        if (!ExtendedObjectDistanceEnabled())
        {
            g_origFadeDistanceScale(scale);
            return;
        }

        float* const band1Seed =
            reinterpret_cast<float*>(wsoff::kDistanceBand1Seed);
        float* const band2Seed =
            reinterpret_cast<float*>(wsoff::kDistanceBand2Seed);
        float* const band3Seed =
            reinterpret_cast<float*>(wsoff::kDistanceBand3Seed);
        float* const band4Seed =
            reinterpret_cast<float*>(wsoff::kDistanceBand4Seed);
        float* const band5Seed =
            reinterpret_cast<float*>(wsoff::kDistanceBand5Seed);

        const float stockBand1Seed = *band1Seed;
        const float stockBand2Seed = *band2Seed;
        const float stockBand3Seed = *band3Seed;
        const float stockBand4Seed = *band4Seed;
        const float stockBand5Seed = *band5Seed;

        constexpr float kHorizonScale = 2112.0f / 1250.0f;

        *band1Seed = stockBand1Seed * kHorizonScale;
        *band2Seed = stockBand2Seed * kHorizonScale;
        *band3Seed = stockBand3Seed * kHorizonScale;
        *band4Seed = stockBand4Seed * kHorizonScale;
        *band5Seed = stockBand5Seed * kHorizonScale;

        g_origFadeDistanceScale(scale);

        // Restore native seed memory immediately. Only the engine-built live
        // table remains extended until its next rebuild.
        *band1Seed = stockBand1Seed;
        *band2Seed = stockBand2Seed;
        *band3Seed = stockBand3Seed;
        *band4Seed = stockBand4Seed;
        *band5Seed = stockBand5Seed;

        static unsigned logged = 0;

        if (logged < 8)
        {
            ++logged;

            const float live1 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand1Live);
            const float live2 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand2Live);
            const float live3 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand3Live);
            const float live4 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand4Live);
            const float live5 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand5Live);

            const float fade1 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand1FadeStart);
            const float fade2 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand2FadeStart);
            const float fade3 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand3FadeStart);
            const float fade4 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand4FadeStart);
            const float fade5 =
                *reinterpret_cast<const float*>(
                    wsoff::kDistanceBand5FadeStart);

            WLOG_INFO(
                "wxl-modern-r4b2d: object distances "
                "scale=%.9g factor=%.9g "
                "b1=%.9g b2=%.9g b3=%.9g b4=%.9g b5=%.9g",
                scale,
                kHorizonScale,
                live1,
                live2,
                live3,
                live4,
                live5);

            WLOG_INFO(
                "wxl-modern-r4b2d: object fade starts "
                "f1=%.9g f2=%.9g f3=%.9g f4=%.9g f5=%.9g",
                fade1,
                fade2,
                fade3,
                fade4,
                fade5);
        }
    }

    bool InstallExtendedObjectDistanceQa()
    {
        const bool installed =
            wxl::hook::Install(
                "R4ExtendedObjectDistance",
                wsoff::kFadeDistanceScale,
                &hkFadeDistanceScale,
                &g_origFadeDistanceScale);

        if (installed)
        {
            WLOG_INFO(
                "wxl-modern-r4b2d: proportional full-table object-distance "
                "QA hook installed (horizon scale=2112/1250)");
        }

        return installed;
    }

    WXL_REGISTER_FEATURE(
        "render-modern-r4-extended-object-distance",
        true,
        InstallExtendedObjectDistanceQa);

    // R4B2-F2: opt-in QA override for the native M2 alpha-key
    // reference. Stock 3.3.5a uses 224/255 for blend mode 1. At long
    // distance, mip-filtered foliage alpha can lose coverage against
    // that high cutoff even while the M2 itself remains drawable.
    //
    // WXL_ALPHA_KEY_REF_BYTE accepts 0..224. Unset = exact stock
    // behaviour. The hook lets the native material setup run first,
    // then overrides ONLY blend-mode-1 alpha reference state.
    m2off::M2_SetupMaterialFn g_origSetupMaterialAlphaQa = nullptr;

    struct AlphaKeyRefQaConfig
    {
        bool enabled = false;
        unsigned byteRef = 224;
        float normalizedRef = 224.0f / 255.0f;
    };

    const AlphaKeyRefQaConfig& GetAlphaKeyRefQaConfig()
    {
        static const AlphaKeyRefQaConfig config = []()
        {
            AlphaKeyRefQaConfig c;

            char raw[32] = {};

            const DWORD n =
                GetEnvironmentVariableA(
                    "WXL_ALPHA_KEY_REF_BYTE",
                    raw,
                    sizeof(raw));

            if (n == 0 || n >= sizeof(raw))
                return c;

            char* end = nullptr;
            const long parsed = std::strtol(raw, &end, 10);

            if (end == raw || (end && *end != '\\0'))
                return c;

            long clamped = parsed;

            if (clamped < 0)
                clamped = 0;

            if (clamped > 224)
                clamped = 224;

            c.enabled = true;
            c.byteRef = static_cast<unsigned>(clamped);
            c.normalizedRef =
                static_cast<float>(clamped) / 255.0f;

            return c;
        }();

        return config;
    }

    void __fastcall hkSetupMaterialAlphaQa(
        void* renderCtx,
        void* edx)
    {
        if (g_origSetupMaterialAlphaQa)
            g_origSetupMaterialAlphaQa(renderCtx, edx);

        const AlphaKeyRefQaConfig& qa =
            GetAlphaKeyRefQaConfig();

        if (!qa.enabled || !renderCtx)
            return;

        const auto* ctx =
            static_cast<const m2off::DrawContext*>(renderCtx);

        if (!ctx->material || !ctx->element)
            return;

        const auto* material =
            static_cast<const m2off::Material*>(ctx->material);

        if (material->blend != 1)
            return;

        const auto* elementBytes =
            static_cast<const uint8_t*>(ctx->element);

        const float elementAlpha =
            *reinterpret_cast<const float*>(
                elementBytes + m2off::kOffElementAlpha);

        float alphaRef =
            elementAlpha * qa.normalizedRef;

        if (alphaRef < 0.0f)
            alphaRef = 0.0f;
        else if (alphaRef > 1.0f)
            alphaRef = 1.0f;

        using PushAlphaRefFn =
            void(__cdecl*)(float alphaRef);

        const auto pushAlphaRef =
            reinterpret_cast<PushAlphaRefFn>(
                m2off::kPushAlphaRef);

        pushAlphaRef(alphaRef);

        static unsigned logged = 0;

        if (logged < 8)
        {
            ++logged;

            WLOG_INFO(
                "wxl-modern-r4b2f2: alpha-key ref QA "
                "byte=%u normalized=%.9g elementAlpha=%.9g "
                "appliedRef=%.9g",
                qa.byteRef,
                qa.normalizedRef,
                elementAlpha,
                alphaRef);
        }
    }

    bool InstallAlphaKeyRefQa()
    {
        const bool installed =
            wxl::hook::Install(
                "R4AlphaKeyRefQa",
                m2off::kSetupMaterial,
                &hkSetupMaterialAlphaQa,
                &g_origSetupMaterialAlphaQa);

        if (installed)
        {
            const AlphaKeyRefQaConfig& qa =
                GetAlphaKeyRefQaConfig();

            WLOG_INFO(
                "wxl-modern-r4b2f2: alpha-key reference QA hook installed "
                "(stock=224 configured=%u enabled=%u)",
                qa.byteRef,
                qa.enabled ? 1u : 0u);
        }

        return installed;
    }

    WXL_REGISTER_FEATURE(
        "render-modern-r4-alpha-key-ref-qa",
        true,
        InstallAlphaKeyRefQa);

    /** @brief Drives the post-process pipeline once per frame from the live device. */
    class RenderModernModule : public ev::EventScript
    {
    public:
        RenderModernModule()
        {
            on<&RenderModernModule::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
            on<&RenderModernModule::OnWorldRenderEnd>(ev::Event::OnWorldRenderEnd);
            on<&RenderModernModule::OnDeviceLost>(ev::Event::OnDeviceLost);
            WLOG_INFO("wxl-render-modern: loaded (D3D12 post-process pipeline)");
        }

    private:
        IDirect3DDevice9On12* on12_ = nullptr;
        IDirect3DDevice9*     dev9_  = nullptr;

        // R3D1A: the core captures this BEFORE the world pass, so this is
        // the depth-stencil surface the world genuinely wrote into.
        IDirect3DSurface9* protonWorldDepth_ = nullptr;

        // R3D3A: exact projection matrix that was live when the world scene
        // finished, retained until the later world -> UI boundary.
        D3DMATRIX protonWorldProjection_ = {};
        bool protonWorldProjectionValid_ = false;
        unsigned protonProjectionSamples_ = 0;
        bool protonProjectionBoundaryLogged_ = false;
        bool protonProjectionCaptureFailLogged_ = false;

        void ReleaseProtonWorldDepth()
        {
            if (protonWorldDepth_)
            {
                protonWorldDepth_->Release();
                protonWorldDepth_ = nullptr;
            }
        }

        static bool DepthStateProbeEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};
                const DWORD n = GetEnvironmentVariableA(
                    "WXL_DEPTH_STATE_PROBE",
                    raw,
                    sizeof(raw));

                if (n == 0 || n >= sizeof(raw))
                    return false;

                const char c = raw[0];

                return c != '0' &&
                       c != 'n' && c != 'N' &&
                       c != 'f' && c != 'F';
            }();

            return enabled;
        }

        static bool ProjectionProbeEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};
                const DWORD n = GetEnvironmentVariableA(
                    "WXL_PROJECTION_PROBE",
                    raw,
                    sizeof(raw));

                if (n == 0 || n >= sizeof(raw))
                    return false;

                const char c = raw[0];

                return c != '0' &&
                       c != 'n' && c != 'N' &&
                       c != 'f' && c != 'F';
            }();

            return enabled;
        }

        void OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
        {
            if (!d3d9fallback::Available())
                return;

            IDirect3DSurface9* depth =
                static_cast<IDirect3DSurface9*>(a.sceneDepth);

            // Hold the exact per-frame world depth until the later
            // world->UI post-process boundary.
            if (depth)
                depth->AddRef();

            ReleaseProtonWorldDepth();
            protonWorldDepth_ = depth;

            if (DepthStateProbeEnabled())
            {
                static bool loggedDepthState = false;

                if (!loggedDepthState)
                {
                    loggedDepthState = true;

                    if (a.sceneProjectionValid && a.sceneProjection)
                    {
                        const float* p = a.sceneProjection;

                        WLOG_INFO(
                            "wxl-modern-r3d3c: pre-world device projection "
                            "m=[%.9g %.9g %.9g %.9g | "
                            "%.9g %.9g %.9g %.9g | "
                            "%.9g %.9g %.9g %.9g | "
                            "%.9g %.9g %.9g %.9g]",
                            p[0],  p[1],  p[2],  p[3],
                            p[4],  p[5],  p[6],  p[7],
                            p[8],  p[9],  p[10], p[11],
                            p[12], p[13], p[14], p[15]);

                        WLOG_INFO(
                            "wxl-modern-r3d3c: pre-world device "
                            "A=%.9g B=%.9g m11=%.9g m15=%.9g",
                            p[10],
                            p[14],
                            p[11],
                            p[15]);
                    }
                    else
                    {
                        WLOG_ERROR(
                            "wxl-modern-r3d3c: pre-world device "
                            "projection capture FAIL");
                    }

                    if (a.viewportValid)
                    {
                        WLOG_INFO(
                            "wxl-modern-r3d3c: pre-world viewport "
                            "x=%u y=%u width=%u height=%u "
                            "minZ=%.9g maxZ=%.9g",
                            a.viewportX,
                            a.viewportY,
                            a.viewportWidth,
                            a.viewportHeight,
                            a.viewportMinZ,
                            a.viewportMaxZ);
                    }
                    else
                    {
                        WLOG_ERROR(
                            "wxl-modern-r3d3c: pre-world viewport "
                            "capture FAIL");
                    }
                }
            }

            // R3D3A1: the D3D device transform at this callback has already
            // been replaced by a screen-space/post-process projection.
            // The engine camera global remains the authoritative projection
            // that generated the world's hardware depth.
            const float* projection = cam::GetProjection();

            if (projection)
            {
                std::memcpy(
                    &protonWorldProjection_,
                    projection,
                    sizeof(protonWorldProjection_));
                protonWorldProjectionValid_ = true;

                if (ProjectionProbeEnabled())
                {
                    ++protonProjectionSamples_;

                    // Six samples, roughly two seconds apart at 60 FPS.
                    // This is enough to move/rotate/zoom the camera and
                    // establish whether the projection authority remains sane.
                    const bool logSample =
                        protonProjectionSamples_ == 1 ||
                        (protonProjectionSamples_ <= 601 &&
                         ((protonProjectionSamples_ - 1) % 120) == 0);

                    if (logSample)
                    {
                        const float* m =
                            reinterpret_cast<const float*>(
                                &protonWorldProjection_);

                        const float aZ = m[10];
                        const float bZ = m[14];
                        const float farDenom = 1.0f - aZ;

                        const float nearPlane =
                            aZ != 0.0f
                                ? (-bZ / aZ)
                                : 0.0f;

                        const float farPlane =
                            farDenom != 0.0f
                                ? (bZ / farDenom)
                                : 0.0f;

                        if (protonProjectionSamples_ == 1)
                        {
                            WLOG_INFO(
                                "wxl-modern-r3d3a1: engine projection matrix "
                                "m=[%.9g %.9g %.9g %.9g | "
                                "%.9g %.9g %.9g %.9g | "
                                "%.9g %.9g %.9g %.9g | "
                                "%.9g %.9g %.9g %.9g]",
                                m[0],  m[1],  m[2],  m[3],
                                m[4],  m[5],  m[6],  m[7],
                                m[8],  m[9],  m[10], m[11],
                                m[12], m[13], m[14], m[15]);
                        }

                        WLOG_INFO(
                            "wxl-modern-r3d3a1: engine projection sample=%u "
                            "xScale=%.9g yScale=%.9g "
                            "A=%.9g B=%.9g near=%.9g far=%.9g "
                            "m11=%.9g m15=%.9g",
                            protonProjectionSamples_,
                            m[0],
                            m[5],
                            aZ,
                            bZ,
                            nearPlane,
                            farPlane,
                            m[11],
                            m[15]);
                    }
                }
            }
            else
            {
                protonWorldProjectionValid_ = false;

                if (ProjectionProbeEnabled() &&
                    !protonProjectionCaptureFailLogged_)
                {
                    protonProjectionCaptureFailLogged_ = true;

                    WLOG_ERROR(
                        "wxl-modern-r3d3a1: engine projection capture FAIL");
                }
            }
        }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            ReleaseProtonWorldDepth();

            protonWorldProjectionValid_ = false;
            protonProjectionSamples_ = 0;
            protonProjectionBoundaryLogged_ = false;
            protonProjectionCaptureFailLogged_ = false;

            d3d9fallback::PrepareForReset();
            Pipeline::Get().PrepareForReset();
        }

        /**
         * @brief Caches the On12 interface from the live device on first sight.
         * @param device  live D3D9 device from the args.
         * @return true when the On12 interface is available.
         */
        bool EnsureOn12(IDirect3DDevice9* device)
        {
            if (on12_ && dev9_ == device) return true;
            if (on12_) { on12_->Release(); on12_ = nullptr; }
            dev9_ = device;
            if (!device) return false;
            if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9On12), (void**)&on12_)) || !on12_)
            {
                WLOG_WARN("wxl-render-modern: QueryInterface(IDirect3DDevice9On12) failed");
                on12_ = nullptr;
                return false;
            }
            WLOG_INFO("wxl-render-modern: On12 acquired (on12=%p)", on12_);
            return true;
        }

        /**
         * @brief Runs the post-process at the world -> UI boundary, on the finished world.
         * @param a  world-render-end args: the live device, the supersample source, and the readable depth.
         */
        void OnWorldRenderEnd(const ev::WorldRenderEndArgs& a)
        {
            IDirect3DDevice9* device = static_cast<IDirect3DDevice9*>(a.device);

            // Proton/Wine R3B path. The On12 interface itself exists there, but the
            // underlying backbuffer cannot be unwrapped for our D3D12 work. Stay in
            // D3D9, resolve the engine MSAA target, process the world, then let WoW
            // draw its interface on top as normal.
            if (d3d9fallback::Available())
            {
                wxl::runtime::render::SetReadableDepthNeeded(false);

                bool fxaaEnabled = false;
                bool smaaEnabled = false;
                Quality fxaaQuality = Quality::Medium;
                Quality smaaQuality = Quality::Medium;

                for (const auto& e : Pipeline::Get().Effects())
                {
                    if (std::strcmp(e->Name(), "FXAA") == 0)
                    {
                        fxaaEnabled = e->Enabled();
                        fxaaQuality = e->GetQuality();
                    }
                    else if (std::strcmp(e->Name(), "SMAA") == 0)
                    {
                        smaaEnabled = e->Enabled();
                        smaaQuality = e->GetQuality();
                    }
                }

                // Consume the exact depth surface captured around the
                // actual world scene pass. Do not query whichever depth
                // surface happens to be bound at this later boundary.
                IDirect3DSurface9* frameWorldDepth =
                    protonWorldDepth_;

                protonWorldDepth_ = nullptr;

                if (ProjectionProbeEnabled() &&
                    protonWorldProjectionValid_ &&
                    !protonProjectionBoundaryLogged_)
                {
                    const float* m =
                        reinterpret_cast<const float*>(
                            &protonWorldProjection_);

                    WLOG_INFO(
                        "wxl-modern-r3d3a1: engine projection snapshot retained "
                        "to world-ui boundary A=%.9g B=%.9g "
                        "xScale=%.9g yScale=%.9g",
                        m[10],
                        m[14],
                        m[0],
                        m[5]);

                    protonProjectionBoundaryLogged_ = true;
                }

                const float* frameWorldProjection =
                    protonWorldProjectionValid_
                        ? reinterpret_cast<const float*>(
                              &protonWorldProjection_)
                        : nullptr;

                d3d9fallback::Frame(
                    device,
                    fxaaEnabled,
                    fxaaQuality,
                    smaaEnabled,
                    smaaQuality,
                    frameWorldDepth,
                    frameWorldProjection);

                if (frameWorldDepth)
                    frameWorldDepth->Release();

                // R3D3A snapshot is frame-scoped. The next world scene
                // completion will supply the next authoritative projection.
                protonWorldProjectionValid_ = false;

                return;
            }

            // Native Windows D3D12 path.
            bool needDepth = false;
            for (const auto& e : Pipeline::Get().Effects())
                if (e->Enabled() && e->NeedsDepth()) { needDepth = true; break; }
            wxl::runtime::render::SetReadableDepthNeeded(needDepth);

            if (!EnsureOn12(device)) return;
            if (!WxlD3D12Device()) return;

            IDirect3DResource9* superSample = static_cast<IDirect3DResource9*>(a.superSampleSource);

            // With no supersampling and nothing enabled the world is already on the backbuffer: skip the pass
            // entirely. A supersample source must always be downsampled, so it forces the pass on.
            if (!superSample && !Pipeline::Get().HasEnabledEffect()) { WxlD3D12DrainDebug(); return; }

            IDirect3DSurface9* bb = nullptr;
            if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

            // The pipeline runs on its own queue, separate from the On12 internal queue. depthSource is the
            // sampleable world depth or null. The projection drives the depth-using effects (SSAO): in-world the
            // live world camera global is valid, but on the glue screens it sits at identity, so the glue
            // boundary passes its own projection through a.proj -- use it when provided, else the world global.
            IDirect3DResource9* depth = static_cast<IDirect3DResource9*>(a.depthSource);
            Pipeline::Get().Frame(on12_, WxlD3D12Device(),
                                  static_cast<IDirect3DResource9*>(bb), superSample, depth,
                                  a.proj ? a.proj : cam::GetProjection(), cam::GetView());

            bb->Release();
            WxlD3D12DrainDebug();
        }
    };

    // File-scope instance self-registers its handlers at DLL load via the EventScript ctor.
    RenderModernModule g_renderModernModule;
}
