// R6 Slot-3 selected mirrored-reflection Win32 backend.
//
// This translation unit is compiled into WarcraftXL but has no live callsite.
// It does not hook DrawIndexedPrimitive and does not enable Slot-3 replacement.
//
// Frozen reflection content:
//   mode 1 = whole sky
//   mode 2 = whole sky + terrain
//   mode 3 = whole sky + terrain + WMO
//
// The first backend is OUTDOOR ONLY.  Portal/interior reflection remains
// fail-closed until that separate native branch is mechanically closed.
//
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#include "water/Slot3ReflectionWin32.hpp"
#include "water/Slot3ReflectionBackendPolicy.hpp"
#include "water/Slot3Core.hpp"

#if defined(_WIN32)

#include "offsets/engine/Camera.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/ADT.hpp"
#include "offsets/game/WMO.hpp"
#include "offsets/game/WorldScene.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdint>

namespace wxl::water::slot3
{
    namespace
    {
        namespace camera =
            wxl::offsets::engine::camera;

        namespace gx =
            wxl::offsets::engine::gx;

        namespace adt =
            wxl::offsets::game::adt;

        namespace wmo =
            wxl::offsets::game::wmo;

        namespace worldscene =
            wxl::offsets::game::worldscene;

        // Reflection-Q whole-sky authority.
        constexpr std::uintptr_t
            kWholeSkyDraw = 0x007F09B0u;

        // Reflection-Y post-cull sequence.
        //
        // The deeper semantic names of these helpers remain unresolved;
        // these neutral names intentionally claim only their proven
        // ordering in the camera-dependent post-cull path.
        constexpr std::uintptr_t
            kPostCullFinalizeA = 0x0079A260u;

        constexpr std::uintptr_t
            kPostCullFinalizeB = 0x00793450u;

        constexpr std::uintptr_t
            kPostCullFinalizeObject =
                0x007CECD0u;

        constexpr std::uintptr_t
            kPostCullObject = 0x00CDAF48u;

        static_assert(
            static_cast<unsigned>(
                D3DFMT_A8R8G8B8) ==
            kNativeReflectionD3dColourFormat,
            "R6 reflection D3D9 format bridge");

        template <class T>
        void ReleaseCom(T*& value) noexcept
        {
            if (value)
            {
                value->Release();
                value = nullptr;
            }
        }

        struct SavedGpuState
        {
            IDirect3DStateBlock9* block = nullptr;
            IDirect3DSurface9* rt0 = nullptr;
            IDirect3DSurface9* depth = nullptr;
            D3DVIEWPORT9 viewport{};

            ~SavedGpuState()
            {
                Reset();
            }

            void Reset() noexcept
            {
                ReleaseCom(depth);
                ReleaseCom(rt0);
                ReleaseCom(block);
                viewport = {};
            }

            bool Capture(
                IDirect3DDevice9* device) noexcept
            {
                Reset();

                if (!device)
                {
                    return false;
                }

                if (FAILED(
                        device->CreateStateBlock(
                            D3DSBT_ALL,
                            &block)) ||
                    !block)
                {
                    Reset();
                    return false;
                }

                if (FAILED(block->Capture()))
                {
                    Reset();
                    return false;
                }

                if (FAILED(
                        device->GetRenderTarget(
                            0,
                            &rt0)) ||
                    !rt0)
                {
                    Reset();
                    return false;
                }

                if (FAILED(
                        device->GetDepthStencilSurface(
                            &depth)) ||
                    !depth)
                {
                    Reset();
                    return false;
                }

                if (FAILED(
                        device->GetViewport(
                            &viewport)))
                {
                    Reset();
                    return false;
                }

                return true;
            }

            bool Restore(
                IDirect3DDevice9* device) noexcept
            {
                if (!device ||
                    !block ||
                    !rt0 ||
                    !depth)
                {
                    return false;
                }

                // Same restore hardening used by Depth-E:
                // state-block Apply first, then explicit surfaces/viewport.
                if (FAILED(block->Apply()))
                {
                    return false;
                }

                if (FAILED(
                        device->SetRenderTarget(
                            0,
                            rt0)))
                {
                    return false;
                }

                if (FAILED(
                        device->SetDepthStencilSurface(
                            depth)))
                {
                    return false;
                }

                if (FAILED(
                        device->SetViewport(
                            &viewport)))
                {
                    return false;
                }

                return true;
            }
        };

