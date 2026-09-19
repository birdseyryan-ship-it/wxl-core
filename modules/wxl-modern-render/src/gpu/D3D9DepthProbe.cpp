// R3D0: Proton D3D9 readable-depth capability probe.
//
// This is deliberately diagnostic only. It does not replace the engine depth
// buffer, issue RESZ, enable AO, or otherwise modify the rendered frame.

#include "gpu/D3D9DepthProbe.hpp"

#include "common/Log.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

namespace wxl::scripts::render_modern::d3d9depthprobe
{
    namespace
    {
        IDirect3DDevice9* g_device = nullptr;
        bool g_probed = false;

        constexpr std::uint32_t FourCC(
            char a, char b, char c, char d)
        {
            return
                static_cast<std::uint32_t>(
                    static_cast<unsigned char>(a)) |
                (static_cast<std::uint32_t>(
                    static_cast<unsigned char>(b)) << 8) |
                (static_cast<std::uint32_t>(
                    static_cast<unsigned char>(c)) << 16) |
                (static_cast<std::uint32_t>(
                    static_cast<unsigned char>(d)) << 24);
        }

        constexpr D3DFORMAT kIntz =
            static_cast<D3DFORMAT>(FourCC('I','N','T','Z'));

        constexpr D3DFORMAT kResz =
            static_cast<D3DFORMAT>(FourCC('R','E','S','Z'));

        constexpr D3DFORMAT kDf24 =
            static_cast<D3DFORMAT>(FourCC('D','F','2','4'));

        const char* Result(HRESULT hr)
        {
            return SUCCEEDED(hr) ? "PASS" : "FAIL";
        }

        void Release(IDirect3DTexture9*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }
    }

    void Reset()
    {
        g_device = nullptr;
        g_probed = false;
    }

    void ProbeOnce(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        if (device != g_device)
        {
            g_device = device;
            g_probed = false;
        }

        if (g_probed)
            return;

        g_probed = true;

        IDirect3D9* d3d = nullptr;
        IDirect3DSurface9* depth = nullptr;
        IDirect3DSwapChain9* swap = nullptr;

        D3DDEVICE_CREATION_PARAMETERS cp = {};
        D3DDISPLAYMODE display = {};
        D3DPRESENT_PARAMETERS pp = {};
        D3DSURFACE_DESC depthDesc = {};

        const HRESULT cpHr =
            device->GetCreationParameters(&cp);

        const HRESULT d3dHr =
            device->GetDirect3D(&d3d);

        HRESULT displayHr = D3DERR_INVALIDCALL;
        HRESULT swapHr = D3DERR_INVALIDCALL;
        HRESULT ppHr = D3DERR_INVALIDCALL;
        HRESULT depthHr =
            device->GetDepthStencilSurface(&depth);

        if (d3d && SUCCEEDED(cpHr))
        {
            displayHr = d3d->GetAdapterDisplayMode(
                cp.AdapterOrdinal, &display);
        }

        swapHr = device->GetSwapChain(0, &swap);
        if (SUCCEEDED(swapHr) && swap)
            ppHr = swap->GetPresentParameters(&pp);

        if (SUCCEEDED(depthHr) && depth)
            depthHr = depth->GetDesc(&depthDesc);

        WLOG_INFO(
            "wxl-modern-depthprobe: BEGIN "
            "cp=%s d3d=%s display=%s swap=%s pp=%s depth=%s",
            Result(cpHr),
            Result(d3dHr),
            Result(displayHr),
            Result(swapHr),
            Result(ppHr),
            Result(depthHr));

        if (SUCCEEDED(depthHr))
        {
            WLOG_INFO(
                "wxl-modern-depthprobe: bound depth "
                "%ux%u fmt=%u msaa=%u quality=%lu",
                depthDesc.Width,
                depthDesc.Height,
                static_cast<unsigned>(depthDesc.Format),
                static_cast<unsigned>(depthDesc.MultiSampleType),
                static_cast<unsigned long>(
                    depthDesc.MultiSampleQuality));
        }

        HRESULT intzCheck = D3DERR_INVALIDCALL;
        HRESULT df24Check = D3DERR_INVALIDCALL;
        HRESULT reszCheck = D3DERR_INVALIDCALL;
        HRESULT depthMsaaCheck = D3DERR_INVALIDCALL;

        if (d3d &&
            SUCCEEDED(cpHr) &&
            SUCCEEDED(displayHr))
        {
            intzCheck = d3d->CheckDeviceFormat(
                cp.AdapterOrdinal,
                cp.DeviceType,
                display.Format,
                D3DUSAGE_DEPTHSTENCIL,
                D3DRTYPE_TEXTURE,
                kIntz);

            df24Check = d3d->CheckDeviceFormat(
                cp.AdapterOrdinal,
                cp.DeviceType,
                display.Format,
                D3DUSAGE_DEPTHSTENCIL,
                D3DRTYPE_TEXTURE,
                kDf24);

            reszCheck = d3d->CheckDeviceFormat(
                cp.AdapterOrdinal,
                cp.DeviceType,
                display.Format,
                D3DUSAGE_RENDERTARGET,
                D3DRTYPE_SURFACE,
                kResz);

            if (SUCCEEDED(depthHr))
            {
                depthMsaaCheck =
                    d3d->CheckDeviceMultiSampleType(
                        cp.AdapterOrdinal,
                        cp.DeviceType,
                        depthDesc.Format,
                        SUCCEEDED(ppHr) ? pp.Windowed : TRUE,
                        depthDesc.MultiSampleType,
                        nullptr);
            }
        }

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ CheckDeviceFormat %s "
            "hr=0x%08X",
            Result(intzCheck),
            static_cast<unsigned>(intzCheck));

