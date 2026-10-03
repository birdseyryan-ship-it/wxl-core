// R6 first-candidate complete Classic constant-packet assembler.
// GPL-3.0-or-later.
//
// Scope is deliberately narrow:
//   Wrath P01 / terrain / Classic A0-B1-Q0 / selector 5.
//
// This layer does not discover or approximate runtime values.  It assembles
// the complete recovered Classic constant ABI only when each separately
// grounded runtime producer has supplied its group.
//
// Exact Classic LiquidType profiles are restricted to launch-Classic IDs
// 1, 5 and 9.  Wrath settings-payload lookalikes 41/61/81 are NOT accepted.
//
// IMPORTANT:
// - no live draw is issued here;
// - no shader/texture is bound here;
// - no unknown row is zero-filled;
// - output is assigned only after BuildConstantPacket() succeeds;
// - the animationTime field intentionally has no guessed unit label: it is
//   the exact uint32 consumed by the Classic periodic helper.

#pragma once

#include "water/Slot3ConstantProducer.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wxl::water::slot3
{

struct ClassicMaterialProfile
{
    std::uint32_t liquidTypeId = 0;
    std::array<float, 18> values{};
};

inline constexpr std::array<
    ClassicMaterialProfile,
    3>
kClassicP01MaterialProfiles{{
    ClassicMaterialProfile{
        1u,
        {{1.0f, 0.0f, 1.0f, 1.0f, 2.5f, 90.0f, 1.0f, 0.5f, 0.5f, 0.20000000298f, 0.69999998808f, 0.20000000298f, 0.10000000149f, 1.0f, 0.5f, 2.5f, 0.0f, 0.0f}}
    },
    ClassicMaterialProfile{
        5u,
        {{1.0f, 0.0f, 1.0f, 1.0f, 2.5f, 90.0f, 0.69999998808f, 0.5f, 0.5f, 0.20000000298f, 0.5f, 0.20000000298f, 0.10000000149f, 1.0f, 0.69999998808f, 2.5f, 0.0f, 0.0f}}
    },
    ClassicMaterialProfile{
        9u,
        {{1.0f, 0.0f, 1.0f, 1.0f, 2.5f, 90.0f, 0.5f, 0.5f, 0.5f, 0.20000000298f, 0.69999998808f, 0.20000000298f, 0.10000000149f, 1.0f, 0.5f, 2.5f, 0.0f, 0.0f}}
    }
}};

inline const ClassicMaterialProfile*
FindClassicP01MaterialProfile(
    std::uint32_t liquidTypeId) noexcept
{
    for (const auto& profile :
         kClassicP01MaterialProfiles)
    {
        if (profile.liquidTypeId ==
            liquidTypeId)
        {
            return &profile;
        }
    }

    return nullptr;
}

inline bool
ClassicP01LiquidTypeSupported(
    std::uint32_t liquidTypeId) noexcept
{
    return
        FindClassicP01MaterialProfile(
            liquidTypeId) != nullptr;
}

// All groups below have a separately recovered producer.
//
// The packet assembler owns the register mapping; the eventual final
// transaction owns acquisition of these runtime values.
struct FirstCandidateRuntimeRows
{
    std::uint32_t liquidTypeId = 0;

    // Exact uint32 consumed by Classic 0x1405773E0 and the C28.x phase.
    std::uint32_t animationTime = 0;

    // Native-Wrath geometry -> world transform translated to Classic C0..C3.
    std::array<Float4, 4> world{};

    // Exact Classic UV transform rows C12/C13/C15 and C16/C17/C19.
    std::array<Float4, 3> uvA{};
    std::array<Float4, 3> uvB{};

    // Exact Classic PS C0..C5 translated from the closed lighting state.
    std::array<Float4, 6> lighting{};

    // Closed reflection-transform producer for Classic PS C13..C16.
    std::array<Float4, 4> reflection{};

    // Closed first-candidate river/water colour branch, Classic PS C17/C18.
    std::array<Float4, 2> waterColour{};

    // Exact Classic projected/screen-coordinate vector.
    Float4 ps31{};

    // Post-direction-helper velocity consumed by Classic 0x1405773E0.
    //
    // Keeping the final X/Y vector as the input is deliberate: it prevents
    // angle/speed provenance from being silently replaced by a guessed
    // cross-version animation-layout equivalence.
    float flowVelocityX = 0.0f;
    float flowVelocityY = 0.0f;

    bool worldReady = false;
    bool uvReady = false;
    bool lightingReady = false;
    bool reflectionReady = false;
    bool waterColourReady = false;
    bool ps31Ready = false;
    bool animationReady = false;
};

template <std::size_t Count>
inline bool
AllFiniteRows(
    const std::array<Float4, Count>& rows) noexcept
{
    for (const auto& row : rows)
    {
        if (!Finite(row))
            return false;
    }

    return true;
}

inline bool
ValidateFirstCandidateRuntimeRows(
    const FirstCandidateRuntimeRows& rows) noexcept
{
    if (!ClassicP01LiquidTypeSupported(
            rows.liquidTypeId))
    {
        return false;
    }

    if (!rows.worldReady ||
        !rows.uvReady ||
        !rows.lightingReady ||
        !rows.reflectionReady ||
        !rows.waterColourReady ||
        !rows.ps31Ready ||
        !rows.animationReady)
    {
        return false;
    }

    if (!AllFiniteRows(rows.world) ||
        !AllFiniteRows(rows.uvA) ||
        !AllFiniteRows(rows.uvB) ||
        !AllFiniteRows(rows.lighting) ||
        !AllFiniteRows(rows.reflection) ||
        !AllFiniteRows(rows.waterColour) ||
        !Finite(rows.ps31))
    {
        return false;
    }

    return
        std::isfinite(rows.flowVelocityX) &&
        std::isfinite(rows.flowVelocityY);
}

// Exact recovered Classic helper 0x1405773E0.
//
// Near-zero velocity => zero translation.
//
// Positive:
//   period = trunc(1000 / velocity)
//   result = (time % period) / period
//
// Negative:
//   period = trunc(-1000 / velocity)
//   result = 1 - (time % period) / period
//
// Deliberate fail-closed bounds prevent undefined C++ casts for malformed
// runtime inputs; valid recovered game inputs remain bit-for-bit in the
// helper's intended integer-period domain.
inline bool
ClassicPeriodicCoordinate(
    std::uint32_t animationTime,
    float velocity,
    float& output) noexcept
{
    constexpr float kNegativeThreshold =
        -0.00001f;

    constexpr float kPositiveThreshold =
        0.00001f;

    constexpr float kPositiveDistance =
        1000.0f;

    constexpr float kNegativeDistance =
        -1000.0f;

    if (!std::isfinite(velocity))
        return false;

    if (velocity <
        kNegativeThreshold)
    {
        const float rawPeriod =
            kNegativeDistance /
            velocity;

        if (!std::isfinite(rawPeriod) ||
            rawPeriod < 1.0f ||
            rawPeriod >
                static_cast<float>(
                    std::numeric_limits<
                        std::uint32_t>::
                        max()))
        {
            return false;
        }

        const auto period =
            static_cast<std::uint32_t>(
                rawPeriod);

        if (period == 0)
            return false;

        output =
            1.0f -
            (
                static_cast<float>(
                    animationTime %
                    period)
                /
                static_cast<float>(
                    period)
            );

        return std::isfinite(output);
    }

    if (velocity >
        kPositiveThreshold)
    {
        const float rawPeriod =
            kPositiveDistance /
            velocity;

        if (!std::isfinite(rawPeriod) ||
            rawPeriod < 1.0f ||
            rawPeriod >
                static_cast<float>(
                    std::numeric_limits<
                        std::uint32_t>::
                        max()))
        {
            return false;
        }

        const auto period =
            static_cast<std::uint32_t>(
                rawPeriod);

        if (period == 0)
            return false;

        output =
            static_cast<float>(
                animationTime %
                period)
            /
            static_cast<float>(
                period);

        return std::isfinite(output);
    }

    output = 0.0f;
    return true;
}

inline bool
BuildFirstCandidatePacket(
    const WrathP01ConstantSnapshot& snapshot,
    const FirstCandidateRuntimeRows& runtime,
    ConstantPacket& output) noexcept
{
    if (!ValidateWrathP01Snapshot(
            snapshot) ||
        !ValidateFirstCandidateRuntimeRows(
            runtime))
    {
        return false;
    }

    const auto* profile =
        FindClassicP01MaterialProfile(
            runtime.liquidTypeId);

    if (!profile)
        return false;

    ConstantInputs constants{};

    // E1: exact model/view and projection bridge.
    if (!TranslateGroundedMatrixRows(
            snapshot,
            constants))
    {
        return false;
    }

    // E3: exact Classic static identity rows C20 and C23.
    if (!TranslateGroundedStaticRows(
            snapshot,
            constants))
    {
        return false;
    }

    // Geometry -> world.
    for (unsigned i = 0;
         i < 4;
         ++i)
    {
        if (!constants.SetVs(
                i,
                runtime.world[i]))
        {
            return false;
        }
    }

    // Two recovered Classic UV-transform groups.
    constexpr std::array<
        unsigned,
        3>
    kUvRowsA{{
        12,
        13,
        15
    }};

    constexpr std::array<
        unsigned,
        3>
    kUvRowsB{{
        16,
        17,
        19
    }};

    for (unsigned i = 0;
         i < 3;
         ++i)
    {
        if (!constants.SetVs(
                kUvRowsA[i],
                runtime.uvA[i]) ||
            !constants.SetVs(
                kUvRowsB[i],
                runtime.uvB[i]))
        {
            return false;
        }
    }

    // Classic C21.y is Float_2.
    if (!constants.SetVs(
            21,
            Float4{
                0.0f,
                profile->values[2],
                0.0f,
                0.0f
            }))
    {
        return false;
    }

    float translationX = 0.0f;
    float translationY = 0.0f;

    if (!ClassicPeriodicCoordinate(
            runtime.animationTime,
            runtime.flowVelocityX,
            translationX) ||
        !ClassicPeriodicCoordinate(
            runtime.animationTime,
            runtime.flowVelocityY,
            translationY))
    {
        return false;
    }

    // Exact dynamic translation matrix rows.
    if (!constants.SetVs(
            24,
            Float4{
                1.0f,
                0.0f,
                0.0f,
                0.0f
            }) ||
        !constants.SetVs(
            25,
            Float4{
                0.0f,
                1.0f,
                0.0f,
                0.0f
            }) ||
        !constants.SetVs(
            27,
            Float4{
                translationX,
                translationY,
                0.0f,
                1.0f
            }))
    {
        return false;
    }

    // Exact Classic C28.x:
    //   float(time % 10000) * pi / 10000
    //
    // C28.w is Float_8.
    constexpr float kPhaseScale =
        0.00031415926059708f;

    if (!constants.SetVs(
            28,
            Float4{
                static_cast<float>(
                    runtime.animationTime %
                    10000u)
                    * kPhaseScale,
                0.0f,
                0.0f,
                profile->values[8]
            }))
    {
        return false;
    }

    // Closed Classic PS common-lighting group.
    for (unsigned i = 0;
         i < 6;
         ++i)
    {
        if (!constants.SetPs(
                i,
                runtime.lighting[i]))
        {
            return false;
        }
    }

    // Closed Classic reflected-camera transform.
    for (unsigned i = 0;
         i < 4;
         ++i)
    {
        if (!constants.SetPs(
                13 + i,
                runtime.reflection[i]))
        {
            return false;
        }
    }

    // First-candidate supported water colour branch.
    for (unsigned i = 0;
         i < 2;
         ++i)
    {
        if (!constants.SetPs(
                17 + i,
                runtime.waterColour[i]))
        {
            return false;
        }
    }

    // Exact recovered material-float mappings.
    if (!constants.SetPs(
            21,
            Float4{
                0.0f,
                profile->values[7] *
                    0.35f,
                0.0f,
                0.0f
            }) ||
        !constants.SetPs(
            28,
            Float4{
                profile->values[13],
                profile->values[14],
                profile->values[15],
                profile->values[6]
            }) ||
        !constants.SetPs(
            31,
            runtime.ps31) ||
        !constants.SetPs(
            32,
            Float4{
                profile->values[12],
                profile->values[11],
                profile->values[9],
                profile->values[10]
            }))
    {
        return false;
    }

    ConstantPacket candidate{};

    if (!BuildConstantPacket(
            constants,
            candidate))
    {
        return false;
    }

    if (!candidate.valid ||
        !ExactSelector5Bank(
            candidate))
    {
        return false;
    }

    output = candidate;
    return true;
}

} // namespace wxl::water::slot3
