// R8 Step 11 structural-material substitution substrate.
//
// 11B-01 intentionally performs NO shader substitution and NO D3D mutation.
// It establishes independently switchable, default-OFF ownership seams only.
//
// WMO:
//   ExtRender / IntRender scope the real WMO render path.
//   EffectBind is observed AFTER the native/effect chain has completed.
//   Only positively identified stock families are candidates.
//
// M2:
//   SetupMaterial is observed AFTER the native/R4 chain has completed.
//   No GxDeviceDraw ownership assumption is used.
//
// Unknown routes always remain native/stock.
//
// GPL-3.0-or-later.

#include "client/CWorldScene/StructuralMaterialPolicy.hpp"

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "offsets/engine/Shader.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/WMO.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace wxl::r8::materials
{
    namespace
    {
        namespace m2  = wxl::offsets::game::m2;
        namespace sh  = wxl::offsets::engine::shader;
        namespace wmo = wxl::offsets::game::wmo;

        constexpr std::uintptr_t kCanonicalImageBase = 0x00400000u;
        constexpr int kOuterAfterNativePriority = -500;

        struct Config
        {
            bool valid = true;
            bool master = false;
            bool wmo = false;
            bool m2 = false;
        };

        Config g_config{};

        m2::M2_SetupMaterialFn g_origM2SetupMaterial = nullptr;

        wmo::Wmo_RenderLeafFn g_origWmoExtRender = nullptr;
        wmo::Wmo_RenderLeafFn g_origWmoIntRender = nullptr;
        sh::EffectBindFn g_origWmoEffectBind = nullptr;

        thread_local unsigned g_wmoRenderDepth = 0;

        std::atomic<bool> g_loggedM2Seam{false};
        std::atomic<bool> g_loggedWmoDiffuse{false};
        std::atomic<bool> g_loggedWmoOpaque{false};
        std::atomic<bool> g_loggedWmoSpecular{false};

        bool ReadBoolEnvironment(const char* name, bool& valid)
        {
            const char* raw = std::getenv(name);

            if (!raw)
                return false;

            std::string text(raw);

            std::transform(
                text.begin(),
                text.end(),
                text.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });

            if (
                text == "1" ||
                text == "true" ||
                text == "yes" ||
                text == "on")
            {
                return true;
            }

            if (
                text == "0" ||
                text == "false" ||
                text == "no" ||
                text == "off")
            {
                return false;
            }

            valid = false;

            WLOG_WARN(
                "r8-step11-material: invalid boolean %s='%s'; "
                "structural material candidate remains OFF",
                name,
                raw);

            return false;
        }

        Config ReadConfig()
        {
            Config out{};

            bool masterValid = true;
            bool wmoValid = true;
            bool m2Valid = true;

            out.master =
                ReadBoolEnvironment(
                    "WXL_R8_STRUCTURAL_MATERIALS",
                    masterValid);

            out.wmo =
                ReadBoolEnvironment(
                    "WXL_R8_WMO_MATERIALS",
                    wmoValid);

            out.m2 =
                ReadBoolEnvironment(
                    "WXL_R8_M2_MATERIALS",
                    m2Valid);

            out.valid =
                masterValid &&
                wmoValid &&
                m2Valid;

            return out;
        }

        bool CanonicalImageBase()
        {
            return
                reinterpret_cast<std::uintptr_t>(
                    GetModuleHandleW(nullptr)) ==
                kCanonicalImageBase;
        }

        const char* WmoFamilyName(WmoFamily family)
        {
            switch (family)
            {
                case WmoFamily::Diffuse:
                    return "Diffuse";
                case WmoFamily::Specular:
                    return "Specular";
                case WmoFamily::Metal:
                    return "Metal";
                case WmoFamily::Env:
                    return "Env";
                case WmoFamily::Opaque:
                    return "Opaque";
                case WmoFamily::EnvMetal:
                    return "EnvMetal";
                case WmoFamily::Composite:
                    return "Composite";
                default:
                    return "UNKNOWN";
            }
        }

        WmoFamily CurrentWmoFamily()
        {
            const std::uintptr_t active =
                *reinterpret_cast<const std::uintptr_t*>(
                    sh::kActiveCollection);

            if (!active)
                return WmoFamily::Unknown;

            for (int i = 0; i < 7; ++i)
            {
                const std::uintptr_t exterior =
                    *reinterpret_cast<const std::uintptr_t*>(
                        sh::kExteriorEffectTable +
                        static_cast<std::uintptr_t>(i) *
                            sizeof(std::uint32_t));

                const std::uintptr_t alternate =
                    *reinterpret_cast<const std::uintptr_t*>(
                        sh::kAltEffectTable +
                        static_cast<std::uintptr_t>(i) *
                            sizeof(std::uint32_t));

                if (
                    active == exterior ||
                    active == alternate)
                {
                    return static_cast<WmoFamily>(i);
                }
            }

            return WmoFamily::Unknown;
        }

        void LogFirstWmoCandidate(WmoFamily family)
        {
            std::atomic<bool>* latch = nullptr;

            switch (family)
            {
                case WmoFamily::Diffuse:
                    latch = &g_loggedWmoDiffuse;
                    break;

                case WmoFamily::Opaque:
                    latch = &g_loggedWmoOpaque;
                    break;

                case WmoFamily::Specular:
                    latch = &g_loggedWmoSpecular;
                    break;

                default:
                    return;
            }

            if (!latch->exchange(true))
            {
                WLOG_INFO(
                    "r8-step11-material: WMO dry-run seam reached "
                    "family=%s; substitution=OFF",
                    WmoFamilyName(family));
            }
        }

        void __fastcall HookM2SetupMaterial(
            void* renderCtx,
            void* edx)
        {
            // Run the complete downstream/native/R4 chain first.
            g_origM2SetupMaterial(
                renderCtx,
                edx);

            if (!g_config.m2)
                return;

            // 11B-01 deliberately does not infer shader ownership from this
            // call alone and performs no D3D mutation.
            if (!g_loggedM2Seam.exchange(true))
            {
                WLOG_INFO(
                    "r8-step11-material: M2 post-native material seam reached; "
                    "substitution=OFF; selector contract pending 11B-02");
            }
        }

        void __cdecl HookWmoEffectBind(
            std::uint32_t vtxIdx,
            std::uint32_t pixIdx)
        {
            // The native binder also advances fog/alpha state. Preserve all of
            // that first; any eventual Step-11 substitution belongs after it.
            g_origWmoEffectBind(
                vtxIdx,
                pixIdx);

            if (
                !g_config.wmo ||
                g_wmoRenderDepth == 0)
            {
                return;
            }

            const WmoFamily family =
                CurrentWmoFamily();

            if (!IsInitialWmoCandidate(family))
                return;

            // 11B-01 is intentionally a dry-run ownership proof.
            LogFirstWmoCandidate(
                family);
        }

        struct WmoRenderScope
        {
            WmoRenderScope()
            {
                ++g_wmoRenderDepth;
            }

            ~WmoRenderScope()
            {
                --g_wmoRenderDepth;
            }
        };

        void __fastcall HookWmoExtRender(
            void* root,
            void* edx,
            void* group,
            int flag)
        {
            WmoRenderScope scope;

            g_origWmoExtRender(
                root,
                edx,
                group,
                flag);
        }

        void __fastcall HookWmoIntRender(
            void* root,
            void* edx,
            void* group,
            int flag)
        {
            WmoRenderScope scope;

            g_origWmoIntRender(
                root,
                edx,
                group,
                flag);
        }

        bool InstallWmoSubstrate()
        {
            bool ok = true;

            ok &=
                wxl::hook::Install(
                    "R8Step11WmoExtScope",
                    wmo::kExtRender,
                    &HookWmoExtRender,
                    &g_origWmoExtRender,
                    kOuterAfterNativePriority);

            ok &=
                wxl::hook::Install(
                    "R8Step11WmoIntScope",
                    wmo::kIntRender,
                    &HookWmoIntRender,
                    &g_origWmoIntRender,
                    kOuterAfterNativePriority);

            ok &=
                wxl::hook::Install(
                    "R8Step11WmoEffectBind",
                    sh::kEffectBind,
                    &HookWmoEffectBind,
                    &g_origWmoEffectBind,
                    kOuterAfterNativePriority);

            return ok;
        }

        bool InstallM2Substrate()
        {
            return
                wxl::hook::Install(
                    "R8Step11M2Material",
                    m2::kSetupMaterial,
                    &HookM2SetupMaterial,
                    &g_origM2SetupMaterial,
                    kOuterAfterNativePriority);
        }

        bool InstallStructuralMaterialSubstrate()
        {
            g_config = ReadConfig();

            if (!g_config.valid)
            {
                WLOG_WARN(
                    "r8-step11-material: configuration invalid; "
                    "candidate OFF and no hooks installed");

                return true;
            }

            // Critical benchmark property:
            // ordinary production launch options do not install any 11B hooks.
            if (!g_config.master)
                return true;

            if (
                !g_config.wmo &&
                !g_config.m2)
            {
                WLOG_WARN(
                    "r8-step11-material: master enabled but both routes are OFF; "
                    "no hooks installed");

                return true;
            }

            if (!CanonicalImageBase())
            {
                WLOG_ERROR(
                    "r8-step11-material: non-canonical image base; "
                    "candidate OFF and no hooks installed");

                return true;
            }

            bool ok = true;

            if (g_config.wmo)
                ok &= InstallWmoSubstrate();

            if (g_config.m2)
                ok &= InstallM2Substrate();

            if (!ok)
            {
                WLOG_ERROR(
                    "r8-step11-material: hook registration incomplete; "
                    "11B-01 qualification must fail");

                return false;
            }

            WLOG_INFO(
                "r8-step11-material: 11B-01 dry-run substrate enabled "
                "master=1 wmo=%u m2=%u mutation=0 "
                "gx-device-draw-owner=0",
                g_config.wmo ? 1u : 0u,
                g_config.m2 ? 1u : 0u);

            return true;
        }
    }

    WXL_REGISTER_FEATURE(
        "r8-step11-structural-material-substrate",
        true,
        InstallStructuralMaterialSubstrate)
}
