// R5 Classic 1.13.2 shadow backport foundation.
//
// R5B2 stages the extension-owned fourth Classic cascade incrementally.
// B1 allocates only the matching engine-managed 2048x2048 sidecar resource;
// it does not bind, render into, or sample that resource.
//
// Classic launch-build authority established by R5A10-R5A13:
//   * four dynamic 2048x2048 shadow bands
//   * half-extents 20 / 60 / 180 / 540
//   * fourth map must be extension-owned on the Wrath client
//
// Copyright (C) 2026 WarcraftXL
// GPLv3.

#include "common/Log.hpp"
#include "engine/events/EventScript.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "offsets/game/ADT.hpp"

#include <windows.h>

#include <cstdint>

namespace wxl::scripts::render_modern::shadows
{
    namespace adt = wxl::offsets::game::adt;
    namespace ev  = wxl::events;

    namespace
    {
        adt::RenderShadowCascadesFn g_origRenderShadowCascades = nullptr;

        struct ClassicCascade4Sidecar
        {
            void* resource = nullptr;
            void* gxObject = nullptr;
            bool allocationAttempted = false;

            // Exact launch-Classic fourth-band half extent established
            // by R5A12.  Matrix/caster use comes in later B2 tranches.
            float extent = 540.0f;
        };

        ClassicCascade4Sidecar g_classicCascade4;

        bool ClassicShadowsEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOWS",
                        raw,
                        sizeof(raw));

                if (count == 0 || count >= sizeof(raw))
                    return false;

                const char c = raw[0];

