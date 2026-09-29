// R6 E4G-A first-candidate runtime bridge. GPL-3.0-or-later.

#include "water/Slot3FirstCandidateRuntime.hpp"

#ifdef _WIN32

#include "water/Slot3ReflectionWin32.hpp"

#include "client/CWorldScene/LiquidDiagnostics.hpp"
#include "engine/hook/Hook.hpp"
#include "offsets/engine/Liquid.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>

#include <d3d9.h>

namespace wxl::water::slot3
{
namespace
{
    namespace liquid =
        wxl::offsets::engine::liquid;

    constexpr std::uintptr_t kAnimationTime =
        0x0086AE20;

    using AnimationTimeFn =
        std::uint32_t(__cdecl*)();

    static_assert(
        sizeof(void*) == 4,
        "R6 address/layout authority is the 32-bit Wrath client");

    // Same function byte-gated and live-qualified by the temporary Step-1
    // provenance probe.
    constexpr std::uintptr_t
        kLightingConstantBuild = 0x008A38B0u;

    using LightingConstantBuildFn =
        void(__cdecl*)(void* record);

    LightingConstantBuildFn
        lightingConstantBuildOriginal = nullptr;

    struct LightingRuntimeCapture
    {
        std::uint64_t invocation = 0;
        std::uintptr_t settings = 0;

        Float4 rawWorldDirection{};
        Float4 nativeAmbient{};
        Float4 nativeDiffuse{};
        Float4 nativeSpecular{};

        bool ready = false;
    };

    // The native liquid Context is thread-local and the renderer consumes this
    // record on the same render thread.  Keep the capture equally local so a
    // stale record can never cross threads.
    thread_local LightingRuntimeCapture
        lightingRuntimeCapture{};

    void ClearLightingRuntimeCapture() noexcept
    {
        lightingRuntimeCapture = {};
    }

    void __cdecl FirstCandidateLightingBuildCapture(
        void* record)
    {
        const auto* context =
            wxl::waterdiag::native::Current();

        const bool eligible =
            context &&
            context->family ==
                wxl::waterdiag::Family::Water &&
            context->pass == 1 &&
            context->settings &&
            record;

        Float4 rawDirection{{
            0.0f,
            0.0f,
            0.0f,
            1.0f
        }};

        bool rawDirectionReady = false;

        if (eligible)
        {
            // Direction starts at transient record +0x78:
            // +0x78 / +0x7C / +0x80.
            rawDirectionReady =
                wxl::waterdiag::native::Read(
                    static_cast<const char*>(
                        record) +
                        0x78,
                    rawDirection.data(),
                    3u * sizeof(float));

            // Never permit an earlier capture from this same invocation to
            // survive a later failed producer call.
            ClearLightingRuntimeCapture();
        }

        // Hook installation is accepted only after the original trampoline is
        // returned by MinHook.  This branch is defensive; it is not a native
        // fallback path.
        if (!lightingConstantBuildOriginal)
            return;

        // Native function remains authoritative and executes exactly once.
        lightingConstantBuildOriginal(
            record);

        if (!eligible ||
            !rawDirectionReady)
        {
            return;
        }

        std::array<Float4, 3>
            nativeRows{};

        // After native 0x008A38B0:
        //   VS c34 = ambient
        //   VS c35 = diffuse
        //   VS c36 = specular RGB + native exponent
        //
        // c33 is intentionally skipped.
        if (!wxl::waterdiag::native::Read(
                reinterpret_cast<const void*>(
                    liquid::kVsConstBlock +
                    34u * sizeof(Float4)),
                nativeRows.data(),
                sizeof(nativeRows)))
        {
            return;
        }

        // Re-run the exact Step2C evidence gate before a transient record may
        // become production-consumable.
        std::array<Float4, 6>
            validatedRows{};

        if (!BuildExactFirstCandidateLightingRows(
                rawDirection,
                nativeRows[0],
                nativeRows[1],
                nativeRows[2],
                validatedRows))
        {
            return;
        }

        LightingRuntimeCapture
            candidate{};

        candidate.invocation =
            context->invocation;

        candidate.settings =
            reinterpret_cast<std::uintptr_t>(
                context->settings);

        candidate.rawWorldDirection =
            rawDirection;

        candidate.nativeAmbient =
            nativeRows[0];

        candidate.nativeDiffuse =
            nativeRows[1];

        candidate.nativeSpecular =
            nativeRows[2];

        candidate.ready = true;

        lightingRuntimeCapture =
            candidate;
    }
}

bool ResolveFirstCandidateLiquidTypeId(
    const void* settings,
    std::uint32_t& liquidTypeId) noexcept
{
    if (!settings)
        return false;

    const auto count =
        *reinterpret_cast<const std::uint32_t*>(
            liquid::kSettingsBankCount);

    if (count == 0 ||
        count > 4096)
    {
        return false;
    }

    // E4 live capture correction:
    //
    // 0x00D43B18 = row count
    // 0x00D43B1C = pointer to row-pointer array
    //
    // Do NOT treat kSettingsBankRows itself as row[0].
    const auto rowsBase =
        *reinterpret_cast<const std::uint32_t*>(
            liquid::kSettingsBankRows);

    if (!rowsBase)
        return false;

    const auto target =
        static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(
                settings));

