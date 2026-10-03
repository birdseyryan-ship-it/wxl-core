// R6 E4G-A first-candidate runtime bridge. GPL-3.0-or-later.
//
// This tranche is intentionally inert with respect to draw replacement.
// It closes only runtime values whose producer/equation is already proven:
//
//   - exact Classic LiquidType IDs 1/5/9 through live Wrath settings-bank
//     inversion;
//   - the exact Wrath engine uint32 time returned by 0x0086AE20;
//   - the exact first-candidate Classic static-flow vector.
//
// E4G-B1 adds two more exact/inert pieces:
//   - a fail-closed ingestion boundary for already-grounded per-draw world rows;
//   - the exact first-candidate Classic UV specialization.
//
// It still does NOT derive/guess the world source, lighting, reflection,
// water-colour or PS31, and it does NOT call the packet/upload/DIP path.

#pragma once

#include "water/Slot3FirstCandidateConstants.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
struct IDirect3DDevice9;
#endif

namespace wxl::water::slot3
{

inline constexpr float kFirstCandidateFlowSpeed =
    0.015f;

inline constexpr float kFirstCandidateFlowVelocityX =
    0.015f;

inline constexpr float kFirstCandidateFlowVelocityY =
    0.0f;

// Exact launch-Classic first-candidate UV specialization.
//
// The supported ID 1/5/9 profiles share Float0=1, Float1=0, Float2=1.
// Applying the recovered Classic rotation/scale construction therefore yields
// the same two affine identity groups for C12/C13/C15 and C16/C17/C19.
inline constexpr std::array<Float4, 3> kFirstCandidateUvA{{
    Float4{{1.0f, 0.0f, 0.0f, 0.0f}},
    Float4{{0.0f, 1.0f, 0.0f, 0.0f}},
    Float4{{0.0f, 0.0f, 0.0f, 1.0f}},
}};

inline constexpr std::array<Float4, 3> kFirstCandidateUvB{{
    Float4{{1.0f, 0.0f, 0.0f, 0.0f}},
    Float4{{0.0f, 1.0f, 0.0f, 0.0f}},
    Float4{{0.0f, 0.0f, 0.0f, 1.0f}},
}};

// Exact Classic projected-coordinate rectangle authority.
//
// E2C/E4 recovered:
//   Classic default rectangle @ 0x141B8E0A0 = {0, 0, 1, 1}
//
// and leaf 0x140BCE620 computes:
//
//   x = (rect3 - rect1) * +0.5
//   y = (rect2 - rect0) * -0.5
//   z = ((rect1 + 1) - (1 - rect3)) * +0.5
//   w = (((1 - rect2) + 1) - rect0) * +0.5
//
// Therefore the exact default/full-target PS CB1[31] row is:
//
//   {+0.5, -0.5, +0.5, +0.5}
//
// E4G-B2 deliberately does NOT claim arbitrary Wrath viewport rectangles are
// numerically equivalent to arbitrary Classic rectangle state. The runtime
// promotion below is restricted to a full-target D3D9 viewport only.
inline constexpr Float4 kClassicDefaultProjectedRect{{
    0.0f,
    0.0f,
    1.0f,
    1.0f
}};

inline constexpr Float4 kFirstCandidateFullTargetPs31{{
    0.5f,
    -0.5f,
    0.5f,
    0.5f
}};

inline bool BuildExactClassicPs31FromRect(
    const Float4& rect,
    Float4& output) noexcept
{
    if (!Finite(rect))
        return false;

    // Preserve the recovered leaf's arithmetic ordering rather than replacing
    // it with a guessed D3D9 half-pixel expression.
    Float4 candidate{};

    candidate[0] =
        rect[3] -
        rect[1];

    candidate[0] *=
        0.5f;

    candidate[1] =
        rect[2] -
        rect[0];

    candidate[1] *=
        -0.5f;

    candidate[2] =
        rect[1] +
        1.0f;

    const float zSubtract =
        1.0f -
        rect[3];

    candidate[2] -=
        zSubtract;

    candidate[2] *=
        0.5f;

    candidate[3] =
        1.0f -
        rect[2];

    candidate[3] +=
        1.0f;

    candidate[3] -=
        rect[0];

    candidate[3] *=
        0.5f;

    if (!Finite(candidate))
        return false;

    output =
        candidate;

    return true;
}

// Strict first-candidate Wrath->Classic specialization.
//
// The three accepted E3R P01 captures all used:
//   viewport 0,0,3840x2160
//   RT0      3840x2160
//
// A future production caller must pass the actual current viewport and target
// dimensions. Offset/scaled/sub-target viewports fail closed rather than being
// promoted through an unproven cross-version rectangle mapping.
inline bool ApplyExactFirstCandidateFullTargetPs31(
    std::uint32_t viewportX,
    std::uint32_t viewportY,
    std::uint32_t viewportWidth,
    std::uint32_t viewportHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (viewportX != 0u ||
        viewportY != 0u ||
        viewportWidth == 0u ||
        viewportHeight == 0u ||
        targetWidth == 0u ||
        targetHeight == 0u ||
        viewportWidth != targetWidth ||
        viewportHeight != targetHeight)
    {
        return false;
    }

    Float4 row{};

    if (!BuildExactClassicPs31FromRect(
            kClassicDefaultProjectedRect,
            row) ||
        row != kFirstCandidateFullTargetPs31)
    {
        return false;
    }

    FirstCandidateRuntimeRows candidate =
        output;

    candidate.ps31 =
        row;

    candidate.ps31Ready =
        true;

    output =
        candidate;

    return true;
}

// Ingestion boundary only. This function deliberately does not infer a world
// transform from a same-shaped native matrix. The caller must supply the exact
// four already-grounded per-draw object/world rows.
inline bool ApplyGroundedFirstCandidateWorldRows(
    const std::array<Float4, 4>& world,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!AllFiniteRows(world))
        return false;

