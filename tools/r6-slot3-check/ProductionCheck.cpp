#include "water/Slot3ProductionPolicy.hpp"
#include <cstdio>
#include <limits>
#include <string>
using namespace wxl::water::slot3;
namespace wd = wxl::waterdiag;
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while(false)

// Fault-inject the transaction contract independently of D3D. Produce models
// secondary native rendering as a mutation even when it returns failure.
struct Backend {
    bool preflight, capture, produce, bind, restore;
    bool dirty = false, nativeSawDirty = false;
    unsigned native = 0, replacement = 0, restores = 0, quarantines = 0;
    std::int32_t replacementHr = 0;
    std::string events;
    bool Preflight() noexcept { events += 'P'; return preflight; }
    bool Capture() noexcept { events += 'C'; return capture; }
    bool Produce() noexcept { events += 'G'; dirty = true; return produce; }
    bool Bind() noexcept { events += 'B'; dirty = true; return bind; }
    bool Restore() noexcept { events += 'R'; ++restores; if (restore) dirty = false; return restore; }
    std::int32_t Native() noexcept { events += 'N'; ++native; nativeSawDirty = dirty; return 42; }
    std::int32_t Replace() noexcept { events += 'D'; ++replacement; return replacementHr; }
    void Quarantine() noexcept { events += 'Q'; ++quarantines; }
};
int main() {
    // All enabled/prerequisite/produce/bind/restore combinations, with both
    // successful and failed replacement HRESULTs. Failed submission is final.
    for (unsigned mask = 0; mask < 64; ++mask) for (int hr : {0, -1}) {
        const bool enabled = (mask & 1) != 0;
        Backend b{bool(mask & 2), bool(mask & 4), bool(mask & 8), bool(mask & 16), bool(mask & 32)};
        b.replacementHr = hr;
        const auto r = ExecuteProduction(enabled, b);
        const bool captured = enabled && b.preflight && b.capture;
        const bool submitted = captured && b.produce && b.bind;
        const bool suppressed = captured && !submitted && !b.restore;
        CHECK(b.native + b.replacement == (suppressed ? 0u : 1u));
        CHECK(!b.nativeSawDirty);
        CHECK(b.restores == (captured ? 1u : 0u));
        CHECK(b.quarantines == (captured && !b.restore ? 1u : 0u));
        CHECK(r.restoreOk == (!captured || b.restore));
        if (submitted) {
            CHECK(r.submission == Submission::Replacement && r.result == hr);
            CHECK(b.replacement == 1 && b.native == 0);
            CHECK(b.events == (b.restore ? "PCGBDR" : "PCGBDRQ"));
        } else if (suppressed) {
            CHECK(r.submission == Submission::SuppressedAfterRestoreFailure);
            CHECK(r.result == static_cast<std::int32_t>(0x8876086cu));
            CHECK(b.events == (b.produce ? "PCGBRQ" : "PCGRQ"));
        } else {
            CHECK(r.submission == Submission::Native && r.result == 42);
            CHECK(b.native == 1 && b.replacement == 0);
            const std::string expected = !enabled ? "N" : !b.preflight ? "PN" :
                !b.capture ? "PCN" : !b.produce ? "PCGRN" : "PCGBRN";
            CHECK(b.events == expected);
        }
    }
    wd::ProfileKey profile{wd::Family::Water, wd::Provider::Terrain, 1,
        wd::kR6BaseWaterVs, wd::kR6BaseWaterPs, wd::kR6BaseWaterDecl,
        wd::kR6WaterMaterialP01, 5};
    for (unsigned id : {1u,5u,9u}) CHECK(FirstCandidateDrawSupported(profile,1,44,id));
    for (unsigned id : {0u,2u,41u,61u,81u,UINT32_MAX}) CHECK(!FirstCandidateDrawSupported(profile,1,44,id));
    CHECK(!FirstCandidateDrawSupported(profile,2,44,1));
    CHECK(!FirstCandidateDrawSupported(profile,1,40,1));
    profile.material=wd::kR6WaterMaterialP02P03;
    CHECK(!FirstCandidateDrawSupported(profile,1,44,1));
    std::uint64_t sequence=7;
    ProductionOrdinals ordinals{};
    CHECK(ReserveProductionOrdinals(sequence,ordinals));
    CHECK(sequence==9 && ordinals.reflection==8 && ordinals.consumer==9);
    sequence=UINT64_MAX-2;
    CHECK(ReserveProductionOrdinals(sequence,ordinals));
    CHECK(sequence==UINT64_MAX && ordinals.reflection==UINT64_MAX-1 && ordinals.consumer==UINT64_MAX);
    const auto saved=ordinals;
    CHECK(!ReserveProductionOrdinals(sequence,ordinals));
    CHECK(sequence==UINT64_MAX && ordinals.reflection==saved.reflection && ordinals.consumer==saved.consumer);
    CHECK(kFirstCandidateDownsampleShift==0);
    std::printf("R6 production transaction: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
