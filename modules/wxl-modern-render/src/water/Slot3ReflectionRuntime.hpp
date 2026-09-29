#pragma once

// R6 P01/A0/B1/Q0 reflection producer contract.
// GPL-3.0-or-later.
//
// This file deliberately separates evidence-backed reflection behaviour from
// the still-unresolved Classic-engine-format -> D3D9-format translation.
//
// Closed authority used here:
//   - B1/t1 is the selected-mode real mirrored reflection.
//   - reflection is across the horizontal water plane.
//   - mode 1 = sky
//   - mode 2 = sky + terrain
//   - mode 3 = sky + terrain + WMO
//   - raw Classic reflection dimensions are parent dimensions divided by
//     2^reflectionDownsampleShift.
//   - the Classic resource builder receives internal engine format ID 6.
//
// Engine format ID 6 is NOT assigned a guessed D3D9 D3DFORMAT here.
// d3dColourFormat must therefore arrive from a separately grounded mapping.
// Zero means that mapping is unavailable and the producer fails closed.
//
// No function in this header binds a D3D resource, calls a native renderer,
// changes camera state, or enables the final water replacement.

#include <array>
#include <cmath>
#include <cstdint>

namespace wxl::water::slot3
{

struct ReflectionVec3
{
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

// Exact native camera matrix representation.
//
// Wrath Camera.hpp establishes these globals as float[16], row-major,
// D3D row-vector convention.  Keep the representation explicit rather than
// silently transposing or converting it.
using ReflectionMatrix4 =
    std::array<std::array<float, 4>, 4>;

inline bool Finite(
    const ReflectionMatrix4& value) noexcept
{
    for (const auto& row : value)
    {
        for (float component : row)
        {
            if (!std::isfinite(component))
                return false;
        }
    }

    return true;
}

inline bool Finite(
    const ReflectionVec3& value) noexcept
{
    return
        std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

inline bool Same(
    const ReflectionVec3& a,
    const ReflectionVec3& b) noexcept
{
    return
        a.x == b.x &&
        a.y == b.y &&
        a.z == b.z;
}

enum ReflectionContent : std::uint32_t
{
    ReflectionSky = 1u << 0,
    ReflectionTerrain = 1u << 1,
    ReflectionWmo = 1u << 2,
};

inline bool SupportedReflectionMode(
    unsigned mode) noexcept
{
    return
        mode >= 1u &&
        mode <= 3u;
}

inline std::uint32_t ReflectionContentMask(
    unsigned mode) noexcept
{
    if (!SupportedReflectionMode(mode))
        return 0;

    std::uint32_t result =
        ReflectionSky;

    if (mode >= 2u)
        result |= ReflectionTerrain;

    if (mode >= 3u)
        result |= ReflectionWmo;

    return result;
}

// Only the WMO-inclusive mode needs the reflected viewer/group-location
// preparation that accompanies WMO visibility/portal state.
inline bool ReflectionNeedsViewerLocate(
    unsigned mode) noexcept
{
    return mode == 3u;
}

struct ReflectionRequest
{
    // Same-frame/native-source identity.
    std::uintptr_t device = 0;
    std::uintptr_t sourceRt = 0;
    std::uint64_t generation = 0;
    std::uint64_t scene = 0;
    std::uint64_t frame = 0;
    std::uint64_t consumerOrdinal = 0;

    // Exact parent-size -> reflected-size relationship recovered from Classic.
    std::uint32_t parentWidth = 0;
    std::uint32_t parentHeight = 0;
    std::uint32_t downsampleShift = 0;

    // Explicit unresolved translation gate.
    // This is a D3D9 format value supplied only after separate evidence closes
    // Classic internal engine format ID 6 -> D3D9 representation.
    // 0 / D3DFMT_UNKNOWN is deliberately rejected.
    std::uint32_t d3dColourFormat = 0;

    unsigned mode = 0;
    float plane = 0.f;

    // Exact native inputs consumed by Wrath's camera builder.
    ReflectionVec3 eye{};
    ReflectionVec3 target{};
};

struct ReflectionPlan
{
    bool valid = false;

    std::uintptr_t device = 0;
    std::uintptr_t sourceRt = 0;
    std::uint64_t generation = 0;
    std::uint64_t scene = 0;
    std::uint64_t frame = 0;
    std::uint64_t consumerOrdinal = 0;

    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    std::uint32_t downsampleShift = 0;
    std::uint32_t d3dColourFormat = 0;

    unsigned mode = 0;
    std::uint32_t contentMask = 0;
    float plane = 0.f;

    ReflectionVec3 originalEye{};
    ReflectionVec3 originalTarget{};
    ReflectionVec3 reflectedEye{};
    ReflectionVec3 reflectedTarget{};
};

inline bool ReflectAcrossHorizontalPlane(
    const ReflectionVec3& input,
    float plane,
    ReflectionVec3& output) noexcept
{
    if (!Finite(input) ||
        !std::isfinite(plane))
    {
        return false;
    }

    ReflectionVec3 candidate =
        input;

    // Exact horizontal-plane mirror:
    //
    //     z' = 2h - z
    //
    // No epsilon, clamp, visual offset or guessed clipping bias.
    candidate.z =
        2.f * plane -
        input.z;

    if (!Finite(candidate))
        return false;

    output = candidate;
    return true;
}

inline bool BuildReflectionPlan(
    const ReflectionRequest& request,
    ReflectionPlan& output) noexcept
{
    if (!request.device ||
        !request.sourceRt ||
        !request.generation ||
        !request.scene ||
        !request.frame ||
        !request.consumerOrdinal ||
        !request.parentWidth ||
        !request.parentHeight ||
        !request.d3dColourFormat ||
        !SupportedReflectionMode(
            request.mode) ||
        !std::isfinite(
            request.plane) ||
        !Finite(
            request.eye) ||
        !Finite(
            request.target))
    {
        return false;
    }

    // Classic computes 1 << shift and integer-divides both parent dimensions.
    // Reject shifts that cannot be represented safely in this 32-bit contract.
    if (request.downsampleShift >= 31u)
        return false;

    const std::uint32_t divisor =
        1u <<
        request.downsampleShift;

    const std::uint32_t width =
        request.parentWidth /
        divisor;

    const std::uint32_t height =
        request.parentHeight /
        divisor;

    if (!width ||
        !height)
    {
        return false;
    }

    ReflectionVec3 reflectedEye{};
    ReflectionVec3 reflectedTarget{};

    if (!ReflectAcrossHorizontalPlane(
            request.eye,
            request.plane,
            reflectedEye) ||
        !ReflectAcrossHorizontalPlane(
            request.target,
            request.plane,
            reflectedTarget))
    {
        return false;
    }

    ReflectionPlan candidate{};

    candidate.device =
        request.device;

    candidate.sourceRt =
        request.sourceRt;

    candidate.generation =
        request.generation;

    candidate.scene =
        request.scene;

    candidate.frame =
        request.frame;

    candidate.consumerOrdinal =
        request.consumerOrdinal;

    candidate.targetWidth =
        width;

    candidate.targetHeight =
        height;

    candidate.downsampleShift =
        request.downsampleShift;

    candidate.d3dColourFormat =
        request.d3dColourFormat;

    candidate.mode =
        request.mode;

    candidate.contentMask =
        ReflectionContentMask(
            request.mode);

    candidate.plane =
        request.plane;

    candidate.originalEye =
        request.eye;

    candidate.originalTarget =
        request.target;

    candidate.reflectedEye =
        reflectedEye;

    candidate.reflectedTarget =
        reflectedTarget;

    candidate.valid = true;

    output = candidate;

    return true;
}

struct ReflectionResourceKey
{
    std::uintptr_t device = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t d3dColourFormat = 0;
};

inline ReflectionResourceKey ResourceKey(
    const ReflectionPlan& plan) noexcept
{
    return {
        plan.device,
        plan.targetWidth,
        plan.targetHeight,
        plan.d3dColourFormat,
    };
}

inline bool SameReflectionResources(
    const ReflectionResourceKey& a,
    const ReflectionResourceKey& b) noexcept
{
    return
        a.device == b.device &&
        a.width == b.width &&
        a.height == b.height &&
        a.d3dColourFormat ==
            b.d3dColourFormat;
}

struct ReflectionContentKey
{
    ReflectionResourceKey resources{};

    std::uintptr_t sourceRt = 0;
    std::uint64_t generation = 0;
    std::uint64_t scene = 0;
    std::uint64_t frame = 0;
    std::uint64_t producerOrdinal = 0;
    std::uint64_t consumerOrdinal = 0;

    unsigned mode = 0;
    float plane = 0.f;

    ReflectionVec3 eye{};
    ReflectionVec3 target{};
};

inline bool BuildReflectionContentKey(
    const ReflectionPlan& plan,
    std::uint64_t producerOrdinal,
    ReflectionContentKey& output) noexcept
{
    if (!plan.valid ||
        !producerOrdinal ||
        producerOrdinal >=
            plan.consumerOrdinal)
    {
        return false;
    }

    ReflectionContentKey candidate{};

    candidate.resources =
        ResourceKey(plan);

    candidate.sourceRt =
        plan.sourceRt;

    candidate.generation =
        plan.generation;

    candidate.scene =
        plan.scene;

    candidate.frame =
        plan.frame;

    candidate.producerOrdinal =
        producerOrdinal;

    candidate.consumerOrdinal =
        plan.consumerOrdinal;

    candidate.mode =
        plan.mode;

    candidate.plane =
        plan.plane;

    candidate.eye =
        plan.originalEye;

    candidate.target =
        plan.originalTarget;

    output = candidate;

    return true;
}

inline bool SameReflectionContent(
    const ReflectionContentKey& a,
    const ReflectionContentKey& b) noexcept
{
    return
        SameReflectionResources(
            a.resources,
            b.resources) &&
        a.sourceRt == b.sourceRt &&
        a.generation == b.generation &&
        a.scene == b.scene &&
        a.frame == b.frame &&
        a.producerOrdinal ==
            b.producerOrdinal &&
        a.consumerOrdinal ==
            b.consumerOrdinal &&
        a.mode == b.mode &&
        a.plane == b.plane &&
        Same(a.eye, b.eye) &&
        Same(a.target, b.target);
}

struct ReflectionLifecycleState
{
    bool resourcesReady = false;
    bool contentReady = false;

    ReflectionResourceKey resourceKey{};
    ReflectionContentKey contentKey{};

    void Reset() noexcept
    {
        resourcesReady = false;
        contentReady = false;
        resourceKey = {};
        contentKey = {};
    }

    bool ResourcesMatch(
        const ReflectionResourceKey& key) const noexcept
    {
        return
            resourcesReady &&
            SameReflectionResources(
                resourceKey,
                key);
    }

    void MarkResourcesReady(
        const ReflectionResourceKey& key) noexcept
    {
        resourceKey = key;
        resourcesReady = true;
        contentReady = false;
        contentKey = {};
    }

    void InvalidateContent() noexcept
    {
        contentReady = false;
        contentKey = {};
    }

    bool MarkContentReady(
        const ReflectionContentKey& key) noexcept
    {
        if (!ResourcesMatch(
                key.resources))
        {
            return false;
        }

        contentKey = key;
        contentReady = true;
        return true;
    }

    bool ContentMatches(
        const ReflectionContentKey& key) const noexcept
    {
        return
            contentReady &&
            SameReflectionContent(
                contentKey,
                key);
    }
};

enum class ReflectionSequenceStatus :
    std::uint8_t
{
    Produced,
    UnavailableRestored,
    RestoreFailed,
};

// Portable transaction contract for the future Win32 backend.
//
// Backend must expose noexcept methods:
//
//   Capture()
//   BindTarget()
//   BuildReflectedCamera()
//   LocateReflectedViewer()
//   RenderSky()
//   RenderTerrain()
//   RenderWmo()
//   RestoreOriginalCamera()
//   LocateOriginalViewer()
//   RestoreGpuState()
//   Quarantine()
//
// Every method except Quarantine returns bool.
//
// Capture occurs before any mutation. Once Capture succeeds, GPU restoration
// is always attempted. Once the reflected-camera build is attempted, original
// camera restoration is always attempted. If reflected WMO viewer-location is
// attempted, original viewer-location is rebuilt after the original camera.
//
// No fallback/native-water submission is owned here; the final Slot-3
// single-submission transaction remains the sole authority for that choice.
template <class Backend>
ReflectionSequenceStatus ExecuteReflectionSequence(
    const ReflectionPlan& plan,
    Backend& backend) noexcept
{
    if (!plan.valid ||
        !SupportedReflectionMode(
            plan.mode))
    {
        return
            ReflectionSequenceStatus::
                UnavailableRestored;
    }

    if (!backend.Capture())
        return
            ReflectionSequenceStatus::
                UnavailableRestored;

    bool contentOk =
        backend.BindTarget();

    bool cameraAttempted = false;
    bool viewerAttempted = false;

    if (contentOk) {
        cameraAttempted = true;

        contentOk =
            backend.BuildReflectedCamera();
    }

    if (contentOk &&
        ReflectionNeedsViewerLocate(
            plan.mode))
    {
        viewerAttempted = true;

        contentOk =
            backend.LocateReflectedViewer();
    }

    if (contentOk)
        contentOk =
            backend.RenderSky();

    if (contentOk &&
        plan.mode >= 2u)
    {
        contentOk =
            backend.RenderTerrain();
    }

    if (contentOk &&
        plan.mode >= 3u)
    {
        contentOk =
            backend.RenderWmo();
    }

    bool cameraRestored = true;
    bool viewerRestored = true;

    if (cameraAttempted) {
        cameraRestored =
            backend.RestoreOriginalCamera();

        if (cameraRestored &&
            viewerAttempted)
        {
            viewerRestored =
                backend.LocateOriginalViewer();
        }
    }

    const bool gpuRestored =
        backend.RestoreGpuState();

    if (!cameraRestored ||
        !viewerRestored ||
        !gpuRestored)
    {
        backend.Quarantine();

        return
            ReflectionSequenceStatus::
                RestoreFailed;
    }

    return
        contentOk
            ? ReflectionSequenceStatus::
                  Produced
            : ReflectionSequenceStatus::
                  UnavailableRestored;
}

} // namespace wxl::water::slot3
