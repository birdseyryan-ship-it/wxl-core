// R8 Step 11B WMO per-pixel material candidate.
// Opt-in only. Unknown/unproven permutations remain stock.
// GPL-3.0-or-later.

#pragma once

#include "client/CWorldScene/StructuralMaterialPolicy.hpp"

#include <cstdint>

namespace wxl::r8::materials
{
    // Called immediately after native EffectBind. Native has therefore
    // already restored the stock GxState shader wrappers for this draw.
    void NoteWmoPerPixelNativeBind();

    // Attempts the narrowly allowlisted 11B-02B1 replacement.
    // false means exact stock/native state is retained.
    bool TryBindWmoPerPixelCandidate(
        WmoFamily family,
        std::uint32_t vtxIdx,
        std::uint32_t pixIdx);

    // If the last WMO draw ended with extension wrappers in GxState,
    // restore the native pair at the enclosing WMO-render boundary.
    void RestoreWmoPerPixelIfPending();
}
