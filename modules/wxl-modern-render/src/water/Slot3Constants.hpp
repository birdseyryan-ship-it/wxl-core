// R6 P01/A0/B1/Q0 constant packet contract. GPL-3.0-or-later.
//
// This is deliberately a packet/validation layer, not a semantic guesser.
// Callers must provide every recovered Classic CB1 row consumed by the narrow
// VS0/PS3 translation from a separately grounded producer. Selector-5 CB5 rows
// are injected here from the exact recovered initializer. Missing/non-finite
// input fails closed; no row is fabricated from visual intuition.
#pragma once

#include "water/Slot3Core.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace wxl::water::slot3
{
inline constexpr unsigned kVsRegisterCount = 29; // c0..c28
inline constexpr unsigned kPsRegisterCount = 48; // c0..c47

using VsRegisters = std::array<Float4, kVsRegisterCount>;
using PsRegisters = std::array<Float4, kPsRegisterCount>;
using VsReadyMask = std::array<bool, kVsRegisterCount>;
using PsReadyMask = std::array<bool, kPsRegisterCount>;

// Exact direct reads in the translated VS0 body.
inline constexpr std::array<unsigned, 25> kRequiredVsRows{{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17,
    19, 20, 21, 23, 24, 25, 27, 28
}};

// Exact direct CB1 reads that survive in the disabled-VolumeFog / selector-5
// PS3 specialization. c40/c42..c47 are injected separately below.
inline constexpr std::array<unsigned, 16> kRequiredPsCb1Rows{{
    0, 1, 2, 3, 4, 5, 13, 14, 15, 16, 17, 18, 21, 28, 31, 32
}};

struct ConstantInputs
{
    VsRegisters vs{};
    PsRegisters ps{};
    VsReadyMask vsReady{};
    PsReadyMask psReady{};

    bool SetVs(unsigned row, const Float4& value) noexcept
    {
        if (row >= vs.size()) return false;
        vs[row] = value;
        vsReady[row] = true;
        return true;
    }

    bool SetPs(unsigned row, const Float4& value) noexcept
    {
        if (row >= ps.size()) return false;
        ps[row] = value;
        psReady[row] = true;
        return true;
    }
};

struct ConstantPacket
{
    VsRegisters vs{};
    PsRegisters ps{};
    bool valid = false;
};

inline bool Finite(const Float4& value) noexcept
{
    for (float component : value)
        if (!std::isfinite(component)) return false;
    return true;
}

inline bool BuildConstantPacket(const ConstantInputs& input,
                                ConstantPacket& output) noexcept
{
    ConstantPacket candidate{};

    for (unsigned row : kRequiredVsRows)
    {
        if (!input.vsReady[row] || !Finite(input.vs[row])) return false;
        candidate.vs[row] = input.vs[row];
    }

    for (unsigned row : kRequiredPsCb1Rows)
    {
        if (!input.psReady[row] || !Finite(input.ps[row])) return false;
        candidate.ps[row] = input.ps[row];
    }

    // First-candidate specialization is selector 5 only. Preserve the exact
    // 8-row recovered bank at c40..c47, including unused row 41, so the CPU
    // packet remains an auditable representation of Classic 0x140BCBB70.
    for (unsigned i = 0; i < kSelector5.size(); ++i)
        candidate.ps[40 + i] = kSelector5[i];

    candidate.valid = true;
    output = candidate;
    return true;
}

inline bool ExactSelector5Bank(const ConstantPacket& packet) noexcept
{
    if (!packet.valid) return false;
    for (unsigned i = 0; i < kSelector5.size(); ++i)
        if (packet.ps[40 + i] != kSelector5[i]) return false;
    return true;
}
} // namespace wxl::water::slot3
