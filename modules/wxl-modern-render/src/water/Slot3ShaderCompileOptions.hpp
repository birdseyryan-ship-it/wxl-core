#pragma once
#ifdef _WIN32
#include <windows.h>
#include <d3dcompiler.h>

namespace wxl::water::slot3::shaders {
// SM3 uses legacy sampler2D/tex2D syntax. ENABLE_STRICTNESS rejects that
// syntax (X3086); it is not the separate IEEE_STRICTNESS arithmetic option.
// Keep the optimization level unchanged and share the exact runtime/CI flags.
inline constexpr unsigned kLegacySm3CompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
}
#endif
