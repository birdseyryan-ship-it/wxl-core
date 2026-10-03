#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace wxl::scripts::render_modern::perf
{
    enum class CpuRegion : std::uint8_t
    {
        FallbackFrameTotal = 0,
        FallbackStateBlock,
        FallbackEndScene,
        FallbackColourResolve,
        FallbackDepthStage1,
        FallbackDepthStage2,
        FallbackBeginScene,

        AoRawDraw,
        AoCompositeDraw,

        SmaaTotal,
        SmaaEdgeDraw,
        SmaaBlendDraw,
        SmaaNeighborhoodDraw,

        WaterSnapshotTotal,
        WaterEndScene,
        WaterColourCopy,
        WaterDepthStage1,
        WaterDepthStage2,
        WaterBeginScene,
        WaterLinearDepth,

        Count,
    };

    // Opt-in only:
    //   WXL_R8_PERF_PROBE=1
    //
    // Default OFF. When disabled, no QPC call is issued from CpuBegin().
    bool Enabled() noexcept;

    // Emits exactly one render-owner activation marker containing the raw
    // process environment observation and the cached Enabled() decision.
    // This is intentionally safe even when the probe is disabled so a live
    // OFF/ON run can prove what the injected process actually received.
    void ReportActivation() noexcept;

    // Returns zero when disabled or when the call is rejected by the
    // single-render-thread ownership gate.
    std::uint64_t CpuBegin() noexcept;

    // Records one elapsed CPU submission interval.
    void CpuEnd(
        CpuRegion region,
        std::uint64_t startTicks) noexcept;

    class CpuScope
    {
    public:
        explicit CpuScope(CpuRegion region) noexcept;
        ~CpuScope() noexcept;

        CpuScope(const CpuScope&) = delete;
        CpuScope& operator=(const CpuScope&) = delete;

    private:
        CpuRegion region_;
        std::uint64_t start_;
    };

    // A1B1 capability probe only.
    //
    // It tests CreateQuery support for timestamp/frequency/disjoint query
    // objects and releases them immediately.
    //
    // It deliberately:
    // - does not Issue()
    // - does not GetData()
    // - does not flush
    // - does not wait
    // - does not alter render state or render targets
    void ProbeGpuQueryCapabilities(
        IDirect3DDevice9* device) noexcept;

    // Called once per processed Proton post-process frame.
    // Emits bounded aggregate CPU summaries every fixed-size window.
    void FrameBoundary(
        unsigned width,
        unsigned height,
        unsigned msaa,
        bool aoActive,
        bool smaaActive) noexcept;

    // Lost/reset boundary. Pending aggregates may be summarized, then all
    // device/query-capability identity is invalidated.
    void Reset() noexcept;
}
