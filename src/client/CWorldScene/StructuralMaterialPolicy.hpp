// R8 Step 11 structural-material candidate policy.
// Pure classification only: no hooks, D3D state, visibility or material mutation.
// GPL-3.0-or-later.

#pragma once

#include <cstdint>

namespace wxl::r8::materials
{
    enum class WmoFamily : int
    {
        Unknown   = -1,
        Diffuse   = 0,
        Specular  = 1,
        Metal     = 2,
        Env       = 3,
        Opaque    = 4,
        EnvMetal  = 5,
        Composite = 6
    };

    // Step-10 live census + exact Classic/Wrath differential:
    // initial implementation tranche only.
    constexpr bool IsInitialWmoCandidate(WmoFamily family)
    {
        return
            family == WmoFamily::Diffuse ||
            family == WmoFamily::Opaque ||
            family == WmoFamily::Specular;
    }

    // Step-10 live M2 census:
    // raw shader IDs 0 and 16 represented the ordinary overwhelmingly
    // dominant routes. This does NOT by itself authorize shader replacement;
    // the final combiner/input contract must also be positively identified.
    constexpr bool IsInitialM2RawShaderId(std::uint16_t shaderId)
    {
        return shaderId == 0 || shaderId == 16;
    }

    // Observed live but not decoded. Explicitly retained as stock/fallback.
    constexpr bool IsExplicitlyDeferredM2RawShaderId(std::uint16_t shaderId)
    {
        return shaderId == 0x8002u;
    }

    static_assert(IsInitialWmoCandidate(WmoFamily::Diffuse));
    static_assert(IsInitialWmoCandidate(WmoFamily::Opaque));
    static_assert(IsInitialWmoCandidate(WmoFamily::Specular));

    static_assert(!IsInitialWmoCandidate(WmoFamily::Metal));
    static_assert(!IsInitialWmoCandidate(WmoFamily::Env));
    static_assert(!IsInitialWmoCandidate(WmoFamily::EnvMetal));
    static_assert(!IsInitialWmoCandidate(WmoFamily::Composite));
    static_assert(!IsInitialWmoCandidate(WmoFamily::Unknown));

    static_assert(IsInitialM2RawShaderId(0));
    static_assert(IsInitialM2RawShaderId(16));
    static_assert(!IsInitialM2RawShaderId(0x8002u));
    static_assert(IsExplicitlyDeferredM2RawShaderId(0x8002u));
}
