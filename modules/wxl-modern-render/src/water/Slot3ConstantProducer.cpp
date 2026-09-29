// R6 P01/A0/B1/Q0 grounded constant producer.
// GPL-3.0-or-later.

#include "water/Slot3ConstantProducer.hpp"

#ifdef _WIN32

#include "offsets/engine/Liquid.hpp"

#include <cstring>

#include <d3d9.h>

namespace wxl::water::slot3
{
namespace
{
    namespace liquid =
        wxl::offsets::engine::liquid;

    constexpr std::uintptr_t kVsExtensionBlock =
        0x00D44F88;

    constexpr std::uintptr_t kPsExtensionBlock =
        0x00D44C48;

    template <typename T>
    const T* Ptr(
        std::uintptr_t address) noexcept
    {
        return reinterpret_cast<const T*>(
            address);
    }

    template <typename T>
    const T* ByteOffset(
        const void* base,
        std::size_t offset) noexcept
    {
        return reinterpret_cast<const T*>(
            reinterpret_cast<const std::uint8_t*>(
                base) +
            offset);
    }

    bool CopyRows(
        const Float4* source,
        Float4* destination,
        std::size_t count) noexcept
    {
        if (!source ||
            !destination ||
            count == 0)
            return false;

        std::memcpy(
            destination,
            source,
            count * sizeof(Float4));

        return true;
    }
}

bool CaptureWrathP01ConstantSnapshot(
    const void* settings,
    WrathP01ConstantSnapshot& output) noexcept
{
    WrathP01ConstantSnapshot candidate{};

    if (!settings)
        return false;

    const auto* const vsMain =
        Ptr<Float4>(
            liquid::kVsConstBlock);

    const auto* const psMain =
        Ptr<Float4>(
            liquid::kPsConstBlock);

    const auto* const vsExtension =
        Ptr<Float4>(
            kVsExtensionBlock);

    const auto* const psExtension =
        Ptr<Float4>(
            kPsExtensionBlock);

    if (!CopyRows(
            vsMain,
            candidate.vsMain.data(),
            candidate.vsMain.size()) ||
        !CopyRows(
            psMain,
            candidate.psMain.data(),
            candidate.psMain.size()) ||
        !CopyRows(
            vsExtension,
            candidate.vsExtension.data(),
            candidate.vsExtension.size()) ||
        !CopyRows(
            psExtension,
            candidate.psExtension.data(),
            candidate.psExtension.size()))
        return false;

    const auto* const settingsFloats =
        ByteOffset<float>(
            settings,
            liquid::kSettingsFloats);

    if (!settingsFloats)
        return false;

    std::memcpy(
        candidate.settingsFloats.data(),
        settingsFloats,
        candidate.settingsFloats.size() *
            sizeof(float));

    candidate.settingsColor0 =
        *ByteOffset<std::uint32_t>(
            settings,
            liquid::kSettingsColor0);

    candidate.settingsColor1 =
        *ByteOffset<std::uint32_t>(
            settings,
            liquid::kSettingsColor1);

    candidate.settingsReady = true;

    const void* const dayNight =
        reinterpret_cast<const void*>(
            liquid::kDayNightInfo);

    if (!dayNight)
        return false;

    candidate.oceanCloseColor =
        *ByteOffset<std::uint32_t>(
            dayNight,
            liquid::kDnOceanCloseColor);

    candidate.oceanFarColor =
        *ByteOffset<std::uint32_t>(
            dayNight,
            liquid::kDnOceanFarColor);

    candidate.riverCloseColor =
        *ByteOffset<std::uint32_t>(
            dayNight,
            liquid::kDnRiverCloseColor);

    candidate.riverFarColor =
        *ByteOffset<std::uint32_t>(
            dayNight,
            liquid::kDnRiverFarColor);

    candidate.oceanAlphaShallow =
        *ByteOffset<float>(
            dayNight,
            liquid::kDnOceanAlphaShallow);

    candidate.riverAlphaShallow =
        *ByteOffset<float>(
            dayNight,
            liquid::kDnRiverAlphaShallow);

    candidate.oceanAlphaDeep =
        *ByteOffset<float>(
            dayNight,
            liquid::kDnOceanAlphaDeep);

    candidate.riverAlphaDeep =
        *ByteOffset<float>(
            dayNight,
            liquid::kDnRiverAlphaDeep);

    candidate.dayNightReady = true;
    candidate.nativeBlocksReady = true;

    if (!ValidateWrathP01Snapshot(
            candidate))
        return false;

    output = candidate;
    return true;
}


bool CaptureWrathP01Viewport(
    IDirect3DDevice9* device,
    WrathP01ConstantSnapshot& output) noexcept
{
    if (!device)
        return false;

    D3DVIEWPORT9 viewport{};

    if (FAILED(
            device->GetViewport(
                &viewport)) ||
        viewport.Width == 0 ||
        viewport.Height == 0)
    {
        return false;
    }

    WrathP01ConstantSnapshot candidate =
        output;

    candidate.viewportX =
        static_cast<float>(
            viewport.X);

    candidate.viewportY =
        static_cast<float>(
            viewport.Y);

    candidate.viewportWidth =
        static_cast<float>(
            viewport.Width);

    candidate.viewportHeight =
        static_cast<float>(
            viewport.Height);

    candidate.viewportMinZ =
        viewport.MinZ;

    candidate.viewportMaxZ =
        viewport.MaxZ;

    candidate.viewportReady =
        true;

    if (!ValidateWrathP01Snapshot(
            candidate))
    {
        return false;
    }

    output =
        candidate;

    return true;
}

bool CaptureWrathP01ConstantSnapshot(
    IDirect3DDevice9* device,
    const void* settings,
    WrathP01ConstantSnapshot& output) noexcept
{
    WrathP01ConstantSnapshot candidate{};

    if (!CaptureWrathP01ConstantSnapshot(
            settings,
            candidate) ||
        !CaptureWrathP01Viewport(
            device,
            candidate))
    {
        return false;
    }

    output =
        candidate;

    return true;
}


} // namespace wxl::water::slot3

#endif
