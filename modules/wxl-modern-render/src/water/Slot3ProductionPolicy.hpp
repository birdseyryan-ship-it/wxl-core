// R6 production orchestration policy. GPL-3.0-or-later.
#pragma once
#include "water/Slot3FirstCandidateRuntime.hpp"
#include "water/Slot3ReflectionRuntime.hpp"

namespace wxl::water::slot3 {

// The recovered launch registration default, NOT a claim about all presets.
inline constexpr unsigned kFirstCandidateDownsampleShift = 0;

enum class ProductionStage {
    Disabled, NativeBridge, Profile, Geometry, Plane, Camera, VolumeFog,
    Snapshot, Rows, Capture, Materials, Shaders, Depth, Reflection, Constants,
    Bind, Submitted, Restore
};
inline const char* Name(ProductionStage s) noexcept {
    switch (s) {
    case ProductionStage::Disabled: return "disabled";
    case ProductionStage::NativeBridge: return "native_state_bridge_unqualified";
    case ProductionStage::Profile: return "profile";
    case ProductionStage::Geometry: return "geometry";
    case ProductionStage::Plane: return "plane";
    case ProductionStage::Camera: return "camera";
    case ProductionStage::VolumeFog: return "volume_fog";
    case ProductionStage::Snapshot: return "snapshot";
    case ProductionStage::Rows: return "runtime_rows";
    case ProductionStage::Capture: return "state_capture";
    case ProductionStage::Materials: return "materials";
    case ProductionStage::Shaders: return "shaders";
    case ProductionStage::Depth: return "linear_depth";
    case ProductionStage::Reflection: return "reflection";
    case ProductionStage::Constants: return "constants";
    case ProductionStage::Bind: return "bind";
    case ProductionStage::Submitted: return "submitted";
    default: return "restore";
    }
}

// Reserve a distinct producer event before the consumer. This is an event
// sequence, not a GPU timestamp. Internal reflection draws do not advance the
// main-scene sequence. Never fabricate an ordinal by subtracting from a draw.
struct ProductionOrdinals {
    std::uint64_t reflection = 0, consumer = 0;
};
inline bool ReserveProductionOrdinals(std::uint64_t& sequence,
                                      ProductionOrdinals& out) noexcept {
    if (sequence > UINT64_MAX - 2u) return false;
    out = {sequence + 1u, sequence + 2u};
    sequence += 2u;
    return true;
}

inline bool FirstCandidateDrawSupported(const waterdiag::ProfileKey& key,
                                        unsigned streams, unsigned stride,
                                        std::uint32_t liquidTypeId) noexcept {
    return waterdiag::AllowlistedReplacementProfile(key) ==
               waterdiag::ReplacementProfile::P01 &&
           streams == 1u && stride == 44u &&
           ClassicP01LiquidTypeSupported(liquidTypeId);
}

// Preparatory native rendering is a MUTATION, even when it targets an owned
// texture. It must run inside Capture/Restore, not in Execute::Prepare.
// Every failure after capture attempts all restores before choosing fallback.
// A failed replacement API call is still a submission attempt.
template<class Backend> DrawResult ExecuteProduction(bool enabled,
                                                     Backend& b) noexcept {
    static_assert(noexcept(b.Preflight()) && noexcept(b.Capture()) &&
                  noexcept(b.Produce()) && noexcept(b.Bind()) &&
                  noexcept(b.Restore()) && noexcept(b.Native()) &&
                  noexcept(b.Replace()) && noexcept(b.Quarantine()));
    if (!enabled || !b.Preflight() || !b.Capture())
        return {b.Native(), Submission::Native, true};
    if (!b.Produce() || !b.Bind()) {
        if (b.Restore()) return {b.Native(), Submission::Native, true};
        b.Quarantine();
        return {static_cast<std::int32_t>(0x8876086cu),
                Submission::SuppressedAfterRestoreFailure, false};
    }
    const std::int32_t result = b.Replace();
    const bool restored = b.Restore();
    if (!restored) b.Quarantine();
    return {result, Submission::Replacement, restored};
}
} // namespace wxl::water::slot3
