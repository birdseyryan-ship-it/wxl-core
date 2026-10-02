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
#include "water/WaterDiagCore.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wxl::r8::materials
{
    namespace
    {
        namespace diag = wxl::waterdiag;
        namespace m2   = wxl::offsets::game::m2;
        namespace sh   = wxl::offsets::engine::shader;
        namespace wmo  = wxl::offsets::game::wmo;

        constexpr std::uintptr_t kCanonicalImageBase = 0x00400000u;
        constexpr int kOuterAfterNativePriority = -500;

        struct Config
        {
            bool valid = true;
            bool master = false;
            bool wmo = false;
            bool m2 = false;
            bool wmoSelectorProof = false;
        };

        Config g_config{};

        m2::M2_SetupMaterialFn g_origM2SetupMaterial = nullptr;

        wmo::Wmo_RenderLeafFn g_origWmoExtRender = nullptr;
        wmo::Wmo_RenderLeafFn g_origWmoIntRender = nullptr;
        sh::EffectBindFn g_origWmoEffectBind = nullptr;

        thread_local unsigned g_wmoRenderDepth = 0;
        thread_local std::uintptr_t g_wmoRoot = 0;

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
            bool selectorProofValid = true;

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

            out.wmoSelectorProof =
                ReadBoolEnvironment(
                    "WXL_R8_WMO_SELECTOR_PROOF",
                    selectorProofValid);

            out.valid =
                masterValid &&
                wmoValid &&
                m2Valid &&
                selectorProofValid;

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

        struct WrapperSnapshot
        {
            std::uint32_t pointer = 0;
            std::uint32_t byteLength = 0;
            std::uint32_t version = 0;
            bool valid = false;
            diag::Digest sha{};
        };

        struct SelectorObservation
        {
            WmoFamily family = WmoFamily::Unknown;
            std::uint32_t vtxIdx = 0;
            std::uint32_t pixIdx = 0;
            std::uint32_t selectedVs = 0;
            std::uint32_t pairedRawVs = 0;
            std::uint32_t selectedPs = 0;
        };

        std::array<SelectorObservation, 64>
            g_selectorObservations{};

        std::size_t g_selectorObservationCount = 0;
        bool g_selectorLimitLogged = false;

        bool ReadClientMemory(
            std::uintptr_t address,
            void* destination,
            std::size_t bytes)
        {
            if (
                !address ||
                !destination ||
                bytes == 0)
            {
                return false;
            }

            SIZE_T copied = 0;

            return
                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(address),
                    destination,
                    bytes,
                    &copied) &&
                copied == bytes;
        }

        template<class T>
        bool ReadClientValue(
            std::uintptr_t address,
            T& value)
        {
            return
                ReadClientMemory(
                    address,
                    &value,
                    sizeof(value));
        }

        WrapperSnapshot SnapshotCollectionWrapper(
            std::uint32_t collection,
            std::size_t slotsOffset,
            std::uint32_t index,
            std::uint32_t slotCount)
        {
            WrapperSnapshot out{};

            if (
                !collection ||
                index >= slotCount)
            {
                return out;
            }

            std::uint32_t wrapper = 0;

            if (
                !ReadClientValue(
                    static_cast<std::uintptr_t>(collection) +
                        slotsOffset +
                        static_cast<std::uintptr_t>(index) *
                            sizeof(std::uint32_t),
                    wrapper) ||
                !wrapper)
            {
                return out;
            }

            out.pointer = wrapper;

            std::uint32_t length = 0;
            std::uint32_t bytecode = 0;

            if (
                !ReadClientValue(
                    static_cast<std::uintptr_t>(wrapper) +
                        sh::kCgxShaderByteLen,
                    length) ||
                !ReadClientValue(
                    static_cast<std::uintptr_t>(wrapper) +
                        sh::kCgxShaderBytePtr,
                    bytecode) ||
                !bytecode ||
                length < sizeof(std::uint32_t) ||
                length > 0x20000u)
            {
                return out;
            }

            std::vector<std::uint8_t> bytes(
                length);

            if (
                !ReadClientMemory(
                    bytecode,
                    bytes.data(),
                    bytes.size()))
            {
                return out;
            }

            std::memcpy(
                &out.version,
                bytes.data(),
                sizeof(out.version));

            out.byteLength = length;
            out.sha =
                diag::Sha256::Of(
                    bytes.data(),
                    bytes.size());

            out.valid = true;

            return out;
        }

        std::string SnapshotHash(
            const WrapperSnapshot& snapshot)
        {
            return
                snapshot.valid
                    ? diag::Hex(snapshot.sha)
                    : std::string("UNAVAILABLE");
        }

        std::string CurrentWmoPath()
        {
            if (!g_wmoRoot)
                return "UNKNOWN";

            std::array<char, 260> path{};

            if (
                !ReadClientMemory(
                    g_wmoRoot +
                        wmo::kOffNameInline,
                    path.data(),
                    path.size()))
            {
                return "UNAVAILABLE";
            }

            const auto end =
                std::find(
                    path.begin(),
                    path.end(),
                    '\0');

            if (end == path.end())
                return "UNTERMINATED";

            return
                std::string(
                    path.begin(),
                    end);
        }

        bool AdmitSelectorObservation(
            WmoFamily family,
            std::uint32_t vtxIdx,
            std::uint32_t pixIdx,
            std::uint32_t selectedVs,
            std::uint32_t pairedRawVs,
            std::uint32_t selectedPs)
        {
            for (
                std::size_t i = 0;
                i < g_selectorObservationCount;
                ++i)
            {
                const auto& existing =
                    g_selectorObservations[i];

                if (
                    existing.family == family &&
                    existing.vtxIdx == vtxIdx &&
                    existing.pixIdx == pixIdx &&
                    existing.selectedVs == selectedVs &&
                    existing.pairedRawVs == pairedRawVs &&
                    existing.selectedPs == selectedPs)
                {
                    return false;
                }
            }

            if (
                g_selectorObservationCount >=
                    g_selectorObservations.size())
            {
                if (!g_selectorLimitLogged)
                {
                    g_selectorLimitLogged = true;

                    WLOG_WARN(
                        "r8-step11-selector: unique observation "
                        "limit reached; proof logging capped");
                }

                return false;
            }

            g_selectorObservations[
                g_selectorObservationCount++] =
                SelectorObservation{
                    family,
                    vtxIdx,
                    pixIdx,
                    selectedVs,
                    pairedRawVs,
                    selectedPs};

            return true;
        }

        void LogWmoSelectorProof(
            WmoFamily family,
            std::uint32_t vtxIdx,
            std::uint32_t pixIdx)
        {
            std::uint32_t active = 0;

            if (
                !ReadClientValue(
                    sh::kActiveCollection,
                    active) ||
                !active)
            {
                return;
            }

            const std::uint32_t pairedRawVtxIdx =
                vtxIdx & ~1u;

            const WrapperSnapshot selectedVs =
                SnapshotCollectionWrapper(
                    active,
                    sh::kCollectionVtxSlots,
                    vtxIdx,
                    90);

            const WrapperSnapshot pairedRawVs =
                SnapshotCollectionWrapper(
                    active,
                    sh::kCollectionVtxSlots,
                    pairedRawVtxIdx,
                    90);

            const WrapperSnapshot selectedPs =
                SnapshotCollectionWrapper(
                    active,
                    sh::kCollectionPixSlots,
                    pixIdx,
                    16);

            if (
                !AdmitSelectorObservation(
                    family,
                    vtxIdx,
                    pixIdx,
                    selectedVs.pointer,
                    pairedRawVs.pointer,
                    selectedPs.pointer))
            {
                return;
            }

            const std::string selectedVsHash =
                SnapshotHash(selectedVs);

            const std::string pairedRawVsHash =
                SnapshotHash(pairedRawVs);

            const std::string selectedPsHash =
                SnapshotHash(selectedPs);

            const std::string path =
                CurrentWmoPath();

            WLOG_INFO(
                "r8-step11-selector: "
                "family=%s "
                "path=\"%s\" "
                "vtx=%u pix=%u "
                "light_bit=%u "
                "paired_raw_vtx=%u "
                "selected_vs=0x%08X "
                "selected_vs_len=%u "
                "selected_vs_ver=0x%08X "
                "selected_vs_sha=%s "
                "paired_vs=0x%08X "
                "paired_vs_len=%u "
                "paired_vs_ver=0x%08X "
                "paired_vs_sha=%s "
                "selected_ps=0x%08X "
                "selected_ps_len=%u "
                "selected_ps_ver=0x%08X "
                "selected_ps_sha=%s "
                "mutation=0",
                WmoFamilyName(family),
                path.c_str(),
                static_cast<unsigned>(vtxIdx),
                static_cast<unsigned>(pixIdx),
                static_cast<unsigned>(vtxIdx & 1u),
                static_cast<unsigned>(pairedRawVtxIdx),
                static_cast<unsigned>(selectedVs.pointer),
                static_cast<unsigned>(selectedVs.byteLength),
                static_cast<unsigned>(selectedVs.version),
                selectedVsHash.c_str(),
                static_cast<unsigned>(pairedRawVs.pointer),
                static_cast<unsigned>(pairedRawVs.byteLength),
                static_cast<unsigned>(pairedRawVs.version),
                pairedRawVsHash.c_str(),
                static_cast<unsigned>(selectedPs.pointer),
                static_cast<unsigned>(selectedPs.byteLength),
                static_cast<unsigned>(selectedPs.version),
                selectedPsHash.c_str());
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

            if (g_config.wmoSelectorProof)
            {
                LogWmoSelectorProof(
                    family,
                    vtxIdx,
                    pixIdx);
            }

            // The original 11B-01 witness remains useful and unchanged.
            LogFirstWmoCandidate(
                family);
        }

        struct WmoRenderScope
        {
            std::uintptr_t previousRoot = 0;

            explicit WmoRenderScope(
                void* root)
                : previousRoot(g_wmoRoot)
            {
                ++g_wmoRenderDepth;

                g_wmoRoot =
                    reinterpret_cast<std::uintptr_t>(
                        root);
            }

            ~WmoRenderScope()
            {
                g_wmoRoot =
                    previousRoot;

                --g_wmoRenderDepth;
            }
        };

        void __fastcall HookWmoExtRender(
            void* root,
            void* edx,
            void* group,
            int flag)
        {
            WmoRenderScope scope(root);

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
            WmoRenderScope scope(root);

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
                "r8-step11-material: structural substrate enabled "
                "master=1 wmo=%u m2=%u selector_proof=%u "
                "mutation=0 gx-device-draw-owner=0",
                g_config.wmo ? 1u : 0u,
                g_config.m2 ? 1u : 0u,
                g_config.wmoSelectorProof ? 1u : 0u);

            return true;
        }
    }

    WXL_REGISTER_FEATURE(
        "r8-step11-structural-material-substrate",
        true,
        InstallStructuralMaterialSubstrate)
}
