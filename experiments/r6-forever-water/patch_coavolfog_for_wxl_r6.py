#!/usr/bin/env python3
"""
Apply the R6 WarcraftXL compatibility delta to pinned CoAVolFog source.

Scope:
- preserve multisampling over the existing WarcraftXL/DXVK d3d9 wrapper by
  adding a runtime-self-tested two-stage StretchRect depth copy:
  MSAA depth -> single-sample same-format depth -> INTZ;
- skip all fog rendering/preparation while the shipped R6 water-only profile
  has every fog density source disabled;
- leave water, grading, glow, far clip and the rest of upstream behavior
  otherwise unchanged.

The script is deliberately exact-string based and fails closed if the pinned
upstream source no longer matches the reviewed revision.
"""
from __future__ import annotations

import sys
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            f"STOP: expected exactly one patch site in {path}, found {count}"
        )
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_coavolfog_for_wxl_r6.py <coa-vfog-root>")

    root = Path(sys.argv[1]).resolve()
    hdr = root / "src" / "msaa_depth.h"
    cpp = root / "src" / "msaa_depth.cpp"
    wrap = root / "src" / "d3d9_wrap.cpp"

    for p in (hdr, cpp, wrap):
        if not p.is_file():
            raise SystemExit(f"STOP: missing pinned upstream source: {p}")

    replace_once(
        hdr,
        """enum class DepthCopyMethod
{
    None,
    Nvapi,
    Resz,
};""",
        """enum class DepthCopyMethod
{
    None,
    Nvapi,
    Resz,
    WxlStretch,
};""",
    )

    replace_once(
        hdr,
        """    DepthCopyMethod m_method = DepthCopyMethod::None;
    IDirect3DSurface9* m_source = nullptr;
    IDirect3DTexture9* m_destination = nullptr;""",
        """    DepthCopyMethod m_method = DepthCopyMethod::None;
    IDirect3DSurface9* m_source = nullptr;
    IDirect3DSurface9* m_intermediate = nullptr;
    IDirect3DTexture9* m_destination = nullptr;""",
    )

    replace_once(
        cpp,
        """    if (!LoadedFromSystemFolder(FirstMethodOf(d3d)))
        return {DepthCopyMethod::None, "d3d9.dll is not the system copy (DXVK or another wrapper), so NVAPI cannot "
                                       "copy its depth"};""",
        """    if (!LoadedFromSystemFolder(FirstMethodOf(d3d)))
        return {DepthCopyMethod::WxlStretch, ""};""",
    )

    replace_once(
        cpp,
        """    case DepthCopyMethod::Resz:
        return "RESZ";
    default:""",
        """    case DepthCopyMethod::Resz:
        return "RESZ";
    case DepthCopyMethod::WxlStretch:
        return "WXL two-stage StretchRect";
    default:""",
    )

    replace_once(
        cpp,
        """    if (method == DepthCopyMethod::Nvapi)
    {
        const NvapiD3D9& nvapi = Nvapi();
        if (nvapi.unavailable[0])
            return false;
        const int sourceStatus = GuardedResourceCall(nvapi.registerResource, source);
        const int destinationStatus = GuardedResourceCall(nvapi.registerResource, destination);
        if (sourceStatus != kNvapiOk || destinationStatus != kNvapiOk)
        {
            VF_LOG_ERROR("NvAPI_D3D9_RegisterResource returned %d for the multisampled depth and %d for INTZ",
                         sourceStatus, destinationStatus);
            if (sourceStatus == kNvapiOk)
                GuardedResourceCall(nvapi.unregisterResource, source);
            if (destinationStatus == kNvapiOk)
                GuardedResourceCall(nvapi.unregisterResource, destination);
            return false;
        }
    }
    source->AddRef();""",
        """    if (method == DepthCopyMethod::Nvapi)
    {
        const NvapiD3D9& nvapi = Nvapi();
        if (nvapi.unavailable[0])
            return false;
        const int sourceStatus = GuardedResourceCall(nvapi.registerResource, source);
        const int destinationStatus = GuardedResourceCall(nvapi.registerResource, destination);
        if (sourceStatus != kNvapiOk || destinationStatus != kNvapiOk)
        {
            VF_LOG_ERROR("NvAPI_D3D9_RegisterResource returned %d for the multisampled depth and %d for INTZ",
                         sourceStatus, destinationStatus);
            if (sourceStatus == kNvapiOk)
                GuardedResourceCall(nvapi.unregisterResource, source);
            if (destinationStatus == kNvapiOk)
                GuardedResourceCall(nvapi.unregisterResource, destination);
            return false;
        }
    }
    if (method == DepthCopyMethod::WxlStretch)
    {
        D3DSURFACE_DESC desc = {};
        IDirect3DDevice9* owner = nullptr;
        const bool described = SUCCEEDED(source->GetDesc(&desc));
        const bool gotDevice = described && SUCCEEDED(source->GetDevice(&owner)) && owner;
        const bool created =
            gotDevice &&
            SUCCEEDED(owner->CreateDepthStencilSurface(desc.Width, desc.Height, desc.Format,
                                                       D3DMULTISAMPLE_NONE, 0, FALSE,
                                                       &m_intermediate, nullptr));
        SafeRelease(owner);
        if (!created || !m_intermediate)
        {
            SafeRelease(m_intermediate);
            return false;
        }
    }
    source->AddRef();""",
    )

    replace_once(
        cpp,
        """    m_method = DepthCopyMethod::None;
    SafeRelease(m_source);
    SafeRelease(m_destination);""",
        """    m_method = DepthCopyMethod::None;
    SafeRelease(m_source);
    SafeRelease(m_intermediate);
    SafeRelease(m_destination);""",
    )

    replace_once(
        cpp,
        """    case DepthCopyMethod::Resz:
        return ResolveThroughResz(dev, m_destination);
    default:
        return false;""",
        """    case DepthCopyMethod::Resz:
        return ResolveThroughResz(dev, m_destination);
    case DepthCopyMethod::WxlStretch:
    {
        if (!m_intermediate || FAILED(dev->EndScene()))
            return false;

        IDirect3DSurface9* destinationSurface = nullptr;
        const bool level =
            SUCCEEDED(m_destination->GetSurfaceLevel(0, &destinationSurface)) &&
            destinationSurface;
        const bool stage1 =
            level &&
            SUCCEEDED(dev->StretchRect(m_source, nullptr, m_intermediate, nullptr, D3DTEXF_NONE));
        const bool stage2 =
            stage1 &&
            SUCCEEDED(dev->StretchRect(m_intermediate, nullptr, destinationSurface, nullptr, D3DTEXF_NONE));
        SafeRelease(destinationSurface);

        const HRESULT begin = dev->BeginScene();
        return level && stage1 && stage2 && SUCCEEDED(begin);
    }
    default:
        return false;""",
    )

    helper_anchor = """struct MultisampleDecision
{
    DepthCopyMethod method = DepthCopyMethod::None;
    const char* off = "";
};
}"""
    helper_replacement = """struct MultisampleDecision
{
    DepthCopyMethod method = DepthCopyMethod::None;
    const char* off = "";
};

bool R6WaterOnlyFogDisabled(const Config& cfg)
{
    return cfg.dataMode == 0 &&
           cfg.density <= 0.0f &&
           cfg.haze <= 0.0f &&
           cfg.groundFog <= 0.0f &&
           cfg.farFog <= 0.0f &&
           cfg.godRays <= 0.0f &&
           cfg.debugView == 0 &&
           !cfg.sunMarker;
}
}"""
    replace_once(wrap, helper_anchor, helper_replacement)

    replace_once(
        wrap,
        """bool FogDevice::Render(const FrameInputs& in, const Config& cfg, FogPass pass, const char** skip)
{
    bool ok = FogActive() && m_renderer.Render(m_real, Depth(), in, cfg, pass);
    if (skip)
        *skip = FogActive() ? m_renderer.LastSkipReason() : "fog inactive";
    return ok;
}""",
        """bool FogDevice::Render(const FrameInputs& in, const Config& cfg, FogPass pass, const char** skip)
{
    if (R6WaterOnlyFogDisabled(cfg))
    {
        if (skip)
            *skip = "R6 water-only profile";
        return false;
    }
    bool ok = FogActive() && m_renderer.Render(m_real, Depth(), in, cfg, pass);
    if (skip)
        *skip = FogActive() ? m_renderer.LastSkipReason() : "fog inactive";
    return ok;
}""",
    )

    replace_once(
        wrap,
        """bool FogDevice::ReadyToRender(const D3DVIEWPORT9& vp, const char** skip)
{
    const bool ready = FogActive() && m_renderer.ReadyToRender(m_real, Depth(), vp);
    if (skip)
        *skip = FogActive() ? m_renderer.LastSkipReason() : "fog inactive";
    return ready;
}""",
        """bool FogDevice::ReadyToRender(const D3DVIEWPORT9& vp, const char** skip)
{
    if (R6WaterOnlyFogDisabled(GlobalConfig().Get()))
    {
        if (skip)
            *skip = "R6 water-only profile";
        return false;
    }
    const bool ready = FogActive() && m_renderer.ReadyToRender(m_real, Depth(), vp);
    if (skip)
        *skip = FogActive() ? m_renderer.LastSkipReason() : "fog inactive";
    return ready;
}""",
    )

    replace_once(
        wrap,
        """bool FogDevice::RenderGodRaysAfterWorld(const char** skip)
{
    const bool ok = FogActive() && m_renderer.RenderGodRaysAfterWorld(m_real, Depth());
    if (skip)
        *skip = FogActive() ? m_renderer.LastSkipReason() : "fog inactive";
    return ok;
}""",
        """bool FogDevice::RenderGodRaysAfterWorld(const char** skip)
{
    if (R6WaterOnlyFogDisabled(GlobalConfig().Get()))
    {
        if (skip)
            *skip = "R6 water-only profile";
        return false;
    }
    const bool ok = FogActive() && m_renderer.RenderGodRaysAfterWorld(m_real, Depth());
    if (skip)
        *skip = FogActive() ? m_renderer.LastSkipReason() : "fog inactive";
    return ok;
}""",
    )

    print("PASS: applied WarcraftXL R6 compatibility patch to pinned CoAVolFog")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