        using BuildCameraMatricesFn =
            void(__cdecl*)(
                const float* eye,
                const float* target);

        using ViewerLocateFn =
            void(__cdecl*)();

        using CullSortTableFn =
            void(__cdecl*)(
                float* portalRect,
                int secondArg);

        using NoArgFn =
            void(__cdecl*)();

        using ThisNoArgFn =
            void(__thiscall*)(
                void* self);

        bool ViewerIsOutdoor() noexcept
        {
            const auto viewerGroup =
                static_cast<std::uintptr_t>(
                    *reinterpret_cast<
                        const std::uint32_t*>(
                            wmo::kCurrentInteriorInstance));

            const unsigned secondGroup =
                *reinterpret_cast<
                    const std::uint32_t*>(
                        worldscene::
                            kViewerSecondGroupFlag);

            return
                NativeReflectionOutdoorStateSupported(
                    viewerGroup,
                    secondGroup);
        }

        bool UploadCurrentCameraMatrices(
            IDirect3DDevice9* expectedDevice) noexcept
        {
            auto* gxDevice =
                *reinterpret_cast<void**>(
                    gx::kGxDevicePtr);

            if (!gxDevice)
            {
                return false;
            }

            auto* engineD3d =
                *reinterpret_cast<IDirect3DDevice9**>(
                    reinterpret_cast<
                        std::uint8_t*>(gxDevice) +
                    gx::kD3DDeviceField);

            if (!engineD3d ||
                engineD3d != expectedDevice)
            {
                return false;
            }

            auto** vtable =
                *reinterpret_cast<void***>(
                    gxDevice);

            if (!vtable)
            {
                return false;
            }

            const auto setProjection =
                reinterpret_cast<
                    gx::GxSetProjectionFn>(
                        vtable[
                            gx::
                                kGxSetProjectionSlot]);

            const auto setView =
                reinterpret_cast<
                    gx::GxSetProjectionFn>(
                        vtable[
                            gx::
                                kGxSetViewSlot]);

            if (!setProjection ||
                !setView)
            {
                return false;
            }

            setProjection(
                gxDevice,
                nullptr,
                reinterpret_cast<const void*>(
                    camera::kProjection));

            setView(
                gxDevice,
                nullptr,
                reinterpret_cast<const void*>(
                    camera::kView));

            const auto refreshShaders =
                reinterpret_cast<
                    gx::ShaderUpdateProjMatrixFn>(
                        gx::
                            kShaderUpdateProjMatrix);

            if (!refreshShaders)
            {
                return false;
            }

            refreshShaders();

            return true;
        }

        bool BuildCamera(
            IDirect3DDevice9* device,
            const Vec3& eye,
            const Vec3& target) noexcept
        {
            const auto build =
                reinterpret_cast<
                    BuildCameraMatricesFn>(
                        camera::
                            kBuildCameraMatrices);

            if (!build)
            {
                return false;
            }

            const float eye3[3] =
            {
                eye.x,
                eye.y,
                eye.z
            };

            const float target3[3] =
            {
                target.x,
                target.y,
                target.z
            };

            build(
                eye3,
                target3);

            return
                UploadCurrentCameraMatrices(
                    device);
        }

        bool LocateViewerOutdoor() noexcept
        {
            const auto locate =
                reinterpret_cast<
                    ViewerLocateFn>(
                        worldscene::
                            kViewerLocate);

            if (!locate)
            {
                return false;
            }

            // Proven no-argument cdecl callsite.
            locate();

            return ViewerIsOutdoor();
        }

