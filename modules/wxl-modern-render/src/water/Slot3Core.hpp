// R6 P01/A0/B1/Q0 contract. GPL-3.0-or-later.
// Portable production policy; this file alone does not activate replacement.
#pragma once
#include "WaterDiagCore.hpp"
#include <cmath>
#include <type_traits>

namespace wxl::water::slot3 {
struct Config {
    bool enabled = false;
    bool valid = true;
    unsigned reflectionMode = 1;
};
template<class Get> Config ParseConfig(Get get) {
    Config c;
    const char* value = get("WXL_CLASSIC_WATER_SLOT3");
    if (!value || std::strcmp(value, "0") == 0) return c;
    if (std::strcmp(value, "1") != 0) { c.valid = false; return c; }
    c.enabled = true;
    value = get("WXL_CLASSIC_WATER_SLOT3_REFLECTION_MODE");
    if (value && !waterdiag::Decimal(value, 1, 3, c.reflectionMode)) {
        c.valid = false;
        c.enabled = false;
    }
    return c;
}

// Exact deterministic selector-5 initializer, Classic 0x140BCBB70.
// Kept as eight float4s even though row 41 is not consumed by slot 3.
using Float4 = std::array<float, 4>;
inline constexpr std::array<Float4, 8> kSelector5{{
    {{0.f, 10000000.f, 0.f, 0.f}}, {{0.f, 0.f, 1.f, -10000.f}},
    {{0.f, 0.f, 0.f, 0.f}}, {{0.f, 0.f, 0.f, 0.f}},
    {{1.f, 0.f, 0.f, 0.f}}, {{0.f, 0.f, 0.f, 10000000.f}},
    {{0.f, 0.f, 1.f, 1.f}}, {{0.f, 0.f, 1.f, 0.f}}
}};
inline constexpr char kNormalAsset[] = "XTextures\\ocean\\newoceanbump.blp";
inline constexpr char kChopAsset[] = "XTextures\\foam\\waterchop7bw.blp";

struct DepthProjection {
    float a = 0.f, b = 0.f;
    bool valid = false;
    static DepthProjection FromWorldProjection(const float* matrix) noexcept {
        if (!matrix) return {};
        for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(matrix[i])) return {};
        // The frozen R3 relation applies to the standard perspective form only.
        if (matrix[3] != 0.f || matrix[7] != 0.f || matrix[11] != 1.f ||
            matrix[15] != 0.f || matrix[14] == 0.f) return {};
        return {matrix[10], matrix[14], true};
    }
    bool Reconstruct(float deviceDepth, float& viewZ) const noexcept {
        if (!valid || !std::isfinite(deviceDepth) || deviceDepth < 0.f || deviceDepth > 1.f)
            return false;
        const float denominator = deviceDepth - a;
        if (denominator == 0.f) return false;
        const float result = b / denominator; // Exact R3 authority, no epsilon/clamp.
        if (!std::isfinite(result) || result <= 0.f) return false;
        viewZ = result;
        return true;
    }
};