    for (std::uint32_t id = 0;
         id < count;
         ++id)
    {
        const auto row =
            *reinterpret_cast<const std::uint32_t*>(
                static_cast<std::uintptr_t>(
                    rowsBase) +
                static_cast<std::uintptr_t>(
                    id) *
                    sizeof(std::uint32_t));

        if (row != target)
            continue;

        // The first candidate is exact launch-Classic IDs 1/5/9 only.
        // Wrath payload lookalikes 41/61/81 remain rejected.
        if (!ClassicP01LiquidTypeSupported(
                id))
        {
            return false;
        }

        liquidTypeId =
            id;

        return true;
    }

    return false;
}

bool CaptureFirstCandidateAnimationTime(
    std::uint32_t& animationTime) noexcept
{
    const auto function =
        reinterpret_cast<AnimationTimeFn>(
            kAnimationTime);

    if (!function)
        return false;

    animationTime =
        function();

    return true;
}

bool PopulateFirstCandidateStaticRuntime(
    const void* settings,
    FirstCandidateRuntimeRows& output) noexcept
{
    std::uint32_t liquidTypeId = 0;
    std::uint32_t animationTime = 0;

    if (!ResolveFirstCandidateLiquidTypeId(
            settings,
            liquidTypeId) ||
        !CaptureFirstCandidateAnimationTime(
            animationTime))
    {
        return false;
    }

    FirstCandidateRuntimeRows candidate =
        output;

    if (!ApplyExactFirstCandidateStaticRuntime(
            liquidTypeId,
            animationTime,
            candidate) ||
        !ApplyExactFirstCandidateStaticUv(
            candidate))
    {
        return false;
    }

    output =
        candidate;

    return true;
}

bool PopulateFirstCandidateWorldRuntime(
    const void* world,
    FirstCandidateRuntimeRows& output) noexcept
{
    return
        ApplyGroundedFirstCandidateWorldPointer(
            world,
            output);
}

bool PopulateFirstCandidateDrawRuntime(
    const void* settings,
    const void* world,
    std::uint32_t viewportX,
    std::uint32_t viewportY,
    std::uint32_t viewportWidth,
    std::uint32_t viewportHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    FirstCandidateRuntimeRows& output) noexcept
{
    FirstCandidateRuntimeRows candidate =
        output;

    WrathP01ConstantSnapshot snapshot{};

    if (!PopulateFirstCandidateStaticRuntime(
            settings,
            candidate) ||
        !PopulateFirstCandidateWorldRuntime(
            world,
            candidate) ||
        !ApplyExactFirstCandidateFullTargetPs31(
            viewportX,
            viewportY,
            viewportWidth,
            viewportHeight,
            targetWidth,
            targetHeight,
            candidate) ||
        !CaptureWrathP01ConstantSnapshot(
            settings,
            snapshot) ||
        !ApplyExactFirstCandidateRiverWaterColour(
            snapshot,
            candidate))
    {
        return false;
    }

    output =
        candidate;

    return true;
}



