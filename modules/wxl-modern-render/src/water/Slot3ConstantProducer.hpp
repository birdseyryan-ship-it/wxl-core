// R6 P01/A0/B1/Q0 grounded constant producer.
// GPL-3.0-or-later.
//
// This layer snapshots already-proven Wrath P01 runtime state and translates
// only equivalences that have explicit project authority.
//
// IMPORTANT:
// - It is NOT a semantic guesser.
// - It does NOT make the whole Classic packet ready by itself.
// - Untranslated Classic rows remain not-ready and BuildConstantPacket()
//   therefore continues to fail closed until later exact translation tranches
//   populate them.
// - First-candidate scope remains terrain P01 / A0 / B1 / Q0 / selector 5.

#pragma once

#include "water/Slot3Constants.hpp"

#include <array>
#include <cstdint>

struct IDirect3DDevice9;

namespace wxl::water::slot3
{

inline constexpr unsigned kWrathVsMainCount = 46;
inline constexpr unsigned kWrathPsMainCount = 6;
inline constexpr unsigned kWrathVsExtensionCount = 12;
inline constexpr unsigned kWrathPsExtensionCount = 6;
inline constexpr unsigned kWrathSettingsFloatCount = 18;

using WrathVsMain =
    std::array<Float4, kWrathVsMainCount>;

using WrathPsMain =
    std::array<Float4, kWrathPsMainCount>;

using WrathVsExtension =
    std::array<Float4, kWrathVsExtensionCount>;

using WrathPsExtension =
    std::array<Float4, kWrathPsExtensionCount>;

using WrathSettingsFloats =
    std::array<float, kWrathSettingsFloatCount>;

struct WrathP01ConstantSnapshot
{
    WrathVsMain vsMain{};
    WrathPsMain psMain{};

    // Stock P01 also uploads:
    //   VS c46..57 <- 0x00D44F88
    //   PS c6..11  <- 0x00D44C48
    WrathVsExtension vsExtension{};
    WrathPsExtension psExtension{};

    WrathSettingsFloats settingsFloats{};

    std::uint32_t settingsColor0 = 0;
    std::uint32_t settingsColor1 = 0;

    std::uint32_t oceanCloseColor = 0;
    std::uint32_t oceanFarColor = 0;
    std::uint32_t riverCloseColor = 0;
    std::uint32_t riverFarColor = 0;

    float oceanAlphaShallow = 0.0f;
    float riverAlphaShallow = 0.0f;
    float oceanAlphaDeep = 0.0f;
    float riverAlphaDeep = 0.0f;

    // Active D3D9 viewport at the exact native P01 draw.
    //
    // E2C recovered the Classic PS CB1[31] equation as a transform of
    // the current viewport/rect state.  We capture the real runtime
    // viewport rather than assuming a full-screen origin or inventing
    // a half-pixel rule.
    float viewportX = 0.0f;
    float viewportY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float viewportMinZ = 0.0f;
    float viewportMaxZ = 1.0f;

    bool viewportReady = false;

    bool nativeBlocksReady = false;
    bool settingsReady = false;
    bool dayNightReady = false;
};

inline bool FiniteSnapshotRow(
    const Float4& row) noexcept
{
    return Finite(row);
}

inline bool ValidateWrathP01Snapshot(
    const WrathP01ConstantSnapshot& snapshot) noexcept
{
    if (!snapshot.nativeBlocksReady ||
        !snapshot.settingsReady ||
        !snapshot.dayNightReady)
        return false;

    if (snapshot.viewportReady)
    {
        if (!std::isfinite(snapshot.viewportX) ||
            !std::isfinite(snapshot.viewportY) ||
            !std::isfinite(snapshot.viewportWidth) ||
            !std::isfinite(snapshot.viewportHeight) ||
            !std::isfinite(snapshot.viewportMinZ) ||
            !std::isfinite(snapshot.viewportMaxZ) ||
            snapshot.viewportWidth <= 0.0f ||
            snapshot.viewportHeight <= 0.0f)
        {
            return false;
        }
    }

    for (const auto& row : snapshot.vsMain)
        if (!FiniteSnapshotRow(row))
            return false;

    for (const auto& row : snapshot.psMain)
        if (!FiniteSnapshotRow(row))
            return false;

    for (const auto& row : snapshot.vsExtension)
        if (!FiniteSnapshotRow(row))
            return false;

    for (const auto& row : snapshot.psExtension)
        if (!FiniteSnapshotRow(row))
            return false;

    for (float value : snapshot.settingsFloats)
        if (!std::isfinite(value))
            return false;

    if (!std::isfinite(snapshot.oceanAlphaShallow) ||
        !std::isfinite(snapshot.riverAlphaShallow) ||
        !std::isfinite(snapshot.oceanAlphaDeep) ||
        !std::isfinite(snapshot.riverAlphaDeep))
        return false;

    return true;
}

// First exact bridge tranche.
//
// Proven project authority:
//   Classic VS CB1[4..7]  = pre-projection model/view transform.
//   Classic VS CB1[8..11] = projection transform.
//
// Proven Wrath P01 native block layout:
//   VS c5..c8 = model/view rows.
//   VS c0..c3 = projection rows.
//
// This routine intentionally translates ONLY those two closed matrix groups.
// Everything else remains unready.
inline bool TranslateGroundedMatrixRows(
    const WrathP01ConstantSnapshot& snapshot,
    ConstantInputs& output) noexcept
{
    if (!ValidateWrathP01Snapshot(snapshot))
        return false;

    ConstantInputs candidate = output;

    // Classic pre-projection model/view rows.
    for (unsigned i = 0; i < 4; ++i)
    {
        if (!candidate.SetVs(
                4 + i,
                snapshot.vsMain[5 + i]))
            return false;
    }

    // Classic projection rows.
    for (unsigned i = 0; i < 4; ++i)
    {
        if (!candidate.SetVs(
                8 + i,
                snapshot.vsMain[i]))
            return false;
    }

    output = candidate;
    return true;
}


// E2C grounded first-candidate rows that require no unresolved
// Classic->Wrath semantic equivalence.
//
// Classic VS CB1[20] and CB1[23] come from the exact static identity
// transform at 0x142170270..0x1421702A0.
//
// Only shader-read components are material:
//   C20.xy
//   C23.xy
//
// Supplying the complete exact static rows is deterministic and does
// not infer any material semantics.
inline bool TranslateGroundedStaticRows(
    const WrathP01ConstantSnapshot& snapshot,
    ConstantInputs& output) noexcept
{
    if (!ValidateWrathP01Snapshot(snapshot))
        return false;

    ConstantInputs candidate = output;

    if (!candidate.SetVs(
            20,
            Float4{1.0f, 0.0f, 0.0f, 0.0f}) ||
        !candidate.SetVs(
            23,
            Float4{0.0f, 0.0f, 0.0f, 1.0f}))
    {
        return false;
    }

    output = candidate;
    return true;
}

inline bool GroundedStaticRowsReady(
    const ConstantInputs& input) noexcept
{
    return
        input.vsReady[20] &&
        input.vsReady[23] &&
        Finite(input.vs[20]) &&
        Finite(input.vs[23]);
}

// Diagnostic-only snapshot of the still-untranslated runtime inputs.
//
// This deliberately does NOT mark any additional Classic register ready.
// It exists so the final few cross-version numeric equivalences can be
// decided from one real P01 draw instead of another static archaeology pass.
struct RuntimeTranslationProbe
{
    bool valid = false;