    FirstCandidateRuntimeRows candidate =
        output;

    candidate.world =
        world;

    candidate.worldReady =
        true;

    output =
        candidate;

    return true;
}

// Exact runtime ingestion of the native per-draw world matrix.
//
// E3R's existing native Context already exposes the P01 draw's `world`
// pointer and captured the complete 4x4 matrix from that pointer.
//
// This bridge does not infer the matrix from a same-numbered shader constant;
// it copies the actual per-draw transform associated with the native geometry
// that the R6 candidate deliberately retains.
inline bool ApplyGroundedFirstCandidateWorldPointer(
    const void* world,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!world)
        return false;

    std::array<Float4, 4> rows{};

    std::memcpy(
        rows.data(),
        world,
        sizeof(rows));

    return
        ApplyGroundedFirstCandidateWorldRows(
            rows,
            output);
}

inline bool ApplyExactFirstCandidateStaticUv(
    FirstCandidateRuntimeRows& output) noexcept
{
    // This specialization is authorised only for the exact supported
    // launch-Classic first-candidate profiles.
    if (!ClassicP01LiquidTypeSupported(
            output.liquidTypeId))
    {
        return false;
    }

    FirstCandidateRuntimeRows candidate =
        output;

    candidate.uvA =
        kFirstCandidateUvA;

    candidate.uvB =
        kFirstCandidateUvB;

    candidate.uvReady =
        true;

    output =
        candidate;

    return true;
}

// Exact launch-Classic first-candidate result for IDs 1/5/9:
//
//   angle = 0 degrees
//   base speed = 0
//   effective speed = 0.015
//
// The recovered Classic direction helper returns sin/cos in xmm0. The water
// caller shuffles cosine into the first stored velocity component and leaves
// sine in the second. Therefore angle 0 is exactly {+0.015, 0}.
// Exact Classic packed-colour conversion.
//
// Classic helper 0x140CFF110 converts the packed colour's R/G/B bytes to
// floating-point channels with the binary32 representation of 1/255.
// Alpha is supplied separately by the selector-1 water-colour branch.
inline constexpr float kClassicPackedColourScale =
    1.0f / 255.0f;