                return
                    c != '0' &&
                    c != 'n' && c != 'N' &&
                    c != 'f' && c != 'F';
            }();

            return enabled;
        }

        void LogNativeShadowResources()
        {
            static bool logged = false;

            if (logged)
                return;

            logged = true;

            const auto resolve =
                reinterpret_cast<adt::Map_TexResolveFn>(
                    adt::kTexResolve);

            const std::int32_t quality =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kEffectiveShadowQuality);

            const std::int32_t mapDimension =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowMapDimension);

            const std::int32_t shadowGroup =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowGroup);

            WLOG_INFO(
                "wxl-modern-r5b2a6: native shadow resource probe begin "
                "quality=%d mapDimension=%d shadowGroup=%d",
                static_cast<int>(quality),
                static_cast<int>(mapDimension),
                static_cast<int>(shadowGroup));

            for (unsigned cascade = 0; cascade < 3; ++cascade)
            {
                const uintptr_t record =
                    adt::kShadowTextureRecordBase +
                    cascade * adt::kShadowTextureRecordStride;

                void* const resourceA =
                    *reinterpret_cast<void* const*>(
                        record +
                        adt::kShadowTextureResourceA);

                void* const resourceB =
                    *reinterpret_cast<void* const*>(
                        record +
                        adt::kShadowTextureResourceB);

                const float extent =
                    *reinterpret_cast<const float*>(
                        record +
                        adt::kShadowCascadeExtent);

                const float recenterSq =
                    *reinterpret_cast<const float*>(
                        record +
                        adt::kShadowCascadeRecenterSq);

                const std::int32_t selector =
                    *reinterpret_cast<const std::int32_t*>(
                        record +
                        adt::kShadowTextureSelector);

                WLOG_INFO(
                    "wxl-modern-r5b2a6: cascade=%u "
                    "record=0x%08X selector=%d "
                    "resourceA=%p resourceB=%p "
                    "extent=%.9g recenterSq=%.9g",
                    cascade,
                    static_cast<unsigned>(record),
                    static_cast<int>(selector),
                    resourceA,
                    resourceB,
                    static_cast<double>(extent),
                    static_cast<double>(recenterSq));

                if (
                    selector < 0 ||
                    selector >= static_cast<std::int32_t>(
                        adt::kShadowTextureResourceSlots))
                {
                    WLOG_WARN(
                        "wxl-modern-r5b2a6: cascade=%u "
                        "invalid native resource selector=%d",
                        cascade,
                        static_cast<int>(selector));

                    continue;
                }

                void* const selected =
                    selector == 0
                        ? resourceA
                        : resourceB;

                if (!selected)
                {
                    WLOG_WARN(
                        "wxl-modern-r5b2a6: cascade=%u "
                        "selected native resource is null "
                        "selector=%d",
                        cascade,
                        static_cast<int>(selector));

                    continue;
                }

                const auto* const bytes =
                    static_cast<const std::uint8_t*>(
                        selected);

                void* const embeddedGxObject =
                    *reinterpret_cast<void* const*>(
                        bytes +
                        adt::kTextureHandleGxObject);

                const std::uint32_t createArg1 =
                    *reinterpret_cast<const std::uint32_t*>(
                        bytes +
                        adt::kTextureHandleCreateArg1);

                const std::uint16_t width =
                    *reinterpret_cast<const std::uint16_t*>(
                        bytes +
                        adt::kTextureHandleWidth);

                const std::uint16_t height =
                    *reinterpret_cast<const std::uint16_t*>(
                        bytes +
                        adt::kTextureHandleHeight);

                const std::uint32_t createArg5 =
                    *reinterpret_cast<const std::uint32_t*>(
                        bytes +
                        adt::kTextureHandleCreateArg5);

                const std::uint32_t createArg6 =
                    *reinterpret_cast<const std::uint32_t*>(
                        bytes +
                        adt::kTextureHandleCreateArg6);

                const std::uint32_t createFlags =
                    *reinterpret_cast<const std::uint32_t*>(
                        bytes +
                        adt::kTextureHandleCreateFlags);

                void* resolvedGxObject = nullptr;

                if (resolve)
                {
                    resolvedGxObject =
                        resolve(
                            selected,
                            1,
                            0);
                }

                WLOG_INFO(
                    "wxl-modern-r5b2a6: metadata "
                    "cascade=%u selected=%p "
                    "width=%u height=%u "
                    "createArg1=0x%08X "
                    "createArg5=0x%08X createArg6=0x%08X "
                    "createFlags=0x%08X "
                    "embeddedGx=%p resolvedGx=%p match=%u",
                    cascade,
                    selected,
                    static_cast<unsigned>(width),
                    static_cast<unsigned>(height),
                    static_cast<unsigned>(createArg1),
                    static_cast<unsigned>(createArg5),
                    static_cast<unsigned>(createArg6),
                    static_cast<unsigned>(createFlags),
                    embeddedGxObject,
                    resolvedGxObject,
                    embeddedGxObject == resolvedGxObject
                        ? 1u
                        : 0u);
            }

            WLOG_INFO(
                "wxl-modern-r5b2a6: native shadow resource probe end");
        }

        void ReleaseClassicCascade4Resource(
            const char* reason)
        {
            void* const resource =
                g_classicCascade4.resource;

            g_classicCascade4.resource = nullptr;
            g_classicCascade4.gxObject = nullptr;

            // A later valid Tier-5 frame may recreate the sidecar.
            g_classicCascade4.allocationAttempted = false;

            if (!resource)
                return;

            const auto release =
                reinterpret_cast<adt::TextureReleaseFn>(
                    adt::kTextureRelease);

            if (!release)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2b1: cascade4 release unavailable "
                    "resource=%p reason=%s",
                    resource,
                    reason ? reason : "unknown");

                return;
            }

            release(resource);

            WLOG_INFO(
                "wxl-modern-r5b2b1: cascade4 sidecar released "
                "resource=%p reason=%s",
                resource,
                reason ? reason : "unknown");
        }

        void EnsureClassicCascade4Resource()
        {
            if (
                g_classicCascade4.resource ||
                g_classicCascade4.allocationAttempted)
            {
                return;
            }

            const std::int32_t quality =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kEffectiveShadowQuality);

            const std::int32_t mapDimension =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowMapDimension);

            if (
                quality != 5 ||
                mapDimension != 2048)
            {
                return;
            }

            const uintptr_t nativeRecord =
                adt::kShadowTextureRecordBase;

            void* const nativeResourceA =
                *reinterpret_cast<void* const*>(
                    nativeRecord +
                    adt::kShadowTextureResourceA);

            void* const nativeResourceB =
                *reinterpret_cast<void* const*>(
                    nativeRecord +
                    adt::kShadowTextureResourceB);

            if (
                !nativeResourceA ||
                nativeResourceB)
            {
                return;
            }

            const auto* const nativeBytes =
                static_cast<const std::uint8_t*>(
                    nativeResourceA);

            const std::uint32_t createArg1 =
                *reinterpret_cast<const std::uint32_t*>(
                    nativeBytes +
                    adt::kTextureHandleCreateArg1);

            const std::uint16_t width =
                *reinterpret_cast<const std::uint16_t*>(
                    nativeBytes +
                    adt::kTextureHandleWidth);

            const std::uint16_t height =
                *reinterpret_cast<const std::uint16_t*>(
                    nativeBytes +
                    adt::kTextureHandleHeight);

            const std::uint32_t createArg5 =
                *reinterpret_cast<const std::uint32_t*>(
                    nativeBytes +
                    adt::kTextureHandleCreateArg5);

            const std::uint32_t createArg6 =
                *reinterpret_cast<const std::uint32_t*>(
                    nativeBytes +
                    adt::kTextureHandleCreateArg6);

            const std::uint32_t createFlags =
                *reinterpret_cast<const std::uint32_t*>(
                    nativeBytes +
                    adt::kTextureHandleCreateFlags);

            // R5B2-A9 exact accepted runtime contract.  Refuse to allocate
            // rather than guessing if the live native path differs.
            if (
                createArg1 != 0 ||
                width != 2048 ||
                height != 2048 ||
                createArg5 != 0x0C ||
                createArg6 != 0x0C ||
                createFlags != 0x00000281)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2b1: native creation contract "
                    "unexpected arg1=0x%08X size=%ux%u "
                    "arg5=0x%08X arg6=0x%08X flags=0x%08X",
                    static_cast<unsigned>(createArg1),
                    static_cast<unsigned>(width),
                    static_cast<unsigned>(height),
                    static_cast<unsigned>(createArg5),
                    static_cast<unsigned>(createArg6),
                    static_cast<unsigned>(createFlags));

                g_classicCascade4.allocationAttempted = true;
                return;
            }

            const auto create =
                reinterpret_cast<adt::Map_CreateTextureHandleFn>(
                    adt::kCreateTextureHandle);

            const auto resolve =
                reinterpret_cast<adt::Map_TexResolveFn>(
                    adt::kTexResolve);

            if (!create || !resolve)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2b1: creator unavailable "
                    "create=%p resolve=%p",
                    create,
                    resolve);

                g_classicCascade4.allocationAttempted = true;
                return;
            }

            g_classicCascade4.allocationAttempted = true;

            void* const resource =
                create(
                    createArg1,
                    static_cast<std::uint32_t>(width),
                    static_cast<std::uint32_t>(height),
                    0,
                    createArg5,
                    createArg6,
                    createFlags,
                    0,
                    adt::kShadowTextureCreateOpaqueArg9,
                    adt::kShadowTextureCreateOpaqueArg10,
                    0);

            if (!resource)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2b1: cascade4 resource "
                    "allocation returned null");
                return;
            }

            const auto* const bytes =
                static_cast<const std::uint8_t*>(
                    resource);

            void* const embeddedGx =
                *reinterpret_cast<void* const*>(
                    bytes +
                    adt::kTextureHandleGxObject);

            const std::uint16_t actualWidth =
                *reinterpret_cast<const std::uint16_t*>(
                    bytes +
                    adt::kTextureHandleWidth);

            const std::uint16_t actualHeight =
                *reinterpret_cast<const std::uint16_t*>(
                    bytes +
                    adt::kTextureHandleHeight);

            const std::uint32_t actualArg1 =
                *reinterpret_cast<const std::uint32_t*>(
                    bytes +
                    adt::kTextureHandleCreateArg1);

            const std::uint32_t actualArg5 =
                *reinterpret_cast<const std::uint32_t*>(
                    bytes +
                    adt::kTextureHandleCreateArg5);

            const std::uint32_t actualArg6 =
                *reinterpret_cast<const std::uint32_t*>(
                    bytes +
                    adt::kTextureHandleCreateArg6);

            const std::uint32_t actualFlags =
                *reinterpret_cast<const std::uint32_t*>(
                    bytes +
                    adt::kTextureHandleCreateFlags);

            void* const resolvedGx =
                resolve(
                    resource,
                    1,
                    0);

            const bool contractMatch =
                actualArg1 == createArg1 &&
                actualWidth == width &&
                actualHeight == height &&
                actualArg5 == createArg5 &&
                actualArg6 == createArg6 &&
                actualFlags == createFlags &&
                embeddedGx &&
                resolvedGx &&
                embeddedGx == resolvedGx;

            if (!contractMatch)
            {
                const auto release =
                    reinterpret_cast<adt::TextureReleaseFn>(
                        adt::kTextureRelease);

                WLOG_WARN(
                    "wxl-modern-r5b2b1: cascade4 returned contract "
                    "mismatch resource=%p gx=%p embeddedGx=%p "
                    "size=%ux%u arg1=0x%08X "
                    "arg5=0x%08X arg6=0x%08X flags=0x%08X",
                    resource,
                    resolvedGx,
                    embeddedGx,
                    static_cast<unsigned>(actualWidth),
                    static_cast<unsigned>(actualHeight),
                    static_cast<unsigned>(actualArg1),
                    static_cast<unsigned>(actualArg5),
                    static_cast<unsigned>(actualArg6),
                    static_cast<unsigned>(actualFlags));

                if (release)
                    release(resource);

                return;
            }

            g_classicCascade4.resource = resource;
            g_classicCascade4.gxObject = resolvedGx;

            WLOG_INFO(
                "wxl-modern-r5b2b1: cascade4 sidecar allocated "
                "resource=%p gx=%p embeddedGx=%p "
                "size=%ux%u arg1=0x%08X "
                "arg5=0x%08X arg6=0x%08X flags=0x%08X "
                "extent=%.9g contractMatch=%u "
                "renderBound=0 casterPass=0 receiverBound=0",
                resource,
                resolvedGx,
                embeddedGx,
                static_cast<unsigned>(actualWidth),
                static_cast<unsigned>(actualHeight),
                static_cast<unsigned>(actualArg1),
                static_cast<unsigned>(actualArg5),
                static_cast<unsigned>(actualArg6),
                static_cast<unsigned>(actualFlags),
                static_cast<double>(
                    g_classicCascade4.extent),
                contractMatch ? 1u : 0u);
        }

        void __cdecl hkRenderShadowCascades(void* shadowObject)
        {
            if (g_origRenderShadowCascades)
                g_origRenderShadowCascades(shadowObject);

            // R5B1-A is observation-only.
            //
            // This exact post-call position is where the extension-owned
            // fourth pass will be inserted in the next tranche.  Do not
            // modify the native object's active index and do not invoke
            // the stock callback with cascade index 3.
            if (!ClassicShadowsEnabled() || !shadowObject)
                return;

            // R5B2-A6: read only native record and engine texture-handle
            // metadata proven by the Wrath executable. No COM calls are made
            // and no render/resource state is changed.
            LogNativeShadowResources();

            // R5B2-B1 allocation-only sidecar staging.  This creates one
            // engine-managed fourth resource using the exact proven native
            // Tier-5 contract.  It is deliberately not bound, rendered into,
            // or exposed to a receiver yet.
            EnsureClassicCascade4Resource();

            static unsigned logged = 0;

            if (logged >= 12)
                return;

            ++logged;

            const auto* bytes =
                static_cast<const std::uint8_t*>(
                    shadowObject);

            const std::int32_t activeIndex =
                *reinterpret_cast<const std::int32_t*>(
                    bytes +
                    adt::kShadowActiveCascadeIndex);

            const std::int32_t state =
                *reinterpret_cast<const std::int32_t*>(
                    bytes +
                    adt::kShadowStateField);

            const std::int32_t mapDimension =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowMapDimension);

            const std::int32_t quality =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kEffectiveShadowQuality);

            const auto callback =
                *reinterpret_cast<void* const*>(
                    adt::kShadowRenderCallbackPtr);

            WLOG_INFO(
                "wxl-modern-r5b1a: post-native shadows "
                "object=%p activeIndex=%d state=%d "
                "mapDimension=%d quality=%d callback=%p "
                "classicEnabled=1",
                shadowObject,
                static_cast<int>(activeIndex),
                static_cast<int>(state),
                static_cast<int>(mapDimension),
                static_cast<int>(quality),
                callback);
        }

        bool InstallClassicShadowFoundation()
        {
            const bool installed =
                wxl::hook::Install(
                    "R5ClassicShadowRenderDispatcher",
                    adt::kRenderShadowCascades,
                    &hkRenderShadowCascades,
                    &g_origRenderShadowCascades);

            if (installed)
            {
                WLOG_INFO(
                    "wxl-modern-r5b1a: Classic shadow foundation installed "
                    "enabled=%u dispatcher=0x%08X callback=0x%08X",
                    ClassicShadowsEnabled() ? 1u : 0u,
                    static_cast<unsigned>(
                        adt::kRenderShadowCascades),
                    static_cast<unsigned>(
                        adt::kShadowCascadeRenderCallback));
            }

            return installed;
        }
    }

    class ClassicShadowSidecarLifecycle
        : public ev::EventScript
    {
    public:
        ClassicShadowSidecarLifecycle()
        {
            on<&ClassicShadowSidecarLifecycle::OnDeviceLost>(
                ev::Event::OnDeviceLost);

            on<&ClassicShadowSidecarLifecycle::OnDeviceReset>(
                ev::Event::OnDeviceReset);
        }

    private:
        void OnDeviceLost(
            const ev::DeviceResetArgs&)
        {
            if (!ClassicShadowsEnabled())
                return;

            ReleaseClassicCascade4Resource(
                "device-lost");
        }

        void OnDeviceReset(
            const ev::DeviceResetArgs&)
        {
            if (!ClassicShadowsEnabled())
                return;

            // Ensure a failed/old attempt cannot suppress recreation
            // on the first valid post-reset Tier-5 shadow frame.
            g_classicCascade4.resource = nullptr;
            g_classicCascade4.gxObject = nullptr;
            g_classicCascade4.allocationAttempted = false;

            WLOG_INFO(
                "wxl-modern-r5b2b1: cascade4 sidecar "
                "rearmed after device reset");
        }
    };

    ClassicShadowSidecarLifecycle
        g_classicShadowSidecarLifecycle;

    WXL_REGISTER_FEATURE(
        "render-modern-r5-classic-shadow-foundation",
        true,
        InstallClassicShadowFoundation);
}
