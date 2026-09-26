#include "Slot3DepthRuntime.hpp"

#if defined(_WIN32)

#include <d3d9.h>
#include <d3dcompiler.h>

namespace wxl::water::slot3 {
namespace {

// Extension-owned API transport format.
//
// The closed R6 evidence proves the numeric quantity required by Classic t6.x:
// linear pre-projection camera-space Z. It does NOT prove that Classic's
// original GBuffer used D3D9 R32F. R32F is therefore used here only as an
// extension-owned single-channel float transport resource. Creation failure
// is fail-closed and does not authorise another guessed representation.
constexpr D3DFORMAT kLinearDepthFormat =
    D3DFMT_R32F;

// Exact frozen-R3 reconstruction:
//
//     viewZ = B / (depth - A)
//
// No epsilon, clamp, far-plane substitution or other invented transform is
// introduced here.
constexpr char kLinearDepthPs[] = R"HLSL(
sampler2D RawDepth : register(s0);
float4 Projection : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float depth = tex2D(RawDepth, uv).x;
    const float viewZ =
        Projection.y / (depth - Projection.x);

    return float4(
        viewZ,
        viewZ,
        viewZ,
        viewZ);
}
)HLSL";

template <class T>
void Release(T*& value) noexcept
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

struct SavedState {
    IDirect3DStateBlock9* block = nullptr;
    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* depth = nullptr;
    D3DVIEWPORT9 viewport{};
    bool haveViewport = false;

    ~SavedState()
    {
        Release(depth);
        Release(rt0);
        Release(block);
    }

    bool Capture(
        IDirect3DDevice9* device) noexcept
    {
        if (!device)
            return false;

        if (FAILED(
                device->CreateStateBlock(
                    D3DSBT_ALL,
                    &block)) ||
            !block)
        {
            return false;
        }

        if (FAILED(
                device->GetRenderTarget(
                    0,
                    &rt0)) ||
            !rt0)
        {
            return false;
        }

        // A null depth-stencil is valid D3D9 state.
        if (FAILED(
                device->GetDepthStencilSurface(
                    &depth)))
        {
            depth = nullptr;
        }

        haveViewport =
            SUCCEEDED(
                device->GetViewport(
                    &viewport));

        return haveViewport;
    }

    bool Restore(
        IDirect3DDevice9* device) noexcept
    {
        if (!device ||
            !block ||
            !rt0 ||
            !haveViewport)
        {
            return false;
        }

        bool ok = true;

        if (FAILED(
                block->Apply()))
        {
            ok = false;
        }

        // Render target and depth-stencil are not part of the D3DSBT_ALL
        // state set; viewport is captured by D3DSBT_ALL, but restoring it
        // explicitly last makes the conversion boundary deterministic.
        if (FAILED(
                device->SetRenderTarget(
                    0,
                    rt0)))
        {
            ok = false;
        }

        if (FAILED(
                device->SetDepthStencilSurface(
                    depth)))
        {
            ok = false;
        }

        if (FAILED(
                device->SetViewport(
                    &viewport)))
        {
            ok = false;
        }

        return ok;
    }
};

} // namespace

DepthRuntime::~DepthRuntime()
{
    Reset();
}

void DepthRuntime::Reset() noexcept
{
    Release(shader_);
    Release(surface_);
    Release(texture_);

    device_ = nullptr;
    lifecycle_.Reset();
}

bool DepthRuntime::EnsureShader(
    IDirect3DDevice9* device) noexcept
{
    if (!device)
        return false;

    if (shader_ &&
        device_ == device)
    {
        return true;
    }

    Release(shader_);

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;

    const HRESULT compile =
        D3DCompile(
            kLinearDepthPs,
            sizeof(kLinearDepthPs) - 1,
            "wxl-r6-slot3-linear-depth",
            nullptr,
            nullptr,
            "main",
            "ps_3_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &code,
            &errors);

    Release(errors);

    if (FAILED(compile) ||
        !code)
    {
        Release(code);
        return false;
    }

    const HRESULT create =
        device->CreatePixelShader(
            static_cast<const DWORD*>(
                code->GetBufferPointer()),
            &shader_);

    Release(code);

    return
        SUCCEEDED(create) &&
        shader_;
}

bool DepthRuntime::EnsureResources(
    IDirect3DDevice9* device,
    const LinearDepthKey& key) noexcept
{
    if (!device ||
        !key.width ||
        !key.height)
    {
        return false;
    }

    if (device_ == device &&
        texture_ &&
        surface_ &&
        lifecycle_.ResourcesMatch(key))
    {
        return EnsureShader(device);
    }

    Reset();
    device_ = device;

    if (!EnsureShader(device)) {
        Reset();
        return false;
    }

    const HRESULT create =
        device->CreateTexture(
            key.width,
            key.height,
            1,
            D3DUSAGE_RENDERTARGET,
            kLinearDepthFormat,
            D3DPOOL_DEFAULT,
            &texture_,
            nullptr);

    if (FAILED(create) ||
        !texture_)
    {
        Reset();
        return false;
    }

    if (FAILED(
            texture_->GetSurfaceLevel(
                0,
                &surface_)) ||
        !surface_)
    {
        Reset();
        return false;
    }

    lifecycle_.MarkResourcesReady(key);

    return true;
}

