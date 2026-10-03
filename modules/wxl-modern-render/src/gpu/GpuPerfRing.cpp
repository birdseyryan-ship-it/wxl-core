#include "gpu/GpuPerfRing.hpp"

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
        constexpr std::size_t kOwnerCount =
            static_cast<std::size_t>(
                GpuOwner::Count);

        constexpr std::size_t kRegionCount =
            static_cast<std::size_t>(
                CpuRegion::Count);

        constexpr std::size_t kRingSize = 8;
        constexpr std::uint64_t kMinHarvestAge = 4;
        constexpr std::uint64_t kEmitEveryHarvestedFrames = 600;
        constexpr std::size_t kSampleCapacity = 2048;

        enum class QueryRead
        {
            Ready,
            NotReady,
            Failed,
        };

        struct QueryPair
        {
            IDirect3DQuery9* begin = nullptr;
            IDirect3DQuery9* end = nullptr;
        };

        struct Slot
        {
            IDirect3DQuery9* disjoint = nullptr;
            IDirect3DQuery9* frequency = nullptr;
            IDirect3DQuery9* totalBegin = nullptr;
            IDirect3DQuery9* totalEnd = nullptr;

            std::array<
                QueryPair,
                kRegionCount
            > regions{};

            std::array<
                bool,
                kRegionCount
            > regionOpen{};

            std::array<
                bool,
                kRegionCount
            > regionIssued{};

            std::uint64_t serial = 0;

            bool allocated = false;
            bool active = false;
            bool pending = false;
        };

        struct GpuStats
        {
            std::uint64_t count = 0;
            long double totalUs = 0.0L;
            double maxUs = 0.0;

            std::size_t sampleCount = 0;
            std::size_t nextSample = 0;

            std::array<
                double,
                kSampleCapacity
            > samples{};
        };

        struct OwnerState
        {
            IDirect3DDevice9* device = nullptr;

            std::array<
                Slot,
                kRingSize
            > slots{};

            std::array<
                GpuStats,
                kRegionCount
            > stats{};

            std::uint64_t serial = 0;
            std::uint64_t windowValidFrames = 0;
            std::uint64_t windowDisjointFrames = 0;
            std::uint64_t windowNotReadyPolls = 0;
            std::uint64_t windowRingBusyFrames = 0;
            std::uint64_t windowIssueFailures = 0;
            std::uint64_t windowGetDataFailures = 0;

            std::size_t cursor = 0;
            int activeIndex = -1;

            bool loggedConfig = false;
        };

        std::array<
            OwnerState,
            kOwnerCount
        > g_owners{};

        std::size_t OwnerIndex(
            GpuOwner owner) noexcept
        {
            return static_cast<std::size_t>(
                owner);
        }

        std::size_t RegionIndex(
            CpuRegion region) noexcept
        {
            return static_cast<std::size_t>(
                region);
        }

        const char* OwnerName(
            GpuOwner owner) noexcept
        {
            switch (owner)
            {
            case GpuOwner::Fallback:
                return "fallback";

            case GpuOwner::WaterSnapshot:
                return "water_snapshot";

            case GpuOwner::WaterLinear:
                return "water_linear";

            default:
                return "unknown";
            }
        }

        const char* RegionName(
            CpuRegion region) noexcept
        {
            switch (region)
            {
            case CpuRegion::FallbackFrameTotal:
                return "fallback.frame.total";

            case CpuRegion::FallbackColourResolve:
                return "fallback.colour_resolve";

            case CpuRegion::FallbackDepthStage1:
                return "fallback.depth_stage1";

            case CpuRegion::FallbackDepthStage2:
                return "fallback.depth_stage2";

            case CpuRegion::AoRawDraw:
                return "ao.raw_draw";

            case CpuRegion::AoCompositeDraw:
                return "ao.composite_draw";

            case CpuRegion::SmaaTotal:
                return "smaa.total";

            case CpuRegion::SmaaEdgeDraw:
                return "smaa.edge_draw";

            case CpuRegion::SmaaBlendDraw:
                return "smaa.blend_draw";

            case CpuRegion::SmaaNeighborhoodDraw:
                return "smaa.neighborhood_draw";

            case CpuRegion::WaterSnapshotTotal:
                return "water.snapshot.total";

            case CpuRegion::WaterColourCopy:
                return "water.colour_copy";

            case CpuRegion::WaterDepthStage1:
                return "water.depth_stage1";

            case CpuRegion::WaterDepthStage2:
                return "water.depth_stage2";

            case CpuRegion::WaterLinearDepth:
                return "water.linear_depth";

            default:
                return "unknown";
            }
        }

        CpuRegion TotalRegion(
            GpuOwner owner) noexcept
        {
            switch (owner)
            {
            case GpuOwner::Fallback:
                return
                    CpuRegion::FallbackFrameTotal;

            case GpuOwner::WaterSnapshot:
                return
                    CpuRegion::WaterSnapshotTotal;

            case GpuOwner::WaterLinear:
                return
                    CpuRegion::WaterLinearDepth;

            default:
                return CpuRegion::Count;
            }
        }

        GpuOwner OwnerForRegion(
            CpuRegion region) noexcept
        {
            switch (region)
            {
            case CpuRegion::FallbackColourResolve:
            case CpuRegion::FallbackDepthStage1:
            case CpuRegion::FallbackDepthStage2:
            case CpuRegion::AoRawDraw:
            case CpuRegion::AoCompositeDraw:
            case CpuRegion::SmaaTotal:
            case CpuRegion::SmaaEdgeDraw:
            case CpuRegion::SmaaBlendDraw:
            case CpuRegion::SmaaNeighborhoodDraw:
                return GpuOwner::Fallback;

            case CpuRegion::WaterColourCopy:
            case CpuRegion::WaterDepthStage1:
            case CpuRegion::WaterDepthStage2:
                return GpuOwner::WaterSnapshot;

            default:
                return GpuOwner::Count;
            }
        }

        bool IsSubRegion(
            GpuOwner owner,
            CpuRegion region) noexcept
        {
            return
                OwnerForRegion(region) ==
                owner;
        }

        template<class T>
        void SafeRelease(
            T*& p) noexcept
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        void ClearSlotState(
            Slot& slot) noexcept
        {
            slot.serial = 0;
            slot.active = false;
            slot.pending = false;

            slot.regionOpen.fill(false);
            slot.regionIssued.fill(false);
        }

        void ReleaseSlot(
            Slot& slot) noexcept
        {
            SafeRelease(slot.disjoint);
            SafeRelease(slot.frequency);
            SafeRelease(slot.totalBegin);
            SafeRelease(slot.totalEnd);

            for (auto& pair : slot.regions)
            {
                SafeRelease(pair.begin);
                SafeRelease(pair.end);
            }

            slot = {};
        }

        void ReleaseOwner(
            OwnerState& state) noexcept
        {
            for (auto& slot : state.slots)
                ReleaseSlot(slot);

            state = {};
        }

        bool CreateQuery(
            IDirect3DDevice9* device,
            D3DQUERYTYPE type,
            IDirect3DQuery9*& out) noexcept
        {
            out = nullptr;

            if (!device)
                return false;

            const HRESULT hr =
                device->CreateQuery(
                    type,
                    &out);

            if (FAILED(hr) ||
                !out)
            {
                SafeRelease(out);
                return false;
            }

            return true;
        }

        bool EnsureSlot(
            GpuOwner owner,
            IDirect3DDevice9* device,
            Slot& slot) noexcept
        {
            if (slot.allocated)
                return true;

            ReleaseSlot(slot);

            if (!CreateQuery(
                    device,
                    D3DQUERYTYPE_TIMESTAMPDISJOINT,
                    slot.disjoint) ||
                !CreateQuery(
                    device,
                    D3DQUERYTYPE_TIMESTAMPFREQ,
                    slot.frequency) ||
                !CreateQuery(
                    device,
                    D3DQUERYTYPE_TIMESTAMP,
                    slot.totalBegin) ||
                !CreateQuery(
                    device,
                    D3DQUERYTYPE_TIMESTAMP,
                    slot.totalEnd))
            {
                ReleaseSlot(slot);
                return false;
            }

            for (std::size_t i = 0;
                 i < kRegionCount;
                 ++i)
            {
                const CpuRegion region =
                    static_cast<CpuRegion>(i);

                if (!IsSubRegion(
                        owner,
                        region))
                {
                    continue;
                }

                if (!CreateQuery(
                        device,
                        D3DQUERYTYPE_TIMESTAMP,
                        slot.regions[i].begin) ||
                    !CreateQuery(
                        device,
                        D3DQUERYTYPE_TIMESTAMP,
                        slot.regions[i].end))
                {
                    ReleaseSlot(slot);
                    return false;
                }
            }

            slot.allocated = true;
            return true;
        }

        template<class T>
        QueryRead ReadQuery(
            IDirect3DQuery9* query,
            T& output) noexcept
        {
            if (!query)
                return QueryRead::Failed;

            const HRESULT hr =
                query->GetData(
                    &output,
                    sizeof(output),
                    0);

            if (hr == S_FALSE)
                return QueryRead::NotReady;

            if (FAILED(hr))
                return QueryRead::Failed;

            return QueryRead::Ready;
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

        void RecordDuration(
            OwnerState& state,
            CpuRegion region,
            UINT64 begin,
            UINT64 end,
            UINT64 frequency) noexcept
        {
            if (end < begin ||
                frequency == 0)
            {
                return;
            }

            const std::size_t index =
                RegionIndex(region);

            if (index >= kRegionCount)
                return;

            const long double ticks =
                static_cast<long double>(
                    end - begin);

            const double us =
                static_cast<double>(
                    ticks *
                    1000000.0L /
                    static_cast<long double>(
                        frequency));

            GpuStats& stat =
                state.stats[index];

            ++stat.count;
            stat.totalUs += us;
            stat.maxUs =
                std::max(
                    stat.maxUs,
                    us);

            if (stat.sampleCount <
                kSampleCapacity)
            {
                stat.samples[
                    stat.sampleCount] = us;

                ++stat.sampleCount;
            }
            else
            {
                stat.samples[
                    stat.nextSample] = us;

                stat.nextSample =
                    (stat.nextSample + 1) %
                    kSampleCapacity;
            }
        }

        void ResetWindowStats(
            OwnerState& state) noexcept
        {
            for (auto& stat : state.stats)
                stat = {};

            state.windowValidFrames = 0;
            state.windowDisjointFrames = 0;
            state.windowNotReadyPolls = 0;
            state.windowRingBusyFrames = 0;
            state.windowIssueFailures = 0;
            state.windowGetDataFailures = 0;
        }

        void EmitGpuSummary(
            GpuOwner owner,
            OwnerState& state) noexcept
        {
            if (state.windowValidFrames == 0)
                return;

            WLOG_INFO(
                "wxl-r8-perf: gpu_summary "
                "owner=%s frames=%llu "
                "disjoint=%llu not_ready=%llu "
                "ring_busy=%llu issue_fail=%llu "
                "getdata_fail=%llu",
                OwnerName(owner),
                static_cast<unsigned long long>(
                    state.windowValidFrames),
                static_cast<unsigned long long>(
                    state.windowDisjointFrames),
                static_cast<unsigned long long>(
                    state.windowNotReadyPolls),
                static_cast<unsigned long long>(
                    state.windowRingBusyFrames),
                static_cast<unsigned long long>(
                    state.windowIssueFailures),
                static_cast<unsigned long long>(
                    state.windowGetDataFailures));

            std::array<
                double,
                kSampleCapacity
            > sorted{};

            for (std::size_t i = 0;
                 i < kRegionCount;
                 ++i)
            {
                const GpuStats& stat =
                    state.stats[i];

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

                const double median =
                    sorted[
                        PercentileIndex(
                            n,
                            50)];

                const double p95 =
                    sorted[
                        PercentileIndex(
                            n,
                            95)];

                const double p99 =
                    sorted[
                        PercentileIndex(
                            n,
                            99)];

                const double mean =
                    static_cast<double>(
                        stat.totalUs /
                        static_cast<long double>(
                            stat.count));

                WLOG_INFO(
                    "wxl-r8-perf: gpu "
                    "owner=%s region=%s "
                    "count=%llu mean_us=%.3f "
                    "median_us=%.3f p95_us=%.3f "
                    "p99_us=%.3f max_us=%.3f",
                    OwnerName(owner),
                    RegionName(
                        static_cast<CpuRegion>(i)),
                    static_cast<unsigned long long>(
                        stat.count),
                    mean,
                    median,
                    p95,
                    p99,
                    stat.maxUs);
            }
        }

        void DropFailedSlot(
            OwnerState& state,
            Slot& slot) noexcept
        {
            ++state.windowGetDataFailures;
            ReleaseSlot(slot);
        }

        void HarvestSlot(
            GpuOwner owner,
            OwnerState& state,
            Slot& slot) noexcept
        {
            if (!slot.pending ||
                state.serial < slot.serial)
            {
                return;
            }

            const std::uint64_t age =
                state.serial -
                slot.serial;

            if (age <
                kMinHarvestAge)
            {
                return;
            }

            BOOL disjoint = FALSE;

            QueryRead read =
                ReadQuery(
                    slot.disjoint,
                    disjoint);

            if (read == QueryRead::NotReady)
            {
                ++state.windowNotReadyPolls;
                return;
            }

            if (read == QueryRead::Failed)
            {
                DropFailedSlot(
                    state,
                    slot);
                return;
            }

            UINT64 frequency = 0;

            read =
                ReadQuery(
                    slot.frequency,
                    frequency);

            if (read == QueryRead::NotReady)
            {
                ++state.windowNotReadyPolls;
                return;
            }

            if (read == QueryRead::Failed)
            {
                DropFailedSlot(
                    state,
                    slot);
                return;
            }

            UINT64 totalBegin = 0;
            UINT64 totalEnd = 0;

            read =
                ReadQuery(
                    slot.totalBegin,
                    totalBegin);

            if (read == QueryRead::NotReady)
            {
                ++state.windowNotReadyPolls;
                return;
            }

            if (read == QueryRead::Failed)
            {
                DropFailedSlot(
                    state,
                    slot);
                return;
            }

            read =
                ReadQuery(
                    slot.totalEnd,
                    totalEnd);

            if (read == QueryRead::NotReady)
            {
                ++state.windowNotReadyPolls;
                return;
            }

            if (read == QueryRead::Failed)
            {
                DropFailedSlot(
                    state,
                    slot);
                return;
            }

            std::array<
                UINT64,
                kRegionCount
            > begins{};

            std::array<
                UINT64,
                kRegionCount
            > ends{};

            for (std::size_t i = 0;
                 i < kRegionCount;
                 ++i)
            {
                if (!slot.regionIssued[i])
                    continue;

                read =
                    ReadQuery(
                        slot.regions[i].begin,
                        begins[i]);

                if (read == QueryRead::NotReady)
                {
                    ++state.windowNotReadyPolls;
                    return;
                }

                if (read == QueryRead::Failed)
                {
                    DropFailedSlot(
                        state,
                        slot);
                    return;
                }

                read =
                    ReadQuery(
                        slot.regions[i].end,
                        ends[i]);

                if (read == QueryRead::NotReady)
                {
                    ++state.windowNotReadyPolls;
                    return;
                }

                if (read == QueryRead::Failed)
                {
                    DropFailedSlot(
                        state,
                        slot);
                    return;
                }
            }

            if (disjoint != FALSE)
            {
                ++state.windowDisjointFrames;
                ClearSlotState(slot);
                return;
            }

            if (frequency == 0)
            {
                DropFailedSlot(
                    state,
                    slot);
                return;
            }

            RecordDuration(
                state,
                TotalRegion(owner),
                totalBegin,
                totalEnd,
                frequency);

            for (std::size_t i = 0;
                 i < kRegionCount;
                 ++i)
            {
                if (!slot.regionIssued[i])
                    continue;

                RecordDuration(
                    state,
                    static_cast<CpuRegion>(i),
                    begins[i],
                    ends[i],
                    frequency);
            }

            ++state.windowValidFrames;
            ClearSlotState(slot);

            if (state.windowValidFrames >=
                kEmitEveryHarvestedFrames)
            {
                EmitGpuSummary(
                    owner,
                    state);

                ResetWindowStats(
                    state);
            }
        }

        void HarvestOwner(
            GpuOwner owner,
            OwnerState& state) noexcept
        {
            for (auto& slot : state.slots)
            {
                HarvestSlot(
                    owner,
                    state,
                    slot);
            }
        }

        std::uint64_t BeginFrame(
            GpuOwner owner,
            IDirect3DDevice9* device) noexcept
        {
            if (!Enabled() ||
                !device)
            {
                return 0;
            }

            const std::size_t ownerIndex =
                OwnerIndex(owner);

            if (ownerIndex >=
                kOwnerCount)
            {
                return 0;
            }

            OwnerState& state =
                g_owners[ownerIndex];

            if (state.device != device)
            {
                ReleaseOwner(state);
                state.device = device;
            }

            ++state.serial;

            HarvestOwner(
                owner,
                state);

            if (state.activeIndex >= 0)
            {
                ++state.windowIssueFailures;
                return 0;
            }

            int selected = -1;

            for (std::size_t offset = 0;
                 offset < kRingSize;
                 ++offset)
            {
                const std::size_t index =
                    (state.cursor + offset) %
                    kRingSize;

                Slot& candidate =
                    state.slots[index];

                if (!candidate.pending &&
                    !candidate.active)
                {
                    selected =
                        static_cast<int>(
                            index);
                    break;
                }
            }

            if (selected < 0)
            {
                ++state.windowRingBusyFrames;
                return 0;
            }

            Slot& slot =
                state.slots[
                    static_cast<std::size_t>(
                        selected)];

            if (!EnsureSlot(
                    owner,
                    device,
                    slot))
            {
                ++state.windowIssueFailures;
                state.cursor =
                    (static_cast<std::size_t>(
                         selected) + 1) %
                    kRingSize;
                return 0;
            }

            ClearSlotState(slot);
            slot.serial = state.serial;

            const HRESULT disjointHr =
                slot.disjoint->Issue(
                    D3DISSUE_BEGIN);

            const HRESULT frequencyHr =
                slot.frequency->Issue(
                    D3DISSUE_END);

            const HRESULT totalBeginHr =
                slot.totalBegin->Issue(
                    D3DISSUE_END);

            if (FAILED(disjointHr) ||
                FAILED(frequencyHr) ||
                FAILED(totalBeginHr))
            {
                if (SUCCEEDED(disjointHr))
                {
                    slot.disjoint->Issue(
                        D3DISSUE_END);
                }

                ++state.windowIssueFailures;
                ReleaseSlot(slot);

                state.cursor =
                    (static_cast<std::size_t>(
                         selected) + 1) %
                    kRingSize;

                return 0;
            }

            slot.active = true;
            state.activeIndex = selected;

            if (!state.loggedConfig)
            {
                state.loggedConfig = true;

                WLOG_INFO(
                    "wxl-r8-perf: gpu_async "
                    "owner=%s ring=%u "
                    "min_harvest_age=%llu "
                    "getdata_flags=0 flush=0 "
                    "wait=0 same_frame=0",
                    OwnerName(owner),
                    static_cast<unsigned>(
                        kRingSize),
                    static_cast<unsigned long long>(
                        kMinHarvestAge));
            }

            return slot.serial;
        }

        void EndFrame(
            GpuOwner owner,
            std::uint64_t serial) noexcept
        {
            const std::size_t ownerIndex =
                OwnerIndex(owner);

            if (ownerIndex >=
                kOwnerCount ||
                serial == 0)
            {
                return;
            }

            OwnerState& state =
                g_owners[ownerIndex];

            if (state.activeIndex < 0)
                return;

            const std::size_t index =
                static_cast<std::size_t>(
                    state.activeIndex);

            Slot& slot =
                state.slots[index];

            if (!slot.active ||
                slot.serial != serial)
            {
                ++state.windowIssueFailures;
                return;
            }

            const HRESULT totalEndHr =
                slot.totalEnd->Issue(
                    D3DISSUE_END);

            const HRESULT disjointEndHr =
                slot.disjoint->Issue(
                    D3DISSUE_END);

            slot.active = false;
            state.activeIndex = -1;

            if (SUCCEEDED(totalEndHr) &&
                SUCCEEDED(disjointEndHr))
            {
                slot.pending = true;
            }
            else
            {
                ++state.windowIssueFailures;
                ReleaseSlot(slot);
            }

            state.cursor =
                (index + 1) %
                kRingSize;
        }

        std::uint64_t BeginRegion(
            CpuRegion region,
            IDirect3DDevice9* device,
            GpuOwner& ownerOut) noexcept
        {
            ownerOut =
                OwnerForRegion(region);

            if (!Enabled() ||
                !device ||
                ownerOut == GpuOwner::Count)
            {
                return 0;
            }

            const std::size_t ownerIndex =
                OwnerIndex(ownerOut);

            OwnerState& state =
                g_owners[ownerIndex];

            if (state.device != device ||
                state.activeIndex < 0)
            {
                return 0;
            }

            Slot& slot =
                state.slots[
                    static_cast<std::size_t>(
                        state.activeIndex)];

            const std::size_t regionIndex =
                RegionIndex(region);

            if (!slot.active ||
                regionIndex >= kRegionCount ||
                slot.regionOpen[regionIndex] ||
                slot.regionIssued[regionIndex])
            {
                return 0;
            }

            QueryPair& pair =
                slot.regions[regionIndex];

            if (!pair.begin ||
                !pair.end)
            {
                return 0;
            }

            const HRESULT hr =
                pair.begin->Issue(
                    D3DISSUE_END);

            if (FAILED(hr))
            {
                ++state.windowIssueFailures;
                return 0;
            }

            slot.regionOpen[regionIndex] = true;
            return slot.serial;
        }

        void EndRegion(
            GpuOwner owner,
            CpuRegion region,
            std::uint64_t serial) noexcept
        {
            const std::size_t ownerIndex =
                OwnerIndex(owner);

            const std::size_t regionIndex =
                RegionIndex(region);

            if (ownerIndex >= kOwnerCount ||
                regionIndex >= kRegionCount ||
                serial == 0)
            {
                return;
            }

            OwnerState& state =
                g_owners[ownerIndex];

            if (state.activeIndex < 0)
                return;

            Slot& slot =
                state.slots[
                    static_cast<std::size_t>(
                        state.activeIndex)];

            if (!slot.active ||
                slot.serial != serial ||
                !slot.regionOpen[regionIndex])
            {
                return;
            }

            QueryPair& pair =
                slot.regions[regionIndex];

            const HRESULT hr =
                pair.end->Issue(
                    D3DISSUE_END);

            slot.regionOpen[regionIndex] = false;

            if (SUCCEEDED(hr))
            {
                slot.regionIssued[regionIndex] = true;
            }
            else
            {
                ++state.windowIssueFailures;
            }
        }
    }


    GpuFrameScope::GpuFrameScope(
        GpuOwner owner,
        IDirect3DDevice9* device) noexcept
        : owner_(owner)
    {
        serial_ =
            BeginFrame(
                owner_,
                device);

        active_ =
            serial_ != 0;
    }


    GpuFrameScope::~GpuFrameScope() noexcept
    {
        if (!active_)
            return;

        EndFrame(
            owner_,
            serial_);
    }


    GpuScope::GpuScope(
        CpuRegion region,
        IDirect3DDevice9* device) noexcept
        : region_(region)
    {
        serial_ =
            BeginRegion(
                region_,
                device,
                owner_);

        active_ =
            serial_ != 0;
    }


    GpuScope::~GpuScope() noexcept
    {
        if (!active_)
            return;

        EndRegion(
            owner_,
            region_,
            serial_);
    }


    void GpuReset() noexcept
    {
        for (auto& state : g_owners)
            ReleaseOwner(state);

        if (Enabled())
        {
            WLOG_INFO(
                "wxl-r8-perf: gpu_reset "
                "queries_released=1 wait=0");
        }
    }
}