inline bool BuildExactClassicPackedColourRow(
    std::uint32_t packedColour,
    float alpha,
    Float4& output) noexcept
{
    if (!std::isfinite(alpha))
        return false;

    const Float4 candidate{{
        static_cast<float>(
            (packedColour >> 16u) & 0xFFu) *
            kClassicPackedColourScale,

        static_cast<float>(
            (packedColour >> 8u) & 0xFFu) *
            kClassicPackedColourScale,

        static_cast<float>(
            packedColour & 0xFFu) *
            kClassicPackedColourScale,

        alpha,
    }};

    if (!Finite(candidate))
        return false;

    output =
        candidate;

    return true;
}

// B5 closed the exact launch-Classic +0x6AC selector for IDs 1/5/9:
//
//   Water      -> proceduralRiverDepthTex -> selector 1
//   Slow Water -> proceduralRiverDepthTex -> selector 1
//   Fast Water -> proceduralRiverDepthTex -> selector 1
//
// Selector 1 builds PS17/PS18 from river close/far colours with the
// independently supplied river shallow/deep alpha values.
inline bool ApplyExactFirstCandidateRiverWaterColour(
    const WrathP01ConstantSnapshot& snapshot,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!ClassicP01LiquidTypeSupported(
            output.liquidTypeId) ||
        !ValidateWrathP01Snapshot(
            snapshot))
    {
        return false;
    }

    std::array<Float4, 2> rows{};

    if (!BuildExactClassicPackedColourRow(
            snapshot.riverCloseColor,
            snapshot.riverAlphaShallow,
            rows[0]) ||
        !BuildExactClassicPackedColourRow(
            snapshot.riverFarColor,
            snapshot.riverAlphaDeep,
            rows[1]))
    {
        return false;
    }

    FirstCandidateRuntimeRows candidate =
        output;

    candidate.waterColour =
        rows;

    candidate.waterColourReady =
        true;

    output =
        candidate;

    return true;
}

// Exact first-candidate Classic PS CB1[0..5] lighting bridge.
//
// Step 2A/2B close the required dataflow for the narrow P01/A0/B1/Q0
// candidate:
//
//   C0.xyz = sign-flipped, normalized primary world-light direction
//   C0.w   = -1
//
//   C1.xyz = ambient-role colour
//   C1.w   = 1
//
//   C2.xyz = diffuse/directional-light colour
//   C2.w   = 1
//
//   C3.xyz = specular colour
//   C3.w   = exact Classic literal 50
//
//   C4.xyz = raw pre-0x008A38B0 world-light direction
//   C4.w   = 1
//
//   C5.xyz = corresponding diffuse/directional-light colour
//   C5.w   = 1
//
// The Wrath live probe mechanically established:
//
//   raw ambient  -> native VS c34
//   raw diffuse  -> native VS c35
//   raw specular -> native VS c36
//
// for 64/64 records, while the raw direction is intentionally NOT native
// c33: c33 is the later view-space transform.
//
// Therefore c33 must never be substituted for C0/C4 here.
//
// C3.w deliberately does NOT use Wrath c36.w.  The launch-Classic common
// writer installs literal 50.0 for this shader path.
//
// The live raw direction was unit length throughout the accepted probe.
// Keep that evidence boundary strict: an unexpected/non-unit direction fails
// closed instead of exercising the Classic producer's separately unqualified
// degenerate fallback path.
inline constexpr float kFirstCandidateLightingDirectionLengthSquaredTolerance =
    1.0e-3f;

