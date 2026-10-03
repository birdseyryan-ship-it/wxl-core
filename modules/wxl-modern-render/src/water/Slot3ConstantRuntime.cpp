// R6 P01/A0/B1/Q0 D3D9 constant uploader. GPL-3.0-or-later.
#include "water/Slot3ConstantRuntime.hpp"

#include <d3d9.h>

namespace wxl::water::slot3
{
bool UploadConstants(IDirect3DDevice9* device,
                     const ConstantPacket& packet) noexcept
{
    if (!device || !packet.valid || !ExactSelector5Bank(packet))
        return false;

    const HRESULT vs = device->SetVertexShaderConstantF(
        0,
        packet.vs[0].data(),
        static_cast<UINT>(packet.vs.size()));
    if (FAILED(vs)) return false;

    const HRESULT ps = device->SetPixelShaderConstantF(
        0,
        packet.ps[0].data(),
        static_cast<UINT>(packet.ps.size()));
    return SUCCEEDED(ps);
}
} // namespace wxl::water::slot3
