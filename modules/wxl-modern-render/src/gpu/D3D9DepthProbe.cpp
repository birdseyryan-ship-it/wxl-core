// R3D0B: Proton D3D9 readable-depth capability + transfer probe.
//
// Diagnostic only. No resource produced here is used by the renderer.

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
        bool g_transfersProbed = false;

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

        template<class T>
        void Release(T*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        HRESULT TextureContainer(IDirect3DSurface9* surface)
        {
            if (!surface)
                return D3DERR_INVALIDCALL;

            IDirect3DTexture9* texture = nullptr;

            const HRESULT hr =
                surface->GetContainer(
                    __uuidof(IDirect3DTexture9),
                    reinterpret_cast<void**>(&texture));

            Release(texture);
            return hr;
        }
    }

    void Reset()
    {
        g_device = nullptr;
        g_probed = false;
        g_transfersProbed = false;
    }

    void ProbeOnce(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        if (device != g_device)
        {
            g_device = device;
            g_probed = false;
            g_transfersProbed = false;
        }

        if (g_probed)
            return;

        g_probed = true;

        IDirect3D9* d3d = nullptr;
        IDirect3DSurface9* depth = nullptr;
        IDirect3DSurface9* backbuffer = nullptr;
        IDirect3DSwapChain9* swap = nullptr;

        D3DDEVICE_CREATION_PARAMETERS cp = {};
        D3DDISPLAYMODE display = {};
        D3DPRESENT_PARAMETERS pp = {};
        D3DSURFACE_DESC depthDesc = {};
        D3DSURFACE_DESC bbDesc = {};

        const HRESULT cpHr =
            device->GetCreationParameters(&cp);

        const HRESULT d3dHr =
            device->GetDirect3D(&d3d);

        HRESULT displayHr = D3DERR_INVALIDCALL;
        HRESULT swapHr = D3DERR_INVALIDCALL;
        HRESULT ppHr = D3DERR_INVALIDCALL;

        HRESULT depthHr =
            device->GetDepthStencilSurface(&depth);

        HRESULT bbHr =
            device->GetBackBuffer(
                0, 0,
                D3DBACKBUFFER_TYPE_MONO,
                &backbuffer);

        if (d3d && SUCCEEDED(cpHr))
            displayHr =
                d3d->GetAdapterDisplayMode(
                    cp.AdapterOrdinal, &display);

        swapHr = device->GetSwapChain(0, &swap);

        if (SUCCEEDED(swapHr) && swap)
            ppHr = swap->GetPresentParameters(&pp);

        if (SUCCEEDED(depthHr) && depth)
            depthHr = depth->GetDesc(&depthDesc);

        if (SUCCEEDED(bbHr) && backbuffer)
            bbHr = backbuffer->GetDesc(&bbDesc);

        WLOG_INFO(
            "wxl-modern-depthprobe: BEGIN "
            "cp=%s d3d=%s display=%s swap=%s pp=%s "
            "depth=%s bb=%s",
            Result(cpHr),
            Result(d3dHr),
            Result(displayHr),
            Result(swapHr),
            Result(ppHr),
            Result(depthHr),
            Result(bbHr));

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
        HRESULT intzMsaaCheck = D3DERR_INVALIDCALL;
        HRESULT intzMatch = D3DERR_INVALIDCALL;

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

                intzMsaaCheck =
                    d3d->CheckDeviceMultiSampleType(
                        cp.AdapterOrdinal,
                        cp.DeviceType,
                        kIntz,
                        SUCCEEDED(ppHr) ? pp.Windowed : TRUE,
                        depthDesc.MultiSampleType,
                        nullptr);
            }

            if (SUCCEEDED(bbHr))
            {
                intzMatch =
                    d3d->CheckDepthStencilMatch(
                        cp.AdapterOrdinal,
                        cp.DeviceType,
                        display.Format,
                        bbDesc.Format,
                        kIntz);
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

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ matching-MSAA support %s "
            "hr=0x%08X",
            Result(intzMsaaCheck),
            static_cast<unsigned>(intzMsaaCheck));

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ depth-stencil match %s "
            "hr=0x%08X",
            Result(intzMatch),
            static_cast<unsigned>(intzMatch));

        UINT testW = 64;
        UINT testH = 64;

        if (SUCCEEDED(depthHr))
        {
            testW = depthDesc.Width;
            testH = depthDesc.Height;
        }

        IDirect3DTexture9* intzTex = nullptr;
        IDirect3DTexture9* df24Tex = nullptr;

        IDirect3DSurface9* intzPlain = nullptr;
        IDirect3DSurface9* intzMsaa = nullptr;

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

        const HRESULT intzPlainCreate =
            device->CreateDepthStencilSurface(
                testW,
                testH,
                kIntz,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,
                &intzPlain,
                nullptr);

        HRESULT intzMsaaCreate = D3DERR_INVALIDCALL;

        if (SUCCEEDED(depthHr))
        {
            intzMsaaCreate =
                device->CreateDepthStencilSurface(
                    testW,
                    testH,
                    kIntz,
                    depthDesc.MultiSampleType,
                    depthDesc.MultiSampleQuality,
                    FALSE,
                    &intzMsaa,
                    nullptr);
        }

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

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ plain depth surface %s "
            "hr=0x%08X",
            Result(intzPlainCreate),
            static_cast<unsigned>(intzPlainCreate));

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ MSAA depth surface %s "
            "hr=0x%08X",
            Result(intzMsaaCreate),
            static_cast<unsigned>(intzMsaaCreate));

        const HRESULT boundContainer =
            TextureContainer(depth);

        const HRESULT plainContainer =
            TextureContainer(intzPlain);

        const HRESULT msaaContainer =
            TextureContainer(intzMsaa);

        WLOG_INFO(
            "wxl-modern-depthprobe: bound-depth texture container %s "
            "hr=0x%08X",
            Result(boundContainer),
            static_cast<unsigned>(boundContainer));

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ plain texture container %s "
            "hr=0x%08X",
            Result(plainContainer),
            static_cast<unsigned>(plainContainer));

        WLOG_INFO(
            "wxl-modern-depthprobe: INTZ MSAA texture container %s "
            "hr=0x%08X",
            Result(msaaContainer),
            static_cast<unsigned>(msaaContainer));

        WLOG_INFO(
            "wxl-modern-depthprobe: END CAPABILITIES");

        Release(intzMsaa);
        Release(intzPlain);
        Release(intzTex);
        Release(df24Tex);
        Release(depth);
        Release(backbuffer);
        Release(swap);
        Release(d3d);
    }

    void ProbeTransfersOnce(IDirect3DDevice9* device,
                            IDirect3DSurface9* boundDepth)
    {
        if (!device || !boundDepth || g_transfersProbed)
            return;

        g_transfersProbed = true;

        D3DSURFACE_DESC desc = {};

        const HRESULT descHr =
            boundDepth->GetDesc(&desc);

        if (FAILED(descHr))
        {
            WLOG_ERROR(
                "wxl-modern-depthprobe: transfer probe "
                "depth GetDesc failed hr=0x%08X",
                static_cast<unsigned>(descHr));
            return;
        }

        WLOG_INFO(
            "wxl-modern-depthprobe: BEGIN TRANSFERS "
            "%ux%u fmt=%u msaa=%u",
            desc.Width,
            desc.Height,
            static_cast<unsigned>(desc.Format),
            static_cast<unsigned>(desc.MultiSampleType));

        IDirect3DTexture9* intzTex = nullptr;
        IDirect3DSurface9* intzTexSurface = nullptr;

        HRESULT hr =
            device->CreateTexture(
                desc.Width,
                desc.Height,
                1,
                D3DUSAGE_DEPTHSTENCIL,
                kIntz,
                D3DPOOL_DEFAULT,
                &intzTex,
                nullptr);

        if (SUCCEEDED(hr) && intzTex)
            hr = intzTex->GetSurfaceLevel(
                0, &intzTexSurface);

        const HRESULT intzTextureReady = hr;

        WLOG_INFO(
            "wxl-modern-depthprobe: transfer INTZ texture target %s "
            "hr=0x%08X",
            Result(intzTextureReady),
            static_cast<unsigned>(intzTextureReady));

        IDirect3DSurface9* samePlain = nullptr;

        const HRESULT samePlainCreate =
            device->CreateDepthStencilSurface(
                desc.Width,
                desc.Height,
                desc.Format,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,
                &samePlain,
                nullptr);

        WLOG_INFO(
            "wxl-modern-depthprobe: transfer same-format plain target "
            "%s hr=0x%08X",
            Result(samePlainCreate),
            static_cast<unsigned>(samePlainCreate));

        IDirect3DSurface9* intzPlain = nullptr;

        const HRESULT intzPlainCreate =
            device->CreateDepthStencilSurface(
                desc.Width,
                desc.Height,
                kIntz,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,
                &intzPlain,
                nullptr);

        WLOG_INFO(
            "wxl-modern-depthprobe: transfer INTZ plain target "
            "%s hr=0x%08X",
            Result(intzPlainCreate),
            static_cast<unsigned>(intzPlainCreate));

        HRESULT directToIntzTexture =
            D3DERR_INVALIDCALL;

        if (intzTexSurface)
        {
            directToIntzTexture =
                device->StretchRect(
                    boundDepth,
                    nullptr,
                    intzTexSurface,
                    nullptr,
                    D3DTEXF_NONE);
        }

        WLOG_INFO(
            "wxl-modern-depthprobe: StretchRect "
            "MSAA-D24X8 -> INTZ-texture %s hr=0x%08X",
            Result(directToIntzTexture),
            static_cast<unsigned>(directToIntzTexture));

        HRESULT toSamePlain =
            D3DERR_INVALIDCALL;

        if (samePlain)
        {
            toSamePlain =
                device->StretchRect(
                    boundDepth,
                    nullptr,
                    samePlain,
                    nullptr,
                    D3DTEXF_NONE);
        }

        WLOG_INFO(
            "wxl-modern-depthprobe: StretchRect "
            "MSAA-depth -> single-sample same-format plain %s "
            "hr=0x%08X",
            Result(toSamePlain),
            static_cast<unsigned>(toSamePlain));

        HRESULT directToIntzPlain =
            D3DERR_INVALIDCALL;

        if (intzPlain)
        {
            directToIntzPlain =
                device->StretchRect(
                    boundDepth,
                    nullptr,
                    intzPlain,
                    nullptr,
                    D3DTEXF_NONE);
        }

        WLOG_INFO(
            "wxl-modern-depthprobe: StretchRect "
            "MSAA-D24X8 -> INTZ-plain %s hr=0x%08X",
            Result(directToIntzPlain),
            static_cast<unsigned>(directToIntzPlain));

        HRESULT samePlainToIntzTexture =
            D3DERR_INVALIDCALL;

        if (SUCCEEDED(toSamePlain) &&
            samePlain &&
            intzTexSurface)
        {
            samePlainToIntzTexture =
                device->StretchRect(
                    samePlain,
                    nullptr,
                    intzTexSurface,
                    nullptr,
                    D3DTEXF_NONE);
        }

        WLOG_INFO(
            "wxl-modern-depthprobe: StretchRect "
            "plain-D24X8 -> INTZ-texture %s hr=0x%08X",
            Result(samePlainToIntzTexture),
            static_cast<unsigned>(
                samePlainToIntzTexture));

        const bool readableEscape =
            SUCCEEDED(directToIntzTexture) ||
            (SUCCEEDED(toSamePlain) &&
             SUCCEEDED(samePlainToIntzTexture));

        WLOG_INFO(
            "wxl-modern-depthprobe: readable-depth transfer escape %s",
            readableEscape ? "PASS" : "FAIL");

        WLOG_INFO(
            "wxl-modern-depthprobe: END TRANSFERS");

        Release(intzPlain);
        Release(samePlain);
        Release(intzTexSurface);
        Release(intzTex);
    }
}
