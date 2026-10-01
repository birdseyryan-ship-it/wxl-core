#pragma once

#include "ps_r7_reference_low.h"
#include "ps_r7_reference_mid.h"
#include "ps_r7_reference_high.h"
#include "ps_r7_reference_noisy_low.h"
#include "ps_r7_reference_noisy_mid.h"
#include "ps_r7_reference_noisy_high.h"

namespace r7_step_cache
{
constexpr float kTolerance = 1.0e-4f;
constexpr int kVariants = 5;

void CheckMatchesReference(IDirect3DDevice9* device)
{
    march_layers::Resources resources;
    IDirect3DTexture9* lights = nullptr;
    const bool ready = march_layers::CreateResources(device, resources) &&
                       SUCCEEDED(device->CreateTexture(32, 1, 1, 0, D3DFMT_A32B32G32R32F,
                                                       D3DPOOL_MANAGED, &lights, nullptr));
    Check(ready, "R7 step-cache FP32 comparison resources created");
    if (!ready)
    {
        if (lights)
            lights->Release();
        return;
    }
    const local_light_gpu::Light sources[2] = {
        {30, 0, 100, {10, 8, 6}}, {220, 25, 60, {4, 6, 8}},
    };
    bool filled = local_light_gpu::FillLights(lights, sources, 2);
    march_layers::BindResources(device, resources);
    device->SetTexture(8, lights);
    for (D3DSAMPLERSTATETYPE state : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER})
        device->SetSamplerState(8, state, D3DTEXF_POINT);
    device->SetSamplerState(8, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(8, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(8, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    const BYTE* reference[2][3] = {
        {g_ps_r7_reference_low, g_ps_r7_reference_mid, g_ps_r7_reference_high},
        {g_ps_r7_reference_noisy_low, g_ps_r7_reference_noisy_mid, g_ps_r7_reference_noisy_high},
    };
    const BYTE* candidate[2][3] = {
        {g_ps_lit_march_low, g_ps_lit_march_mid, g_ps_lit_march_high},
        {g_ps_lit_noisy_march_low, g_ps_lit_noisy_march_mid, g_ps_lit_noisy_march_high},
    };
    for (int authored = 0; authored < 2; ++authored)
    {
        for (int quality = 0; quality < 3; ++quality)
        {
            IDirect3DPixelShader9* shaders[2] = {};
            bool rendered = filled &&
                SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(reference[authored][quality]),
                                                     &shaders[0])) &&
                SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(candidate[authored][quality]),
                                                     &shaders[1]));
            float worst = 0;
            float visibleLight = 0;
            int partial = 0;
            for (int variant = 0; variant < kVariants && rendered; ++variant)
            {
                float constants[march_layers::kConstantRegisters][4];
                march_layers::FillConstants(constants, variant > 0, variant == 2 ? 17.0f : 0.0f);
                constants[53][0] = 2;
                constants[53][1] = 301;
                constants[53][2] = 0.3f;
                if (variant == 3)
                {
                    constants[78][0] = 0;
                }
                if (variant == 4)
                {
                    for (int layer = 0; layer < 4; ++layer)
                    {
                        constants[12 + layer * 6][0] = 0;
                        constants[17 + layer * 6][2] = 99999;
                        constants[17 + layer * 6][3] = 1;
                    }
                }
                if (authored)
                {
                    for (int layer = 0; layer < 3; ++layer)
                    {
                        const int first = 36 + layer * 4;
                        constants[first][3] = 0.025f;
                        constants[first + 1][0] = 5;
                        constants[first + 1][3] = 0.05f;
                        constants[first + 2][0] = 0.1f;
                        constants[first + 2][3] = 0.35f;
                        constants[first + 3][0] = 0.7f;
                        constants[first + 3][1] = 0.3f;
                    }
                    device->SetTexture(10, resources.noise);
                    device->SetSamplerState(10, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(10, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                    device->SetSamplerState(10, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                    device->SetSamplerState(10, D3DSAMP_SRGBTEXTURE, FALSE);
                    for (D3DSAMPLERSTATETYPE address : {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW})
                        device->SetSamplerState(10, address, D3DTADDRESS_WRAP);
                }
                float pixels[2][march_layers::kSize * march_layers::kSize * 4] = {};
                rendered = march_layers::RenderMarch(device, resources, shaders[0], constants, pixels[0]) &&
                           march_layers::RenderMarch(device, resources, shaders[1], constants, pixels[1]);
                for (UINT i = 0; i < march_layers::kSize * march_layers::kSize * 4 && rendered; ++i)
                {
                    rendered = std::isfinite(pixels[0][i]) && std::isfinite(pixels[1][i]);
                    worst = std::max(worst, std::fabs(pixels[0][i] - pixels[1][i]) /
                                            std::max(1.0f, std::fabs(pixels[0][i])));
                }
                for (UINT i = 0; i < march_layers::kSize * march_layers::kSize && rendered; ++i)
                    partial += pixels[1][i * 4 + 3] > 0.02f && pixels[1][i * 4 + 3] < 0.9f;
                if (variant == 0 && rendered)
                {
                    float unlit[march_layers::kSize * march_layers::kSize * 4] = {};
                    constants[53][0] = 0;
                    rendered = march_layers::RenderMarch(device, resources, shaders[0], constants, unlit);
                    for (UINT i = 0; i < march_layers::kSize * march_layers::kSize && rendered; ++i)
                        for (UINT channel = 0; channel < 3; ++channel)
                            visibleLight = std::max(visibleLight,
                                std::fabs(pixels[0][i * 4 + channel] - unlit[i * 4 + channel]));
                }
            }
            for (IDirect3DPixelShader9* shader : shaders)
                if (shader)
                    shader->Release();
            char label[224];
            std::snprintf(label, sizeof(label),
                          "R7 quality %d authored-noise %d: cached/reference match %.2g, %d partial, light %.3g",
                          quality + 1, authored, worst, partial, visibleLight);
            Check(rendered && worst <= kTolerance && partial > 0 && visibleLight > 1.0e-5f, label);
        }
    }
    device->SetRenderTarget(0, resources.previousTarget);
    device->SetDepthStencilSurface(resources.previousDepth);
    resources.previousState->Apply();
    lights->Release();
}
}
