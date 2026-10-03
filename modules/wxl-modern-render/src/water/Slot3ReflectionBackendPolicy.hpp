// R6 Slot-3 selected-reflection native-backend policy.
//
// This file contains portable policy only.  It does not perform D3D work,
// call native WoW functions, or enable the replacement path.
//
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#include "water/Slot3ReflectionRuntime.hpp"

#include <cstdint>

namespace wxl::water::slot3
{
    // Closed bridge:
    // Classic reflection format ID 6
    //   -> low-level Gx format 2
    //   -> Wrath D3D9 numeric format 21
    //   -> D3DFMT_A8R8G8B8.
    constexpr unsigned kNativeReflectionD3dColourFormat = 21u;

    inline bool NativeReflectionFormatSupported(
        unsigned format) noexcept
    {
        return format ==
            kNativeReflectionD3dColourFormat;
    }

    // First backend deliberately supports only the mechanically-closed
    // outdoor path.  Any active primary/secondary WMO viewer-group state
    // requires the separate portal/interior branch and therefore fails
    // closed to untouched native water.
    inline bool NativeReflectionOutdoorStateSupported(
        std::uintptr_t viewerGroup,
        unsigned secondGroupFlag) noexcept
    {
        return
            viewerGroup == 0 &&
            secondGroupFlag == 0;
    }

    inline bool NativeReflectionPlanSupported(
        const ReflectionPlan& plan) noexcept
    {
        return
            plan.valid &&
            SupportedReflectionMode(plan.mode) &&
            plan.targetWidth != 0 &&
            plan.targetHeight != 0 &&
            NativeReflectionFormatSupported(
                plan.d3dColourFormat);
    }
}