inline bool BuildExactFirstCandidateLightingRows(
    const Float4& rawWorldDirection,
    const Float4& nativeAmbient,
    const Float4& nativeDiffuse,
    const Float4& nativeSpecular,
    std::array<Float4, 6>& output) noexcept
{
    if (!Finite(rawWorldDirection) ||
        !Finite(nativeAmbient) ||
        !Finite(nativeDiffuse) ||
        !Finite(nativeSpecular))
    {
        return false;
    }

    // Step-1 native-row invariants were 64/64:
    // c34.w = 1, c35.w = 1, c36.w = 6.
    //
    // These are evidence gates, not values copied into Classic C3.w.
    if (nativeAmbient[3] != 1.0f ||
        nativeDiffuse[3] != 1.0f ||
        nativeSpecular[3] != 6.0f)
    {
        return false;
    }

    float lengthSquared =
        rawWorldDirection[0] *
        rawWorldDirection[0];

    lengthSquared +=
        rawWorldDirection[1] *
        rawWorldDirection[1];

    lengthSquared +=
        rawWorldDirection[2] *
        rawWorldDirection[2];

    if (!std::isfinite(lengthSquared) ||
        std::fabs(
            lengthSquared -
            1.0f) >
            kFirstCandidateLightingDirectionLengthSquaredTolerance)
    {
        return false;
    }

    const float length =
        std::sqrt(
            lengthSquared);

    if (!std::isfinite(length) ||
        !(length > 0.0f))
    {
        return false;
    }

    const float inverseLength =
        1.0f /
        length;

    const float directionX =
        rawWorldDirection[0] *
        inverseLength;

    const float directionY =
        rawWorldDirection[1] *
        inverseLength;

    const float directionZ =
        rawWorldDirection[2] *
        inverseLength;

    const std::array<Float4, 6>
        candidate{{
            Float4{{
                -directionX,
                -directionY,
                -directionZ,
                -1.0f
            }},

            Float4{{
                nativeAmbient[0],
                nativeAmbient[1],
                nativeAmbient[2],
                1.0f
            }},

            Float4{{
                nativeDiffuse[0],
                nativeDiffuse[1],
                nativeDiffuse[2],
                1.0f
            }},

            Float4{{
                nativeSpecular[0],
                nativeSpecular[1],
                nativeSpecular[2],
                50.0f
            }},

            Float4{{
                rawWorldDirection[0],
                rawWorldDirection[1],
                rawWorldDirection[2],
                1.0f
            }},

            Float4{{
                nativeDiffuse[0],
                nativeDiffuse[1],
                nativeDiffuse[2],
                1.0f
            }},
        }};

    for (const Float4& row : candidate)
    {
        if (!Finite(row))
            return false;
    }

    output =
        candidate;

    return true;
}

// Atomic ingestion boundary only.
//
// The production caller must supply the raw pre-0x008A38B0 direction from
// the same native P01 lighting build and same-draw c34/c35/c36 rows.
// This helper performs no hook installation, no address discovery and no DIP.
inline bool ApplyGroundedFirstCandidateLightingRows(
    const Float4& rawWorldDirection,
    const Float4& nativeAmbient,
    const Float4& nativeDiffuse,
    const Float4& nativeSpecular,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!ClassicP01LiquidTypeSupported(
            output.liquidTypeId))
    {
        return false;
    }

    std::array<Float4, 6>
        rows{};

    if (!BuildExactFirstCandidateLightingRows(
            rawWorldDirection,
            nativeAmbient,
            nativeDiffuse,
            nativeSpecular,
            rows))
    {
        return false;
    }

    FirstCandidateRuntimeRows candidate =
        output;

    candidate.lighting =
        rows;

    candidate.lightingReady =
        true;

    output =
        candidate;

    return true;
}

