// R6 read-only view of the already-proven CP09 pre-Water transport. GPL-3.0-or-later.
//
// The returned COM pointers are BORROWED. Callers may use them only on the
// native render thread during the current valid scene/frame; never Release,
// retain, or use them across reset/world transitions. Raw INTZ is explicitly
// named as such: it is NOT Classic t6 and must pass through the frozen R3
// view-Z reconstruction/conversion before slot-3 binding.
#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wxl::waterdiag
{
struct PreWaterSnapshotView
{
    IDirect3DTexture9* colour = nullptr;
    IDirect3DTexture9* rawDepthIntz = nullptr;
    uintptr_t device = 0;
    uintptr_t sourceRt = 0;
    uint64_t generation = 0;
    uint64_t scene = 0;
    uint64_t frame = 0;
    uint64_t colourProducerOrdinal = 0;
    uint64_t depthProducerOrdinal = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned colourFormat = 0;
    unsigned depthFormat = 0;
};

inline bool SnapshotMetadataCompatible(const PreWaterSnapshotView& view,
                                       uintptr_t expectedDevice,
                                       uintptr_t expectedSourceRt,
                                       uint64_t consumerOrdinal) noexcept
{
    return view.colour && view.rawDepthIntz && view.device && view.sourceRt &&
        expectedDevice && expectedSourceRt && view.device == expectedDevice &&
        view.sourceRt == expectedSourceRt && view.generation && view.scene &&
        view.frame && view.colourProducerOrdinal && view.depthProducerOrdinal &&
        view.colourProducerOrdinal < consumerOrdinal &&
        view.depthProducerOrdinal < consumerOrdinal && view.width && view.height;
}

// Read-only borrowing adapter for CP09-owned textures. It does not AddRef and
// does not alter any D3D state. Returns false rather than exposing stale or
// cross-device/cross-RT resources.
bool BorrowPreWaterSnapshot(IDirect3DDevice9* expectedDevice,
                            uintptr_t expectedSourceRt,
                            uint64_t consumerOrdinal,
                            PreWaterSnapshotView& output) noexcept;
} // namespace wxl::waterdiag