DepthRuntime::ConvertStatus
DepthRuntime::Convert(
    IDirect3DDevice9* device,
    IDirect3DTexture9* rawDepth,
    const LinearDepthKey& key) noexcept
{
    if (!device ||
        !rawDepth ||
        !surface_ ||
        !shader_)
    {
        return ConvertStatus::FailedRestored;
    }

    SavedState saved;

    // Nothing has been changed if state capture itself fails.
    if (!saved.Capture(device))
        return ConvertStatus::FailedRestored;

    bool ok = true;

    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = key.width;
    viewport.Height = key.height;
    viewport.MinZ = 0.f;
    viewport.MaxZ = 1.f;

    const float constants[4] = {
        key.a,
        key.b,
        0.f,
        0.f,
    };

    struct Vertex {
        float x;
        float y;
        float z;
        float rhw;
        float u;
        float v;
    };

    const float right =
        static_cast<float>(
            key.width) -
        0.5f;

    const float bottom =
        static_cast<float>(
            key.height) -
        0.5f;

    const Vertex quad[4] = {
        {
            -0.5f,
            -0.5f,
            0.f,
            1.f,
            0.f,
            0.f,
        },
        {
            right,
            -0.5f,
            0.f,
            1.f,
            1.f,
            0.f,
        },
        {
            -0.5f,
            bottom,
            0.f,
            1.f,
            0.f,
            1.f,
        },
        {
            right,
            bottom,
            0.f,
            1.f,
            1.f,
            1.f,
        },
    };

    if (FAILED(
            device->SetRenderTarget(
                0,
                surface_)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetDepthStencilSurface(
                nullptr)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetViewport(
                &viewport)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetVertexShader(
                nullptr)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetFVF(
                D3DFVF_XYZRHW |
                D3DFVF_TEX1)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetPixelShader(
                shader_)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetPixelShaderConstantF(
                0,
                constants,
                1)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetTexture(
                0,
                rawDepth)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_ZENABLE,
                FALSE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_ZWRITEENABLE,
                FALSE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_STENCILENABLE,
                FALSE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_ALPHATESTENABLE,
                FALSE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_ALPHABLENDENABLE,
                FALSE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_FOGENABLE,
                FALSE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_SCISSORTESTENABLE,
                FALSE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_CULLMODE,
                D3DCULL_NONE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetRenderState(
                D3DRS_COLORWRITEENABLE,
                D3DCOLORWRITEENABLE_RED)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetSamplerState(
                0,
                D3DSAMP_MINFILTER,
                D3DTEXF_POINT)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetSamplerState(
                0,
                D3DSAMP_MAGFILTER,
                D3DTEXF_POINT)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetSamplerState(
                0,
                D3DSAMP_MIPFILTER,
                D3DTEXF_NONE)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetSamplerState(
                0,
                D3DSAMP_ADDRESSU,
                D3DTADDRESS_CLAMP)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetSamplerState(
                0,
                D3DSAMP_ADDRESSV,
                D3DTADDRESS_CLAMP)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->SetSamplerState(
                0,
                D3DSAMP_SRGBTEXTURE,
                0)))
    {
        ok = false;
    }

    if (ok &&
        FAILED(
            device->DrawPrimitiveUP(
                D3DPT_TRIANGLESTRIP,
                2,
                quad,
                sizeof(Vertex))))
    {
        ok = false;
    }

    // Remove raw INTZ from the temporary sampler before restoring.
    // SavedState remains authoritative for the caller's original s0.
    device->SetTexture(
        0,
        nullptr);

    const bool restored =
        saved.Restore(device);

    // This is intentionally distinct from ordinary unavailability.
    // A future live integration must quarantine/suppress native rendering
    // rather than issuing a native fallback after failed restoration.
    if (!restored)
        return ConvertStatus::RestoreFailed;

    return
        ok
            ? ConvertStatus::Converted
            : ConvertStatus::FailedRestored;
}

DepthProduceStatus DepthRuntime::Produce(
    IDirect3DDevice9* device,
    const wxl::waterdiag::PreWaterSnapshotView& snapshot,
    std::uintptr_t expectedSourceRt,
    std::uint64_t consumerOrdinal,
    const DepthProjection& projection,
    LinearDepthView& output) noexcept
{
    LinearDepthKey requested{};

    if (!device ||
        !BuildLinearDepthKey(
            snapshot,
            reinterpret_cast<std::uintptr_t>(
                device),
            expectedSourceRt,
            consumerOrdinal,
            projection,
            requested))
    {
        return
            DepthProduceStatus::Unavailable;
    }

    if (!EnsureResources(
            device,
            requested))
    {
        return
            DepthProduceStatus::Unavailable;
    }

    if (!ReadyFor(requested)) {
        lifecycle_.InvalidateContent();

        const ConvertStatus converted =
            Convert(
                device,
                snapshot.rawDepthIntz,
                requested);

        if (converted ==
            ConvertStatus::RestoreFailed)
        {
            lifecycle_.InvalidateContent();

            return
                DepthProduceStatus::RestoreFailed;
        }

        if (converted !=
            ConvertStatus::Converted)
        {
            lifecycle_.InvalidateContent();

            return
                DepthProduceStatus::Unavailable;
        }

        if (!lifecycle_.MarkContentReady(
                requested))
        {
            return
                DepthProduceStatus::Unavailable;
        }
    }

    LinearDepthView candidate{};
    candidate.texture = texture_;
    candidate.key = lifecycle_.contentKey;

    output = candidate;

    return
        DepthProduceStatus::Ready;
}

} // namespace wxl::water::slot3

#endif
