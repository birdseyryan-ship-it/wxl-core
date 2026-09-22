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
#include "game/Gx.hpp"
#include "offsets/engine/Shader.hpp"
#include "offsets/game/ADT.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::scripts::render_modern::shadows
{
    namespace adt   = wxl::offsets::game::adt;
    namespace ev    = wxl::events;
    namespace shoff = wxl::offsets::engine::shader;

    namespace
    {
        adt::RenderShadowCascadesFn g_origRenderShadowCascades = nullptr;

        using BuildShadowCascadesFn =
            void(__cdecl*)(
                void* shadowObject,
                void* context,
                void* helper,
                std::int32_t mode);

        BuildShadowCascadesFn
            g_origBuildShadowCascades = nullptr;

        bool g_classicGeometryBuildHookInstalled = false;

        adt::BindTerrainShadowMapFn
            g_origBindTerrainShadowMap = nullptr;

        adt::BindTerrainShadowMapFn
            g_origBindTerrainShadowMapAlt = nullptr;

        bool g_classicReceiverHooksInstalled = false;

        struct ClassicCascade4Sidecar
        {
            void* resource = nullptr;
            void* gxObject = nullptr;
            bool allocationAttempted = false;
            bool casterProofReturned = false;

            // The extension-owned resource now carries the logical
            // Classic third dynamic band (180). Native slot 2 remains
            // the far 540 band because live movement testing proved that
            // configuration stable while native slot-2=180 visibly pops.
            float matrix[16] = {};
            bool matrixValid = false;
            std::uint64_t geometryFrames = 0;

            float extent = 180.0f;
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

        bool ClassicShadowCasterProofEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_CASTER_PROOF",
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

        bool ClassicShadowMatrixProofEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_MATRIX_PROOF",
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

        bool ClassicShadowJoinedProofEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_JOINED_PROOF",
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

        bool ClassicShadowClone540ProofEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_CLONE540_PROOF",
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

        bool ClassicShadowGeometryProofEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_GEOMETRY_PROOF",
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

        bool ClassicShadowKeepNativeFarDiagnosticEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_KEEP_NATIVE_FAR_DIAG",
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

        bool ClassicShadowNative540DiagnosticEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_NATIVE_540_DIAG",
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

        bool ClassicShadowReceiverBindProofEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_RECEIVER_BIND_PROOF",
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
            g_classicCascade4.casterProofReturned = false;

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

        void TrySyntheticCascade4MatrixProof()
        {
            static bool attempted = false;

            if (
                !ClassicShadowMatrixProofEnabled() ||
                attempted)
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

            const auto constructor =
                reinterpret_cast<
                    adt::ShadowObjectConstructorFn>(
                    adt::kShadowObjectConstructor);

            const auto builder =
                reinterpret_cast<
                    adt::ShadowCascadeBuildCallbackFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowCascadeBuildCallbackPtr));

            if (
                !constructor ||
                !builder ||
                reinterpret_cast<uintptr_t>(builder) !=
                    adt::kShadowCascadeBuildCallback)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2e1: synthetic matrix proof refused "
                    "constructor=%p builder=%p expectedBuilder=0x%08X",
                    reinterpret_cast<void*>(constructor),
                    reinterpret_cast<void*>(builder),
                    static_cast<unsigned>(
                        adt::kShadowCascadeBuildCallback));

                return;
            }

            alignas(16)
            std::uint8_t synthetic[
                adt::kShadowObjectSize] = {};

            void* const syntheticObject =
                static_cast<void*>(synthetic);

            void* const constructed =
                constructor(
                    syntheticObject,
                    nullptr);

            if (constructed != syntheticObject)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2e1: synthetic matrix proof refused "
                    "constructor returned=%p expected=%p",
                    constructed,
                    syntheticObject);

                return;
            }

            const float extent =
                g_classicCascade4.extent;

            auto writeFloat =
                [&synthetic](
                    std::size_t offset,
                    float value)
                {
                    *reinterpret_cast<float*>(
                        synthetic + offset) =
                            value;
                };

            // Exact 0x875F80 pre-builder layout.
            writeFloat(
                adt::kShadowSyntheticExtent,
                extent);

            // Exact x87 store order at 0x8760D6..0x8760FA:
            // {-extent,+extent,-extent,+extent}.
            writeFloat(
                adt::kShadowSyntheticBound0,
                -extent);

            writeFloat(
                adt::kShadowSyntheticBound1,
                extent);

            writeFloat(
                adt::kShadowSyntheticBound2,
                -extent);

            writeFloat(
                adt::kShadowSyntheticBound3,
                extent);

            // Exact 0x876103..0x876117 seed values.
            writeFloat(
                adt::kShadowSyntheticSeed0,
                0.0f);

            writeFloat(
                adt::kShadowSyntheticSeed1,
                1.0f);

            writeFloat(
                adt::kShadowSyntheticSeed2,
                0.0f);

            writeFloat(
                adt::kShadowSyntheticSeed3,
                1.0f);

            void* const cascadeRecord =
                synthetic +
                adt::kShadowCascadeRecordBase;

            float* const matrix =
                reinterpret_cast<float*>(
                    synthetic +
                    adt::kShadowCascadeMatrixBase);

            void* const builderVector =
                reinterpret_cast<void*>(
                    adt::kShadowSyntheticBuilderVector);

            attempted = true;

            WLOG_INFO(
                "wxl-modern-r5b2e1: synthetic matrix build begin "
                "object=%p record=%p matrix=%p "
                "builder=%p builderSlot=0 "
                "extent=%.9g mapDimension=%d "
                "bounds=%.9g/%.9g/%.9g/%.9g "
                "seeds=0/1/0/1 "
                "finalizerCalled=0 casterPass=0 "
                "renderBound=0 receiverBound=0",
                syntheticObject,
                cascadeRecord,
                matrix,
                reinterpret_cast<void*>(builder),
                static_cast<double>(extent),
                static_cast<int>(mapDimension),
                static_cast<double>(-extent),
                static_cast<double>(extent),
                static_cast<double>(-extent),
                static_cast<double>(extent));

            // Exact legal native synthetic call shape:
            //
            //   push 0
            //   push syntheticObject
            //   push 0xD43278
            //   push syntheticObject+0x9C4
            //   push syntheticObject+0x6C
            //   call [0xD4315C]
            builder(
                cascadeRecord,
                matrix,
                builderVector,
                syntheticObject,
                0);

            bool finite = true;
            bool nonZero = false;

            for (unsigned i = 0; i < 16; ++i)
            {
                const float value =
                    matrix[i];

                if (value != 0.0f)
                    nonZero = true;

                if (
                    value != value ||
                    value < -1.0e20f ||
                    value > 1.0e20f)
                {
                    finite = false;
                    break;
                }
            }

            WLOG_INFO(
                "wxl-modern-r5b2e1: matrix row0 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(matrix[0]),
                static_cast<double>(matrix[1]),
                static_cast<double>(matrix[2]),
                static_cast<double>(matrix[3]));

            WLOG_INFO(
                "wxl-modern-r5b2e1: matrix row1 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(matrix[4]),
                static_cast<double>(matrix[5]),
                static_cast<double>(matrix[6]),
                static_cast<double>(matrix[7]));

            WLOG_INFO(
                "wxl-modern-r5b2e1: matrix row2 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(matrix[8]),
                static_cast<double>(matrix[9]),
                static_cast<double>(matrix[10]),
                static_cast<double>(matrix[11]));

            WLOG_INFO(
                "wxl-modern-r5b2e1: matrix row3 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(matrix[12]),
                static_cast<double>(matrix[13]),
                static_cast<double>(matrix[14]),
                static_cast<double>(matrix[15]));

            const std::int32_t activeIndex =
                *reinterpret_cast<const std::int32_t*>(
                    synthetic +
                    adt::kShadowActiveCascadeIndex);

            const std::int32_t state =
                *reinterpret_cast<const std::int32_t*>(
                    synthetic +
                    adt::kShadowStateField);

            WLOG_INFO(
                "wxl-modern-r5b2e1: synthetic matrix build returned "
                "extent=%.9g finite=%u nonZero=%u "
                "activeIndex=%d state=%d "
                "finalizerCalled=0 casterPass=0 "
                "renderBound=0 receiverBound=0",
                static_cast<double>(extent),
                finite ? 1u : 0u,
                nonZero ? 1u : 0u,
                static_cast<int>(activeIndex),
                static_cast<int>(state));

            if (!ClassicShadowJoinedProofEnabled())
                return;

            if (
                !g_classicCascade4.resource ||
                !g_classicCascade4.gxObject)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2f4b: joined proof refused "
                    "fourth sidecar unavailable "
                    "resource=%p gx=%p",
                    g_classicCascade4.resource,
                    g_classicCascade4.gxObject);

                return;
            }

            const auto postBuild =
                reinterpret_cast<
                    adt::ShadowSyntheticPostBuildFn>(
                    adt::kShadowSyntheticPostBuildHelper);

            const auto finalizer =
                reinterpret_cast<
                    adt::ShadowFinalizerFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowFinalizerCallbackPtr));

            if (
                !postBuild ||
                !finalizer ||
                reinterpret_cast<uintptr_t>(finalizer) !=
                    adt::kShadowFinalizerCallback)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2f4b: finalizer proof refused "
                    "postBuild=%p finalizer=%p "
                    "expectedFinalizer=0x%08X",
                    reinterpret_cast<void*>(postBuild),
                    reinterpret_cast<void*>(finalizer),
                    static_cast<unsigned>(
                        adt::kShadowFinalizerCallback));

                return;
            }

            // Exact stock 0x876126 call:
            //   push syntheticObject+0x24
            //   ECX = syntheticObject+0x6C
            //   call 0x983990
            postBuild(
                cascadeRecord,
                nullptr,
                synthetic +
                    adt::kShadowSyntheticPostBuildArgument);

            // Preserve the live global context because F4B remains an
            // isolated producer proof, not yet the production fourth-cascade
            // path.
            float savedVector[3] =
            {
                *reinterpret_cast<const float*>(
                    adt::kShadowSyntheticCasterVector + 0),
                *reinterpret_cast<const float*>(
                    adt::kShadowSyntheticCasterVector + 4),
                *reinterpret_cast<const float*>(
                    adt::kShadowSyntheticCasterVector + 8),
            };

            std::uint32_t savedContext[
                adt::kShadowSyntheticContextCount] = {};

            for (
                std::size_t i = 0;
                i < adt::kShadowSyntheticContextCount;
                ++i)
            {
                savedContext[i] =
                    *reinterpret_cast<const std::uint32_t*>(
                        adt::kShadowSyntheticContextBase +
                        i * sizeof(std::uint32_t));
            }

            const auto restoreSyntheticGlobals =
                [&savedVector, &savedContext]()
                {
                    *reinterpret_cast<float*>(
                        adt::kShadowSyntheticCasterVector + 0) =
                            savedVector[0];

                    *reinterpret_cast<float*>(
                        adt::kShadowSyntheticCasterVector + 4) =
                            savedVector[1];

                    *reinterpret_cast<float*>(
                        adt::kShadowSyntheticCasterVector + 8) =
                            savedVector[2];

                    for (
                        std::size_t i = 0;
                        i < adt::kShadowSyntheticContextCount;
                        ++i)
                    {
                        *reinterpret_cast<std::uint32_t*>(
                            adt::kShadowSyntheticContextBase +
                            i * sizeof(std::uint32_t)) =
                                savedContext[i];
                    }
                };

            // Exact stock state immediately before D43160.
            *reinterpret_cast<float*>(
                adt::kShadowSyntheticCasterVector + 0) =
                    1.0f;

            *reinterpret_cast<float*>(
                adt::kShadowSyntheticCasterVector + 4) =
                    0.0f;

            *reinterpret_cast<float*>(
                adt::kShadowSyntheticCasterVector + 8) =
                    0.0f;

            for (
                std::size_t i = 0;
                i < adt::kShadowSyntheticContextCount;
                ++i)
            {
                *reinterpret_cast<std::uint32_t*>(
                    adt::kShadowSyntheticContextBase +
                    i * sizeof(std::uint32_t)) =
                        *reinterpret_cast<const std::uint32_t*>(
                            synthetic +
                            adt::kShadowSyntheticContextObjectBase +
                            i * sizeof(std::uint32_t));
            }

            *reinterpret_cast<std::int32_t*>(
                synthetic +
                adt::kShadowSyntheticMode) =
                    3;

            const std::int32_t generationToken =
                static_cast<std::int32_t>(
                    *reinterpret_cast<const std::uint8_t*>(
                        adt::kShadowGenerationByte));

            // R5B2-F4A closes the missing stock transition:
            //
            //   0x87617F:
            //     mov [syntheticObject + 0xA84], EDI
            //
            // At Tier 5, EDI is still zero at this instruction.  The
            // constructor initializes +0xA84 to -1, but the native
            // synthetic path explicitly activates legal slot 0 before
            // calling D43160.  F3 omitted this write, which left the
            // finalizer with activeIndex=-1 and therefore produced an
            // empty D25320 caster-work record.
            const std::int32_t constructorActiveIndex =
                *reinterpret_cast<const std::int32_t*>(
                    synthetic +
                    adt::kShadowActiveCascadeIndex);

            *reinterpret_cast<std::int32_t*>(
                synthetic +
                adt::kShadowActiveCascadeIndex) =
                    0;

            const std::int32_t beforeActiveIndex =
                *reinterpret_cast<const std::int32_t*>(
                    synthetic +
                    adt::kShadowActiveCascadeIndex);

            const std::int32_t beforeState =
                *reinterpret_cast<const std::int32_t*>(
                    synthetic +
                    adt::kShadowStateField);

            WLOG_INFO(
                "wxl-modern-r5b2f4b: joined proof prepare "
                "object=%p mode=3 generation=%d "
                "constructorActiveIndex=%d "
                "activeIndexBeforeFinalizer=%d stateBefore=%d "
                "stockSyntheticActivation=1 "
                "postBuildHelper=1 builderSlot=0 extent=540 "
                "casterPassPending=1 receiverBound=0",
                syntheticObject,
                static_cast<int>(generationToken),
                static_cast<int>(constructorActiveIndex),
                static_cast<int>(beforeActiveIndex),
                static_cast<int>(beforeState));

            const std::int32_t finalizerResult =
                finalizer(
                    syntheticObject,
                    generationToken);

            const std::int32_t afterActiveIndex =
                *reinterpret_cast<const std::int32_t*>(
                    synthetic +
                    adt::kShadowActiveCascadeIndex);

            const std::int32_t afterState =
                *reinterpret_cast<const std::int32_t*>(
                    synthetic +
                    adt::kShadowStateField);

            if (finalizerResult != 1)
            {
                restoreSyntheticGlobals();

                WLOG_WARN(
                    "wxl-modern-r5b2f4b: joined proof refused "
                    "finalizerResult=%d "
                    "globalsRestored=1 receiverBound=0",
                    static_cast<int>(finalizerResult));

                return;
            }

            const auto callback =
                reinterpret_cast<
                    adt::ShadowCascadeRenderCallbackFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowRenderCallbackPtr));

            const auto resolve =
                reinterpret_cast<
                    adt::Map_TexResolveFn>(
                    adt::kTexResolve);

            if (
                !callback ||
                reinterpret_cast<uintptr_t>(callback) !=
                    adt::kShadowCascadeRenderCallback ||
                !resolve)
            {
                restoreSyntheticGlobals();

                WLOG_WARN(
                    "wxl-modern-r5b2f4b: joined proof refused "
                    "callback=%p expectedCallback=0x%08X "
                    "resolve=%p globalsRestored=1 "
                    "receiverBound=0",
                    reinterpret_cast<void*>(callback),
                    static_cast<unsigned>(
                        adt::kShadowCascadeRenderCallback),
                    reinterpret_cast<void*>(resolve));

                return;
            }

            const std::int32_t shadowGroup =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowGroup);

            void* renderTexture = nullptr;
            void* destinationTexture = nullptr;

            if (shadowGroup != 0)
            {
                void* const sharedHandle =
                    *reinterpret_cast<void* const*>(
                        adt::kShadowCasterSharedResource);

                if (!sharedHandle)
                {
                    restoreSyntheticGlobals();

                    WLOG_WARN(
                        "wxl-modern-r5b2f4b: joined proof refused "
                        "shared caster resource null "
                        "globalsRestored=1 receiverBound=0");

                    return;
                }

                // Exact native synthetic arg3.
                renderTexture =
                    resolve(
                        sharedHandle,
                        1,
                        0);

                // Substitute only the proven extension-owned fourth
                // destination for the stock temporary destination.
                destinationTexture =
                    g_classicCascade4.gxObject;
            }
            else
            {
                // Native group-0 contract puts the per-cascade texture
                // in arg3 and leaves arg4 null.
                renderTexture =
                    g_classicCascade4.gxObject;

                destinationTexture =
                    nullptr;
            }

            if (
                !renderTexture ||
                (
                    shadowGroup != 0 &&
                    !destinationTexture
                ))
            {
                restoreSyntheticGlobals();

                WLOG_WARN(
                    "wxl-modern-r5b2f4b: joined proof refused "
                    "shadowGroup=%d renderTexture=%p "
                    "destinationTexture=%p "
                    "globalsRestored=1 receiverBound=0",
                    static_cast<int>(shadowGroup),
                    renderTexture,
                    destinationTexture);

                return;
            }

            const uintptr_t casterState =
                adt::kShadowCasterStateBase;

            const std::uint32_t work0 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField0);

            const std::uint32_t work10 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField10);

            const std::uint32_t work1C =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField1C);

            const bool expectedFullCaster =
                work0 != 0 ||
                work10 != 0 ||
                work1C != 0;

            WLOG_INFO(
                "wxl-modern-r5b2f4b: joined caster invoke "
                "object=%p legalIndex=0 "
                "extent=540 matrix00=%.9g "
                "finalizerResult=%d "
                "activeIndexAfter=%d stateAfter=%d "
                "shadowGroup=%d "
                "renderTexture=%p destinationTexture=%p "
                "arg5=0x%08X "
                "workState=%08X/%08X/%08X "
                "expectedPath=%s "
                "sidecarTarget=1 receiverBound=0",
                syntheticObject,
                static_cast<double>(matrix[0]),
                static_cast<int>(finalizerResult),
                static_cast<int>(afterActiveIndex),
                static_cast<int>(afterState),
                static_cast<int>(shadowGroup),
                renderTexture,
                destinationTexture,
                static_cast<unsigned>(
                    adt::kShadowSyntheticCasterVector),
                static_cast<unsigned>(work0),
                static_cast<unsigned>(work10),
                static_cast<unsigned>(work1C),
                expectedFullCaster
                    ? "full-caster"
                    : "fast-transfer");

            const std::int32_t casterResult =
                callback(
                    syntheticObject,
                    0,
                    renderTexture,
                    destinationTexture,
                    reinterpret_cast<void*>(
                        adt::kShadowSyntheticCasterVector));

            // Restore only after the native caster has consumed the
            // temporary synthetic globals.
            restoreSyntheticGlobals();

            const bool casterProof =
                casterResult == 1;

            WLOG_INFO(
                "wxl-modern-r5b2f4b: joined caster returned "
                "finalizerResult=%d callbackResult=%d "
                "casterProof=%u expectedPath=%s "
                "extent=540 legalIndex=0 "
                "sidecarTarget=1 globalsRestored=1 "
                "receiverBound=0",
                static_cast<int>(finalizerResult),
                static_cast<int>(casterResult),
                casterProof ? 1u : 0u,
                expectedFullCaster
                    ? "full-caster"
                    : "fast-transfer");

            return;

        }

        void PrepareClassicShadowGeometry(
            void* shadowObject)
        {
            if (
                !ClassicShadowGeometryProofEnabled() ||
                !shadowObject)
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

            auto* const bytes =
                static_cast<std::uint8_t*>(
                    shadowObject);

            // Classic launch geometry for the two near cascades.
            //
            // Slot 2 deliberately remains 640 during the stock build.
            // That preserves Wrath's proven broad caster candidate set,
            // which safely contains every caster needed by both the
            // later 180 map and the extension-owned 540 map.
            constexpr float stockBuildExtents[3] =
            {
                20.0f,
                60.0f,
                640.0f,
            };

            for (std::size_t slot = 0; slot < 3; ++slot)
            {
                *reinterpret_cast<float*>(
                    bytes +
                    adt::kShadowSyntheticExtent +
                    slot * sizeof(float)) =
                        stockBuildExtents[slot];

                const uintptr_t record =
                    adt::kShadowTextureRecordBase +
                    slot *
                    adt::kShadowTextureRecordStride;

                *reinterpret_cast<float*>(
                    record +
                    adt::kShadowCascadeExtent) =
                        stockBuildExtents[slot];
            }

            static bool logged = false;

            if (!logged)
            {
                logged = true;

                WLOG_INFO(
                    "wxl-modern-r5b2g2: pre-native geometry "
                    "stockBuildExtents=20/60/640 "
                    "mapDimension=2048 "
                    "slot2BroadCasterCoverage=1 "
                    "receiver4Bound=0");
            }
        }

        bool RenderClassicGeometryTargetFromSlot2(
            const std::uint8_t* liveSnapshot,
            float targetExtent,
            void* targetTexture,
            float* matrixOut,
            const char* label,
            std::uint32_t work0,
            std::uint32_t work10,
            std::uint32_t work1C)
        {
            if (
                !liveSnapshot ||
                !targetTexture ||
                !matrixOut)
            {
                return false;
            }

            const auto builder =
                reinterpret_cast<
                    adt::ShadowCascadeBuildCallbackFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowCascadeBuildCallbackPtr));

            const auto callback =
                reinterpret_cast<
                    adt::ShadowCascadeRenderCallbackFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowRenderCallbackPtr));

            const auto resolve =
                reinterpret_cast<
                    adt::Map_TexResolveFn>(
                    adt::kTexResolve);

            if (
                !builder ||
                reinterpret_cast<uintptr_t>(builder) !=
                    adt::kShadowCascadeBuildCallback ||
                !callback ||
                reinterpret_cast<uintptr_t>(callback) !=
                    adt::kShadowCascadeRenderCallback ||
                !resolve)
            {
                return false;
            }

            constexpr std::size_t slot = 2;

            alignas(16)
            std::uint8_t clone[
                adt::kShadowObjectSize] = {};

            std::memcpy(
                clone,
                liveSnapshot,
                sizeof(clone));

            void* const cloneObject =
                static_cast<void*>(
                    clone);

            void* const cloneRecord =
                clone +
                adt::kShadowCascadeRecordBase +
                slot *
                adt::kShadowCascadeRecordStride;

            float* const cloneMatrix =
                reinterpret_cast<float*>(
                    clone +
                    adt::kShadowCascadeMatrixBase +
                    slot *
                    adt::kShadowCascadeMatrixStride);

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticExtent +
                slot * sizeof(float)) =
                    targetExtent;

            constexpr std::size_t boundsStride = 0x10;
            const std::size_t boundsOffset =
                slot * boundsStride;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound0 +
                boundsOffset) =
                    -targetExtent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound1 +
                boundsOffset) =
                    targetExtent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound2 +
                boundsOffset) =
                    -targetExtent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound3 +
                boundsOffset) =
                    targetExtent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed0 +
                boundsOffset) =
                    0.0f;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed1 +
                boundsOffset) =
                    1.0f;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed2 +
                boundsOffset) =
                    0.0f;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed3 +
                boundsOffset) =
                    1.0f;

            const uintptr_t nativeRecord =
                adt::kShadowTextureRecordBase +
                slot *
                adt::kShadowTextureRecordStride;

            void* const recordArgument =
                reinterpret_cast<void*>(
                    nativeRecord +
                    adt::kShadowCasterRecordArgument);

            const uintptr_t casterState =
                adt::kShadowCasterStateBase +
                slot *
                adt::kShadowCasterStateStride;

            std::uint8_t nativeRecordSnapshot[
                adt::kShadowTextureRecordStride] = {};

            std::uint8_t casterStateSnapshot[
                adt::kShadowCasterStateStride] = {};

            std::memcpy(
                nativeRecordSnapshot,
                reinterpret_cast<const void*>(
                    nativeRecord),
                sizeof(nativeRecordSnapshot));

            std::memcpy(
                casterStateSnapshot,
                reinterpret_cast<const void*>(
                    casterState),
                sizeof(casterStateSnapshot));

            builder(
                cloneRecord,
                cloneMatrix,
                recordArgument,
                cloneObject,
                2);

            // Restore any shared CPU-side state immediately after the
            // matrix builder.  The clone matrix is extension-owned.
            std::memcpy(
                reinterpret_cast<void*>(
                    nativeRecord),
                nativeRecordSnapshot,
                sizeof(nativeRecordSnapshot));

            std::memcpy(
                reinterpret_cast<void*>(
                    casterState),
                casterStateSnapshot,
                sizeof(casterStateSnapshot));

            bool finite = true;
            bool nonZero = false;

            for (unsigned i = 0; i < 16; ++i)
            {
                const float value =
                    cloneMatrix[i];

                if (
                    value != value ||
                    value < -1.0e20f ||
                    value > 1.0e20f)
                {
                    finite = false;
                }

                if (value != 0.0f)
                    nonZero = true;
            }

            if (
                !finite ||
                !nonZero)
            {
                return false;
            }

            const std::int32_t shadowGroup =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowGroup);

            void* renderTexture = nullptr;
            void* destinationTexture = nullptr;

            if (shadowGroup != 0)
            {
                void* const sharedHandle =
                    *reinterpret_cast<void* const*>(
                        adt::kShadowCasterSharedResource);

                if (!sharedHandle)
                    return false;

                renderTexture =
                    resolve(
                        sharedHandle,
                        1,
                        0);

                destinationTexture =
                    targetTexture;
            }
            else
            {
                renderTexture =
                    targetTexture;

                destinationTexture =
                    nullptr;
            }

            if (
                !renderTexture ||
                (
                    shadowGroup != 0 &&
                    !destinationTexture
                ))
            {
                return false;
            }

            // Restore the exact live 640 slot-2 work record before each
            // caster call so multiple Classic projections can consume
            // the same broad candidate set independently.
            std::memcpy(
                reinterpret_cast<void*>(
                    casterState),
                casterStateSnapshot,
                sizeof(casterStateSnapshot));

            const std::int32_t callbackResult =
                callback(
                    cloneObject,
                    2,
                    renderTexture,
                    destinationTexture,
                    recordArgument);

            std::memcpy(
                reinterpret_cast<void*>(
                    nativeRecord),
                nativeRecordSnapshot,
                sizeof(nativeRecordSnapshot));

            std::memcpy(
                reinterpret_cast<void*>(
                    casterState),
                casterStateSnapshot,
                sizeof(casterStateSnapshot));

            if (callbackResult != 1)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g2: target caster failed "
                    "label=%s extent=%.9g "
                    "callbackResult=%d "
                    "workState=%08X/%08X/%08X "
                    "legalIndex=2 receiver4Bound=0",
                    label ? label : "unknown",
                    static_cast<double>(
                        targetExtent),
                    static_cast<int>(
                        callbackResult),
                    static_cast<unsigned>(
                        work0),
                    static_cast<unsigned>(
                        work10),
                    static_cast<unsigned>(
                        work1C));

                return false;
            }

            std::memcpy(
                matrixOut,
                cloneMatrix,
                16 * sizeof(float));

            return true;
        }

        void RenderClassicShadowGeometry(
            void* shadowObject)
        {
            if (
                !ClassicShadowGeometryProofEnabled() ||
                !g_classicGeometryBuildHookInstalled ||
                !shadowObject ||
                !g_classicCascade4.resource ||
                !g_classicCascade4.gxObject)
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

            // Diagnostic isolation:
            //
            // PrepareClassicShadowGeometry() has already asked the native
            // builder for 20 / 60 / broad-640. Normally G2 now replaces
            // legal slot 2 with the Classic 180 map and also renders the
            // 540 sidecar.
            //
            // When this switch is enabled, deliberately stop here so the
            // native receiver sees 20 / 60 / 640. This isolates whether
            // the visible movement pop begins specifically with the
            // post-build 640 -> 180 conversion.
            if (ClassicShadowKeepNativeFarDiagnosticEnabled())
            {
                static bool logged = false;

                if (!logged)
                {
                    logged = true;

                    WLOG_INFO(
                        "wxl-modern-r5g3c-diag: "
                        "native far retained "
                        "extents=20/60/640 "
                        "render180=0 render540=0");
                }

                g_classicCascade4.matrixValid = false;
                return;
            }

            auto* const liveBytes =
                static_cast<std::uint8_t*>(
                    shadowObject);

            const std::int32_t activeIndex =
                *reinterpret_cast<const std::int32_t*>(
                    liveBytes +
                    adt::kShadowActiveCascadeIndex);

            const std::int32_t cascade2Enabled =
                *reinterpret_cast<const std::int32_t*>(
                    liveBytes +
                    adt::kShadowCascadeEnabledBase +
                    2 *
                    adt::kShadowCascadeEnabledStride);

            if (
                activeIndex < 2 ||
                !cascade2Enabled)
            {
                return;
            }

            constexpr std::size_t slot = 2;

            const uintptr_t nativeRecord =
                adt::kShadowTextureRecordBase +
                slot *
                adt::kShadowTextureRecordStride;

            const float nativeExtent =
                *reinterpret_cast<const float*>(
                    nativeRecord +
                    adt::kShadowCascadeExtent);

            const float objectExtent =
                *reinterpret_cast<const float*>(
                    liveBytes +
                    adt::kShadowSyntheticExtent +
                    slot * sizeof(float));

            if (
                nativeExtent != 640.0f ||
                objectExtent != 640.0f)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g2: broad slot2 gate failed "
                    "nativeExtent=%.9g objectExtent=%.9g "
                    "expected=640 receiver4Bound=0",
                    static_cast<double>(
                        nativeExtent),
                    static_cast<double>(
                        objectExtent));

                return;
            }

            const uintptr_t casterState =
                adt::kShadowCasterStateBase +
                slot *
                adt::kShadowCasterStateStride;

            const std::uint32_t work0 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField0);

            const std::uint32_t work10 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField10);

            const std::uint32_t work1C =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField1C);

            if (
                work0 == 0 &&
                work10 == 0 &&
                work1C == 0)
            {
                return;
            }

            const auto resolve =
                reinterpret_cast<
                    adt::Map_TexResolveFn>(
                    adt::kTexResolve);

            if (!resolve)
                return;

            const std::int32_t selector =
                *reinterpret_cast<const std::int32_t*>(
                    nativeRecord +
                    adt::kShadowTextureSelector);

            if (
                selector < 0 ||
                selector >=
                    static_cast<std::int32_t>(
                        adt::kShadowTextureResourceSlots))
            {
                return;
            }

            void* const nativeHandle =
                *reinterpret_cast<void* const*>(
                    nativeRecord +
                    static_cast<std::size_t>(
                        selector) *
                    sizeof(void*));

            if (!nativeHandle)
                return;

            void* const nativeCascade2Texture =
                resolve(
                    nativeHandle,
                    1,
                    0);

            if (!nativeCascade2Texture)
                return;

            alignas(16)
            std::uint8_t liveSnapshot[
                adt::kShadowObjectSize] = {};

            std::memcpy(
                liveSnapshot,
                shadowObject,
                sizeof(liveSnapshot));

            float matrix180[16] = {};
            float matrix540[16] = {};

            // Bounded isolation test:
            //
            // Keep native slots 0/1 at Classic 20/60, but render slot 2
            // directly at 540 instead of shrinking it to 180. The
            // extension-owned sidecar is deliberately not consumed.
            //
            // If the known Elwynn pop disappears here, the defect is
            // specifically associated with making native slot 2 the
            // 180 band rather than with the first two Classic cascades.
            if (ClassicShadowNative540DiagnosticEnabled())
            {
                float matrixNative540[16] = {};

                const bool renderedNative540 =
                    RenderClassicGeometryTargetFromSlot2(
                        liveSnapshot,
                        540.0f,
                        nativeCascade2Texture,
                        matrixNative540,
                        "native-cascade2-540-diag",
                        work0,
                        work10,
                        work1C);

                if (!renderedNative540)
                {
                    WLOG_WARN(
                        "wxl-modern-r5g3c-diag3: "
                        "native540 render failed");

                    g_classicCascade4.matrixValid = false;
                    return;
                }

                std::memcpy(
                    liveBytes +
                        adt::kShadowCascadeMatrixBase +
                        slot *
                        adt::kShadowCascadeMatrixStride,
                    matrixNative540,
                    sizeof(matrixNative540));

                *reinterpret_cast<float*>(
                    liveBytes +
                    adt::kShadowSyntheticExtent +
                    slot * sizeof(float)) =
                        540.0f;

                constexpr std::size_t diagnosticBoundsStride = 0x10;

                const std::size_t diagnosticBoundsOffset =
                    slot *
                    diagnosticBoundsStride;

                *reinterpret_cast<float*>(
                    liveBytes +
                    adt::kShadowSyntheticBound0 +
                    diagnosticBoundsOffset) =
                        -540.0f;

                *reinterpret_cast<float*>(
                    liveBytes +
                    adt::kShadowSyntheticBound1 +
                    diagnosticBoundsOffset) =
                        540.0f;

                *reinterpret_cast<float*>(
                    liveBytes +
                    adt::kShadowSyntheticBound2 +
                    diagnosticBoundsOffset) =
                        -540.0f;

                *reinterpret_cast<float*>(
                    liveBytes +
                    adt::kShadowSyntheticBound3 +
                    diagnosticBoundsOffset) =
                        540.0f;

                *reinterpret_cast<float*>(
                    nativeRecord +
                    adt::kShadowCascadeExtent) =
                        540.0f;

                // Ensure no stale sidecar matrix can participate even if
                // somebody accidentally leaves the receiver switches on.
                g_classicCascade4.matrixValid = false;

                static bool loggedNative540 = false;

                if (!loggedNative540)
                {
                    loggedNative540 = true;

                    WLOG_INFO(
                        "wxl-modern-r5g3c-diag3: "
                        "native540 PASS "
                        "extents=20/60/540 "
                        "nativeSlot2=540 "
                        "sidecarMatrixValid=0");
                }

                return;
            }

            // Final four-band architecture:
            //
            //   native 0 = 20
            //   native 1 = 60
            //   sidecar  = 180
            //   native 2 = 540
            //
            // Live isolation proved native slot-2=540 is stable while
            // native slot-2=180 causes the repeatable Elwynn movement pop.
            const bool rendered540 =
                RenderClassicGeometryTargetFromSlot2(
                    liveSnapshot,
                    540.0f,
                    nativeCascade2Texture,
                    matrix540,
                    "cascade4-native540",
                    work0,
                    work10,
                    work1C);

            const bool rendered180 =
                RenderClassicGeometryTargetFromSlot2(
                    liveSnapshot,
                    180.0f,
                    g_classicCascade4.gxObject,
                    matrix180,
                    "cascade3-sidecar180",
                    work0,
                    work10,
                    work1C);

            if (
                !rendered180 ||
                !rendered540)
            {
                WLOG_WARN(
                    "wxl-modern-r5g3c: geometry frame incomplete "
                    "rendered180=%u rendered540=%u "
                    "layout=native20/60/540+sidecar180",
                    rendered180 ? 1u : 0u,
                    rendered540 ? 1u : 0u);

                g_classicCascade4.matrixValid = false;
                return;
            }

            // Publish the stable far 540 projection into legal native
            // slot 2. The stock receiver therefore continues to see a
            // coherent native far map/matrix pair.
            std::memcpy(
                liveBytes +
                    adt::kShadowCascadeMatrixBase +
                    slot *
                    adt::kShadowCascadeMatrixStride,
                matrix540,
                sizeof(matrix540));

            *reinterpret_cast<float*>(
                liveBytes +
                adt::kShadowSyntheticExtent +
                slot * sizeof(float)) =
                    540.0f;

            constexpr std::size_t boundsStride = 0x10;
            const std::size_t boundsOffset =
                slot * boundsStride;

            *reinterpret_cast<float*>(
                liveBytes +
                adt::kShadowSyntheticBound0 +
                boundsOffset) =
                    -540.0f;

            *reinterpret_cast<float*>(
                liveBytes +
                adt::kShadowSyntheticBound1 +
                boundsOffset) =
                    540.0f;

            *reinterpret_cast<float*>(
                liveBytes +
                adt::kShadowSyntheticBound2 +
                boundsOffset) =
                    -540.0f;

            *reinterpret_cast<float*>(
                liveBytes +
                adt::kShadowSyntheticBound3 +
                boundsOffset) =
                    540.0f;

            *reinterpret_cast<float*>(
                nativeRecord +
                adt::kShadowCascadeExtent) =
                    540.0f;

            // The extension sidecar carries the intermediate 180 band.
            std::memcpy(
                g_classicCascade4.matrix,
                matrix180,
                sizeof(matrix180));

            g_classicCascade4.matrixValid =
                true;

            ++g_classicCascade4.geometryFrames;

            if (
                g_classicCascade4.geometryFrames <= 4)
            {
                const float* const matrix20 =
                    reinterpret_cast<const float*>(
                        liveBytes +
                        adt::kShadowCascadeMatrixBase);

                const float* const matrix60 =
                    reinterpret_cast<const float*>(
                        liveBytes +
                        adt::kShadowCascadeMatrixBase +
                        adt::kShadowCascadeMatrixStride);

                WLOG_INFO(
                    "wxl-modern-r5b2g2: geometry frame PASS "
                    "frame=%llu "
                    "extents=20/60/180/540 "
                    "scale00=%.9g/%.9g/%.9g/%.9g "
                    "workState=%08X/%08X/%08X "
                    "legalCasterIndex=2 "
                    "sidecarMatrixValid=1 "
                    "receiver4Bound=0",
                    static_cast<unsigned long long>(
                        g_classicCascade4.geometryFrames),
                    static_cast<double>(
                        matrix20[0]),
                    static_cast<double>(
                        matrix60[0]),
                    static_cast<double>(
                        matrix180[0]),
                    static_cast<double>(
                        matrix540[0]),
                    static_cast<unsigned>(
                        work0),
                    static_cast<unsigned>(
                        work10),
                    static_cast<unsigned>(
                        work1C));
            }
        }

        void TryCurrentFrameClone540CasterProof(
            void* shadowObject)
        {
            static bool attempted = false;

            if (
                !ClassicShadowClone540ProofEnabled() ||
                attempted ||
                !shadowObject ||
                !g_classicCascade4.resource ||
                !g_classicCascade4.gxObject)
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

            const auto* const liveBytes =
                static_cast<const std::uint8_t*>(
                    shadowObject);

            const std::int32_t activeIndex =
                *reinterpret_cast<const std::int32_t*>(
                    liveBytes +
                    adt::kShadowActiveCascadeIndex);

            const std::int32_t cascade2Enabled =
                *reinterpret_cast<const std::int32_t*>(
                    liveBytes +
                    adt::kShadowCascadeEnabledBase +
                    2 * adt::kShadowCascadeEnabledStride);

            if (
                activeIndex < 2 ||
                !cascade2Enabled)
            {
                return;
            }

            const auto builder =
                reinterpret_cast<
                    adt::ShadowCascadeBuildCallbackFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowCascadeBuildCallbackPtr));

            const auto callback =
                reinterpret_cast<
                    adt::ShadowCascadeRenderCallbackFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowRenderCallbackPtr));

            const auto resolve =
                reinterpret_cast<
                    adt::Map_TexResolveFn>(
                    adt::kTexResolve);

            if (
                !builder ||
                reinterpret_cast<uintptr_t>(builder) !=
                    adt::kShadowCascadeBuildCallback ||
                !callback ||
                reinterpret_cast<uintptr_t>(callback) !=
                    adt::kShadowCascadeRenderCallback ||
                !resolve)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2g1: proof refused "
                    "builder=%p expectedBuilder=0x%08X "
                    "callback=%p expectedCallback=0x%08X "
                    "resolve=%p receiverBound=0",
                    reinterpret_cast<void*>(builder),
                    static_cast<unsigned>(
                        adt::kShadowCascadeBuildCallback),
                    reinterpret_cast<void*>(callback),
                    static_cast<unsigned>(
                        adt::kShadowCascadeRenderCallback),
                    reinterpret_cast<void*>(resolve));

                return;
            }

            constexpr std::size_t slot = 2;

            const uintptr_t nativeRecord =
                adt::kShadowTextureRecordBase +
                slot * adt::kShadowTextureRecordStride;

            const float nativeRecordExtent =
                *reinterpret_cast<const float*>(
                    nativeRecord +
                    adt::kShadowCascadeExtent);

            const std::size_t objectExtentOffset =
                adt::kShadowSyntheticExtent +
                slot * sizeof(float);

            const float liveObjectExtent =
                *reinterpret_cast<const float*>(
                    liveBytes +
                    objectExtentOffset);

            if (
                nativeRecordExtent != 640.0f ||
                liveObjectExtent != 640.0f)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2g1: proof refused "
                    "nativeRecordExtent=%.9g "
                    "liveObjectExtent=%.9g "
                    "expected=640 receiverBound=0",
                    static_cast<double>(
                        nativeRecordExtent),
                    static_cast<double>(
                        liveObjectExtent));

                return;
            }

            const uintptr_t casterState =
                adt::kShadowCasterStateBase +
                slot * adt::kShadowCasterStateStride;

            const std::uint32_t work0Before =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField0);

            const std::uint32_t work10Before =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField10);

            const std::uint32_t work1CBefore =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField1C);

            if (
                work0Before == 0 &&
                work10Before == 0 &&
                work1CBefore == 0)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2g1: proof refused "
                    "live slot2 caster workspace empty "
                    "workState=%08X/%08X/%08X "
                    "receiverBound=0",
                    static_cast<unsigned>(
                        work0Before),
                    static_cast<unsigned>(
                        work10Before),
                    static_cast<unsigned>(
                        work1CBefore));

                return;
            }

            alignas(16)
            std::uint8_t clone[
                adt::kShadowObjectSize] = {};

            std::memcpy(
                clone,
                shadowObject,
                sizeof(clone));

            void* const cloneObject =
                static_cast<void*>(
                    clone);

            void* const cloneRecord =
                clone +
                adt::kShadowCascadeRecordBase +
                slot * adt::kShadowCascadeRecordStride;

            float* const cloneMatrix =
                reinterpret_cast<float*>(
                    clone +
                    adt::kShadowCascadeMatrixBase +
                    slot * adt::kShadowCascadeMatrixStride);

            const float* const liveMatrix =
                reinterpret_cast<const float*>(
                    liveBytes +
                    adt::kShadowCascadeMatrixBase +
                    slot * adt::kShadowCascadeMatrixStride);

            float beforeMatrix[16] = {};

            std::memcpy(
                beforeMatrix,
                liveMatrix,
                sizeof(beforeMatrix));

            // Preserve the entire native cascade-2 resource/config record
            // and the live slot-2 caster workspace.  The matrix builder is
            // allowed to consume the exact native record+0x28 contract,
            // but any incidental writes are restored before the caster is
            // invoked.  The only retained output is the extension-owned
            // clone's rebuilt matrix/state.
            std::uint8_t nativeRecordSnapshot[
                adt::kShadowTextureRecordStride] = {};

            std::uint8_t casterStateSnapshot[
                adt::kShadowCasterStateStride] = {};

            std::memcpy(
                nativeRecordSnapshot,
                reinterpret_cast<const void*>(
                    nativeRecord),
                sizeof(nativeRecordSnapshot));

            std::memcpy(
                casterStateSnapshot,
                reinterpret_cast<const void*>(
                    casterState),
                sizeof(casterStateSnapshot));

            const float extent =
                g_classicCascade4.extent;

            *reinterpret_cast<float*>(
                clone +
                objectExtentOffset) =
                    extent;

            const std::size_t boundsStride =
                0x10;

            const std::size_t boundsOffset =
                slot * boundsStride;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound0 +
                boundsOffset) =
                    -extent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound1 +
                boundsOffset) =
                    extent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound2 +
                boundsOffset) =
                    -extent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticBound3 +
                boundsOffset) =
                    extent;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed0 +
                boundsOffset) =
                    0.0f;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed1 +
                boundsOffset) =
                    1.0f;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed2 +
                boundsOffset) =
                    0.0f;

            *reinterpret_cast<float*>(
                clone +
                adt::kShadowSyntheticSeed3 +
                boundsOffset) =
                    1.0f;

            void* const recordArgument =
                reinterpret_cast<void*>(
                    nativeRecord +
                    adt::kShadowCasterRecordArgument);

            attempted = true;

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 build begin "
                "liveObject=%p cloneObject=%p "
                "legalIndex=2 "
                "liveExtent=%.9g targetExtent=%.9g "
                "builder=%p builderArg=%p "
                "workStateBefore=%08X/%08X/%08X "
                "nativeObjectUntouched=1 "
                "receiverBound=0",
                shadowObject,
                cloneObject,
                static_cast<double>(
                    liveObjectExtent),
                static_cast<double>(
                    extent),
                reinterpret_cast<void*>(
                    builder),
                recordArgument,
                static_cast<unsigned>(
                    work0Before),
                static_cast<unsigned>(
                    work10Before),
                static_cast<unsigned>(
                    work1CBefore));

            // Exact legal normal-builder callback shape for native slot 2:
            //
            //   arg1 = clone + 0x6C + 2*0xF4
            //   arg2 = clone + 0x9C4 + 2*0x40
            //   arg3 = native record2 + 0x28
            //   arg4 = clone root
            //   arg5 = legal slot 2
            //
            // This deliberately does NOT call the three-slot 0x874890
            // routine and never exposes native index 3.
            builder(
                cloneRecord,
                cloneMatrix,
                recordArgument,
                cloneObject,
                2);

            const bool nativeRecordMutated =
                std::memcmp(
                    nativeRecordSnapshot,
                    reinterpret_cast<const void*>(
                        nativeRecord),
                    sizeof(nativeRecordSnapshot)) != 0;

            const bool casterWorkspaceMutated =
                std::memcmp(
                    casterStateSnapshot,
                    reinterpret_cast<const void*>(
                        casterState),
                    sizeof(casterStateSnapshot)) != 0;

            // Restore all native/shared proof inputs before the actual
            // caster callback.  The callback therefore sees the exact
            // already-proven current-frame slot-2 workspace and record
            // contract from D1, paired only with the cloned 540 matrix.
            std::memcpy(
                reinterpret_cast<void*>(
                    nativeRecord),
                nativeRecordSnapshot,
                sizeof(nativeRecordSnapshot));

            std::memcpy(
                reinterpret_cast<void*>(
                    casterState),
                casterStateSnapshot,
                sizeof(casterStateSnapshot));

            bool finite = true;
            bool nonZero = false;
            unsigned changedCount = 0;
            float maxAbsDelta = 0.0f;

            for (unsigned i = 0; i < 16; ++i)
            {
                const float value =
                    cloneMatrix[i];

                if (value != 0.0f)
                    nonZero = true;

                if (
                    value != value ||
                    value < -1.0e20f ||
                    value > 1.0e20f)
                {
                    finite = false;
                }

                const float delta =
                    value -
                    beforeMatrix[i];

                const float absDelta =
                    delta < 0.0f
                        ? -delta
                        : delta;

                if (absDelta != 0.0f)
                    ++changedCount;

                if (absDelta > maxAbsDelta)
                    maxAbsDelta = absDelta;
            }

            WLOG_INFO(
                "wxl-modern-r5b2g1: live640 matrix row0 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    beforeMatrix[0]),
                static_cast<double>(
                    beforeMatrix[1]),
                static_cast<double>(
                    beforeMatrix[2]),
                static_cast<double>(
                    beforeMatrix[3]));

            WLOG_INFO(
                "wxl-modern-r5b2g1: live640 matrix row1 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    beforeMatrix[4]),
                static_cast<double>(
                    beforeMatrix[5]),
                static_cast<double>(
                    beforeMatrix[6]),
                static_cast<double>(
                    beforeMatrix[7]));

            WLOG_INFO(
                "wxl-modern-r5b2g1: live640 matrix row2 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    beforeMatrix[8]),
                static_cast<double>(
                    beforeMatrix[9]),
                static_cast<double>(
                    beforeMatrix[10]),
                static_cast<double>(
                    beforeMatrix[11]));

            WLOG_INFO(
                "wxl-modern-r5b2g1: live640 matrix row3 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    beforeMatrix[12]),
                static_cast<double>(
                    beforeMatrix[13]),
                static_cast<double>(
                    beforeMatrix[14]),
                static_cast<double>(
                    beforeMatrix[15]));

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 matrix row0 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    cloneMatrix[0]),
                static_cast<double>(
                    cloneMatrix[1]),
                static_cast<double>(
                    cloneMatrix[2]),
                static_cast<double>(
                    cloneMatrix[3]));

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 matrix row1 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    cloneMatrix[4]),
                static_cast<double>(
                    cloneMatrix[5]),
                static_cast<double>(
                    cloneMatrix[6]),
                static_cast<double>(
                    cloneMatrix[7]));

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 matrix row2 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    cloneMatrix[8]),
                static_cast<double>(
                    cloneMatrix[9]),
                static_cast<double>(
                    cloneMatrix[10]),
                static_cast<double>(
                    cloneMatrix[11]));

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 matrix row3 "
                "%.9g %.9g %.9g %.9g",
                static_cast<double>(
                    cloneMatrix[12]),
                static_cast<double>(
                    cloneMatrix[13]),
                static_cast<double>(
                    cloneMatrix[14]),
                static_cast<double>(
                    cloneMatrix[15]));

            const std::int32_t cloneActiveIndex =
                *reinterpret_cast<const std::int32_t*>(
                    clone +
                    adt::kShadowActiveCascadeIndex);

            const std::int32_t cloneState =
                *reinterpret_cast<const std::int32_t*>(
                    clone +
                    adt::kShadowStateField);

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 build returned "
                "finite=%u nonZero=%u changedCount=%u "
                "maxAbsDelta=%.9g "
                "cloneActiveIndex=%d cloneState=%d "
                "nativeRecordMutated=%u "
                "casterWorkspaceMutated=%u "
                "nativeInputsRestored=1 "
                "receiverBound=0",
                finite ? 1u : 0u,
                nonZero ? 1u : 0u,
                changedCount,
                static_cast<double>(
                    maxAbsDelta),
                static_cast<int>(
                    cloneActiveIndex),
                static_cast<int>(
                    cloneState),
                nativeRecordMutated ? 1u : 0u,
                casterWorkspaceMutated ? 1u : 0u);

            if (
                !finite ||
                !nonZero ||
                changedCount == 0 ||
                cloneActiveIndex < 2)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g1: proof refused "
                    "matrix/state gate finite=%u nonZero=%u "
                    "changedCount=%u cloneActiveIndex=%d "
                    "receiverBound=0",
                    finite ? 1u : 0u,
                    nonZero ? 1u : 0u,
                    changedCount,
                    static_cast<int>(
                        cloneActiveIndex));

                return;
            }

            const std::int32_t shadowGroup =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowGroup);

            void* renderTexture = nullptr;
            void* destinationTexture = nullptr;

            if (shadowGroup != 0)
            {
                void* const sharedHandle =
                    *reinterpret_cast<void* const*>(
                        adt::kShadowCasterSharedResource);

                if (!sharedHandle)
                {
                    WLOG_WARN(
                        "wxl-modern-r5b2g1: proof refused "
                        "shared caster resource null "
                        "receiverBound=0");

                    return;
                }

                renderTexture =
                    resolve(
                        sharedHandle,
                        1,
                        0);

                destinationTexture =
                    g_classicCascade4.gxObject;
            }
            else
            {
                renderTexture =
                    g_classicCascade4.gxObject;

                destinationTexture =
                    nullptr;
            }

            if (
                !renderTexture ||
                (
                    shadowGroup != 0 &&
                    !destinationTexture
                ))
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g1: proof refused "
                    "shadowGroup=%d renderTexture=%p "
                    "destinationTexture=%p "
                    "receiverBound=0",
                    static_cast<int>(
                        shadowGroup),
                    renderTexture,
                    destinationTexture);

                return;
            }

            const std::uint32_t work0 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField0);

            const std::uint32_t work10 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField10);

            const std::uint32_t work1C =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField1C);

            const bool fullCasterPathExpected =
                work0 != 0 ||
                work10 != 0 ||
                work1C != 0;

            if (!fullCasterPathExpected)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g1: proof refused "
                    "restored caster workspace unexpectedly empty "
                    "workState=%08X/%08X/%08X "
                    "receiverBound=0",
                    static_cast<unsigned>(
                        work0),
                    static_cast<unsigned>(
                        work10),
                    static_cast<unsigned>(
                        work1C));

                return;
            }

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 caster invoke "
                "cloneObject=%p legalIndex=2 "
                "targetExtent=540 "
                "shadowGroup=%d "
                "renderTexture=%p destinationTexture=%p "
                "recordArgument=%p "
                "workState=%08X/%08X/%08X "
                "expectedPath=full-caster "
                "sidecarTarget=1 "
                "nativeObjectUntouched=1 "
                "receiverBound=0",
                cloneObject,
                static_cast<int>(
                    shadowGroup),
                renderTexture,
                destinationTexture,
                recordArgument,
                static_cast<unsigned>(
                    work0),
                static_cast<unsigned>(
                    work10),
                static_cast<unsigned>(
                    work1C));

            const std::int32_t callbackResult =
                callback(
                    cloneObject,
                    2,
                    renderTexture,
                    destinationTexture,
                    recordArgument);

            g_classicCascade4.casterProofReturned =
                callbackResult == 1;

            if (callbackResult != 1)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g1: clone540 caster "
                    "unexpected callback result=%d "
                    "receiverBound=0",
                    static_cast<int>(
                        callbackResult));
            }

            WLOG_INFO(
                "wxl-modern-r5b2g1: clone540 caster returned "
                "legalIndex=2 targetExtent=540 "
                "callbackResult=%d "
                "expectedPath=full-caster "
                "casterProof=%u "
                "sidecar=%p gx=%p "
                "receiverBound=0 nativeIndex3Used=0",
                static_cast<int>(
                    callbackResult),
                g_classicCascade4.casterProofReturned
                    ? 1u
                    : 0u,
                g_classicCascade4.resource,
                g_classicCascade4.gxObject);
        }

        void TryCascade2CasterIntoSidecarProof(
            void* shadowObject)
        {
            static bool attempted = false;

            if (
                !ClassicShadowCasterProofEnabled() ||
                attempted ||
                !shadowObject ||
                !g_classicCascade4.resource ||
                !g_classicCascade4.gxObject)
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

            const auto* const bytes =
                static_cast<const std::uint8_t*>(
                    shadowObject);

            const std::int32_t activeIndex =
                *reinterpret_cast<const std::int32_t*>(
                    bytes +
                    adt::kShadowActiveCascadeIndex);

            if (activeIndex < 2)
                return;

            const std::int32_t cascade2Enabled =
                *reinterpret_cast<const std::int32_t*>(
                    bytes +
                    adt::kShadowCascadeEnabledBase +
                    2 * adt::kShadowCascadeEnabledStride);

            if (!cascade2Enabled)
                return;

            const uintptr_t nativeRecord =
                adt::kShadowTextureRecordBase +
                2 * adt::kShadowTextureRecordStride;

            const float nativeExtent =
                *reinterpret_cast<const float*>(
                    nativeRecord +
                    adt::kShadowCascadeExtent);

            // This tranche deliberately duplicates the already-valid Wrath
            // cascade-2 matrix.  It is NOT yet the Classic 540 matrix.
            if (nativeExtent != 640.0f)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2d1: caster proof refused "
                    "native cascade2 extent=%.9g expected=640",
                    static_cast<double>(nativeExtent));

                return;
            }

            const auto callback =
                reinterpret_cast<
                    adt::ShadowCascadeRenderCallbackFn>(
                    *reinterpret_cast<void* const*>(
                        adt::kShadowRenderCallbackPtr));

            if (
                !callback ||
                reinterpret_cast<uintptr_t>(callback) !=
                    adt::kShadowCascadeRenderCallback)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2d1: caster proof refused "
                    "callback=%p expected=0x%08X",
                    reinterpret_cast<void*>(callback),
                    static_cast<unsigned>(
                        adt::kShadowCascadeRenderCallback));

                return;
            }

            const auto resolve =
                reinterpret_cast<adt::Map_TexResolveFn>(
                    adt::kTexResolve);

            if (!resolve)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2d1: caster proof refused "
                    "texture resolver unavailable");

                return;
            }

            const std::int32_t shadowGroup =
                *reinterpret_cast<const std::int32_t*>(
                    adt::kShadowGroup);

            void* renderTexture = nullptr;
            void* destinationTexture = nullptr;

            if (shadowGroup != 0)
            {
                void* const sharedHandle =
                    *reinterpret_cast<void* const*>(
                        adt::kShadowCasterSharedResource);

                if (!sharedHandle)
                {
                    attempted = true;

                    WLOG_WARN(
                        "wxl-modern-r5b2d1: caster proof refused "
                        "shared caster resource is null");

                    return;
                }

                renderTexture =
                    resolve(
                        sharedHandle,
                        1,
                        0);

                destinationTexture =
                    g_classicCascade4.gxObject;
            }
            else
            {
                renderTexture =
                    g_classicCascade4.gxObject;

                destinationTexture = nullptr;
            }

            if (!renderTexture)
            {
                attempted = true;

                WLOG_WARN(
                    "wxl-modern-r5b2d1: caster proof refused "
                    "resolved render texture is null "
                    "shadowGroup=%d",
                    static_cast<int>(shadowGroup));

                return;
            }

            void* const recordArgument =
                reinterpret_cast<void*>(
                    nativeRecord +
                    adt::kShadowCasterRecordArgument);

            const uintptr_t casterState =
                adt::kShadowCasterStateBase +
                2 * adt::kShadowCasterStateStride;

            const std::uint32_t work0 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField0);

            const std::uint32_t work10 =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField10);

            const std::uint32_t work1C =
                *reinterpret_cast<const std::uint32_t*>(
                    casterState +
                    adt::kShadowCasterWorkField1C);

            const bool fullCasterPathExpected =
                work0 != 0 ||
                work10 != 0 ||
                work1C != 0;

            attempted = true;

            WLOG_INFO(
                "wxl-modern-r5b2d1: caster proof invoke "
                "object=%p legalIndex=2 "
                "renderTexture=%p destinationTexture=%p "
                "recordArgument=%p shadowGroup=%d "
                "matrixSource=native-cascade2 "
                "matrixExtent=640 sidecarTargetExtent=540 "
                "workState=%08X/%08X/%08X "
                "expectedPath=%s receiverBound=0",
                shadowObject,
                renderTexture,
                destinationTexture,
                recordArgument,
                static_cast<int>(shadowGroup),
                static_cast<unsigned>(work0),
                static_cast<unsigned>(work10),
                static_cast<unsigned>(work1C),
                fullCasterPathExpected
                    ? "full-caster"
                    : "fast-transfer");

            const std::int32_t callbackResult =
                callback(
                    shadowObject,
                    2,
                    renderTexture,
                    destinationTexture,
                    recordArgument);

            g_classicCascade4.casterProofReturned =
                callbackResult == 1;

            if (callbackResult != 1)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2d1: caster proof "
                    "unexpected callback result=%d",
                    static_cast<int>(callbackResult));
            }

            WLOG_INFO(
                "wxl-modern-r5b2d1: caster proof returned "
                "legalIndex=2 sidecar=%p gx=%p "
                "callbackResult=%d expectedPath=%s "
                "casterProof=%u receiverBound=0",
                g_classicCascade4.resource,
                g_classicCascade4.gxObject,
                static_cast<int>(callbackResult),
                fullCasterPathExpected
                    ? "full-caster"
                    : "fast-transfer",
                g_classicCascade4.casterProofReturned
                    ? 1u
                    : 0u);
        }


        // -------------------------------------------------------------------------
        // R5B2-G3B visible terrain receiver.
        //
        // The stock Tier-5 Terrain3_pcf ps_3_0 receiver consumes:
        //   s5 = auxiliary/static receiver
        //   s6/s7/s8 = dynamic cascades 20/60/180 after G2R1
        //
        // The matching stock terrain VS already exports TEXCOORD3 = world position,
        // even though the stock PCF PS does not declare that semantic. G3B therefore
        // extends only the live pixel shader: declare TEXCOORD3 on a free input,
        // project it through the extension-owned 540 matrix, and consume the sidecar
        // at s9. Native VS permutations and the first three receiver branches remain
        // byte-for-byte stock.
        // -------------------------------------------------------------------------

        using TerrainReceiverDrawFn =
            adt::Map_SurfaceChunkDrawShaderFn;

        TerrainReceiverDrawFn
            g_origClassicTerrainReceiverDraw = nullptr;

        bool g_classicReceiverShaderHookInstalled = false;

        std::unordered_map<void*, void*>
            g_classicReceiverPatchedShaders;

        typedef HRESULT(WINAPI* PFN_D3DAssemble)(
            LPCVOID,
            SIZE_T,
            LPCSTR,
            const D3D_SHADER_MACRO*,
            ID3DInclude*,
            UINT,
            ID3DBlob**,
            ID3DBlob**);

        typedef HRESULT(WINAPI* PFN_D3DDisassemble)(
            LPCVOID,
            SIZE_T,
            UINT,
            LPCSTR,
            ID3DBlob**);

        bool ClassicShadowReceiverShaderProofEnabled()
        {
            static const bool enabled = []()
            {
                char raw[16] = {};

                const DWORD count =
                    GetEnvironmentVariableA(
                        "WXL_CLASSIC_SHADOW_RECEIVER_SHADER_PROOF",
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

        HMODULE ClassicShadowCompiler()
        {
            HMODULE compiler =
                GetModuleHandleA(
                    "d3dcompiler_47.dll");

            return compiler
                ? compiler
                : LoadLibraryA(
                    "d3dcompiler_47.dll");
        }

        std::string TrimShaderText(
            const std::string& value)
        {
            std::size_t begin = 0;
            std::size_t end = value.size();

            while (
                begin < end &&
                std::isspace(
                    static_cast<unsigned char>(
                        value[begin])))
            {
                ++begin;
            }

            while (
                end > begin &&
                std::isspace(
                    static_cast<unsigned char>(
                        value[end - 1])))
            {
                --end;
            }

            return
                value.substr(
                    begin,
                    end - begin);
        }

        int MaxShaderRegister(
            const std::string& text,
            char prefix)
        {
            int maximum = -1;

            for (
                std::size_t i = 0;
                i + 1 < text.size();
                ++i)
            {
                if (text[i] != prefix)
                    continue;

                if (i > 0)
                {
                    const unsigned char previous =
                        static_cast<unsigned char>(
                            text[i - 1]);

                    if (
                        std::isalnum(previous) ||
                        previous == '_')
                    {
                        continue;
                    }
                }

                std::size_t j = i + 1;
                int value = 0;
                bool any = false;

                while (
                    j < text.size() &&
                    std::isdigit(
                        static_cast<unsigned char>(
                            text[j])))
                {
                    value =
                        value * 10 +
                        (text[j] - '0');

                    ++j;
                    any = true;
                }

                if (
                    any &&
                    value > maximum)
                {
                    maximum = value;
                }
            }

            return maximum;
        }

        bool ContainsShaderRegister(
            const std::string& text,
            char prefix,
            int number)
        {
            char token[16] = {};

            std::snprintf(
                token,
                sizeof(token),
                "%c%d",
                prefix,
                number);

            const std::size_t tokenLength =
                std::strlen(token);

            std::size_t at = 0;

            while (
                (at = text.find(
                    token,
                    at)) != std::string::npos)
            {
                const bool leftOk =
                    at == 0 ||
                    !(
                        std::isalnum(
                            static_cast<unsigned char>(
                                text[at - 1])) ||
                        text[at - 1] == '_');

                const std::size_t after =
                    at + tokenLength;

                const bool rightOk =
                    after >= text.size() ||
                    !std::isdigit(
                        static_cast<unsigned char>(
                            text[after]));

                if (
                    leftOk &&
                    rightOk)
                {
                    return true;
                }

                at = after;
            }

            return false;
        }

        bool FindShaderSemanticRegister(
            const std::string& text,
            const char* semantic,
            std::string& reg)
        {
            if (!semantic)
                return false;

            std::string needle =
                "dcl_";

            needle += semantic;
            needle += " ";

            const std::size_t at =
                text.find(needle);

            if (at == std::string::npos)
                return false;

            const std::size_t begin =
                at + needle.size();

            std::size_t end =
                text.find(
                    '\n',
                    begin);

            if (end == std::string::npos)
                end = text.size();

            reg =
                TrimShaderText(
                    text.substr(
                        begin,
                        end - begin));

            const std::size_t component =
                reg.find('.');

            if (component != std::string::npos)
                reg.resize(component);

            return
                reg.size() >= 2 &&
                reg[0] == 'v';
        }

        std::size_t AfterLastShaderDeclaration(
            const std::string& text)
        {
            std::size_t position = 0;
            std::size_t best = 0;

            while (position < text.size())
            {
                std::size_t end =
                    text.find(
                        '\n',
                        position);

                if (end == std::string::npos)
                    end = text.size();

                const std::string line =
                    TrimShaderText(
                        text.substr(
                            position,
                            end - position));

                if (
                    line.compare(
                        0,
                        3,
                        "dcl") == 0)
                {
                    best =
                        end < text.size()
                            ? end + 1
                            : end;
                }

                position =
                    end < text.size()
                        ? end + 1
                        : end;
            }

            return best;
        }

        struct ShaderLine
        {
            std::size_t begin = 0;
            std::size_t end = 0;
            std::string text;
        };

        std::vector<ShaderLine>
        SplitShaderLines(
            const std::string& text)
        {
            std::vector<ShaderLine> lines;

            std::size_t position = 0;

            while (position < text.size())
            {
                std::size_t end =
                    text.find(
                        '\n',
                        position);

                if (end == std::string::npos)
                    end = text.size();

                ShaderLine line;
                line.begin = position;
                line.end =
                    end < text.size()
                        ? end + 1
                        : end;
                line.text =
                    TrimShaderText(
                        text.substr(
                            position,
                            end - position));

                lines.push_back(line);

                position =
                    end < text.size()
                        ? end + 1
                        : end;
            }

            return lines;
        }

        std::string ShaderDestination(
            const std::string& line)
        {
            const std::size_t firstSpace =
                line.find_first_of(
                    " \t");

            if (firstSpace == std::string::npos)
                return std::string();

            const std::size_t comma =
                line.find(
                    ',',
                    firstSpace + 1);

            if (comma == std::string::npos)
                return std::string();

            return
                TrimShaderText(
                    line.substr(
                        firstSpace + 1,
                        comma - firstSpace - 1));
        }

        std::string InjectClassicCascade4Receiver(
            const std::string& text)
        {
            if (
                text.find("ps_3_0") == std::string::npos ||
                text.find("dcl_2d s8") == std::string::npos ||
                text.find("dcl_2d s9") != std::string::npos ||
                text.find("dcl_texcoord3") != std::string::npos)
            {
                return std::string();
            }

            if (
                ContainsShaderRegister(
                    text,
                    'c',
                    31) ||
                ContainsShaderRegister(
                    text,
                    'c',
                    32) ||
                ContainsShaderRegister(
                    text,
                    'c',
                    33) ||
                ContainsShaderRegister(
                    text,
                    'c',
                    34) ||
                ContainsShaderRegister(
                    text,
                    'c',
                    35))
            {
                return std::string();
            }

            std::string tc4;
            std::string tc5;
            std::string tc6;

            if (
                !FindShaderSemanticRegister(
                    text,
                    "texcoord4",
                    tc4) ||
                !FindShaderSemanticRegister(
                    text,
                    "texcoord5",
                    tc5) ||
                !FindShaderSemanticRegister(
                    text,
                    "texcoord6",
                    tc6))
            {
                return std::string();
            }

            const int maxInput =
                MaxShaderRegister(
                    text,
                    'v');

            if (
                maxInput < 0 ||
                maxInput >= 9 ||
                ContainsShaderRegister(
                    text,
                    'v',
                    9))
            {
                return std::string();
            }

            const int maxTemp =
                MaxShaderRegister(
                    text,
                    'r');

            if (
                maxTemp < 0 ||
                maxTemp + 7 > 31)
            {
                return std::string();
            }

            const std::vector<ShaderLine> lines =
                SplitShaderLines(
                    text);

            int firstS8 = -1;
            int lastS8 = -1;
            unsigned s8Samples = 0;

            for (
                std::size_t i = 0;
                i < lines.size();
                ++i)
            {
                const std::string& line =
                    lines[i].text;

                const bool textureInstruction =
                    line.compare(
                        0,
                        5,
                        "texld") == 0;

                if (
                    textureInstruction &&
                    line.find("s8") != std::string::npos)
                {
                    if (firstS8 < 0)
                        firstS8 =
                            static_cast<int>(i);

                    lastS8 =
                        static_cast<int>(i);

                    ++s8Samples;
                }
            }

            if (
                firstS8 < 0 ||
                lastS8 < firstS8 ||
                s8Samples != 5)
            {
                return std::string();
            }

            int finalElse = -1;

            for (
                int i = firstS8 - 1;
                i >= 0;
                --i)
            {
                if (lines[i].text == "else")
                {
                    finalElse = i;
                    break;
                }

                if (
                    lines[i].text == "endif")
                {
                    break;
                }
            }

            if (finalElse < 0)
                return std::string();

            int finalEndif = -1;

            for (
                std::size_t i =
                    static_cast<std::size_t>(
                        lastS8 + 1);
                i < lines.size();
                ++i)
            {
                if (lines[i].text == "endif")
                {
                    finalEndif =
                        static_cast<int>(i);
                    break;
                }

                if (
                    lines[i].text == "else")
                {
                    return std::string();
                }
            }

            if (finalEndif <= lastS8)
                return std::string();

            // R5G3C bounded diagnostic:
            // expose only the exact live outer native receiver branch
            // surrounding s8.  This is the branch the fourth 540 cascade
            // must extend/replace coherently.
            {
                static unsigned outerBranchDiagnostics = 0;

                if (outerBranchDiagnostics < 4)
                {
                    ++outerBranchDiagnostics;

                    const int diagnosticFirst =
                        finalElse > 3
                            ? finalElse - 3
                            : 0;

                    const int diagnosticLast =
                        finalEndif + 2 <
                            static_cast<int>(lines.size())
                                ? finalEndif + 2
                                : static_cast<int>(lines.size()) - 1;

                    WLOG_INFO(
                        "wxl-modern-r5g3c-outer: "
                        "receiver outer branch "
                        "firstS8=%d lastS8=%d "
                        "finalElse=%d finalEndif=%d "
                        "s8Samples=%u",
                        firstS8,
                        lastS8,
                        finalElse,
                        finalEndif,
                        s8Samples);

                    for (
                        int diagnosticLine = diagnosticFirst;
                        diagnosticLine <= diagnosticLast;
                        ++diagnosticLine)
                    {
                        WLOG_INFO(
                            "wxl-modern-r5g3c-outer: "
                            "line=%d text=[%s]",
                            diagnosticLine + 1,
                            lines[
                                static_cast<std::size_t>(
                                    diagnosticLine)].text.c_str());
                    }
                }
            }

            int resultLine =
                finalEndif - 1;

            while (
                resultLine > lastS8 &&
                lines[resultLine].text.empty())
            {
                --resultLine;
            }

            if (resultLine <= lastS8)
                return std::string();

            const std::string result =
                ShaderDestination(
                    lines[resultLine].text);

            if (
                result.empty() ||
                result[0] != 'r')
            {
                return std::string();
            }

            const std::size_t dclAt =
                AfterLastShaderDeclaration(
                    text);

            if (dclAt == 0)
                return std::string();

            const int T0 = maxTemp + 1;
            const int T1 = maxTemp + 2;
            const int T2 = maxTemp + 3;
            const int T3 = maxTemp + 4;
            const int T4 = maxTemp + 5;
            const int T5 = maxTemp + 6;

            char line[256] = {};
            std::string injected;

            // Compute extension-owned 180 projection directly from the
            // terrain world position supplied in TEXCOORD3.
            std::snprintf(
                line,
                sizeof(line),
                "    mov r%d.xyz, v9\n"
                "    mov r%d.w, c34.z\n",
                T0,
                T0);
            injected += line;

            for (int axis = 0; axis < 3; ++axis)
            {
                std::snprintf(
                    line,
                    sizeof(line),
                    "    dp4 r%d.%c, r%d, c%d\n",
                    T1,
                    "xyz"[axis],
                    T0,
                    31 + axis);
                injected += line;
            }

            // Nested selection. We are already inside the stock final
            // ELSE, so the near native branches have failed. Use 180
            // while it contains the receiver; otherwise fall through
            // untouched to the original native s8 / 540 branch.
            std::snprintf(
                line,
                sizeof(line),
                "    abs r%d.x, r%d.x\n"
                "    abs r%d.y, r%d.y\n"
                "    max r%d.x, r%d.x, r%d.y\n"
                "    if_lt r%d.x, c34.z\n",
                T5, T1,
                T5, T1,
                T5, T5, T5,
                T5);
            injected += line;

            std::snprintf(
                line,
                sizeof(line),
                "    mad r%d.x, r%d.x, c34.x, c34.x\n"
                "    mad r%d.y, r%d.y, c34.x, c34.x\n"
                "    mov r%d.w, c34.w\n",
                T1, T1,
                T1, T1,
                T1);
            injected += line;

            // Five comparison samples, matching the established Wrath
            // Terrain3_pcf receiver footprint. All maps remain 2048^2,
            // so the existing one-texel offset constants remain valid.
            std::snprintf(
                line,
                sizeof(line),
                "    texldl r%d, r%d, s9\n"
                "    add r%d.xy, r%d, c3\n"
                "    mov r%d.zw, r%d\n"
                "    texldl r%d, r%d, s9\n"
                "    add r%d.x, r%d.x, r%d.x\n",
                T2, T1,
                T3, T1,
                T3, T1,
                T4, T3,
                T2, T2, T4);
            injected += line;

            const int offsetConstants[3] =
                {5, 7, 9};

            for (int offset : offsetConstants)
            {
                std::snprintf(
                    line,
                    sizeof(line),
                    "    add r%d.xy, r%d, c%d\n"
                    "    texldl r%d, r%d, s9\n"
                    "    add r%d.x, r%d.x, r%d.x\n",
                    T3, T1, offset,
                    T4, T3,
                    T2, T2, T4);
                injected += line;
            }

            // c34.y = 1/5. The intermediate 180 band is not the final
            // cascade, so do not fade it toward white. Classic authority
            // showed nested selection and no proven inter-cascade blend.
            std::snprintf(
                line,
                sizeof(line),
                "    mul %s, r%d.x, c34.y\n"
                "    else\n",
                result.c_str(),
                T2);
            injected += line;

            const std::string declarations =
                "    dcl_texcoord3 v9\n"
                "    dcl_2d s9\n";

            std::string output = text;

            const std::size_t branchInjectAt =
                lines[finalElse].end;

            const std::size_t closeInjectAt =
                lines[finalEndif].begin;

            if (
                dclAt < branchInjectAt &&
                branchInjectAt < closeInjectAt)
            {
                // Insert from highest original offset to lowest so all
                // recorded positions remain valid.
                output.insert(
                    closeInjectAt,
                    "    endif\n");

                output.insert(
                    branchInjectAt,
                    injected);

                output.insert(
                    dclAt,
                    declarations);
            }
            else
            {
                return std::string();
            }

            return output;
        }

        void* MakeClassicReceiverShaderWrapper(
            const void* bytecode,
            std::uint32_t length)
        {
            auto* const device =
                static_cast<IDirect3DDevice9*>(
                    wxl::game::gx::RawDevice());

            if (
                !device ||
                !bytecode ||
                length == 0)
            {
                return nullptr;
            }

            IDirect3DPixelShader9* shader = nullptr;

            if (
                FAILED(
                    device->CreatePixelShader(
                        static_cast<const DWORD*>(
                            bytecode),
                        &shader)) ||
                !shader)
            {
                return nullptr;
            }

            auto* const copy =
                new std::uint8_t[length];

            std::memcpy(
                copy,
                bytecode,
                length);

            auto* const wrapper =
                new std::uint8_t[
                    shoff::kCgxShaderWrapBytes]();

            *reinterpret_cast<void**>(
                wrapper +
                shoff::kCgxShaderHandle) =
                    shader;

            *reinterpret_cast<std::uint32_t*>(
                wrapper +
                shoff::kCgxShaderCreated) =
                    1;

            *reinterpret_cast<std::uint32_t*>(
                wrapper +
                shoff::kCgxShaderByteLen) =
                    length;

            *reinterpret_cast<const void**>(
                wrapper +
                shoff::kCgxShaderBytePtr) =
                    copy;

            return wrapper;
        }

        void* BuildClassicCascade4ReceiverShader(
            void* stock)
        {
            if (!stock)
                return nullptr;

            const auto* const bytecode =
                *reinterpret_cast<const std::uint8_t* const*>(
                    static_cast<const std::uint8_t*>(
                        stock) +
                    shoff::kCgxShaderBytePtr);

            const std::uint32_t length =
                *reinterpret_cast<const std::uint32_t*>(
                    static_cast<const std::uint8_t*>(
                        stock) +
                    shoff::kCgxShaderByteLen);

            if (
                !bytecode ||
                length < 8 ||
                length > 0x20000)
            {
                return nullptr;
            }

            std::uint32_t version = 0;

            std::memcpy(
                &version,
                bytecode,
                sizeof(version));

            if (version != 0xFFFF0300u)
                return nullptr;

            HMODULE const compiler =
                ClassicShadowCompiler();

            const auto disassemble =
                reinterpret_cast<PFN_D3DDisassemble>(
                    compiler
                        ? GetProcAddress(
                              compiler,
                              "D3DDisassemble")
                        : nullptr);

            const auto assemble =
                reinterpret_cast<PFN_D3DAssemble>(
                    compiler
                        ? GetProcAddress(
                              compiler,
                              "D3DAssemble")
                        : nullptr);

            if (
                !disassemble ||
                !assemble)
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g3b: "
                    "d3dcompiler_47 disassemble/assemble unavailable");

                return nullptr;
            }

            ID3DBlob* textBlob = nullptr;

            if (
                FAILED(
                    disassemble(
                        bytecode,
                        length,
                        D3D_DISASM_INSTRUCTION_ONLY,
                        nullptr,
                        &textBlob)) ||
                !textBlob)
            {
                return nullptr;
            }

            const char* const disassemblyBytes =
                static_cast<const char*>(
                    textBlob->GetBufferPointer());

            const std::size_t disassemblySize =
                textBlob->GetBufferSize();

            std::string text(
                disassemblyBytes,
                disassemblySize);

            textBlob->Release();

            // D3DDisassemble may prepend human-readable metadata which
            // D3DAssemble does not necessarily accept as shader source.
            // Keep only the actual ps_3_0 program and strip comment-only
            // disassembly lines before applying the fourth-cascade surgery.
            const std::size_t profileStart =
                text.find(
                    "ps_3_0");

            if (profileStart == std::string::npos)
                return nullptr;

            text.erase(
                0,
                profileStart);

            // ID3DBlob is length-delimited data, not something we should
            // treat as an unbounded C string. Some ps_3_0 permutations
            // expose trailing NUL/non-text bytes after the valid assembly.
            //
            // Shader assembly from this point is pure ASCII. Truncate at
            // the first byte that cannot belong to the textual program,
            // then remove ordinary trailing whitespace before parsing.
            for (
                std::size_t textByte = 0;
                textByte < text.size();
                ++textByte)
            {
                const unsigned char value =
                    static_cast<unsigned char>(
                        text[textByte]);

                const bool validTextByte =
                    value == '\n' ||
                    value == '\r' ||
                    value == '\t' ||
                    (value >= 0x20 && value <= 0x7E);

                if (!validTextByte)
                {
                    text.resize(
                        textByte);
                    break;
                }
            }

            while (
                !text.empty() &&
                (
                    text.back() == ' ' ||
                    text.back() == '\t' ||
                    text.back() == '\r' ||
                    text.back() == '\n'
                ))
            {
                text.pop_back();
            }

            {
                const std::vector<ShaderLine> sourceLines =
                    SplitShaderLines(
                        text);

                std::string normalized;

                for (const ShaderLine& sourceLine : sourceLines)
                {
                    const std::string& line =
                        sourceLine.text;

                    if (
                        line.empty() ||
                        line.compare(
                            0,
                            2,
                            "//") == 0 ||
                        line[0] == ';')
                    {
                        continue;
                    }

                    std::string assemblyLine =
                        line;

                    // D3DDisassemble emits constant declarations using
                    // readable "reg = values" syntax, while D3DAssemble
                    // requires ordinary assembly operand syntax:
                    //
                    //     def c0 = x, y, z, w
                    //       ->
                    //     def c0, x, y, z, w
                    //
                    // Restrict the rewrite to definition instructions only.
                    if (
                        assemblyLine.compare(
                            0,
                            4,
                            "def ") == 0 ||
                        assemblyLine.compare(
                            0,
                            5,
                            "defi ") == 0 ||
                        assemblyLine.compare(
                            0,
                            5,
                            "defb ") == 0)
                    {
                        const std::size_t equals =
                            assemblyLine.find(
                                " = ");

                        if (equals != std::string::npos)
                        {
                            assemblyLine.replace(
                                equals,
                                3,
                                ", ");
                        }
                    }

                    // The runtime disassembler also emits floating
                    // immediates in scientific notation, e.g.
                    //
                    //     def c0, -3.44827580e+00, ...
                    //
                    // D3DAssemble rejects the exponent token in this
                    // assembly path. Re-emit only float-def operands as
                    // ordinary fixed-decimal literals.
                    if (
                        assemblyLine.compare(
                            0,
                            4,
                            "def ") == 0)
                    {
                        const std::size_t firstComma =
                            assemblyLine.find(
                                ',');

                        if (firstComma != std::string::npos)
                        {
                            double value0 = 0.0;
                            double value1 = 0.0;
                            double value2 = 0.0;
                            double value3 = 0.0;

                            if (
                                std::sscanf(
                                    assemblyLine.c_str() +
                                        firstComma + 1,
                                    " %lf, %lf, %lf, %lf",
                                    &value0,
                                    &value1,
                                    &value2,
                                    &value3) == 4)
                            {
                                char fixedDef[512] = {};

                                std::snprintf(
                                    fixedDef,
                                    sizeof(fixedDef),
                                    "%s, %.9f, %.9f, %.9f, %.9f",
                                    assemblyLine.substr(
                                        0,
                                        firstComma).c_str(),
                                    value0,
                                    value1,
                                    value2,
                                    value3);

                                assemblyLine =
                                    fixedDef;
                            }
                        }
                    }

                    // D3DDisassemble exposes the legacy specular-color
                    // input semantic as "specular0". D3D shader assembly
                    // represents that value as COLOR usage index 1.
                    //
                    //     dcl_specular0 vN
                    //       ->
                    //     dcl_color1 vN
                    //
                    // Rewrite only this exact declaration alias.
                    if (
                        assemblyLine.compare(
                            0,
                            14,
                            "dcl_specular0 ") == 0)
                    {
                        assemblyLine.replace(
                            0,
                            14,
                            "dcl_color1 ");
                    }

                    // D3DDisassemble renders ps_3_0 absolute-value
                    // source modifiers using readable vertical-bar syntax:
                    //
                    //     |r3|       -> r3_abs
                    //     |r3.x|     -> r3_abs.x
                    //     -|r3.xyz|  -> -r3_abs.xyz
                    //
                    // D3DAssemble expects the legacy _abs source modifier.
                    // Normalize every complete |...| operand on the line so
                    // all stock receiver permutations use assembler syntax.
                    std::size_t absOpen =
                        assemblyLine.find(
                            '|');

                    while (absOpen != std::string::npos)
                    {
                        const std::size_t absClose =
                            assemblyLine.find(
                                '|',
                                absOpen + 1);

                        if (absClose == std::string::npos)
                            break;

                        std::string absOperand =
                            assemblyLine.substr(
                                absOpen + 1,
                                absClose - absOpen - 1);

                        // abs(-x) == abs(x). If the disassembler ever places
                        // the negation inside the bars, discard that inner
                        // sign. A negation outside the bars is naturally
                        // preserved by replacing only the |...| span.
                        if (
                            !absOperand.empty() &&
                            absOperand[0] == '-')
                        {
                            absOperand.erase(
                                0,
                                1);
                        }

                        if (!absOperand.empty())
                        {
                            const std::size_t swizzle =
                                absOperand.find(
                                    '.');

                            if (swizzle == std::string::npos)
                            {
                                absOperand +=
                                    "_abs";
                            }
                            else
                            {
                                absOperand.insert(
                                    swizzle,
                                    "_abs");
                            }

                            assemblyLine.replace(
                                absOpen,
                                absClose - absOpen + 1,
                                absOperand);

                            absOpen =
                                assemblyLine.find(
                                    '|',
                                    absOpen +
                                        absOperand.size());
                        }
                        else
                        {
                            break;
                        }
                    }

                    normalized += assemblyLine;
                    normalized += '\n';
                }

                text.swap(
                    normalized);
            }

            const std::string patched =
                InjectClassicCascade4Receiver(
                    text);

            if (patched.empty())
                return nullptr;

            ID3DBlob* codeBlob = nullptr;
            ID3DBlob* errorBlob = nullptr;

            const HRESULT hr =
                assemble(
                    patched.c_str(),
                    patched.size(),
                    "wxlClassicCascade4Receiver",
                    nullptr,
                    nullptr,
                    0,
                    &codeBlob,
                    &errorBlob);

            if (
                FAILED(hr) ||
                !codeBlob)
            {
                // Bounded diagnostic: report the exact neighbourhood
                // around each assembler-reported failure rather than only
                // the beginning of the shader. Rendering behaviour remains
                // unchanged and stock fallback remains intact.
                static unsigned receiverAsmDiagnosticCount = 0;

                if (receiverAsmDiagnosticCount < 4)
                {
                    ++receiverAsmDiagnosticCount;

                    const std::vector<ShaderLine> diagnosticLines =
                        SplitShaderLines(
                            patched);

                    unsigned failureLine = 0;

                    const char* const errorText =
                        errorBlob
                            ? static_cast<const char*>(
                                  errorBlob->GetBufferPointer())
                            : nullptr;

                    if (errorText)
                    {
                        std::sscanf(
                            errorText,
                            "Line %u:",
                            &failureLine);
                    }

                    WLOG_WARN(
                        "wxl-modern-r5b2g3b-diag: "
                        "failure stock=%p reportedLine=%u totalLines=%u",
                        stock,
                        failureLine,
                        static_cast<unsigned>(
                            diagnosticLines.size()));

                    std::size_t firstLine = 0;
                    std::size_t lastLine =
                        diagnosticLines.size();

                    if (
                        failureLine > 0 &&
                        failureLine <= diagnosticLines.size())
                    {
                        firstLine =
                            failureLine > 5
                                ? static_cast<std::size_t>(
                                      failureLine - 5)
                                : 0;

                        lastLine =
                            static_cast<std::size_t>(
                                failureLine + 4);

                        if (lastLine > diagnosticLines.size())
                            lastLine = diagnosticLines.size();
                    }
                    else
                    {
                        lastLine =
                            diagnosticLines.size() < 32
                                ? diagnosticLines.size()
                                : 32;
                    }

                    for (
                        std::size_t diagnosticLine = firstLine;
                        diagnosticLine < lastLine;
                        ++diagnosticLine)
                    {
                        WLOG_WARN(
                            "wxl-modern-r5b2g3b-diag: "
                            "asm-context line %u: [%s]",
                            static_cast<unsigned>(
                                diagnosticLine + 1),
                            diagnosticLines[
                                diagnosticLine].text.c_str());
                    }
                }

                WLOG_WARN(
                    "wxl-modern-r5b2g3b: "
                    "receiver reassemble failed: %s",
                    errorBlob
                        ? static_cast<const char*>(
                              errorBlob->GetBufferPointer())
                        : "?");

                if (errorBlob)
                    errorBlob->Release();

                if (codeBlob)
                    codeBlob->Release();

                return nullptr;
            }

            if (errorBlob)
                errorBlob->Release();

            void* const wrapper =
                MakeClassicReceiverShaderWrapper(
                    codeBlob->GetBufferPointer(),
                    static_cast<std::uint32_t>(
                        codeBlob->GetBufferSize()));

            const std::uint32_t outputLength =
                static_cast<std::uint32_t>(
                    codeBlob->GetBufferSize());

            codeBlob->Release();

            if (wrapper)
            {
                WLOG_INFO(
                    "wxl-modern-r5b2g3b: "
                    "patched Terrain3_pcf receiver "
                    "stock=%p bytes=%u->%u "
                    "world=TEXCOORD3 sidecar180=s9 "
                    "matrixRegs=c31-c33 "
                    "nestedBeforeNative540=1 filter=5cmp",
                    stock,
                    static_cast<unsigned>(
                        length),
                    static_cast<unsigned>(
                        outputLength));
            }

            return wrapper;
        }

        void* GetClassicCascade4ReceiverShader(
            void* stock)
        {
            const auto found =
                g_classicReceiverPatchedShaders.find(
                    stock);

            if (
                found !=
                    g_classicReceiverPatchedShaders.end())
            {
                return found->second;
            }

            void* const patched =
                BuildClassicCascade4ReceiverShader(
                    stock);

            g_classicReceiverPatchedShaders.emplace(
                stock,
                patched);

            return patched;
        }

        void ReleaseClassicReceiverShaders()
        {
            for (
                auto& entry :
                g_classicReceiverPatchedShaders)
            {
                auto* const wrapper =
                    static_cast<std::uint8_t*>(
                        entry.second);

                if (!wrapper)
                    continue;

                auto* const shader =
                    *reinterpret_cast<
                        IDirect3DPixelShader9**>(
                        wrapper +
                        shoff::kCgxShaderHandle);

                if (shader)
                    shader->Release();

                auto* const bytecode =
                    *reinterpret_cast<
                        const std::uint8_t**>(
                        wrapper +
                        shoff::kCgxShaderBytePtr);

                delete[] bytecode;
                delete[] wrapper;
            }

            g_classicReceiverPatchedShaders.clear();
        }

        void CopyShadowSamplerState8To9(
            IDirect3DDevice9* device)
        {
            if (!device)
                return;

            const D3DSAMPLERSTATETYPE states[] =
            {
                D3DSAMP_ADDRESSU,
                D3DSAMP_ADDRESSV,
                D3DSAMP_ADDRESSW,
                D3DSAMP_BORDERCOLOR,
                D3DSAMP_MAGFILTER,
                D3DSAMP_MINFILTER,
                D3DSAMP_MIPFILTER,
                D3DSAMP_MIPMAPLODBIAS,
                D3DSAMP_MAXMIPLEVEL,
                D3DSAMP_MAXANISOTROPY,
                D3DSAMP_SRGBTEXTURE,
                D3DSAMP_ELEMENTINDEX,
                D3DSAMP_DMAPOFFSET
            };

            for (
                const D3DSAMPLERSTATETYPE state :
                states)
            {
                DWORD value = 0;

                if (
                    SUCCEEDED(
                        device->GetSamplerState(
                            8,
                            state,
                            &value)))
                {
                    device->SetSamplerState(
                        9,
                        state,
                        value);
                }
            }
        }

        void UploadClassicCascade4ReceiverConstants()
        {
            float constants[5][4] = {};

            // Native shadow matrices are stored column-major. The stock
            // terrain VS c37..c48 block consumes the first three matrix
            // columns as dp4 rows. Preserve that exact convention for the
            // extension-owned fourth matrix in PS c31..c33.
            for (int row = 0; row < 3; ++row)
            {
                constants[row][0] =
                    g_classicCascade4.matrix[row + 0];
                constants[row][1] =
                    g_classicCascade4.matrix[row + 4];
                constants[row][2] =
                    g_classicCascade4.matrix[row + 8];
                constants[row][3] =
                    g_classicCascade4.matrix[row + 12];
            }

            // c34:
            //   x = projected [-1,+1] -> UV scale/bias
            //   y = 1/5 for the five hardware comparison results
            //   z = one
            //   w = zero / texldl LOD
            constants[3][0] = 0.5f;
            constants[3][1] = 0.2f;
            constants[3][2] = 1.0f;
            constants[3][3] = 0.0f;

            // c35:
            //   xy = Classic final-cascade fade 0.70 -> 0.99
            //   zw = unused; the 180 -> 540 handoff now consumes the
            //        exact native outer-cascade fade operand directly.
            constants[4][0] = -3.44827586f;
            constants[4][1] =  3.41379310f;
            constants[4][2] =  0.0f;
            constants[4][3] =  0.0f;

            reinterpret_cast<
                shoff::ShaderConstantsSetHelperFn>(
                    shoff::kShaderConstantsSet)(
                        4,
                        31,
                        &constants[0][0],
                        5);
        }

        void __fastcall hkClassicTerrainReceiverDraw(
            void* node,
            void* edx)
        {
            if (
                !g_origClassicTerrainReceiverDraw ||
                !ClassicShadowsEnabled() ||
                !ClassicShadowGeometryProofEnabled() ||
                !ClassicShadowReceiverBindProofEnabled() ||
                !ClassicShadowReceiverShaderProofEnabled() ||
                !g_classicGeometryBuildHookInstalled ||
                !g_classicReceiverHooksInstalled ||
                !g_classicReceiverShaderHookInstalled ||
                !g_classicCascade4.resource ||
                !g_classicCascade4.gxObject ||
                !g_classicCascade4.matrixValid)
            {
                if (g_origClassicTerrainReceiverDraw)
                    g_origClassicTerrainReceiverDraw(
                        node,
                        edx);

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
                mapDimension != 2048 ||
                !node)
            {
                g_origClassicTerrainReceiverDraw(
                    node,
                    edx);

                return;
            }

            const std::uint32_t layers =
                *reinterpret_cast<const std::uint8_t*>(
                    static_cast<const std::uint8_t*>(
                        node) +
                    adt::kOffChunkNodeLayerCount);

            if (
                layers == 0 ||
                layers > 4)
            {
                g_origClassicTerrainReceiverDraw(
                    node,
                    edx);

                return;
            }

            void** const activeShaders =
                reinterpret_cast<void**>(
                    adt::kActiveTerrainPs);

            void* const stock =
                activeShaders[
                    layers - 1];

            if (!stock)
            {
                g_origClassicTerrainReceiverDraw(
                    node,
                    edx);

                return;
            }

            void* const patched =
                GetClassicCascade4ReceiverShader(
                    stock);

            if (!patched)
            {
                g_origClassicTerrainReceiverDraw(
                    node,
                    edx);

                return;
            }

            void* const gxDevice =
                *reinterpret_cast<void* const*>(
                    adt::kGxDeviceSingleton);

            const auto setState =
                reinterpret_cast<
                    adt::Map_SamplerBindFn>(
                    adt::kSetSamplerTexture);

            auto* const rawDevice =
                static_cast<IDirect3DDevice9*>(
                    wxl::game::gx::RawDevice());

            if (
                !gxDevice ||
                !setState ||
                !rawDevice)
            {
                g_origClassicTerrainReceiverDraw(
                    node,
                    edx);

                return;
            }

            CopyShadowSamplerState8To9(
                rawDevice);

            UploadClassicCascade4ReceiverConstants();

            setState(
                gxDevice,
                nullptr,
                adt::kClassicCascade4StatePathA,
                g_classicCascade4.gxObject);

            setState(
                gxDevice,
                nullptr,
                adt::kGxStatePixelShader,
                patched);

            g_origClassicTerrainReceiverDraw(
                node,
                edx);

            setState(
                gxDevice,
                nullptr,
                adt::kGxStatePixelShader,
                stock);

            setState(
                gxDevice,
                nullptr,
                adt::kClassicCascade4StatePathA,
                nullptr);

            static unsigned logged = 0;

            if (logged < 6)
            {
                ++logged;

                WLOG_INFO(
                    "wxl-modern-r5b2g3b: "
                    "receiver draw PASS "
                    "layers=%u stock=%p patched=%p "
                    "sampler=s9 matrixValid=1 "
                    "extents=20/60/180/540 "
                    "shaderConsumesFourth=1 "
                    "filter=5cmp",
                    static_cast<unsigned>(
                        layers),
                    stock,
                    patched);
            }
        }

        void BindClassicCascade4ReceiverTexture(
            std::int32_t gxTextureState,
            const char* pathName,
            unsigned& logCounter)
        {
            if (
                !ClassicShadowReceiverBindProofEnabled() ||
                !g_classicReceiverHooksInstalled ||
                !g_classicCascade4.resource ||
                !g_classicCascade4.gxObject ||
                !g_classicCascade4.matrixValid)
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

            void* const gxDevice =
                *reinterpret_cast<void* const*>(
                    adt::kGxDeviceSingleton);

            const auto setTexture =
                reinterpret_cast<
                    adt::Map_SamplerBindFn>(
                    adt::kSetSamplerTexture);

            if (
                !gxDevice ||
                !setTexture)
            {
                if (logCounter < 4)
                {
                    ++logCounter;

                    WLOG_WARN(
                        "wxl-modern-r5b2g3a: receiver bind refused "
                        "path=%s state=0x%02X "
                        "gxDevice=%p setTexture=%p",
                        pathName ? pathName : "unknown",
                        static_cast<unsigned>(
                            gxTextureState),
                        gxDevice,
                        reinterpret_cast<void*>(
                            setTexture));
                }

                return;
            }

            setTexture(
                gxDevice,
                nullptr,
                gxTextureState,
                g_classicCascade4.gxObject);

            if (logCounter < 4)
            {
                ++logCounter;

                const int sampler =
                    static_cast<int>(
                        gxTextureState -
                        adt::kGxStateTexture0);

                WLOG_INFO(
                    "wxl-modern-r5b2g3a: receiver sidecar bound "
                    "path=%s "
                    "state=0x%02X sampler=t%d "
                    "gx=%p matrixValid=%u "
                    "geometryFrames=%llu "
                    "extent=540 shaderConsumesFourth=0",
                    pathName ? pathName : "unknown",
                    static_cast<unsigned>(
                        gxTextureState),
                    sampler,
                    g_classicCascade4.gxObject,
                    g_classicCascade4.matrixValid
                        ? 1u
                        : 0u,
                    static_cast<unsigned long long>(
                        g_classicCascade4.geometryFrames));
            }
        }

        void __cdecl hkBindTerrainShadowMap()
        {
            if (g_origBindTerrainShadowMap)
                g_origBindTerrainShadowMap();

            static unsigned logged = 0;

            BindClassicCascade4ReceiverTexture(
                adt::kClassicCascade4StatePathA,
                "A",
                logged);
        }

        void __cdecl hkBindTerrainShadowMapAlt()
        {
            if (g_origBindTerrainShadowMapAlt)
                g_origBindTerrainShadowMapAlt();

            static unsigned logged = 0;

            BindClassicCascade4ReceiverTexture(
                adt::kClassicCascade4StatePathB,
                "B",
                logged);
        }

        void __cdecl hkBuildShadowCascades(
            void* shadowObject,
            void* context,
            void* helper,
            std::int32_t mode)
        {
            // G2R1 correction:
            //
            // 0x874890 is the actual matrix/caster BUILD seam.
            // The previous G2 candidate prepared extents from the
            // 0x874FB0 dispatcher hook, which is too late because
            // 0x874890 has already generated the matrices by then.
            //
            // Prepare 20/60/640 here, immediately before the exact
            // original build call.  Slot 2 remains broad at 640 so
            // its valid full-caster candidate set covers both the
            // post-build 180 map and fourth 540 sidecar.
            if (
                g_classicGeometryBuildHookInstalled &&
                ClassicShadowsEnabled() &&
                ClassicShadowGeometryProofEnabled() &&
                shadowObject)
            {
                PrepareClassicShadowGeometry(
                    shadowObject);
            }

            if (g_origBuildShadowCascades)
            {
                g_origBuildShadowCascades(
                    shadowObject,
                    context,
                    helper,
                    mode);
            }
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

            // R5B2-G2: staged launch-Classic four-band geometry.
            //
            // Stock build supplies 20 / 60 / broad-640.  G2 then
            // rerenders legal slot 2 as 180 and renders the fourth
            // sidecar as 540 from the same broad full-caster set.
            RenderClassicShadowGeometry(
                shadowObject);

            // R5B2-G1: current-frame 540 producer join.
            //
            // Clone the already-valid post-native shadow object, rebuild
            // only legal slot 2 at Classic's 540 half-extent using the
            // registered native matrix builder, then consume the already
            // proven current-frame slot-2 full-caster workspace while
            // redirecting only the destination to the fourth sidecar.
            //
            // Native object/cascades are never modified and index 3 is
            // never presented to native code.
            TryCurrentFrameClone540CasterProof(
                shadowObject);

            // R5B2-E1: independently opt-in build-only proof.
            //
            // Construct extension-owned synthetic shadow state and ask
            // Wrath's exact registered builder for a legal slot-0 matrix.
            // No finalizer, caster, render-target or receiver call occurs.
            TrySyntheticCascade4MatrixProof();

            // R5B2-D1: opt-in, one-shot target-path proof.  Re-render the
            // already-valid native cascade #2 through the exact native caster
            // callback, but direct its per-cascade destination to the
            // extension-owned sidecar.  Legal native index 2 only.
            //
            // There is still no fourth matrix and no receiver binding.
            TryCascade2CasterIntoSidecarProof(
                shadowObject);

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
            // Install the true build seam first.  The hook remains
            // completely dormant until BOTH hooks have installed and
            // g_classicGeometryBuildHookInstalled becomes true.
            const bool buildInstalled =
                wxl::hook::Install(
                    "R5ClassicShadowBuild",
                    adt::kBuildShadowCascades,
                    &hkBuildShadowCascades,
                    &g_origBuildShadowCascades);

            const bool renderInstalled =
                buildInstalled &&
                wxl::hook::Install(
                    "R5ClassicShadowRenderDispatcher",
                    adt::kRenderShadowCascades,
                    &hkRenderShadowCascades,
                    &g_origRenderShadowCascades);

            g_classicGeometryBuildHookInstalled =
                buildInstalled &&
                renderInstalled;

            const bool receiverAInstalled =
                g_classicGeometryBuildHookInstalled &&
                wxl::hook::Install(
                    "R5ClassicTerrainShadowBindA",
                    adt::kBindTerrainShadowMap,
                    &hkBindTerrainShadowMap,
                    &g_origBindTerrainShadowMap);

            const bool receiverBInstalled =
                receiverAInstalled &&
                wxl::hook::Install(
                    "R5ClassicTerrainShadowBindB",
                    adt::kBindTerrainShadowMapAlt,
                    &hkBindTerrainShadowMapAlt,
                    &g_origBindTerrainShadowMapAlt);

            g_classicReceiverHooksInstalled =
                receiverAInstalled &&
                receiverBInstalled;

            const bool receiverShaderInstalled =
                g_classicReceiverHooksInstalled &&
                wxl::hook::Install(
                    "R5ClassicTerrainReceiverShader",
                    adt::kSurfaceChunkDrawShader,
                    &hkClassicTerrainReceiverDraw,
                    &g_origClassicTerrainReceiverDraw);

            g_classicReceiverShaderHookInstalled =
                receiverShaderInstalled;

            if (
                g_classicGeometryBuildHookInstalled &&
                g_classicReceiverHooksInstalled &&
                g_classicReceiverShaderHookInstalled)
            {
                WLOG_INFO(
                    "wxl-modern-r5b2g2r1: Classic shadow build/render "
                    "hooks installed enabled=%u "
                    "build=0x%08X dispatcher=0x%08X "
                    "callback=0x%08X",
                    ClassicShadowsEnabled() ? 1u : 0u,
                    static_cast<unsigned>(
                        adt::kBuildShadowCascades),
                    static_cast<unsigned>(
                        adt::kRenderShadowCascades),
                    static_cast<unsigned>(
                        adt::kShadowCascadeRenderCallback));

                WLOG_INFO(
                    "wxl-modern-r5b2g3b: receiver hooks installed "
                    "pathA=0x%08X fourthStateA=0x%02X samplerA=t9 "
                    "pathB=0x%08X fourthStateB=0x%02X samplerB=t8 "
                    "terrainDraw=0x%08X shaderProofEnabled=%u",
                    static_cast<unsigned>(
                        adt::kBindTerrainShadowMap),
                    static_cast<unsigned>(
                        adt::kClassicCascade4StatePathA),
                    static_cast<unsigned>(
                        adt::kBindTerrainShadowMapAlt),
                    static_cast<unsigned>(
                        adt::kClassicCascade4StatePathB),
                    static_cast<unsigned>(
                        adt::kSurfaceChunkDrawShader),
                    ClassicShadowReceiverShaderProofEnabled()
                        ? 1u
                        : 0u);
            }
            else
            {
                WLOG_WARN(
                    "wxl-modern-r5b2g3b: Classic shadow hook install "
                    "incomplete build=%u render=%u "
                    "receiverA=%u receiverB=%u receiverShader=%u",
                    buildInstalled ? 1u : 0u,
                    renderInstalled ? 1u : 0u,
                    receiverAInstalled ? 1u : 0u,
                    receiverBInstalled ? 1u : 0u,
                    receiverShaderInstalled ? 1u : 0u);
            }

            return
                buildInstalled &&
                renderInstalled &&
                receiverAInstalled &&
                receiverBInstalled &&
                receiverShaderInstalled;
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

            ReleaseClassicReceiverShaders();

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
            g_classicCascade4.casterProofReturned = false;

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
