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
#include "game/Camera.hpp"
#include "engine/gpu/Proxy.hpp"
#include "client/CWorldScene/RenderModernBridge.hpp"

#include "gpu/Pipeline.hpp"
#include "gpu/D3D9Fallback.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>

#include <cstring>

// The live-engine half of the graphics module. The proxy (d3d9.dll) owns the shared D3D12 device + queue and
// runs On12; this module drives a D3D12 post-process pass on its own queue. It runs at the world -> UI
// boundary (OnWorldRenderEnd): the 3D scene is done but the UI has not drawn. The core hands it the finished
// world (in the backbuffer, or in a render-size offscreen surface when supersampling is on) plus a readable
// depth when a depth-using effect asked for one; the module runs the enabled effects and writes the result
// onto the native backbuffer, and the UI then draws crisp on top.
namespace wxl::scripts::render_modern
{
    namespace ev  = wxl::events;
    namespace cam = wxl::game::camera;

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

                d3d9fallback::Frame(
                    device,
                    fxaaEnabled,
                    fxaaQuality,
                    smaaEnabled,
                    smaaQuality,
                    frameWorldDepth);

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
