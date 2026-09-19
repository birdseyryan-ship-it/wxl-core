#pragma once

// GFX-R2 compatibility seam for wxl-modern-render.
//
// R2 intentionally provides only the API surface required for a completely
// disabled/passthrough modern renderer. The readable-depth redirect is
// implemented in R3 when depth-using post-processing is enabled.

namespace wxl::runtime::render
{
    void SetReadableDepthNeeded(bool needed);
}
