// wxl-render-modern: the dev-overlay panel exposing the post-process effects (enable + quality tier).
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "engine/ui/ImGuiHost.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/D3D9Fallback.hpp"
#include "engine/gpu/Proxy.hpp"

#include "imgui.h"

#include <cstring>

// Registers a "Graphics" panel with the dev overlay. The panel is generic over the effect chain: each effect
// exposes an enable toggle and a quality tier, so effects added later appear here automatically. Anti-aliasing
// methods are mutually exclusive; SSAO and supersampling are independent and stack with everything.
namespace wxl::scripts::render_modern
{
    namespace
    {
        const char* const k_qualityNames[] = { "Low", "Medium", "High", "Ultra" };

        void __cdecl DrawGraphicsPanel(void*)
        {
            const auto& effects = Pipeline::Get().Effects();
            if (effects.empty())
            {
                ImGui::TextDisabled("(no effects registered)");
                return;
            }

            const bool protonFallback = d3d9fallback::Available();

            if (protonFallback)
            {
                ImGui::TextColored(
                    ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                    "Proton D3D9 post-process backend");

                bool proof = d3d9fallback::ProofTint();
                if (ImGui::Checkbox("Proof Tint (diagnostic)", &proof))
                    d3d9fallback::SetProofTint(proof);

                if (proof)
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                        "Diagnostic tint active; FXAA is temporarily bypassed.");

                ImGui::TextDisabled(
                    "Engine MSAA is resolved to a single-sample world image before post-FX.");
                ImGui::Spacing();
            }
            else if (Pipeline::Get().MsaaActive())
            {
                ImGui::TextColored(
                    ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                    "Engine MSAA active (effects supported)");
                ImGui::Spacing();
            }

            for (const auto& e : effects)
            {
                // R3A validates colour-only AA first. Depth-dependent effects remain locked
                // until the readable-depth/world redirect is implemented and proven.
                if (e->NeedsDepth()) continue;

                // R3B intentionally ports only FXAA first. SMAA comes after the
                // fallback frame path itself is proven; CMAA2 is a later backend
                // decision because its current implementation is D3D12 compute/UAV.
                if (protonFallback && std::strcmp(e->Name(), "FXAA") != 0)
                {
                    ImGui::TextDisabled("%s (pending Proton backend port)", e->Name());
                    continue;
                }

                ImGui::PushID(e.get());

                bool on = e->Enabled();
                if (ImGui::Checkbox(e->Name(), &on))
                {
                    e->SetEnabled(on);
                    // Anti-aliasing methods are mutually exclusive: enabling one disables the other AA methods.
                    // SSAO is not anti-aliasing, so it is left alone and stacks with whichever AA is on.
                    if (on && e->IsAntiAliasing())
                        for (const auto& other : effects)
                            if (other.get() != e.get() && other->IsAntiAliasing())
                                other->SetEnabled(false);
                }

                int q = static_cast<int>(e->GetQuality());
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                // Each effect shows its first QualityLevels() names from k_qualityNames -- all are Low/Medium/High
                // now (the Ultra tier was retired when SSAO became GTAO at every tier; the name is kept for any
                // future 4-tier effect).
                if (ImGui::Combo("##quality", &q, k_qualityNames, e->QualityLevels()))
                    e->SetQuality(static_cast<Quality>(q));

                // Effect-specific live tuning controls (SSAO sliders, ...), shown while the effect is enabled.
                if (on)
                    e->DrawTuning();

                ImGui::PopID();
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (protonFallback)
                ImGui::TextDisabled(
                    "R3B: SMAA/CMAA2, SSAO and Render Scale remain locked.");
            else
                ImGui::TextDisabled(
                    "R3: SSAO and Render Scale locked pending readable-depth validation.");
        }

        // File-scope registration: adds the panel at DLL load, before the overlay first draws.
        struct PanelRegistrar
        {
            PanelRegistrar() { wxl::ui::AddPanel("Graphics", &DrawGraphicsPanel, nullptr); }
        } g_panelRegistrar;
    }
}
