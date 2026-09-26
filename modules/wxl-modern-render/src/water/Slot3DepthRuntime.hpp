#pragma once

#include "Slot3Core.hpp"
#include "Slot3SnapshotView.hpp"
#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
struct IDirect3DPixelShader9;

namespace wxl::water::slot3 {

constexpr std::uint32_t kIntzFormat =
    std::uint32_t('I') |
    (std::uint32_t('N') << 8) |
    (std::uint32_t('T') << 16) |
    (std::uint32_t('Z') << 24);

struct LinearDepthKey {
    std::uintptr_t device = 0;
    std::uintptr_t sourceRt = 0;
    std::uintptr_t rawDepth = 0;
    std::uint64_t generation = 0;
    std::uint64_t scene = 0;
    std::uint64_t frame = 0;
    std::uint64_t sourceDepthOrdinal = 0;
    unsigned width = 0;
    unsigned height = 0;
    float a = 0.f;
    float b = 0.f;
};

inline bool SameLinearDepthResources(
    const LinearDepthKey& lhs,
    const LinearDepthKey& rhs) noexcept
{
    return lhs.device != 0 &&
        lhs.device == rhs.device &&
        lhs.width == rhs.width &&
        lhs.height == rhs.height;
}

inline bool SameLinearDepthContent(
    const LinearDepthKey& lhs,
    const LinearDepthKey& rhs) noexcept
{
    return SameLinearDepthResources(lhs, rhs) &&
        lhs.sourceRt == rhs.sourceRt &&
        lhs.rawDepth == rhs.rawDepth &&
        lhs.generation == rhs.generation &&
        lhs.scene == rhs.scene &&
        lhs.frame == rhs.frame &&
        lhs.sourceDepthOrdinal == rhs.sourceDepthOrdinal &&
        lhs.a == rhs.a &&
        lhs.b == rhs.b;
}

inline bool BuildLinearDepthKey(
    const wxl::waterdiag::PreWaterSnapshotView& snapshot,
    std::uintptr_t expectedDevice,
    std::uintptr_t expectedSourceRt,
    std::uint64_t consumerOrdinal,
    const DepthProjection& projection,
    LinearDepthKey& output) noexcept
{
    if (!expectedDevice ||
        !expectedSourceRt ||
        !consumerOrdinal ||
        !projection.valid ||
        !std::isfinite(projection.a) ||
        !std::isfinite(projection.b) ||
        projection.b == 0.f ||
        snapshot.depthFormat != kIntzFormat ||
        snapshot.device != expectedDevice ||
        !wxl::waterdiag::SnapshotMetadataCompatible(
            snapshot,
            expectedDevice,
            expectedSourceRt,
            consumerOrdinal))
    {
        return false;
    }

    LinearDepthKey candidate{};
    candidate.device = expectedDevice;
    candidate.sourceRt = expectedSourceRt;
    candidate.rawDepth =
        reinterpret_cast<std::uintptr_t>(
            snapshot.rawDepthIntz);
    candidate.generation =
        snapshot.generation;
    candidate.scene =
        snapshot.scene;
    candidate.frame =
        snapshot.frame;
    candidate.sourceDepthOrdinal =
        snapshot.depthProducerOrdinal;
    candidate.width =
        snapshot.width;
    candidate.height =
        snapshot.height;
    candidate.a =
        projection.a;
    candidate.b =
        projection.b;

    output = candidate;
    return true;
}

struct LinearDepthLifecycleState {
    bool resourcesReady = false;
    bool contentReady = false;
    LinearDepthKey resourceKey{};
    LinearDepthKey contentKey{};

    void Reset() noexcept
    {
        resourcesReady = false;
        contentReady = false;
        resourceKey = {};
        contentKey = {};
    }

    bool ResourcesMatch(
        const LinearDepthKey& key) const noexcept
    {
        return resourcesReady &&
            SameLinearDepthResources(
                resourceKey,
                key);
    }

    void MarkResourcesReady(
        const LinearDepthKey& key) noexcept
    {
        resourcesReady = true;
        contentReady = false;
        resourceKey = key;
        contentKey = {};
    }

    void InvalidateContent() noexcept
    {
        contentReady = false;
        contentKey = {};
    }

    bool MarkContentReady(
        const LinearDepthKey& key) noexcept
    {
        if (!ResourcesMatch(key))
            return false;

        contentKey = key;
        contentReady = true;
        return true;
    }

    bool ContentMatches(
        const LinearDepthKey& key) const noexcept
    {
        return contentReady &&
            SameLinearDepthContent(
                contentKey,
                key);
    }
};

enum class DepthProduceStatus : std::uint8_t {
    Ready,
    Unavailable,
    RestoreFailed,
};

struct LinearDepthView {
    // Borrowed pointer. Ownership remains with DepthRuntime.
    IDirect3DTexture9* texture = nullptr;
    LinearDepthKey key{};
};

class DepthRuntime {
public:
    DepthRuntime() = default;
    ~DepthRuntime();

    DepthRuntime(
        const DepthRuntime&) = delete;

    DepthRuntime& operator=(
        const DepthRuntime&) = delete;

    DepthProduceStatus Produce(
        IDirect3DDevice9* device,
        const wxl::waterdiag::PreWaterSnapshotView& snapshot,
        std::uintptr_t expectedSourceRt,
        std::uint64_t consumerOrdinal,
        const DepthProjection& projection,
        LinearDepthView& output) noexcept;

    // Explicit lost/reset lifecycle boundary.
    // All D3DPOOL_DEFAULT resources and compiled shader state are released.
    void Reset() noexcept;

    bool ReadyFor(
        const LinearDepthKey& key) const noexcept
    {
        return texture_ &&
            surface_ &&
            shader_ &&
            lifecycle_.ContentMatches(key);
    }

private:
    enum class ConvertStatus : std::uint8_t {
        Converted,
        FailedRestored,
        RestoreFailed,
    };

    bool EnsureResources(
        IDirect3DDevice9* device,
        const LinearDepthKey& key) noexcept;

    bool EnsureShader(
        IDirect3DDevice9* device) noexcept;

    ConvertStatus Convert(
        IDirect3DDevice9* device,
        IDirect3DTexture9* rawDepth,
        const LinearDepthKey& key) noexcept;

    // Device pointer is borrowed. D3D resources below are owned.
    IDirect3DDevice9* device_ = nullptr;
    IDirect3DTexture9* texture_ = nullptr;
    IDirect3DSurface9* surface_ = nullptr;
    IDirect3DPixelShader9* shader_ = nullptr;

    LinearDepthLifecycleState lifecycle_{};
};

} // namespace wxl::water::slot3