        bool CullOutdoor() noexcept
        {
            if (!ViewerIsOutdoor())
            {
                return false;
            }

            const auto cullSort =
                reinterpret_cast<
                    CullSortTableFn>(
                        wmo::
                            kCullSortTable);

            const auto finalizeA =
                reinterpret_cast<
                    NoArgFn>(
                        kPostCullFinalizeA);

            const auto finalizeB =
                reinterpret_cast<
                    NoArgFn>(
                        kPostCullFinalizeB);

            const auto finalizeObject =
                reinterpret_cast<
                    ThisNoArgFn>(
                        kPostCullFinalizeObject);

            if (!cullSort ||
                !finalizeA ||
                !finalizeB ||
                !finalizeObject)
            {
                return false;
            }

            // Reflection-Y:
            //   cdecl(
            //       portalRectPointer = 0x00ADF58C,
            //       secondArg = 0)
            //
            // This wrapper performs the complete 64-slot native terrain /
            // WMO / liquid / horizon cull walk and is the sole direct
            // caller of ADT::kCullChunks.
            cullSort(
                reinterpret_cast<float*>(
                    wmo::kPortalRect),
                0);

            finalizeA();
            finalizeB();

            finalizeObject(
                reinterpret_cast<void*>(
                    kPostCullObject));

            return true;
        }

        bool BindReflectionTarget(
            IDirect3DDevice9* device,
            IDirect3DSurface9* surface,
            IDirect3DSurface9* depth,
            unsigned width,
            unsigned height) noexcept
        {
            if (!device ||
                !surface ||
                !depth ||
                width == 0 ||
                height == 0)
            {
                return false;
            }

            D3DCAPS9 caps{};

            if (FAILED(
                    device->GetDeviceCaps(
                        &caps)))
            {
                return false;
            }

            if (FAILED(
                    device->SetRenderTarget(
                        0,
                        surface)))
            {
                return false;
            }

            // Keep the secondary target isolated from any additional
            // render targets that happened to be active in the main scene.
            const DWORD rtCount =
                std::min<DWORD>(
                    caps.NumSimultaneousRTs,
                    4u);

            for (DWORD index = 1;
                 index < rtCount;
                 ++index)
            {
                if (FAILED(
                        device->SetRenderTarget(
                            index,
                            nullptr)))
                {
                    return false;
                }
            }

            if (FAILED(
                    device->SetDepthStencilSurface(
                        depth)))
            {
                return false;
            }

            D3DVIEWPORT9 viewport{};

            viewport.X = 0;
            viewport.Y = 0;
            viewport.Width = width;
            viewport.Height = height;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;

            if (FAILED(
                    device->SetViewport(
                        &viewport)))
            {
                return false;
            }

            // The colour target is deliberately not assigned an invented
            // clear colour.  The proven whole-sky pass owns reflection
            // colour production.  Only depth is reset for the secondary
            // geometry passes.
            if (FAILED(
                    device->Clear(
                        0,
                        nullptr,
                        D3DCLEAR_ZBUFFER,
                        0,
                        1.0f,
                        0)))
            {
                return false;
            }

            return true;
        }

        class SequenceBackend
        {
        public:
            SequenceBackend(
                IDirect3DDevice9* device,
                const ReflectionPlan& plan,
                IDirect3DSurface9* surface,
                IDirect3DSurface9* depth,
                bool& quarantine) noexcept
                : device_(device),
                  plan_(plan),
                  surface_(surface),
                  depth_(depth),
                  quarantine_(quarantine)
            {
            }

            bool Capture() noexcept
            {
                // Refuse the unclosed portal/interior branch before the
                // transaction mutates any camera or GPU state.
                if (!ViewerIsOutdoor())
                {
                    return false;
                }

                return saved_.Capture(
                    device_);
            }

            bool BindTarget() noexcept
            {
                return
                    BindReflectionTarget(
                        device_,
                        surface_,
                        depth_,
                        plan_.targetWidth,
                        plan_.targetHeight);
            }

            bool BuildReflectedCamera() noexcept
            {
                return
                    BuildCamera(
                        device_,
                        plan_.reflectedEye,
                        plan_.reflectedTarget);
            }

            bool LocateReflectedViewer() noexcept
            {
                // Mode 3 only, as required by Reflection-U.
                //
                // If the mirrored camera resolves into an interior/portal
                // group, this first backend has no authorised path and
                // therefore fails closed.
                return
                    LocateViewerOutdoor();
            }

