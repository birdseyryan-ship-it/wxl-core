#include "client/CWorldScene/WmoC28ProofCore.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
using namespace wxl::r8::materials;
unsigned checks = 0;
void Check(bool ok) { ++checks; if (!ok) { std::fprintf(stderr, "FAIL check %u\n", checks); std::exit(1); } }
struct Receiver
{
    std::vector<unsigned> calls;
    unsigned fail = 999;
    std::int32_t GetVertexShaderConstantF(unsigned reg, float* out, unsigned count)
    { calls.push_back(reg); Check(count == 1); for (unsigned i = 0; i < 4; ++i) out[i] = float(reg + i); return reg == fail ? -1 : 0; }
    std::int32_t GetPixelShaderConstantF(unsigned reg, float* out, unsigned count)
    { calls.push_back(1000 + reg); Check(count == 1); for (unsigned i = 0; i < 4; ++i) out[i] = -float(reg + i); return reg == fail ? -1 : 0; }
};
int main()
{
    for (unsigned bits = 0; bits < 32; ++bits)
    {
        bool p = bits & 1, master = bits & 2, wmo = bits & 4, m2 = bits & 8, pp = bits & 16;
        Check(c28::Compatible(p, master, wmo, m2, pp) == (!p || (master && wmo && !m2 && !pp)));
    }
    // Exhaust the index space, including all richer/light and unknown families.
    for (int family = -1; family < 8; ++family)
        for (unsigned vtx = 0; vtx < 95; ++vtx)
            for (unsigned pix = 0; pix < 18; ++pix)
            {
                bool low = vtx == 31 || vtx == 41 || vtx == 51;
                bool high = vtx == 61 || vtx == 71 || vtx == 81;
                bool expected = (family == 0 || family == 4) && ((low && pix == 5) || (high && pix == 7));
                auto f = static_cast<WmoFamily>(family);
                auto vs = low ? c28::kVs30 : c28::kVs60;
                auto raw = low ? c28::kRaw30 : c28::kRaw60;
                auto ps = family == 0 ? (pix == 5 ? c28::kDiffuse5 : c28::kDiffuse7) : (pix == 5 ? c28::kOpaque5 : c28::kOpaque7);
                Check(c28::Qualified(f, vtx, pix, vs, raw, ps) == expected);
                Check(!c28::Qualified(f, vtx, pix, "UNKNOWN", raw, ps));
                Check(!c28::Qualified(f, vtx, pix, vs, "UNKNOWN", ps));
                Check(!c28::Qualified(f, vtx, pix, vs, raw, "UNKNOWN"));
            }
    for (unsigned fail : {10u, 11u, 12u, 28u, 29u, 31u, 999u})
    {
        Receiver d; d.fail = fail;
        auto s = c28::ReadConstants(d);
        Check(d.calls == std::vector<unsigned>({10,11,12,28,29,1031}));
        for (unsigned i = 0; i < 6; ++i)
        {
            Check(s[i].Valid() == (c28::kRegisters[i] != fail));
            Check(s[i].value[2] == (i == 5 ? -33.0f : float(c28::kRegisters[i] + 2)));
        }
    }
    c28::Constant invalid; Check(!invalid.Valid());
    invalid.hr = 0; invalid.value[0] = std::numeric_limits<float>::quiet_NaN(); Check(!invalid.Valid());
    invalid.value[0] = std::numeric_limits<float>::infinity(); Check(!invalid.Valid());
    // Faults before/after observation preserve exactly one original call and HRESULT.
    for (int mode = 0; mode < 4; ++mode)
    {
        int calls = 0, afterCalls = 0, failures = 0;
        auto result = c28::ObserveDraw([&] { if (mode == 1) throw 1; return mode != 3; },
            [&] { ++calls; return -1234; }, [&] (int hr) { Check(hr == -1234); ++afterCalls; if (mode == 2) throw 2; }, [&] { ++failures; });
        Check(calls == 1 && result == -1234);
        Check(afterCalls == (mode == 1 || mode == 3 ? 0 : 1));
        Check(failures == (mode == 1 || mode == 2 ? 1 : 0));
    }
    int nativeCalls = 0;
    try { c28::ObserveDraw([] { return true; }, [&]() -> int { ++nativeCalls; throw 7; }, [](int) {}, [] {}); Check(false); }
    catch (int x) { Check(x == 7 && nativeCalls == 1); }
    std::printf("C28_CORE_CHECK: PASS checks=%u\n", checks);
}