    WrathVsMain vsMain{};
    WrathPsMain psMain{};
    WrathVsExtension vsExtension{};
    WrathPsExtension psExtension{};
    WrathSettingsFloats settingsFloats{};

    std::uint32_t settingsColor0 = 0;
    std::uint32_t settingsColor1 = 0;

    std::uint32_t oceanCloseColor = 0;
    std::uint32_t oceanFarColor = 0;
    std::uint32_t riverCloseColor = 0;
    std::uint32_t riverFarColor = 0;

    float oceanAlphaShallow = 0.0f;
    float riverAlphaShallow = 0.0f;
    float oceanAlphaDeep = 0.0f;
    float riverAlphaDeep = 0.0f;

    float viewportX = 0.0f;
    float viewportY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float viewportMinZ = 0.0f;
    float viewportMaxZ = 1.0f;
};

inline bool BuildRuntimeTranslationProbe(
    const WrathP01ConstantSnapshot& snapshot,
    RuntimeTranslationProbe& output) noexcept
{
    if (!ValidateWrathP01Snapshot(snapshot))
        return false;

    RuntimeTranslationProbe candidate{};

    candidate.vsMain = snapshot.vsMain;
    candidate.psMain = snapshot.psMain;
    candidate.vsExtension = snapshot.vsExtension;
    candidate.psExtension = snapshot.psExtension;
    candidate.settingsFloats = snapshot.settingsFloats;

    candidate.settingsColor0 = snapshot.settingsColor0;
    candidate.settingsColor1 = snapshot.settingsColor1;

    candidate.oceanCloseColor = snapshot.oceanCloseColor;
    candidate.oceanFarColor = snapshot.oceanFarColor;
    candidate.riverCloseColor = snapshot.riverCloseColor;
    candidate.riverFarColor = snapshot.riverFarColor;

    candidate.oceanAlphaShallow = snapshot.oceanAlphaShallow;
    candidate.riverAlphaShallow = snapshot.riverAlphaShallow;
    candidate.oceanAlphaDeep = snapshot.oceanAlphaDeep;
    candidate.riverAlphaDeep = snapshot.riverAlphaDeep;

    candidate.viewportX = snapshot.viewportX;
    candidate.viewportY = snapshot.viewportY;
    candidate.viewportWidth = snapshot.viewportWidth;
    candidate.viewportHeight = snapshot.viewportHeight;
    candidate.viewportMinZ = snapshot.viewportMinZ;
    candidate.viewportMaxZ = snapshot.viewportMaxZ;

    candidate.valid = true;

    output = candidate;
    return true;
}

inline bool GroundedMatrixRowsReady(
    const ConstantInputs& input) noexcept
{
    for (unsigned row = 4; row <= 11; ++row)
    {
        if (!input.vsReady[row] ||
            !Finite(input.vs[row]))
            return false;
    }

    return true;
}

} // namespace wxl::water::slot3

#ifdef _WIN32

namespace wxl::water::slot3
{

// Reads the already-produced Wrath P01 native constant/settings state.
//
// `settings` MUST be the exact live P01 settings object supplied by the
// current native liquid draw. A null pointer fails closed.
//
// This function does not mutate engine memory and does not upload constants.
bool CaptureWrathP01ConstantSnapshot(
    const void* settings,
    WrathP01ConstantSnapshot& output) noexcept;


// Captures the exact D3D9 viewport at the native P01 draw.
bool CaptureWrathP01Viewport(
    IDirect3DDevice9* device,
    WrathP01ConstantSnapshot& output) noexcept;

// Atomic convenience capture for the final integration path.
bool CaptureWrathP01ConstantSnapshot(
    IDirect3DDevice9* device,
    const void* settings,
    WrathP01ConstantSnapshot& output) noexcept;

} // namespace wxl::water::slot3

#endif