// Exact FastTrack-I source-side PS CB1[13..16] equation.
//
// Reconciliation proved:
//
//   PS CB1 base  = 0x142282EF0
//   FastTrack-I SRC = 0x142282F80 = PS CB1[9]
//   PS13 = SRC + 0x40
//   PS14 = SRC + 0x50
//   PS15 = SRC + 0x60
//   PS16 = SRC + 0x70
//
// The recovered source-side writer constructs each of those rows as:
//
//   (view[row].x * projection[0] +
//    view[row].y * projection[1]) +
//   (view[row].z * projection[2] +
//    view[row].w * projection[3])
//
// The grouping below intentionally preserves the recovered SSE addition
// ordering rather than replacing it with an opaque matrix helper.
//
// This closes the Classic equation only. It does NOT assert that any
// same-shaped Wrath matrix is automatically a valid Classic input.
// Callers must supply already-grounded reflected-camera view/projection rows.
inline bool BuildExactFastTrackIReflectionRows(
    const std::array<Float4, 4>& view,
    const std::array<Float4, 4>& projection,
    std::array<Float4, 4>& output) noexcept
{
    if (!AllFiniteRows(view) ||
        !AllFiniteRows(projection))
    {
        return false;
    }

    std::array<Float4, 4> candidate{};

    for (std::size_t row = 0;
         row < candidate.size();
         ++row)
    {
        for (std::size_t component = 0;
             component < candidate[row].size();
             ++component)
        {
            float xy =
                view[row][0] *
                projection[0][component];

            xy +=
                view[row][1] *
                projection[1][component];

            float zw =
                view[row][2] *
                projection[2][component];

            zw +=
                view[row][3] *
                projection[3][component];

            candidate[row][component] =
                xy +
                zw;
        }
    }

    if (!AllFiniteRows(candidate))
        return false;

    output =
        candidate;

    return true;
}

// Atomic ingestion boundary for reflected matrices whose provenance has
// already been grounded by the caller.
//
// The ordinary draw-runtime composer does not call this yet. Runtime
// acquisition from the mirrored-reflection transaction remains a separate
// qualification gate.
inline bool ApplyGroundedFirstCandidateReflectionMatrices(
    const std::array<Float4, 4>& reflectedView,
    const std::array<Float4, 4>& reflectedProjection,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!ClassicP01LiquidTypeSupported(
            output.liquidTypeId))
    {
        return false;
    }

    std::array<Float4, 4> rows{};

    if (!BuildExactFastTrackIReflectionRows(
            reflectedView,
            reflectedProjection,
            rows))
    {
        return false;
    }

    FirstCandidateRuntimeRows candidate =
        output;

    candidate.reflection =
        rows;

    candidate.reflectionReady =
        true;

    output =
        candidate;

    return true;
}

inline bool ApplyExactFirstCandidateStaticRuntime(
    std::uint32_t liquidTypeId,
    std::uint32_t animationTime,
    FirstCandidateRuntimeRows& output) noexcept
{
    if (!ClassicP01LiquidTypeSupported(
            liquidTypeId))
    {
        return false;
    }

    FirstCandidateRuntimeRows candidate =
        output;

    candidate.liquidTypeId =
        liquidTypeId;

    candidate.animationTime =
        animationTime;

    candidate.flowVelocityX =
        kFirstCandidateFlowVelocityX;

    candidate.flowVelocityY =
        kFirstCandidateFlowVelocityY;

    candidate.animationReady =
        true;

    output =
        candidate;

    return true;
}

#ifdef _WIN32

struct NativeReflectionCandidate;

// Installs the dormant first-candidate lighting producer.
//
// This hook does not replace a draw.  It observes the exact transient record
// passed to native 0x008A38B0 while native::Current() identifies the same
// base-Water/pass-1 invocation.
//
// The final Slot-3 feature must establish the native liquid Context hooks
// before relying on this capture.  Merely compiling this function does not
// install anything.
bool InstallFirstCandidateLightingCapture() noexcept;

// Consume the same-draw lighting capture before a secondary render can change
// native transient lighting/constant state. Does not require reflection yet.
bool PopulateFirstCandidateLightingRuntime(
    std::uint64_t invocation,
    const void* settings,
    FirstCandidateRuntimeRows& output) noexcept;

