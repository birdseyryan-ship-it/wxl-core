#pragma once

#include "gpu/PerfProbe.hpp"

#include <cstdint>

struct IDirect3DDevice9;

namespace wxl::scripts::render_modern::perf
{
    enum class GpuOwner : std::uint8_t
    {
        Fallback = 0,
        WaterSnapshot,
        WaterLinear,
        Count,
    };

    // A1B2 diagnostic-only asynchronous GPU frame bracket.
    //
    // The implementation uses a bounded query ring. It never waits for the
    // frame it just issued, never uses D3DGETDATA_FLUSH, and only harvests
    // slots after a fixed minimum frame age.
    class GpuFrameScope
    {
    public:
        GpuFrameScope(
            GpuOwner owner,
            IDirect3DDevice9* device) noexcept;

        ~GpuFrameScope() noexcept;

        GpuFrameScope(
            const GpuFrameScope&) = delete;

        GpuFrameScope& operator=(
            const GpuFrameScope&) = delete;

    private:
        GpuOwner owner_;
        std::uint64_t serial_ = 0;
        bool active_ = false;
    };

    // Timestamp pair nested inside the currently active owner frame.
    // A scope that has no matching active owner is a zero-work no-op.
    class GpuScope
    {
    public:
        GpuScope(
            CpuRegion region,
            IDirect3DDevice9* device) noexcept;

        ~GpuScope() noexcept;

        GpuScope(
            const GpuScope&) = delete;

        GpuScope& operator=(
            const GpuScope&) = delete;

    private:
        GpuOwner owner_ = GpuOwner::Count;
        CpuRegion region_ = CpuRegion::Count;
        std::uint64_t serial_ = 0;
        bool active_ = false;
    };

    // Device-lost/reset boundary. Releases all outstanding query objects
    // without waiting for pending results.
    void GpuReset() noexcept;
}