        WLOG_INFO(
            "wxl-modern-depthprobe: DF24 CheckDeviceFormat %s "
            "hr=0x%08X",
            Result(df24Check),
            static_cast<unsigned>(df24Check));

        WLOG_INFO(
            "wxl-modern-depthprobe: RESZ CheckDeviceFormat %s "
            "hr=0x%08X",
            Result(reszCheck),
            static_cast<unsigned>(reszCheck));

        WLOG_INFO(
            "wxl-modern-depthprobe: bound-depth MSAA support %s "
            "hr=0x%08X",
            Result(depthMsaaCheck),
            static_cast<unsigned>(depthMsaaCheck));

        UINT testW = 64;
        UINT testH = 64;

        if (SUCCEEDED(depthHr))
        {
            testW = depthDesc.Width;
            testH = depthDesc.Height;
        }

        IDirect3DTexture9* intzTex = nullptr;
        IDirect3DTexture9* df24Tex = nullptr;

        const HRESULT intzCreate =
            device->CreateTexture(
                testW,
                testH,
                1,
                D3DUSAGE_DEPTHSTENCIL,
                kIntz,
                D3DPOOL_DEFAULT,
                &intzTex,
                nullptr);

        const HRESULT df24Create =
            device->CreateTexture(
                testW,
                testH,
                1,
                D3DUSAGE_DEPTHSTENCIL,
                kDf24,
                D3DPOOL_DEFAULT,
                &df24Tex,
                nullptr);

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ CreateTexture "
            "%ux%u %s hr=0x%08X",
            testW,
            testH,
            Result(intzCreate),
            static_cast<unsigned>(intzCreate));

        WLOG_INFO(
            "wxl-modern-depthprobe: DF24 CreateTexture "
            "%ux%u %s hr=0x%08X",
            testW,
            testH,
            Result(df24Create),
            static_cast<unsigned>(df24Create));

        const bool candidate =
            SUCCEEDED(depthHr) &&
            depthDesc.MultiSampleType != D3DMULTISAMPLE_NONE &&
            SUCCEEDED(intzCheck) &&
            SUCCEEDED(intzCreate) &&
            SUCCEEDED(reszCheck);

        WLOG_INFO(
            "wxl-modern-depthprobe: MSAA readable-depth candidate %s "
            "(requires live RESZ proof next)",
            candidate ? "PASS" : "FAIL");

        WLOG_INFO(
            "wxl-modern-depthprobe: END");

        Release(intzTex);
        Release(df24Tex);

        if (depth)
            depth->Release();

        if (swap)
            swap->Release();

        if (d3d)
            d3d->Release();
    }
}
