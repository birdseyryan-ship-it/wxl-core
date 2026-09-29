#pragma once

#include <cstdint>
#include <string_view>

namespace wxl::water::slot3 {

struct MaterialAssetSpec {
    std::string_view path{};
    std::uint32_t frameCount = 0;
};

// Build-pinned Classic 1.13.2.31650 Material-1 water resources.
//
// Recovered LiquidType/LiquidMaterial evidence closes these identities:
//   t5 = XTextures\ocean\newoceanbump.blp
//   t7 = XTextures\foam\waterchop7bw.blp
//
// Both resources have exactly one frame. No animation or filename
// substitution is therefore introduced by this producer.
inline constexpr MaterialAssetSpec kT5Asset{
    R"(XTextures\ocean\newoceanbump.blp)",
    1u
};

inline constexpr MaterialAssetSpec kT7Asset{
    R"(XTextures\foam\waterchop7bw.blp)",
    1u
};

struct MaterialLifecycleState {
    std::uintptr_t t5Handle = 0;
    std::uintptr_t t5Gx = 0;
    std::uintptr_t t7Handle = 0;
    std::uintptr_t t7Gx = 0;

    void Reset() noexcept
    {
        t5Handle = 0;
        t5Gx = 0;
        t7Handle = 0;
        t7Gx = 0;
    }

    bool SetT5(
        std::uintptr_t handle,
        std::uintptr_t gx) noexcept
    {
        if (!handle || !gx)
            return false;

        t5Handle = handle;
        t5Gx = gx;
        return true;
    }

    bool SetT7(
        std::uintptr_t handle,
        std::uintptr_t gx) noexcept
    {
        if (!handle || !gx)
            return false;

        t7Handle = handle;
        t7Gx = gx;
        return true;
    }

    bool Ready() const noexcept
    {
        return
            t5Handle != 0 &&
            t5Gx != 0 &&
            t7Handle != 0 &&
            t7Gx != 0;
    }
};

enum class MaterialProduceStatus : std::uint8_t {
    Ready,
    Unavailable,
};

struct MaterialView {
    // All pointers are borrowed from MaterialRuntime.
    // The native texture handles own their resolved Gx resources.
    void* t5Handle = nullptr;
    void* t5Gx = nullptr;
    void* t7Handle = nullptr;
    void* t7Gx = nullptr;
};

class MaterialRuntime {
public:
    MaterialRuntime() = default;
    ~MaterialRuntime();

    MaterialRuntime(
        const MaterialRuntime&) = delete;

    MaterialRuntime& operator=(
        const MaterialRuntime&) = delete;

    MaterialProduceStatus Produce(
        MaterialView& output) noexcept;

    // Drops this extension's native handle references using the matching
    // stock texture-release path. It does not touch sampler state.
    void Reset() noexcept;

    bool Ready() const noexcept
    {
        return state_.Ready();
    }

private:
    MaterialLifecycleState state_{};
};

} // namespace wxl::water::slot3
