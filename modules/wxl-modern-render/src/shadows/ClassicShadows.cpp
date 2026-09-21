// R5 Classic 1.13.2 shadow backport foundation.
//
// This tranche deliberately changes no rendering.  It validates the exact
// post-native-cascade insertion seam before any fourth-map resources exist.
//
// Classic launch-build authority established by R5A10-R5A13:
//   * four dynamic 2048x2048 shadow bands
//   * half-extents 20 / 60 / 180 / 540
//   * fourth map must be extension-owned on the Wrath client
//
// Copyright (C) 2026 WarcraftXL
// GPLv3.

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "offsets/game/ADT.hpp"

#include <windows.h>

#include <cstdint>

namespace wxl::scripts::render_modern::shadows
{
    namespace adt = wxl::offsets::game::adt;

    namespace
    {
        adt::RenderShadowCascadesFn g_origRenderShadowCascades = nullptr;

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

    WXL_REGISTER_FEATURE(
        "render-modern-r5-classic-shadow-foundation",
        true,
        InstallClassicShadowFoundation);
}
