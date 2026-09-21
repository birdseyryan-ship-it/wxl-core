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
#include <d3d9.h>

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

            WLOG_INFO(
                "wxl-modern-r5b2a: native shadow resource probe begin");

            for (unsigned cascade = 0; cascade < 3; ++cascade)
            {
                const uintptr_t record =
                    adt::kShadowTextureRecordBase +
                    cascade * adt::kShadowTextureRecordStride;

                const int activeSlot =
                    *reinterpret_cast<const int*>(
                        record +
                        adt::kShadowTextureActiveSlot);

                void* handles[
                    adt::kShadowTextureSelectableSlots
                ] = {};

                for (
                    size_t slot = 0;
                    slot < adt::kShadowTextureSelectableSlots;
                    ++slot)
                {
                    handles[slot] =
                        *reinterpret_cast<void* const*>(
                            record +
                            slot * sizeof(void*));
                }

                WLOG_INFO(
                    "wxl-modern-r5b2a: cascade=%u "
                    "record=0x%08X activeSlot=%d "
                    "handles=%p,%p,%p,%p",
                    cascade,
                    static_cast<unsigned>(record),
                    activeSlot,
                    handles[0],
                    handles[1],
                    handles[2],
                    handles[3]);

                if (
                    activeSlot < 0 ||
                    activeSlot >= static_cast<int>(
                        adt::kShadowTextureSelectableSlots))
                {
                    WLOG_WARN(
                        "wxl-modern-r5b2a: cascade=%u "
                        "invalid activeSlot=%d",
                        cascade,
                        activeSlot);

                    continue;
                }

                void* const handle =
                    handles[activeSlot];

                if (!handle || !resolve)
                {
                    WLOG_WARN(
                        "wxl-modern-r5b2a: cascade=%u "
                        "selected handle unavailable handle=%p "
                        "resolve=%p",
                        cascade,
                        handle,
                        resolve);

                    continue;
                }

                void* const raw =
                    resolve(
                        handle,
                        1,
                        0);

                if (!raw)
                {
                    WLOG_WARN(
                        "wxl-modern-r5b2a: cascade=%u "
                        "texture resolve returned null "
                        "handle=%p",
                        cascade,
                        handle);

                    continue;
                }

                auto* const texture =
                    static_cast<IDirect3DTexture9*>(
                        raw);

                D3DSURFACE_DESC desc = {};

                const HRESULT hr =
                    texture->GetLevelDesc(
                        0,
                        &desc);

                if (FAILED(hr))
                {
                    WLOG_WARN(
                        "wxl-modern-r5b2a: cascade=%u "
                        "GetLevelDesc failed hr=0x%08X "
                        "handle=%p raw=%p",
                        cascade,
                        static_cast<unsigned>(hr),
                        handle,
                        raw);

                    continue;
                }

                const unsigned format =
                    static_cast<unsigned>(
                        desc.Format);

                const char f0 =
                    static_cast<char>(
                        format & 0xFFu);

                const char f1 =
                    static_cast<char>(
                        (format >> 8) & 0xFFu);

                const char f2 =
                    static_cast<char>(
                        (format >> 16) & 0xFFu);

                const char f3 =
                    static_cast<char>(
                        (format >> 24) & 0xFFu);

                WLOG_INFO(
                    "wxl-modern-r5b2a: descriptor "
                    "cascade=%u handle=%p raw=%p "
                    "width=%u height=%u "
                    "format=0x%08X fourcc='%c%c%c%c' "
                    "usage=0x%08X pool=%u type=%u",
                    cascade,
                    handle,
                    raw,
                    static_cast<unsigned>(
                        desc.Width),
                    static_cast<unsigned>(
                        desc.Height),
                    format,
                    f0,
                    f1,
                    f2,
                    f3,
                    static_cast<unsigned>(
                        desc.Usage),
                    static_cast<unsigned>(
                        desc.Pool),
                    static_cast<unsigned>(
                        desc.Type));
            }

            WLOG_INFO(
                "wxl-modern-r5b2a: native shadow resource probe end");
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

            // R5B2-A: inspect the exact native texture backing the three
            // live Wrath cascades once. This is observation-only: no state,
            // resource or binding is changed.
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