bool CaptureFirstCandidateWaterPlane(
    IDirect3DDevice9* device,
    unsigned primitiveType,
    std::int32_t baseVertex,
    std::uint32_t minVertex,
    std::uint32_t vertexCount,
    std::uint32_t startIndex,
    std::uint32_t primitiveCount,
    float& waterPlane) noexcept
{
    // This is deliberately the narrow production form of the Step2G/2R
    // diagnostic.  It recovers only the one scalar whose provenance is now
    // closed; it does not rescan the complete mesh or derive statistics.

    if (!device ||
        primitiveType !=
            static_cast<unsigned>(
                D3DPT_TRIANGLESTRIP) ||
        vertexCount == 0u ||
        primitiveCount == 0u)
    {
        return false;
    }

    IDirect3DVertexDeclaration9*
        declaration = nullptr;

    HRESULT hr =
        device->GetVertexDeclaration(
            &declaration);

    if (FAILED(hr) ||
        !declaration)
    {
        return false;
    }

    std::array<D3DVERTEXELEMENT9, 65>
        elements{};

    UINT elementCount =
        static_cast<UINT>(
            elements.size());

    hr =
        declaration->GetDeclaration(
            elements.data(),
            &elementCount);

    declaration->Release();
    declaration = nullptr;

    if (FAILED(hr) ||
        elementCount == 0u ||
        elementCount >
            elements.size())
    {
        return false;
    }

    bool positionFound = false;
    WORD positionOffset = 0;
    BYTE positionType =
        D3DDECLTYPE_UNUSED;

    for (UINT i = 0;
         i < elementCount;
         ++i)
    {
        const D3DVERTEXELEMENT9&
            element =
                elements[i];

        if (element.Stream == 0xFFu)
            break;

        if (element.Stream == 0u &&
            element.Usage ==
                D3DDECLUSAGE_POSITION &&
            element.UsageIndex == 0u)
        {
            positionFound = true;
            positionOffset =
                element.Offset;
            positionType =
                element.Type;
            break;
        }
    }

    if (!positionFound ||
        (positionType !=
             D3DDECLTYPE_FLOAT3 &&
         positionType !=
             D3DDECLTYPE_FLOAT4))
    {
        return false;
    }

    IDirect3DVertexBuffer9*
        vertexBuffer = nullptr;

    UINT streamOffset = 0;
    UINT stride = 0;

    hr =
        device->GetStreamSource(
            0,
            &vertexBuffer,
            &streamOffset,
            &stride);

    if (FAILED(hr) ||
        !vertexBuffer ||
        stride <
            static_cast<UINT>(
                positionOffset) +
            12u)
    {
        if (vertexBuffer)
            vertexBuffer->Release();

        return false;
    }

    IDirect3DIndexBuffer9*
        indexBuffer = nullptr;

    hr =
        device->GetIndices(
            &indexBuffer);

    if (FAILED(hr) ||
        !indexBuffer)
    {
        vertexBuffer->Release();
        return false;
    }

    D3DVERTEXBUFFER_DESC
        vertexDesc{};

    D3DINDEXBUFFER_DESC
        indexDesc{};

    const HRESULT vertexDescHr =
        vertexBuffer->GetDesc(
            &vertexDesc);

    const HRESULT indexDescHr =
        indexBuffer->GetDesc(
            &indexDesc);

    if (FAILED(vertexDescHr) ||
        FAILED(indexDescHr))
    {
        indexBuffer->Release();
        vertexBuffer->Release();
        return false;
    }

    UINT indexSize = 0;

    if (indexDesc.Format ==
        D3DFMT_INDEX16)
    {
        indexSize = 2u;
    }
    else if (indexDesc.Format ==
             D3DFMT_INDEX32)
    {
        indexSize = 4u;
    }
    else
    {
        indexBuffer->Release();
        vertexBuffer->Release();
        return false;
    }

    const std::uint64_t
        indexByteOffset =
            std::uint64_t(
                startIndex) *
            std::uint64_t(
                indexSize);

    if (indexByteOffset >
            indexDesc.Size ||
        std::uint64_t(indexSize) >
            indexDesc.Size ||
        indexByteOffset +
            std::uint64_t(indexSize) >
            indexDesc.Size)
    {
        indexBuffer->Release();
        vertexBuffer->Release();
        return false;
    }

    void* indexBytes = nullptr;

    const HRESULT indexLockHr =
        indexBuffer->Lock(
            static_cast<UINT>(
                indexByteOffset),
            indexSize,
            &indexBytes,
            D3DLOCK_READONLY);

    if (FAILED(indexLockHr) ||
        !indexBytes)
    {
        indexBuffer->Release();
        vertexBuffer->Release();
        return false;
    }

    std::uint32_t index = 0;

    if (indexSize == 2u)
    {
        std::uint16_t value = 0;

        std::memcpy(
            &value,
            indexBytes,
            sizeof(value));

        index = value;
    }
    else
    {
        std::memcpy(
            &index,
            indexBytes,
            sizeof(index));
    }

    const HRESULT indexUnlockHr =
        indexBuffer->Unlock();

    indexBuffer->Release();
    indexBuffer = nullptr;

    if (FAILED(indexUnlockHr))
    {
        vertexBuffer->Release();
        return false;
    }

    const std::uint64_t
        declaredEnd =
            std::uint64_t(
                minVertex) +
            std::uint64_t(
                vertexCount);

    if (std::uint64_t(index) <
            std::uint64_t(minVertex) ||
        std::uint64_t(index) >=
            declaredEnd)
    {
        vertexBuffer->Release();
        return false;
    }

    const std::int64_t
        actualSigned =
            std::int64_t(
                baseVertex) +
            std::int64_t(
                index);

    if (actualSigned < 0)
    {
        vertexBuffer->Release();
        return false;
    }

    const std::uint64_t
        actualVertex =
            static_cast<std::uint64_t>(
                actualSigned);

    const std::uint64_t
        positionByteOffset =
            std::uint64_t(
                streamOffset) +
            actualVertex *
                std::uint64_t(
                    stride) +
            std::uint64_t(
                positionOffset);

    if (positionByteOffset >
            vertexDesc.Size ||
        positionByteOffset + 12u >
            vertexDesc.Size)
    {
        vertexBuffer->Release();
        return false;
    }

    void* positionBytes = nullptr;

    const HRESULT vertexLockHr =
        vertexBuffer->Lock(
            static_cast<UINT>(
                positionByteOffset),
            12u,
            &positionBytes,
            D3DLOCK_READONLY);

    if (FAILED(vertexLockHr) ||
        !positionBytes)
    {
        vertexBuffer->Release();
        return false;
    }

    float position[3]{};

    std::memcpy(
        position,
        positionBytes,
        sizeof(position));

    const HRESULT vertexUnlockHr =
        vertexBuffer->Unlock();

    vertexBuffer->Release();
    vertexBuffer = nullptr;

    if (FAILED(vertexUnlockHr) ||
        !std::isfinite(position[0]) ||
        !std::isfinite(position[1]) ||
        !std::isfinite(position[2]))
    {
        return false;
    }

    // Commit only after the exact sample has survived every native buffer,
    // declaration, index-range and finite-value gate.
    waterPlane =
        position[2];

    return true;
}