struct Stamp {
    uintptr_t device = 0, sourceRt = 0;
    uint64_t generation = 0, scene = 0, frame = 0, ordinal = 0;
};
inline bool BeforeSameDraw(const Stamp& producer, const Stamp& draw) noexcept {
    return producer.device && producer.sourceRt && producer.generation && producer.scene &&
        producer.frame && producer.ordinal && producer.device == draw.device &&
        producer.sourceRt == draw.sourceRt && producer.generation == draw.generation &&
        producer.scene == draw.scene && producer.frame == draw.frame && producer.ordinal < draw.ordinal;
}
struct Prerequisites {
    waterdiag::ProfileKey profile{};
    unsigned streams = 0, stride = 0;
    Stamp draw{}, colour{}, depth{}, reflection{};
    bool colourReady = false, depthReady = false, reflectionReady = false;
    bool exactNormalReady = false, exactChopReady = false, constantsReady = false;
    bool shadersReady = false, aboveWater = false, volumeFogDisabled = false;
    bool deviceLost = true, quarantined = false, inReflection = false;
    float selector = 0.f, waterPlane = 0.f, reflectionPlane = 0.f;
    unsigned areaExtras = 0, reflectionBranch = 0, rippleQuality = 0;
    unsigned producedReflectionMode = 0;
    DepthProjection projection{};
};
enum class Rejection {
    None, Disabled, DeviceUnavailable, ReflectionRecursion, Profile, Geometry,
    Permutation, Selector, VolumeFog, BelowWater, Colour, Depth, Reflection,
    MaterialAssets, Constants, Shaders
};
inline Rejection Evaluate(const Config& c, const Prerequisites& p) noexcept {
    if (!c.valid || !c.enabled) return Rejection::Disabled;
    if (p.deviceLost || p.quarantined) return Rejection::DeviceUnavailable;
    if (p.inReflection) return Rejection::ReflectionRecursion;
    if (waterdiag::AllowlistedReplacementProfile(p.profile) != waterdiag::ReplacementProfile::P01)
        return Rejection::Profile;
    if (p.streams != 1 || p.stride != 44) return Rejection::Geometry;
    if (p.areaExtras != 0 || p.reflectionBranch != 1 || p.rippleQuality != 0)
        return Rejection::Permutation;
    if (p.selector != 5.f) return Rejection::Selector; // Rejects NaN as well.
    if (!p.volumeFogDisabled) return Rejection::VolumeFog;
    if (!p.aboveWater) return Rejection::BelowWater;
    if (!p.colourReady || !BeforeSameDraw(p.colour, p.draw)) return Rejection::Colour;
    if (!p.depthReady || !p.projection.valid || !BeforeSameDraw(p.depth, p.draw)) return Rejection::Depth;
    if (!p.reflectionReady || c.reflectionMode < 1 || c.reflectionMode > 3 ||
        p.producedReflectionMode != c.reflectionMode || !BeforeSameDraw(p.reflection, p.draw) ||
        !std::isfinite(p.waterPlane) || !std::isfinite(p.reflectionPlane) ||
        p.waterPlane != p.reflectionPlane) return Rejection::Reflection;
    if (!p.exactNormalReady || !p.exactChopReady) return Rejection::MaterialAssets;
    if (!p.constantsReady) return Rejection::Constants;
    if (!p.shadersReady) return Rejection::Shaders;
    return Rejection::None;
}

// Consumed by the final-DIP integration only once ALL producers are wired.
// Backend contract: Prepare is output-neutral; Capture is read-only. Every
// method is noexcept. Restore must restore partially-applied Bind state too.
// The transaction owns both submission choices, so no caller issues fallback.
// A replacement API failure is still a submission attempt: never draw twice.
enum class Submission { Native, Replacement, SuppressedAfterRestoreFailure };
struct DrawResult { int32_t result; Submission submission; bool restoreOk; };
template<class Backend> DrawResult Execute(Rejection rejection, Backend& b) noexcept {
    static_assert(noexcept(b.Prepare()) && noexcept(b.Capture()) && noexcept(b.Bind()) &&
        noexcept(b.Restore()) && noexcept(b.Native()) && noexcept(b.Replace()) && noexcept(b.Quarantine()));
    if (rejection != Rejection::None || !b.Prepare() || !b.Capture())
        return {b.Native(), Submission::Native, true};
    if (!b.Bind()) {
        if (b.Restore()) return {b.Native(), Submission::Native, true};
        // Untouched native state cannot be promised after a failed D3D restore.
        // Do not render native with partially-mutated state; quarantine instead.
        b.Quarantine();
        return {static_cast<int32_t>(0x8876086cu), Submission::SuppressedAfterRestoreFailure, false};
    }
    const int32_t result = b.Replace();
    const bool restored = b.Restore();
    if (!restored) b.Quarantine();
    return {result, Submission::Replacement, restored};
}

// No automatic restoration to an invented false value on nested scopes.
struct ReflectionGuard {
    bool& active;
    bool entered;
    explicit ReflectionGuard(bool& value) noexcept : active(value), entered(!value) {
        if (entered) active = true;
    }
    ~ReflectionGuard() { if (entered) active = false; }
    ReflectionGuard(const ReflectionGuard&) = delete;
    ReflectionGuard& operator=(const ReflectionGuard&) = delete;
};
} // namespace wxl::water::slot3
