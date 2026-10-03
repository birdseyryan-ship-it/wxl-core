// The d3d9.dll proxy: route the client through D3D9On12 when available,
// expose the shared D3D12 device/queue to WarcraftXL, and lazily load the runtime.
//
// GFX-R2 forward port:
//   - On12/device/capture behaviour derived from the matching July core.
//   - Runtime loading retains the v1.1 lazy first-create design.
//   - Nothing is loaded from DllMain.
//
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include <windows.h>
#include <d3d9.h>

#include "engine/gpu/Device.hpp"
#include "engine/gpu/Capture.hpp"

using wxl::gpu::Log;

namespace
{
    using Create9Fn       = IDirect3D9* (WINAPI*)(UINT);
    using Create9ExFn     = HRESULT     (WINAPI*)(UINT, IDirect3D9Ex**);
    using Create9On12Fn   = IDirect3D9* (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
    using Create9On12ExFn = HRESULT     (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**);

    Create9Fn       g_realCreate9       = nullptr;
    Create9ExFn     g_realCreate9Ex     = nullptr;
    Create9On12Fn   g_realCreate9On12   = nullptr;
    Create9On12ExFn g_realCreate9On12Ex = nullptr;

    HMODULE LoadRealD3D9()
    {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);

        if (n != 0 && n < MAX_PATH - 16)
        {
            lstrcatA(path, "\\d3d9.dll");
            if (HMODULE r = LoadLibraryA(path))
                return r;
        }

        return LoadLibraryA("d3d9_real.dll");
    }

    void EnsureReal()
    {
        if (g_realCreate9 || g_realCreate9Ex ||
            g_realCreate9On12 || g_realCreate9On12Ex)
            return;

        HMODULE r = LoadRealD3D9();

        if (!r)
        {
            Log("d3d9proxy: FAILED to load the system d3d9.dll");
            return;
        }

        g_realCreate9 =
            reinterpret_cast<Create9Fn>(
                GetProcAddress(r, "Direct3DCreate9"));

        g_realCreate9Ex =
            reinterpret_cast<Create9ExFn>(
                GetProcAddress(r, "Direct3DCreate9Ex"));

        g_realCreate9On12 =
            reinterpret_cast<Create9On12Fn>(
                GetProcAddress(r, "Direct3DCreate9On12"));

        g_realCreate9On12Ex =
            reinterpret_cast<Create9On12ExFn>(
                GetProcAddress(r, "Direct3DCreate9On12Ex"));

        Log("d3d9proxy: system d3d9 loaded "
            "(9=%p Ex=%p On12=%p On12Ex=%p)",
            g_realCreate9,
            g_realCreate9Ex,
            g_realCreate9On12,
            g_realCreate9On12Ex);
    }

    void EnsureRuntimeLoaded()
    {
        static bool attempted = false;

        if (attempted)
            return;

        attempted = true;

        if (!LoadLibraryA("WarcraftXL.dll"))
            Log("d3d9proxy: WarcraftXL.dll not loaded (win32=%lu)",
                GetLastError());
    }
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    EnsureReal();
    EnsureRuntimeLoaded();

    if (g_realCreate9On12)
    {
        D3D9ON12_ARGS args = wxl::gpu::MakeOn12Args();

        IDirect3D9* d3d =
            g_realCreate9On12(sdkVersion, &args, 1);

        Log("d3d9proxy: Direct3DCreate9On12 -> %p "
            "(D3D12 %p queue %p)",
            d3d,
            args.pD3D12Device,
            args.ppD3D12Queues[0]);

        if (d3d)
        {
            return wxl::gpu::capture::Wrap(
                d3d,
                static_cast<ID3D12CommandQueue*>(
                    args.ppD3D12Queues[0]));
        }
    }

    Log("d3d9proxy: native Direct3DCreate9 fallback");

    return g_realCreate9
        ? g_realCreate9(sdkVersion)
        : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(
    UINT sdkVersion,
    IDirect3D9Ex** out)
{
    EnsureReal();
    EnsureRuntimeLoaded();

    if (g_realCreate9On12Ex)
    {
        D3D9ON12_ARGS args = wxl::gpu::MakeOn12Args();

        HRESULT hr =
            g_realCreate9On12Ex(
                sdkVersion,
                &args,
                1,
                out);

        Log("d3d9proxy: Direct3DCreate9On12Ex -> "
            "hr=0x%08lx (D3D12 %p queue %p)",
            hr,
            args.pD3D12Device,
            args.ppD3D12Queues[0]);

        if (SUCCEEDED(hr))
        {
            if (out && *out)
            {
                *out = wxl::gpu::capture::WrapEx(
                    *out,
                    static_cast<ID3D12CommandQueue*>(
                        args.ppD3D12Queues[0]));
            }

            return hr;
        }
    }

    Log("d3d9proxy: native Direct3DCreate9Ex fallback");

    if (g_realCreate9Ex)
        return g_realCreate9Ex(sdkVersion, out);

    if (out)
        *out = nullptr;

    return E_NOINTERFACE;
}

extern "C" __declspec(dllexport)
ID3D12Device* WxlD3D12Device()
{
    return wxl::gpu::Device();
}

extern "C" __declspec(dllexport)
ID3D12CommandQueue* WxlD3D12Queue()
{
    if (ID3D12CommandQueue* q =
            wxl::gpu::capture::PresentQueue())
        return q;

    return wxl::gpu::Queue();
}

extern "C" __declspec(dllexport)
void WxlD3D12DrainDebug()
{
    wxl::gpu::DrainDebug();
}

extern "C" __declspec(dllexport)
void WxlSetSsaaFactor(float factor)
{
    wxl::gpu::capture::SetSsaaFactor(factor);
}

extern "C" __declspec(dllexport)
float WxlGetSsaaFactor()
{
    return wxl::gpu::capture::SsaaFactor();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    // Deliberately empty. Loading system d3d9 and WarcraftXL.dll from
    // DllMain would execute LoadLibrary while the loader lock is held.
    (void)reason;
    return TRUE;
}