bool InstallFirstCandidateLightingCapture() noexcept
{
    if (lightingConstantBuildOriginal)
        return true;

    static constexpr std::array<
        std::uint8_t,
        16>
    kExpectedLightingBuild{{
        0x55, 0x8b, 0xec, 0x81,
        0xec, 0xa4, 0x00, 0x00,
        0x00, 0xd9, 0xe8, 0xa1,
        0x88, 0xdf, 0xc5, 0x00
    }};

    std::array<std::uint8_t, 16>
        actual{};

    if (!wxl::waterdiag::native::Read(
            reinterpret_cast<const void*>(
                kLightingConstantBuild),
            actual.data(),
            actual.size()) ||
        actual !=
            kExpectedLightingBuild)
    {
        return false;
    }

    ClearLightingRuntimeCapture();

    if (!wxl::hook::Install(
            "R6.FirstCandidateLightingCapture",
            kLightingConstantBuild,
            &FirstCandidateLightingBuildCapture,
            &lightingConstantBuildOriginal,
            100))
    {
        lightingConstantBuildOriginal =
            nullptr;

        ClearLightingRuntimeCapture();
        return false;
    }

    return true;
}

void ResetFirstCandidateLightingCapture() noexcept
{
    ClearLightingRuntimeCapture();
}

bool PopulateFirstCandidateLightingRuntime(
    std::uint64_t invocation,
    const void* settings,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!invocation || !settings || !lightingRuntimeCapture.ready ||
        lightingRuntimeCapture.invocation != invocation ||
        lightingRuntimeCapture.settings != reinterpret_cast<std::uintptr_t>(settings))
        return false;
    return ApplyGroundedFirstCandidateLightingRows(
        lightingRuntimeCapture.rawWorldDirection,
        lightingRuntimeCapture.nativeAmbient,
        lightingRuntimeCapture.nativeDiffuse,
        lightingRuntimeCapture.nativeSpecular,
        output);
}

bool PopulateFirstCandidateDynamicRuntime(
    std::uint64_t invocation,
    const void* settings,
    std::uint64_t consumerOrdinal,
    const NativeReflectionCandidate& reflection,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!invocation ||
        !settings ||
        !consumerOrdinal ||
        !lightingRuntimeCapture.ready ||
        lightingRuntimeCapture.invocation !=
            invocation ||
        lightingRuntimeCapture.settings !=
            reinterpret_cast<std::uintptr_t>(
                settings) ||
        reflection.key.consumerOrdinal !=
            consumerOrdinal)
    {
        return false;
    }

    FirstCandidateRuntimeRows
        candidate =
            output;

    if (!ApplyGroundedFirstCandidateLightingRows(
            lightingRuntimeCapture.
                rawWorldDirection,
            lightingRuntimeCapture.
                nativeAmbient,
            lightingRuntimeCapture.
                nativeDiffuse,
            lightingRuntimeCapture.
                nativeSpecular,
            candidate) ||
        !ApplyNativeReflectionCandidateRuntime(
            reflection,
            candidate))
    {
        return false;
    }

    // Both dynamic row groups must become ready together.
    if (!candidate.lightingReady ||
        !candidate.reflectionReady)
    {
        return false;
    }

    output =
        candidate;

    return true;
}


} // namespace wxl::water::slot3

#endif