// Invalidates the current thread's captured transient-light record.
// Intended for lost/reset/world lifecycle boundaries in the final runtime.
void ResetFirstCandidateLightingCapture() noexcept;

// Exact production reflection-plane source for the authorised first candidate.
//
// The cross-version authority frozen by Step2R is:
//
//   Classic selected-mode reflection plane
//       = base/sample-0 liquid surface height
//
//   Wrath P01 functional equivalent
//       = world-space Z of the first referenced native POSITION0 sample
//
// This routine reproduces that exact native-DIP sampling rule directly:
//   - triangle-strip P01 geometry only;
//   - stream 0 POSITION0 FLOAT3/FLOAT4 only;
//   - exact first index referenced by this DIP;
//   - BaseVertexIndex applied before reading POSITION0;
//   - index/VB locks are D3DLOCK_READONLY;
//   - no min/max/mean/sphere/world/camera substitute;
//   - output is committed only after complete validation.
//
// It performs no draw, state mutation, reflection rendering or packet upload.
bool CaptureFirstCandidateWaterPlane(
    IDirect3DDevice9* device,
    unsigned primitiveType,
    std::int32_t baseVertex,
    std::uint32_t minVertex,
    std::uint32_t vertexCount,
    std::uint32_t startIndex,
    std::uint32_t primitiveCount,
    float& waterPlane) noexcept;

// Atomically closes the two remaining dynamic row groups for one candidate:
//
//   - Classic PS C0..C5 from the exact same native Water/pass-1 invocation;
//   - Classic PS C13..C16 from the qualified mirrored-reflection candidate.
//
// The reflection candidate must explicitly name this exact consumer ordinal.
// No resource binding, constant upload or draw occurs here.
bool PopulateFirstCandidateDynamicRuntime(
    std::uint64_t invocation,
    const void* settings,
    std::uint64_t consumerOrdinal,
    const NativeReflectionCandidate& reflection,
    FirstCandidateRuntimeRows& output) noexcept;

// `settings` is the exact live native P01 settings pointer.
//
// E4 live capture proved that kSettingsBankRows / 0x00D43B1C is a POINTER
// LOCATION. It must first be dereferenced to obtain the row-pointer array.
// Treating 0x00D43B1C itself as row[0] is forbidden.
bool ResolveFirstCandidateLiquidTypeId(
    const void* settings,
    std::uint32_t& liquidTypeId) noexcept;

// Exact zero-argument wrapper at 0x0086AE20.
//
// Wrath's native P01 animation helpers consume its EAX result as their uint32
// timing value before performing the native animation arithmetic.
bool CaptureFirstCandidateAnimationTime(
    std::uint32_t& animationTime) noexcept;

// Composition of the two exact runtime producers above plus the already-closed
// static Classic flow vector.
//
// No other readiness flag is modified.
bool PopulateFirstCandidateStaticRuntime(
    const void* settings,
    FirstCandidateRuntimeRows& output) noexcept;

// Exact per-draw world source bridge.
//
// `world` must be the native P01 draw-context world pointer already exposed by
// the existing R6 native Context.  No address discovery is performed here.
bool PopulateFirstCandidateWorldRuntime(
    const void* world,
    FirstCandidateRuntimeRows& output) noexcept;

// Atomic composition of every first-candidate draw-runtime group closed
// through E4G-B6A:
//
//   - LiquidType ID 1/5/9
//   - exact animation time
//   - exact static Classic UV rows
//   - native-geometry world transform
//   - exact full-target Classic PS31
//   - exact selector-1 river PS17/PS18 water-colour rows
//
// Lighting and reflection remain intentionally fail-closed.
bool PopulateFirstCandidateDrawRuntime(
    const void* settings,
    const void* world,
    std::uint32_t viewportX,
    std::uint32_t viewportY,
    std::uint32_t viewportWidth,
    std::uint32_t viewportHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    FirstCandidateRuntimeRows& output) noexcept;

#endif

} // namespace wxl::water::slot3
