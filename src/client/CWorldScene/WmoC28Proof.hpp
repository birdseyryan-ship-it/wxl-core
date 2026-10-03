// R8 Step 11B-02B2A. Default-OFF read-only WMO constant proof. GPL-3.0-or-later.
#pragma once
#include "client/CWorldScene/WmoC28ProofCore.hpp"
#include <cstdint>

namespace wxl::r8::materials
{
// Called only after validated proof configuration. No static event subscription.
void InitializeWmoC28Proof();
void EnterWmoC28ProofScope(std::uintptr_t root) noexcept;
void LeaveWmoC28ProofScope() noexcept;
// Invalidate BEFORE every native EffectBind, even an unqualified/non-WMO bind.
void InvalidateWmoC28Proof() noexcept;
// Collection hashes are acquired directly after native EffectBind; never inferred
// from the hardware shader at that earlier seam.
void ObserveWmoC28Bind(WmoFamily family, const char* path, unsigned vtx, unsigned pix,
                       std::uint32_t collection, const char* vs, const char* raw,
                       const char* ps) noexcept;
}