            bool RenderSky() noexcept
            {
                const auto drawSky =
                    reinterpret_cast<
                        NoArgFn>(
                            kWholeSkyDraw);

                if (!drawSky)
                {
                    return false;
                }

                drawSky();

                return true;
            }

            bool RenderTerrain() noexcept
            {
                if (!CullOutdoor())
                {
                    return false;
                }

                const auto drawTerrain =
                    reinterpret_cast<
                        NoArgFn>(
                            adt::kRenderChunks);

                if (!drawTerrain)
                {
                    return false;
                }

                drawTerrain();

                return true;
            }

            bool RenderWmo() noexcept
            {
                const auto drawWmo =
                    reinterpret_cast<
                        NoArgFn>(
                            wmo::
                                kSceneRenderInstanceGroups);

                if (!drawWmo)
                {
                    return false;
                }

                drawWmo();

                return true;
            }

            bool RestoreOriginalCamera() noexcept
            {
                if (!BuildCamera(
                        device_,
                        plan_.originalEye,
                        plan_.originalTarget))
                {
                    return false;
                }

                // Mode 2 mutates the outdoor cull/list state but has no
                // ViewerLocate stage in the validated Reflection-U
                // sequence, so rebuild the original-camera cull state here.
                if (plan_.mode == 2u)
                {
                    return CullOutdoor();
                }

                return true;
            }

            bool LocateOriginalViewer() noexcept
            {
                // Mode 3 only.
                //
                // Restore the camera-driven viewer state first, then rebuild
                // the outdoor cull/list state for the main camera.
                if (!LocateViewerOutdoor())
                {
                    return false;
                }

                return CullOutdoor();
            }

            bool RestoreGpuState() noexcept
            {
                if (!saved_.Restore(
                        device_))
                {
                    return false;
                }

                // The D3D state block restores the original device matrices.
                // Refresh the shader system's cached projection afterwards,
                // matching the native world-render bracket.
                const auto refreshShaders =
                    reinterpret_cast<
                        gx::ShaderUpdateProjMatrixFn>(
                            gx::
                                kShaderUpdateProjMatrix);

                if (!refreshShaders)
                {
                    return false;
                }

                refreshShaders();

                return true;
            }

            void Quarantine() noexcept
            {
                // ExecuteReflectionSequence reports RestoreFailed after
                // this callback. Produce() owns portable lifecycle
                // invalidation immediately after the sequence returns.
                quarantine_ = true;
            }

        private:
            IDirect3DDevice9* device_ = nullptr;
            const ReflectionPlan& plan_;

            IDirect3DSurface9* surface_ = nullptr;
            IDirect3DSurface9* depth_ = nullptr;

            SavedGpuState saved_{};

            bool& quarantine_;
        };
    }

    Win32ReflectionRuntime::~Win32ReflectionRuntime()
    {
        Reset();
    }

    void Win32ReflectionRuntime::ReleaseResources() noexcept
    {
        ReleaseCom(depth_);
        ReleaseCom(surface_);
        ReleaseCom(texture_);

        device_ = nullptr;
        depthFormat_ = 0;
    }

    void Win32ReflectionRuntime::Reset() noexcept
    {
        ReleaseResources();

        lifecycle_.Reset();
        lastContent_ = {};

        quarantined_ = false;
    }

    void Win32ReflectionRuntime::ResetContentOnly() noexcept
    {
        lifecycle_.InvalidateContent();
        lastContent_ = {};
    }

