// R6 Slot-3 selected mirrored-reflection Win32 backend.
//
// The backend is intentionally inert: no caller is wired to Produce() yet.
// It owns only the secondary reflection target/resources and the bounded
// camera/cull/render transaction required to produce t1.
//
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#include "water/Slot3ReflectionRuntime.hpp"

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;

namespace wxl::water::slot3
{
    struct NativeReflectionCandidate
    {
        // Borrowed pointer.  Ownership remains with Win32ReflectionRuntime.
        IDirect3DTexture9* texture = nullptr;

        ReflectionContentKey key{};

        unsigned mode = 0;
        bool ready = false;
    };

    class Win32ReflectionRuntime
    {
    public:
        Win32ReflectionRuntime() = default;
        ~Win32ReflectionRuntime();

        Win32ReflectionRuntime(
            const Win32ReflectionRuntime&) = delete;

        Win32ReflectionRuntime& operator=(
            const Win32ReflectionRuntime&) = delete;

        void Reset() noexcept;

        ReflectionSequenceStatus Produce(
            IDirect3DDevice9* device,
            const ReflectionPlan& plan,
            std::uint64_t producerOrdinal,
            bool& reflectionActive,
            NativeReflectionCandidate& out) noexcept;

        IDirect3DTexture9* Texture() const noexcept
        {
            return texture_;
        }

        bool ContentMatches(
            const ReflectionContentKey& key) const noexcept
        {
            return lifecycle_.ContentMatches(key);
        }

        bool Quarantined() const noexcept
        {
            return quarantined_;
        }

    private:
        bool EnsureResources(
            IDirect3DDevice9* device,
            const ReflectionPlan& plan) noexcept;

        void ReleaseResources() noexcept;
        void ResetContentOnly() noexcept;

        IDirect3DDevice9* device_ = nullptr;

        IDirect3DTexture9* texture_ = nullptr;
        IDirect3DSurface9* surface_ = nullptr;
        IDirect3DSurface9* depth_ = nullptr;

        unsigned depthFormat_ = 0;

        ReflectionLifecycleState lifecycle_{};
        ReflectionContentKey lastContent_{};

        bool quarantined_ = false;
    };
}
