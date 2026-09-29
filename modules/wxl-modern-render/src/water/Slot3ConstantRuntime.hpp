// R6 P01/A0/B1/Q0 D3D9 constant uploader. GPL-3.0-or-later.
// Inert until called by a future fully-gated replacement transaction.
#pragma once

#include "water/Slot3Constants.hpp"

struct IDirect3DDevice9;

namespace wxl::water::slot3
{
// Uploads the exact typed packet to the explicit SM3 register ranges used by
// Slot3Shaders.hpp. This function performs no draw and binds no shader/texture.
bool UploadConstants(IDirect3DDevice9* device,
                     const ConstantPacket& packet) noexcept;
} // namespace wxl::water::slot3
