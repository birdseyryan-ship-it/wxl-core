// R8 Step 11B-02B2A: portable, read-only observation contract. GPL-3.0-or-later.
#pragma once
#include "client/CWorldScene/StructuralMaterialPolicy.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace wxl::r8::materials::c28
{
inline constexpr std::string_view kVs30 = "b5bcd972af880b5242802435f478a79ef41e588e2e5c61dbc411374cd6672593";
inline constexpr std::string_view kVs60 = "3eb44db2e8ebea42a75a31586992e009b8143cc0936d50ed7b9d617eb5341a9c";
inline constexpr std::string_view kRaw30 = "929f23fcaa265eb0a40f22dd58774669132043d98bcd111e09a058806c1f380e";
inline constexpr std::string_view kRaw60 = "1d29d928f3416de9e21eca61fd77bb1e8054a2d541e9cd84b2b9c145f6803fe7";
inline constexpr std::string_view kDiffuse5 = "24c2c665054205b408bba34848906b73fadb33aba72abff3a1545d8804ff3a9b";
inline constexpr std::string_view kDiffuse7 = "259824c48d1717aef69a51a09b04bd519a01e75afe4c3eec34b719d1b01f5bb1";
inline constexpr std::string_view kOpaque5 = "6b2c56523812cdffe77383c1dbc481abe73d96bef4ef12808d996f1d5221def1";
inline constexpr std::string_view kOpaque7 = "e761d8f215688ac549a49ca4faf20ec15d0ea3e0c8084985f0a8732bcdfa7937";

constexpr bool Compatible(bool proof, bool master, bool wmo, bool m2, bool perPixel)
{
    return !proof || (master && wmo && !m2 && !perPixel);
}
constexpr bool Slots(WmoFamily family, unsigned vtx, unsigned pix)
{
    if (family != WmoFamily::Diffuse && family != WmoFamily::Opaque) return false;
    return (pix == 5 && (vtx == 31 || vtx == 41 || vtx == 51)) ||
           (pix == 7 && (vtx == 61 || vtx == 71 || vtx == 81));
}
constexpr bool Qualified(WmoFamily family, unsigned vtx, unsigned pix,
                         std::string_view vs, std::string_view raw, std::string_view ps)
{
    if (!Slots(family, vtx, pix)) return false;
    if (vs != (pix == 5 ? kVs30 : kVs60) || raw != (pix == 5 ? kRaw30 : kRaw60)) return false;
    return ps == (family == WmoFamily::Diffuse ? (pix == 5 ? kDiffuse5 : kDiffuse7)
                                              : (pix == 5 ? kOpaque5 : kOpaque7));
}

inline constexpr std::array<unsigned, 6> kRegisters{10, 11, 12, 28, 29, 31};
struct Constant
{
    std::int32_t hr = -1;
    std::array<float, 4> value{};
    bool Valid() const
    {
        if (hr < 0) return false;
        for (float x : value) if (!std::isfinite(x)) return false;
        return true;
    }
};
// Instantiated against the real IDirect3DDevice9 in production, and a fake receiver
// in the executable tests. This API surface contains only getters.
template<class Device> std::array<Constant, 6> ReadConstants(Device& device)
{
    std::array<Constant, 6> out{};
    for (unsigned i = 0; i < 5; ++i)
        out[i].hr = static_cast<std::int32_t>(device.GetVertexShaderConstantF(kRegisters[i], out[i].value.data(), 1));
    out[5].hr = static_cast<std::int32_t>(device.GetPixelShaderConstantF(31, out[5].value.data(), 1));
    return out;
}
// Observer failures may disable capture, but must never suppress/retry a native
// draw or reinterpret its HRESULT. The original call is OUTSIDE both catch blocks.
template<class Before, class Forward, class After, class Failure>
auto ObserveDraw(Before before, Forward forward, After after, Failure failure)
{
    bool captured = false;
    try { captured = before(); } catch (...) { failure(); }
    auto result = forward();
    if (captured) { try { after(result); } catch (...) { failure(); } }
    return result;
}
}
