#include "gpu/PerfProbe.hpp"

#include "common/Log.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace wxl::scripts::render_modern::perf
{
    namespace
    {
        constexpr std::size_t kRegionCount =
            static_cast<std::size_t>(
                CpuRegion::Count);

        constexpr std::size_t kSampleCapacity =
            2048;

        constexpr std::uint64_t kEmitEveryFrames =
            600;

        struct Stats
        {
            std::uint64_t count = 0;
            std::uint64_t totalTicks = 0;
            std::uint64_t maxTicks = 0;

            std::size_t sampleCount = 0;
            std::size_t nextSample = 0;

            std::array<
                std::uint64_t,
                kSampleCapacity
            > samples{};
        };

        std::array<
            Stats,
            kRegionCount
        > g_stats{};

        constexpr std::array<
            const char*,
            kRegionCount
        > kRegionNames = {{
            "fallback.frame.total",
            "fallback.state_block",
            "fallback.end_scene",
            "fallback.colour_resolve",
            "fallback.depth_stage1",
            "fallback.depth_stage2",
            "fallback.begin_scene",

            "ao.raw_draw",
            "ao.composite_draw",

            "smaa.total",
            "smaa.edge_draw",
            "smaa.blend_draw",
            "smaa.neighborhood_draw",

            "water.snapshot.total",
            "water.end_scene",
            "water.colour_copy",
            "water.depth_stage1",
            "water.depth_stage2",
            "water.begin_scene",
            "water.linear_depth",
        }};

        static_assert(
            kRegionNames.size() ==
            kRegionCount);

        LARGE_INTEGER g_frequency{};
        bool g_frequencyReady = false;

        DWORD g_ownerThread = 0;
        std::uint64_t g_foreignThreadSamples = 0;

        IDirect3DDevice9* g_probedDevice = nullptr;

        std::uint64_t g_frameWindow = 0;

        unsigned g_lastWidth = 0;
        unsigned g_lastHeight = 0;
        unsigned g_lastMsaa = 0;
        bool g_lastAo = false;
        bool g_lastSmaa = false;

        bool TruthyEnvironment(
            const char* name) noexcept
        {
            char raw[16] = {};

            const DWORD n =
                GetEnvironmentVariableA(
                    name,
                    raw,
                    sizeof(raw));

            if (n == 0 ||
                n >= sizeof(raw))
            {
                return false;
            }

            const char c = raw[0];

            return
                c != '0' &&
                c != 'n' &&
                c != 'N' &&
                c != 'f' &&
                c != 'F';
        }

        bool EnsureFrequency() noexcept
        {
            if (g_frequencyReady)
                return g_frequency.QuadPart > 0;

            g_frequencyReady =
                QueryPerformanceFrequency(
                    &g_frequency) != FALSE;

            return
                g_frequencyReady &&
                g_frequency.QuadPart > 0;
        }

        bool AcceptThread() noexcept
        {
            const DWORD current =
                GetCurrentThreadId();

            if (g_ownerThread == 0)
            {
                g_ownerThread = current;
                return true;
            }

            if (current == g_ownerThread)
                return true;

            ++g_foreignThreadSamples;
            return false;
        }

        double TicksToUs(
            long double ticks) noexcept
        {
            if (!EnsureFrequency())
                return 0.0;

            return static_cast<double>(
                ticks *
                1000000.0L /
                static_cast<long double>(
                    g_frequency.QuadPart));
        }

        std::size_t PercentileIndex(
            std::size_t count,
            unsigned percentile) noexcept
        {
            if (count <= 1)
                return 0;

            const std::size_t numerator =
                (count - 1) *
                static_cast<std::size_t>(
                    percentile);

            return std::min(
                count - 1,
                (numerator + 99) / 100);
        }

        void ResetStats() noexcept
        {
            for (auto& stat : g_stats)
                stat = {};

            g_frameWindow = 0;
            g_foreignThreadSamples = 0;
        }

        void EmitSummary() noexcept
        {
            if (!Enabled() ||
                g_frameWindow == 0 ||
                !EnsureFrequency())
            {
                return;
            }

            WLOG_INFO(
                "wxl-r8-perf: cpu_summary "
                "frames=%llu resolution=%ux%u "
                "msaa=%u ao=%u smaa=%u "
                "thread=%lu foreign=%llu",
                static_cast<unsigned long long>(
                    g_frameWindow),
                g_lastWidth,
                g_lastHeight,
                g_lastMsaa,
                g_lastAo ? 1u : 0u,
                g_lastSmaa ? 1u : 0u,
                static_cast<unsigned long>(
                    g_ownerThread),
                static_cast<unsigned long long>(
                    g_foreignThreadSamples));

            std::array<
                std::uint64_t,
                kSampleCapacity
            > sorted{};

            for (std::size_t i = 0;
                 i < kRegionCount;
                 ++i)
            {
                const Stats& stat =
                    g_stats[i];

                if (stat.count == 0 ||
                    stat.sampleCount == 0)
                {
                    continue;
                }

                const std::size_t n =
                    std::min(
                        stat.sampleCount,
                        kSampleCapacity);

                for (std::size_t j = 0;
                     j < n;
                     ++j)
                {
                    sorted[j] =
                        stat.samples[j];
                }

                std::sort(
                    sorted.begin(),
                    sorted.begin() +
                    static_cast<std::ptrdiff_t>(
                        n));

                const std::uint64_t median =
                    sorted[
                        PercentileIndex(
                            n,
                            50)];

                const std::uint64_t p95 =
                    sorted[
                        PercentileIndex(
                            n,
                            95)];

                const std::uint64_t p99 =
                    sorted[
                        PercentileIndex(
                            n,
                            99)];

                const long double meanTicks =
                    static_cast<long double>(
                        stat.totalTicks) /
                    static_cast<long double>(
                        stat.count);

                WLOG_INFO(
                    "wxl-r8-perf: cpu "
                    "region=%s count=%llu "
                    "mean_us=%.3f "
                    "median_us=%.3f "
                    "p95_us=%.3f "
                    "p99_us=%.3f "
                    "max_us=%.3f",
                    kRegionNames[i],
                    static_cast<unsigned long long>(
                        stat.count),
                    TicksToUs(meanTicks),
                    TicksToUs(median),
                    TicksToUs(p95),
                    TicksToUs(p99),
                    TicksToUs(stat.maxTicks));
            }
        }

        struct QueryCapability
        {
            bool supported = false;
            HRESULT hr = D3DERR_INVALIDCALL;
        };

        QueryCapability TestQuery(
            IDirect3DDevice9* device,
            D3DQUERYTYPE type) noexcept
        {
            QueryCapability result{};

            if (!device)
                return result;

            IDirect3DQuery9* query = nullptr;

            result.hr =
                device->CreateQuery(
                    type,
                    &query);

            result.supported =
                SUCCEEDED(result.hr) &&
                query != nullptr;

            if (query)
                query->Release();

            return result;
        }
    }


    bool Enabled() noexcept
    {
        static const bool enabled =
            TruthyEnvironment(
                "WXL_R8_PERF_PROBE");

        return enabled;
    }


    std::uint64_t CpuBegin() noexcept
    {
        if (!Enabled() ||
            !AcceptThread())
        {
            return 0;
        }

        LARGE_INTEGER now{};

        if (!QueryPerformanceCounter(
                &now))
        {
            return 0;
        }

        return static_cast<std::uint64_t>(
            now.QuadPart);
    }


    void CpuEnd(
        CpuRegion region,
        std::uint64_t startTicks) noexcept
    {
        if (!startTicks ||
            !Enabled())
        {
            return;
        }

        LARGE_INTEGER now{};

        if (!QueryPerformanceCounter(
                &now))
        {
            return;
        }

        const std::uint64_t endTicks =
            static_cast<std::uint64_t>(
                now.QuadPart);

        if (endTicks < startTicks)
            return;

        const std::uint64_t elapsed =
            endTicks - startTicks;

        const std::size_t index =
            static_cast<std::size_t>(
                region);

        if (index >= kRegionCount)
            return;

        Stats& stat =
            g_stats[index];

        ++stat.count;
        stat.totalTicks += elapsed;
        stat.maxTicks =
            std::max(
                stat.maxTicks,
                elapsed);

        if (stat.sampleCount <
            kSampleCapacity)
        {
            stat.samples[
                stat.sampleCount] =
                elapsed;

            ++stat.sampleCount;
        }
        else
        {
            stat.samples[
                stat.nextSample] =
                elapsed;

            stat.nextSample =
                (stat.nextSample + 1) %
                kSampleCapacity;
        }
    }


    CpuScope::CpuScope(
        CpuRegion region) noexcept
        : region_(region),
          start_(CpuBegin())
    {
    }


    CpuScope::~CpuScope() noexcept
    {
        CpuEnd(
            region_,
            start_);
    }


    void ProbeGpuQueryCapabilities(
        IDirect3DDevice9* device) noexcept
    {
        if (!Enabled() ||
            !device ||
            !AcceptThread())
        {
            return;
        }

        if (g_probedDevice == device)
            return;

        g_probedDevice = device;

        const QueryCapability timestamp =
            TestQuery(
                device,
                D3DQUERYTYPE_TIMESTAMP);

        const QueryCapability frequency =
            TestQuery(
                device,
                D3DQUERYTYPE_TIMESTAMPFREQ);

        const QueryCapability disjoint =
            TestQuery(
                device,
                D3DQUERYTYPE_TIMESTAMPDISJOINT);

        WLOG_INFO(
            "wxl-r8-perf: gpu_query_caps "
            "timestamp=%u timestamp_hr=0x%08X "
            "frequency=%u frequency_hr=0x%08X "
            "disjoint=%u disjoint_hr=0x%08X "
            "capability_only=1 issue=0 getdata=0 wait=0",
            timestamp.supported ? 1u : 0u,
            static_cast<unsigned>(
                timestamp.hr),
            frequency.supported ? 1u : 0u,
            static_cast<unsigned>(
                frequency.hr),
            disjoint.supported ? 1u : 0u,
            static_cast<unsigned>(
                disjoint.hr));
    }


    void FrameBoundary(
        unsigned width,
        unsigned height,
        unsigned msaa,
        bool aoActive,
        bool smaaActive) noexcept
    {
        if (!Enabled() ||
            !AcceptThread())
        {
            return;
        }

        g_lastWidth = width;
        g_lastHeight = height;
        g_lastMsaa = msaa;
        g_lastAo = aoActive;
        g_lastSmaa = smaaActive;

        ++g_frameWindow;

        if (g_frameWindow >=
            kEmitEveryFrames)
        {
            EmitSummary();
            ResetStats();
        }
    }


    void Reset() noexcept
    {
        if (!Enabled())
            return;

        EmitSummary();

        ResetStats();

        g_probedDevice = nullptr;
        g_ownerThread = 0;

        WLOG_INFO(
            "wxl-r8-perf: reset "
            "device_capability_invalidated=1 "
            "pending_cpu_stats_cleared=1");
    }
}
