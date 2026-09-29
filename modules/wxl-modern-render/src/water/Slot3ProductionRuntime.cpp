// P01 production coordinator. GPL-3.0-or-later.
#include "water/Slot3ProductionRuntime.hpp"
#ifdef _WIN32
#include "water/Slot3GpuState.hpp"
#include "water/Slot3ConstantRuntime.hpp"
#include "water/Slot3ReflectionBackendPolicy.hpp"
#include <cstring>

namespace wxl::water::slot3 {
namespace {
template<class T> struct Com {
    T* value = nullptr;
    ~Com() { if (value) value->Release(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
};
}

bool ProductionRuntime::EnsureZeroVolume(IDirect3DDevice9* device) noexcept {
    if (zeroDevice_ == device && zeroVolume_) return true;
    if (zeroVolume_) { zeroVolume_->Release(); zeroVolume_ = nullptr; }
    zeroDevice_ = nullptr;
    // All four channels are zero; channel ordering does not change the
    // recovered one-texel fallback. No enabled VolumeFog path enters here.
    if (FAILED(device->CreateVolumeTexture(1, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
                                          D3DPOOL_MANAGED, &zeroVolume_, nullptr)) ||
        !zeroVolume_) return false;
    D3DLOCKED_BOX box{};
    if (FAILED(zeroVolume_->LockBox(0, &box, nullptr, 0))) {
        zeroVolume_->Release(); zeroVolume_ = nullptr; return false;
    }
    const std::uint32_t zero = 0;
    const bool valid = box.pBits && box.RowPitch >= 4 && box.SlicePitch >= 4;
    if (valid) std::memcpy(box.pBits, &zero, sizeof(zero));
    const HRESULT unlock = zeroVolume_->UnlockBox(0);
    if (!valid || FAILED(unlock)) {
        zeroVolume_->Release(); zeroVolume_ = nullptr; return false;
    }
    zeroDevice_ = device;
    return true;
}

void ProductionRuntime::Reset() noexcept {
    if (bridge_) bridge_->Reset();
    shaders_.Reset(); materials_.Reset(); depth_.Reset(); reflection_.Reset();
    if (zeroVolume_) { zeroVolume_->Release(); zeroVolume_ = nullptr; }
    zeroDevice_ = nullptr;
    ResetFirstCandidateLightingCapture();
    internal_ = reflectionActive_ = quarantined_ = false;
}

struct ProductionRuntime::Transaction {
    ProductionRuntime& owner;
    const ProductionDraw& draw;
    ProductionTrace& trace;
    GpuState gpu{};
    Com<IDirect3DSurface9> rt{};
    D3DSURFACE_DESC target{};
    D3DVIEWPORT9 viewport{};
    waterdiag::PreWaterSnapshotView snapshot{};
    WrathP01ConstantSnapshot nativeConstants{};
    FirstCandidateRuntimeRows rows{};
    DepthProjection projection{};
    ReflectionPlan plan{};
    NativeReflectionCandidate reflection{};
    LinearDepthView linearDepth{};
    MaterialView material{};
    ConstantPacket packet{};
    bool restoreUncertain = false;

    bool Preflight() noexcept {
        trace.stage = ProductionStage::NativeBridge;
        if (!owner.Operational() || owner.internal_ || !draw.device || !draw.native)
            return false;
        trace.stage = ProductionStage::Profile;
        if (!ResolveFirstCandidateLiquidTypeId(draw.settings, trace.liquidTypeId) ||
            !FirstCandidateDrawSupported(draw.profile, 1, 44, trace.liquidTypeId))
            return false;
        trace.stage = ProductionStage::Geometry;
        Com<IDirect3DVertexBuffer9> stream;
        UINT offset = 0, stride = 0, frequency = 0;
        if (FAILED(draw.device->GetStreamSource(0, &stream.value, &offset, &stride)) ||
            !stream.value || stride != 44 ||
            FAILED(draw.device->GetStreamSourceFreq(0, &frequency)) || frequency != 1)
            return false;
        trace.stage = ProductionStage::Plane;
        if (!CaptureFirstCandidateWaterPlane(draw.device, unsigned(draw.type),
                draw.base, draw.min, draw.vertices, draw.start, draw.primitives,
                trace.plane)) return false;
        trace.stage = ProductionStage::Camera;
        ReflectionVec3 eye{}, targetPoint{};
        if (!owner.bridge_->Camera(eye, targetPoint) || !Finite(eye) ||
            !Finite(targetPoint) || !(eye.z > trace.plane)) return false;
        trace.stage = ProductionStage::VolumeFog;
        if (!owner.bridge_->VolumeFogDisabledOrUnsupported()) return false;
        trace.stage = ProductionStage::Snapshot;
        if (FAILED(draw.device->GetRenderTarget(0, &rt.value)) || !rt.value ||
            FAILED(rt.value->GetDesc(&target)) ||
            FAILED(draw.device->GetViewport(&viewport)) ||
            viewport.MinZ != 0.0f || viewport.MaxZ != 1.0f ||
            !waterdiag::BorrowPreWaterSnapshot(draw.device,
                reinterpret_cast<std::uintptr_t>(rt.value),
                draw.ordinals.consumer, snapshot)) return false;
        if (snapshot.width != target.Width || snapshot.height != target.Height)
            return false;
        trace.colourOrdinal = snapshot.colourProducerOrdinal;
        trace.depthOrdinal = snapshot.depthProducerOrdinal;
        trace.reflectionOrdinal = draw.ordinals.reflection;
        trace.consumerOrdinal = draw.ordinals.consumer;
        trace.stage = ProductionStage::Rows;
        // Snapshot all main-camera constants and lighting BEFORE rendering a
        // secondary scene. No constant is re-read from the reflected scene.
        if (!PopulateFirstCandidateDrawRuntime(draw.settings, draw.world,
                viewport.X, viewport.Y, viewport.Width, viewport.Height,
                target.Width, target.Height, rows) ||
            !CaptureWrathP01ConstantSnapshot(draw.device, draw.settings, nativeConstants) ||
            !PopulateFirstCandidateLightingRuntime(draw.invocation, draw.settings, rows))
            return false;
        std::array<float, 16> projectionMatrix{};
        std::memcpy(projectionMatrix.data(), nativeConstants.vsMain.data(), sizeof(projectionMatrix));
        projection = DepthProjection::FromWorldProjection(projectionMatrix.data());
        if (!projection.valid) return false;
        ReflectionRequest request{};
        request.device = snapshot.device; request.sourceRt = snapshot.sourceRt;
        request.generation = snapshot.generation; request.scene = snapshot.scene;
        request.frame = snapshot.frame; request.consumerOrdinal = draw.ordinals.consumer;
        request.parentWidth = target.Width; request.parentHeight = target.Height;
        request.downsampleShift = kFirstCandidateDownsampleShift;
        request.d3dColourFormat = kNativeReflectionD3dColourFormat;
        request.mode = owner.config_.reflectionMode; request.plane = trace.plane;
        request.eye = eye; request.target = targetPoint;
        return draw.ordinals.reflection > snapshot.depthProducerOrdinal &&
               draw.ordinals.reflection < draw.ordinals.consumer &&
               BuildReflectionPlan(request, plan);
    }
    bool Capture() noexcept {
        trace.stage = ProductionStage::Capture;
        return gpu.Capture(draw.device) && owner.bridge_->Capture(draw.device);
    }
    bool Produce() noexcept {
        // The material loader, depth converter and native reflection are all
        // inside the enclosing CPU/GPU restoration transaction.
        trace.stage = ProductionStage::Materials;
        if (owner.materials_.Produce(material) != MaterialProduceStatus::Ready)
            return false;
        trace.stage = ProductionStage::Shaders;
        if (!owner.shaders_.Ensure(draw.device)) return false;
        trace.stage = ProductionStage::Depth;
        const auto depthStatus = owner.depth_.Produce(draw.device, snapshot,
            snapshot.sourceRt, draw.ordinals.consumer, projection, linearDepth);
        if (depthStatus != DepthProduceStatus::Ready) {
            restoreUncertain = depthStatus == DepthProduceStatus::RestoreFailed;
            return false;
        }
        trace.stage = ProductionStage::Reflection;
        const auto reflected = owner.reflection_.Produce(draw.device, plan,
            draw.ordinals.reflection, owner.reflectionActive_, reflection);
        if (reflected != ReflectionSequenceStatus::Produced) {
            restoreUncertain = reflected == ReflectionSequenceStatus::RestoreFailed;
            return false;
        }
        trace.stage = ProductionStage::Constants;
        ReflectionContentKey expected{};
        if (!BuildReflectionContentKey(plan, draw.ordinals.reflection, expected) ||
            !SameReflectionContent(expected, reflection.key) ||
            !ApplyNativeReflectionCandidateRuntime(reflection, rows) ||
            !BuildFirstCandidatePacket(nativeConstants, rows, packet) ||
            !owner.EnsureZeroVolume(draw.device)) return false;
        Prerequisites p{};
        p.profile = draw.profile; p.streams = 1; p.stride = 44;
        p.draw = {snapshot.device, snapshot.sourceRt, snapshot.generation,
                  snapshot.scene, snapshot.frame, draw.ordinals.consumer};
        p.colour = p.depth = p.reflection = p.draw;
        p.colour.ordinal = snapshot.colourProducerOrdinal;
        p.depth.ordinal = snapshot.depthProducerOrdinal;
        p.reflection.ordinal = draw.ordinals.reflection;
        p.colourReady = snapshot.colour != nullptr;
        p.depthReady = linearDepth.texture != nullptr;
        p.reflectionReady = reflection.ready;
        p.exactNormalReady = material.t5Handle && material.t5Gx;
        p.exactChopReady = material.t7Handle && material.t7Gx;
        p.constantsReady = packet.valid;
        p.shadersReady = owner.shaders_.ReadyFor(draw.device);
        p.aboveWater = plan.originalEye.z > trace.plane;
        p.volumeFogDisabled = owner.bridge_->VolumeFogDisabledOrUnsupported();
        p.deviceLost = false; p.selector = 5; p.reflectionBranch = 1;
        p.waterPlane = trace.plane; p.reflectionPlane = reflection.key.plane;
        p.producedReflectionMode = reflection.mode; p.projection = projection;
        trace.resourcesReady = Evaluate(owner.config_, p) == Rejection::None;
        return trace.resourcesReady;
    }
    bool Bind() noexcept {
        trace.stage = ProductionStage::Bind;
        // Native reflection may have rebound geometry. Put the captured
        // native DIP geometry back before applying the replacement resources.
        // CPU/Gx state is restored now, and again on ALL exit paths.
        const bool nativeRestored = owner.bridge_->Restore(draw.device);
        const bool gpuRestored = gpu.Restore(draw.device);
        if (!nativeRestored || !gpuRestored) {
            restoreUncertain = true; return false;
        }
        if (!owner.bridge_->BindMaterialsAndSamplers(draw.device, material)) return false;
        return SUCCEEDED(draw.device->SetTexture(0, snapshot.colour)) &&
               SUCCEEDED(draw.device->SetTexture(1, reflection.texture)) &&
               SUCCEEDED(draw.device->SetTexture(6, linearDepth.texture)) &&
               SUCCEEDED(draw.device->SetTexture(13, owner.zeroVolume_)) &&
               UploadConstants(draw.device, packet) &&
               SUCCEEDED(draw.device->SetVertexShader(owner.shaders_.Vertex())) &&
               SUCCEEDED(draw.device->SetPixelShader(owner.shaders_.Pixel()));
    }
    bool Restore() noexcept {
        // Do not short-circuit one owner's restoration on the other's failure.
        const bool native = owner.bridge_->Restore(draw.device);
        const bool d3d = gpu.Restore(draw.device);
        if (!native || !d3d || restoreUncertain) {
            trace.stage = ProductionStage::Restore; return false;
        }
        return true;
    }
    std::int32_t Native() noexcept {
        return draw.native(draw.device, draw.type, draw.base, draw.min,
                           draw.vertices, draw.start, draw.primitives);
    }
    std::int32_t Replace() noexcept {
        trace.stage = ProductionStage::Submitted;
        // Only this call submits replacement geometry. No catch/fallback
        // surrounds it. A failing HRESULT never triggers a second draw.
        return draw.native(draw.device, draw.type, draw.base, draw.min,
                           draw.vertices, draw.start, draw.primitives);
    }
    void Quarantine() noexcept { owner.quarantined_ = true; }
};

DrawResult ProductionRuntime::Draw(const ProductionDraw& draw,
                                   ProductionTrace& trace) noexcept {
    trace = {};
    if (!draw.native) return {D3DERR_INVALIDCALL, Submission::Native, true};
    Transaction transaction{*this, draw, trace};
    // Preflight must see the outer value. The guard is entered after that
    // check and remains live through producers, submission and restoration.
    if (!Requested() || internal_ || !transaction.Preflight())
        return {transaction.Native(), Submission::Native, true};
    ReflectionGuard guard(internal_);
    // The portable transaction performs Preflight again; preserve that API
    // without re-reading mutable native state by using a forwarding wrapper.
    struct Prepared {
        Transaction& t;
        bool Preflight() noexcept { return true; }
        bool Capture() noexcept { return t.Capture(); }
        bool Produce() noexcept { return t.Produce(); }
        bool Bind() noexcept { return t.Bind(); }
        bool Restore() noexcept { return t.Restore(); }
        std::int32_t Native() noexcept { return t.Native(); }
        std::int32_t Replace() noexcept { return t.Replace(); }
        void Quarantine() noexcept { t.Quarantine(); }
    } prepared{transaction};
    return ExecuteProduction(true, prepared);
}
} // namespace wxl::water::slot3
#endif