    bool Win32ReflectionRuntime::EnsureResources(
        IDirect3DDevice9* device,
        const ReflectionPlan& plan) noexcept
    {
        if (!device ||
            quarantined_ ||
            !NativeReflectionPlanSupported(
                plan))
        {
            return false;
        }

        const ReflectionResourceKey key =
            ResourceKey(plan);

        IDirect3DSurface9*
            currentDepth = nullptr;

        if (FAILED(
                device->
                    GetDepthStencilSurface(
                        &currentDepth)) ||
            !currentDepth)
        {
            ReleaseCom(currentDepth);
            return false;
        }

        D3DSURFACE_DESC depthDesc{};

        const HRESULT depthDescResult =
            currentDepth->GetDesc(
                &depthDesc);

        ReleaseCom(currentDepth);

        if (FAILED(depthDescResult))
        {
            return false;
        }

        const unsigned depthFormat =
            static_cast<unsigned>(
                depthDesc.Format);

        if (device_ == device &&
            texture_ &&
            surface_ &&
            depth_ &&
            depthFormat_ == depthFormat &&
            lifecycle_.ResourcesMatch(key))
        {
            return true;
        }

        IDirect3DTexture9*
            newTexture = nullptr;

        IDirect3DSurface9*
            newSurface = nullptr;

        IDirect3DSurface9*
            newDepth = nullptr;

        const D3DFORMAT colourFormat =
            static_cast<D3DFORMAT>(
                plan.d3dColourFormat);

        if (FAILED(
                device->CreateTexture(
                    plan.targetWidth,
                    plan.targetHeight,
                    1,
                    D3DUSAGE_RENDERTARGET,
                    colourFormat,
                    D3DPOOL_DEFAULT,
                    &newTexture,
                    nullptr)) ||
            !newTexture)
        {
            ReleaseCom(newTexture);
            return false;
        }

        if (FAILED(
                newTexture->GetSurfaceLevel(
                    0,
                    &newSurface)) ||
            !newSurface)
        {
            ReleaseCom(newSurface);
            ReleaseCom(newTexture);
            return false;
        }

        // The colour target is a texture render target and therefore
        // non-MSAA.  Reuse the native world's proven depth FORMAT while
        // creating a size-compatible non-MSAA depth surface.  No depth
        // format is invented.
        if (FAILED(
                device->
                    CreateDepthStencilSurface(
                        plan.targetWidth,
                        plan.targetHeight,
                        depthDesc.Format,
                        D3DMULTISAMPLE_NONE,
                        0,
                        TRUE,
                        &newDepth,
                        nullptr)) ||
            !newDepth)
        {
            ReleaseCom(newDepth);
            ReleaseCom(newSurface);
            ReleaseCom(newTexture);
            return false;
        }

        ReleaseResources();

        device_ = device;

        texture_ = newTexture;
        surface_ = newSurface;
        depth_ = newDepth;

        depthFormat_ = depthFormat;

        lifecycle_.MarkResourcesReady(
            key);

        lastContent_ = {};

        return true;
    }

    ReflectionSequenceStatus
    Win32ReflectionRuntime::Produce(
        IDirect3DDevice9* device,
        const ReflectionPlan& plan,
        std::uint64_t producerOrdinal,
        bool& reflectionActive,
        NativeReflectionCandidate& out) noexcept
    {
        out = {};

        if (quarantined_ ||
            !NativeReflectionPlanSupported(
                plan))
        {
            return
                ReflectionSequenceStatus::
                    UnavailableRestored;
        }

        ReflectionContentKey
            content{};

        if (!BuildReflectionContentKey(
                plan,
                producerOrdinal,
                content))
        {
            return
                ReflectionSequenceStatus::
                    UnavailableRestored;
        }

        if (!EnsureResources(
                device,
                plan))
        {
            ResetContentOnly();

            return
                ReflectionSequenceStatus::
                    UnavailableRestored;
        }

        ReflectionGuard
            guard(reflectionActive);

        if (!guard.entered)
        {
            ResetContentOnly();

            return
                ReflectionSequenceStatus::
                    UnavailableRestored;
        }

        SequenceBackend backend(
            device,
            plan,
            surface_,
            depth_,
            quarantined_);

        const ReflectionSequenceStatus
            status =
                ExecuteReflectionSequence(
                    plan,
                    backend);

        if (status !=
            ReflectionSequenceStatus::
                Produced)
        {
            ResetContentOnly();

            if (status ==
                ReflectionSequenceStatus::
                    RestoreFailed)
            {
                quarantined_ = true;
            }

            return status;
        }

        if (!lifecycle_.MarkContentReady(
                content))
        {
            ResetContentOnly();

            return
                ReflectionSequenceStatus::
                    UnavailableRestored;
        }

        lastContent_ = content;

        out.texture = texture_;
        out.key = content;
        out.mode = plan.mode;
        out.ready = true;

        return
            ReflectionSequenceStatus::
                Produced;
    }
}

#endif
