#include "Slot3MaterialRuntime.hpp"

#if defined(_WIN32)

#include "offsets/game/ADT.hpp"

namespace wxl::water::slot3 {
namespace {

namespace adt =
    wxl::offsets::game::adt;

void ReleaseHandle(
    std::uintptr_t& handle) noexcept
{
    if (!handle)
        return;

    const auto release =
        reinterpret_cast<
            adt::TextureReleaseFn>(
                adt::kTextureRelease);

    release(
        reinterpret_cast<void*>(
            handle));

    handle = 0;
}

bool LoadAndResolve(
    const MaterialAssetSpec& asset,
    std::uintptr_t& handleOut,
    std::uintptr_t& gxOut) noexcept
{
    handleOut = 0;
    gxOut = 0;

    // Reuse the stock one-argument map texture helper. It constructs the
    // native wrap/filter/load flags internally using current client state,
    // then calls the central TextureCreate API. No guessed flags are passed
    // by WarcraftXL.
    const auto load =
        reinterpret_cast<
            adt::Map_LoadTextureFn>(
                adt::kMapLoadTexture);

    const auto resolve =
        reinterpret_cast<
            adt::Map_TexResolveFn>(
                adt::kTexResolve);

    void* const handle =
        load(
            asset.path.data());

    if (!handle)
        return false;

    // R5-established native handle resolution contract.
    void* const gx =
        resolve(
            handle,
            1,
            0);

    if (!gx) {
        auto owned =
            reinterpret_cast<std::uintptr_t>(
                handle);

        ReleaseHandle(owned);
        return false;
    }

    handleOut =
        reinterpret_cast<std::uintptr_t>(
            handle);

    gxOut =
        reinterpret_cast<std::uintptr_t>(
            gx);

    return true;
}

} // namespace

MaterialRuntime::~MaterialRuntime()
{
    Reset();
}

void MaterialRuntime::Reset() noexcept
{
    // Release in reverse acquisition order.
    ReleaseHandle(
        state_.t7Handle);

    ReleaseHandle(
        state_.t5Handle);

    state_.Reset();
}

MaterialProduceStatus MaterialRuntime::Produce(
    MaterialView& output) noexcept
{
    if (!state_.Ready()) {
        Reset();

        std::uintptr_t t5Handle = 0;
        std::uintptr_t t5Gx = 0;

        if (!LoadAndResolve(
                kT5Asset,
                t5Handle,
                t5Gx))
        {
            return
                MaterialProduceStatus::Unavailable;
        }

        if (!state_.SetT5(
                t5Handle,
                t5Gx))
        {
            ReleaseHandle(
                t5Handle);

            return
                MaterialProduceStatus::Unavailable;
        }

        std::uintptr_t t7Handle = 0;
        std::uintptr_t t7Gx = 0;

        if (!LoadAndResolve(
                kT7Asset,
                t7Handle,
                t7Gx))
        {
            Reset();

            return
                MaterialProduceStatus::Unavailable;
        }

        if (!state_.SetT7(
                t7Handle,
                t7Gx))
        {
            ReleaseHandle(
                t7Handle);

            Reset();

            return
                MaterialProduceStatus::Unavailable;
        }
    }

    MaterialView candidate{};

    candidate.t5Handle =
        reinterpret_cast<void*>(
            state_.t5Handle);

    candidate.t5Gx =
        reinterpret_cast<void*>(
            state_.t5Gx);

    candidate.t7Handle =
        reinterpret_cast<void*>(
            state_.t7Handle);

    candidate.t7Gx =
        reinterpret_cast<void*>(
            state_.t7Gx);

    output = candidate;

    return
        MaterialProduceStatus::Ready;
}

} // namespace wxl::water::slot3

#endif
