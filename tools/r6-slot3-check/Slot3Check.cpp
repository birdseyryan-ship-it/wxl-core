#include "water/Slot3Core.hpp"
#include "water/Slot3Constants.hpp"
#include "water/Slot3DepthRuntime.hpp"
#include "water/Slot3Shaders.hpp"
#include "water/Slot3SnapshotView.hpp"
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
using namespace wxl::water::slot3;
namespace wd = wxl::waterdiag;
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (false)
struct Backend {
    bool prepare = true, capture = true, bind = true, restore = true;
    unsigned natives = 0, replacements = 0, restores = 0, quarantines = 0;
    bool mutated = false, nativeSawMutation = false;
    int32_t replacementResult = 7;
    bool Prepare() noexcept { return prepare; }
    bool Capture() noexcept { return capture; }
    bool Bind() noexcept { mutated = true; return bind; }
    bool Restore() noexcept { ++restores; if (restore) mutated = false; return restore; }
    int32_t Native() noexcept { ++natives; nativeSawMutation = mutated; return 3; }
    int32_t Replace() noexcept { ++replacements; return replacementResult; }
    void Quarantine() noexcept { ++quarantines; }
};
Prerequisites Ready() {
    Prerequisites p;
    p.profile = {wd::Family::Water, wd::Provider::Terrain, 1, wd::kR6BaseWaterVs,
        wd::kR6BaseWaterPs, wd::kR6BaseWaterDecl, wd::kR6WaterMaterialP01, 5};
    p.streams = 1; p.stride = 44; p.selector = 5; p.reflectionBranch = 1;
    p.draw = {1, 2, 1, 1, 1, 20}; p.colour = p.depth = p.reflection = {1, 2, 1, 1, 1, 10};
    p.colourReady = p.depthReady = p.reflectionReady = p.exactNormalReady = p.exactChopReady = true;
    p.constantsReady = p.shadersReady = p.aboveWater = p.volumeFogDisabled = true;
    p.deviceLost = false; p.producedReflectionMode = 1; p.projection = {1.01f, -1.01f, true};
    return p;
}
int main() {
    std::map<std::string, std::string> env;
    auto get = [&](const char* k)->const char* { auto i=env.find(k); return i==env.end()?nullptr:i->second.c_str(); };
    CHECK(!ParseConfig(get).enabled);
    for (auto value : {"", "true", " 1", "01", "2", "1 "}) {
        env["WXL_CLASSIC_WATER_SLOT3"] = value; CHECK(!ParseConfig(get).enabled);
    }
    env["WXL_CLASSIC_WATER_SLOT3"] = "1"; CHECK(ParseConfig(get).enabled);
    for (auto value : {"0", "4", "-1", "nan", "999999999999999"}) {
        env["WXL_CLASSIC_WATER_SLOT3_REFLECTION_MODE"] = value; CHECK(!ParseConfig(get).enabled);
    }
    env.erase("WXL_CLASSIC_WATER_SLOT3_REFLECTION_MODE");
    const Config config=ParseConfig(get); const auto ready=Ready();
    CHECK(Evaluate(config, ready)==Rejection::None);
    CHECK(Evaluate({}, ready)==Rejection::Disabled);
    for (unsigned h=0; h<4; ++h) for (unsigned byte=0; byte<32; ++byte) {
        auto p=ready; wd::Digest* hashes[]={&p.profile.vs,&p.profile.ps,&p.profile.declaration,&p.profile.material};
        (*hashes[h])[byte]^=1; CHECK(Evaluate(config,p)==Rejection::Profile);
    }
    for (auto family : {wd::Family::Unknown,wd::Family::WaterNoSpec,wd::Family::ProcWater,wd::Family::Magma}) {
        auto p=ready;p.profile.family=family;CHECK(Evaluate(config,p)==Rejection::Profile);
    }
    for (auto provider : {wd::Provider::Unknown,wd::Provider::Wmo}) {
        auto p=ready;p.profile.provider=provider;CHECK(Evaluate(config,p)==Rejection::Profile);
    }
    {auto p=ready;p.profile.material=wd::kR6WaterMaterialP02P03;CHECK(Evaluate(config,p)==Rejection::Profile);}
    for (unsigned field=0;field<6;++field) {
        auto p=ready;
        switch(field){case 0:++p.depth.generation;break;case 1:++p.depth.frame;break;
            case 2:++p.depth.scene;break;case 3:++p.depth.sourceRt;break;
            case 4:++p.depth.device;break;case 5:p.depth.ordinal=p.draw.ordinal;break;}
        CHECK(Evaluate(config,p)==Rejection::Depth);
    }
    for (auto member : {&Prerequisites::colourReady,&Prerequisites::depthReady,&Prerequisites::reflectionReady,
        &Prerequisites::exactNormalReady,&Prerequisites::exactChopReady,&Prerequisites::constantsReady,
        &Prerequisites::shadersReady,&Prerequisites::aboveWater,&Prerequisites::volumeFogDisabled}) {
        auto p=ready;p.*member=false;CHECK(Evaluate(config,p)!=Rejection::None);
    }
    {auto p=ready;p.selector=std::numeric_limits<float>::quiet_NaN();CHECK(Evaluate(config,p)==Rejection::Selector);}
    {auto p=ready;p.rippleQuality=1;CHECK(Evaluate(config,p)==Rejection::Permutation);}
    {auto p=ready;p.areaExtras=1;CHECK(Evaluate(config,p)==Rejection::Permutation);}
    {auto p=ready;p.reflectionBranch=2;CHECK(Evaluate(config,p)==Rejection::Permutation);}
    {auto p=ready;p.inReflection=true;CHECK(Evaluate(config,p)==Rejection::ReflectionRecursion);}
    {auto p=ready;p.reflectionPlane=1;CHECK(Evaluate(config,p)==Rejection::Reflection);}
    {auto p=ready;p.stride=40;CHECK(Evaluate(config,p)==Rejection::Geometry);}
    // Inject every Prepare/Capture/Bind failure; native must see restored state.
    for (unsigned which=0;which<3;++which) {
        Backend b;if(which==0)b.prepare=false;if(which==1)b.capture=false;if(which==2)b.bind=false;
        const auto result=Execute(Rejection::None,b);
        CHECK(result.submission==Submission::Native);CHECK(result.result==3);
        CHECK(b.natives==1 && b.replacements==0 && !b.nativeSawMutation);
    }
    for (int32_t result : {7, -1}) { // HRESULT failure still forbids duplicate fallback.
        Backend b;b.replacementResult=result;const auto r=Execute(Rejection::None,b);
        CHECK(r.result==result);CHECK(r.submission==Submission::Replacement);
        CHECK(b.replacements==1 && b.natives==0 && b.restores==1 && !b.mutated);
    }
    {Backend b;b.bind=false;b.restore=false;const auto r=Execute(Rejection::None,b);
        CHECK(r.submission==Submission::SuppressedAfterRestoreFailure);CHECK(b.natives==0 && b.replacements==0 && b.quarantines==1);}
    {Backend b;b.restore=false;const auto r=Execute(Rejection::None,b);
        CHECK(!r.restoreOk);CHECK(b.natives==0 && b.replacements==1 && b.quarantines==1);}
    {Backend b;Execute(Rejection::Disabled,b);CHECK(b.natives==1 && b.replacements==0 && b.restores==0);}
    bool inReflection=false;
    {ReflectionGuard first(inReflection);CHECK(first.entered && inReflection);
        {ReflectionGuard nested(inReflection);CHECK(!nested.entered && inReflection);}
        CHECK(inReflection);}
    CHECK(!inReflection);
    float matrix[16]={};matrix[0]=matrix[5]=1;matrix[10]=1.01f;matrix[11]=1;matrix[14]=-1.01f;
    auto projection=DepthProjection::FromWorldProjection(matrix);CHECK(projection.valid);
    for (float expected : {1.f,2.f,5.f,10.f,50.f,100.f}) {
        const float d=matrix[10]+matrix[14]/expected;float actual=0;
        CHECK(projection.Reconstruct(d,actual));CHECK(std::abs(actual-expected)<expected*0.00001f);
    }
    float untouched=123;CHECK(!projection.Reconstruct(std::numeric_limits<float>::quiet_NaN(),untouched));CHECK(untouched==123);
    matrix[11]=0;CHECK(!DepthProjection::FromWorldProjection(matrix).valid);
    CHECK(kSelector5[0][1]==10000000.f && kSelector5[5][3]==10000000.f);
    CHECK(kSelector5[4][0]==1.f && kSelector5[7][2]==1.f);

    // Typed Classic constant packet is fail-closed. No missing CB1 row is
    // silently zero-filled and the selector-5 bank is always injected exactly.
    ConstantInputs ci;
    auto tagged = [](unsigned row, float base) {
        return Float4{{base + float(row), base + float(row) + 0.25f,
            base + float(row) + 0.5f, base + float(row) + 0.75f}};
    };
    for (unsigned row : kRequiredVsRows) CHECK(ci.SetVs(row, tagged(row, 100.f)));
    for (unsigned row : kRequiredPsCb1Rows) CHECK(ci.SetPs(row, tagged(row, 200.f)));
    ConstantPacket packet;
    CHECK(BuildConstantPacket(ci, packet));
    CHECK(packet.valid && ExactSelector5Bank(packet));
    for (unsigned row : kRequiredVsRows) CHECK(packet.vs[row] == tagged(row, 100.f));
    for (unsigned row : kRequiredPsCb1Rows) CHECK(packet.ps[row] == tagged(row, 200.f));
    for (unsigned i = 0; i < kSelector5.size(); ++i) CHECK(packet.ps[40 + i] == kSelector5[i]);
    { auto bad = ci; bad.vsReady[kRequiredVsRows.front()] = false;
      ConstantPacket untouched; untouched.valid = true;
      CHECK(!BuildConstantPacket(bad, untouched)); CHECK(untouched.valid); }
    { auto bad = ci; bad.psReady[kRequiredPsCb1Rows.back()] = false;
      ConstantPacket untouched; CHECK(!BuildConstantPacket(bad, untouched)); CHECK(!untouched.valid); }
    { auto bad = ci; bad.ps[kRequiredPsCb1Rows.front()][0] = std::numeric_limits<float>::quiet_NaN();
      ConstantPacket untouched; CHECK(!BuildConstantPacket(bad, untouched)); }
    const Float4 packetZero{{0.f,0.f,0.f,0.f}};
    CHECK(!ci.SetVs(kVsRegisterCount, packetZero));
    CHECK(!ci.SetPs(kPsRegisterCount, packetZero));

    // CP09 transport exposure is metadata-gated and explicitly distinguishes
    // raw INTZ from the future Classic-compatible linear-depth resource.
    wxl::waterdiag::PreWaterSnapshotView snapshot{};
    snapshot.colour = reinterpret_cast<IDirect3DTexture9*>(uintptr_t(1));
    snapshot.rawDepthIntz = reinterpret_cast<IDirect3DTexture9*>(uintptr_t(2));
    snapshot.device = 3; snapshot.sourceRt = 4; snapshot.generation = 5;
    snapshot.scene = 6; snapshot.frame = 7; snapshot.colourProducerOrdinal = 8;
    snapshot.depthProducerOrdinal = 9; snapshot.width = 3840; snapshot.height = 2160;
    CHECK(wxl::waterdiag::SnapshotMetadataCompatible(snapshot, 3, 4, 10));
    CHECK(!wxl::waterdiag::SnapshotMetadataCompatible(snapshot, 30, 4, 10));
    CHECK(!wxl::waterdiag::SnapshotMetadataCompatible(snapshot, 3, 40, 10));
    CHECK(!wxl::waterdiag::SnapshotMetadataCompatible(snapshot, 3, 4, 9));
    { auto bad = snapshot; bad.rawDepthIntz = nullptr;
      CHECK(!wxl::waterdiag::SnapshotMetadataCompatible(bad, 3, 4, 10)); }

    // The pre-water raw INTZ snapshot may only become Classic s6 after an
    // exact frozen-R3 depth reconstruction into linear pre-projection view-Z.
    wxl::waterdiag::PreWaterSnapshotView depthSnapshot = snapshot;
    depthSnapshot.depthFormat = kIntzFormat;

    const DepthProjection depthProjection{
        1.01f,
        -1.01f,
        true
    };

    LinearDepthKey depthKey{};

    CHECK(
        BuildLinearDepthKey(
            depthSnapshot,
            3,
            4,
            10,
            depthProjection,
            depthKey));

    CHECK(
        depthKey.device == 3 &&
        depthKey.sourceRt == 4);

    CHECK(
        depthKey.rawDepth ==
        uintptr_t(2));

    CHECK(
        depthKey.generation == 5 &&
        depthKey.scene == 6 &&
        depthKey.frame == 7);

    CHECK(
        depthKey.sourceDepthOrdinal == 9);

    CHECK(
        depthKey.width == 3840 &&
        depthKey.height == 2160);

    CHECK(
        depthKey.a == depthProjection.a &&
        depthKey.b == depthProjection.b);

    {
        auto bad = depthSnapshot;
        bad.depthFormat = 0;

        LinearDepthKey untouchedKey =
            depthKey;

        CHECK(
            !BuildLinearDepthKey(
                bad,
                3,
                4,
                10,
                depthProjection,
                untouchedKey));

        CHECK(
            SameLinearDepthContent(
                untouchedKey,
                depthKey));
    }

    {
        auto badProjection =
            depthProjection;

        badProjection.valid =
            false;

        LinearDepthKey untouchedKey =
            depthKey;

        CHECK(
            !BuildLinearDepthKey(
                depthSnapshot,
                3,
                4,
                10,
                badProjection,
                untouchedKey));

        CHECK(
            SameLinearDepthContent(
                untouchedKey,
                depthKey));
    }

    {
        auto badProjection =
            depthProjection;

        badProjection.b = 0.f;

        LinearDepthKey untouchedKey{};

        CHECK(
            !BuildLinearDepthKey(
                depthSnapshot,
                3,
                4,
                10,
                badProjection,
                untouchedKey));
    }

    {
        auto badProjection =
            depthProjection;

        badProjection.a =
            std::numeric_limits<float>::
                quiet_NaN();

        LinearDepthKey untouchedKey{};

        CHECK(
            !BuildLinearDepthKey(
                depthSnapshot,
                3,
                4,
                10,
                badProjection,
                untouchedKey));
    }

    {
        LinearDepthKey untouchedKey{};

        CHECK(
            !BuildLinearDepthKey(
                depthSnapshot,
                30,
                4,
                10,
                depthProjection,
                untouchedKey));
    }

    {
        LinearDepthKey untouchedKey{};

        CHECK(
            !BuildLinearDepthKey(
                depthSnapshot,
                3,
                40,
                10,
                depthProjection,
                untouchedKey));
    }

    {
        LinearDepthKey untouchedKey{};

        CHECK(
            !BuildLinearDepthKey(
                depthSnapshot,
                3,
                4,
                9,
                depthProjection,
                untouchedKey));
    }

    LinearDepthKey same = depthKey;

    CHECK(
        SameLinearDepthResources(
            depthKey,
            same));

    CHECK(
        SameLinearDepthContent(
            depthKey,
            same));

    ++same.frame;

    CHECK(
        SameLinearDepthResources(
            depthKey,
            same));

    CHECK(
        !SameLinearDepthContent(
            depthKey,
            same));

    same = depthKey;
    ++same.generation;

    CHECK(
        SameLinearDepthResources(
            depthKey,
            same));

    CHECK(
        !SameLinearDepthContent(
            depthKey,
            same));

    same = depthKey;
    ++same.rawDepth;

    CHECK(
        SameLinearDepthResources(
            depthKey,
            same));

    CHECK(
        !SameLinearDepthContent(
            depthKey,
            same));

    same = depthKey;
    same.a += 0.001f;

    CHECK(
        SameLinearDepthResources(
            depthKey,
            same));

    CHECK(
        !SameLinearDepthContent(
            depthKey,
            same));

    same = depthKey;
    ++same.width;

    CHECK(
        !SameLinearDepthResources(
            depthKey,
            same));

    CHECK(
        !SameLinearDepthContent(
            depthKey,
            same));

    same = depthKey;
    ++same.device;

    CHECK(
        !SameLinearDepthResources(
            depthKey,
            same));

    CHECK(
        !SameLinearDepthContent(
            depthKey,
            same));

    CHECK(
        DepthProduceStatus::Ready !=
        DepthProduceStatus::Unavailable);

    CHECK(
        DepthProduceStatus::RestoreFailed !=
        DepthProduceStatus::Unavailable);

    LinearDepthLifecycleState lifecycle{};

    CHECK(!lifecycle.resourcesReady);
    CHECK(!lifecycle.contentReady);
    CHECK(!lifecycle.ResourcesMatch(depthKey));
    CHECK(!lifecycle.ContentMatches(depthKey));

    lifecycle.MarkResourcesReady(depthKey);

    CHECK(lifecycle.resourcesReady);
    CHECK(!lifecycle.contentReady);
    CHECK(lifecycle.ResourcesMatch(depthKey));
    CHECK(!lifecycle.ContentMatches(depthKey));

    CHECK(
        lifecycle.MarkContentReady(
            depthKey));

    CHECK(
        lifecycle.ContentMatches(
            depthKey));

    auto nextFrameDepthKey =
        depthKey;

    ++nextFrameDepthKey.frame;

    CHECK(
        lifecycle.ResourcesMatch(
            nextFrameDepthKey));

    CHECK(
        !lifecycle.ContentMatches(
            nextFrameDepthKey));

    lifecycle.InvalidateContent();

    CHECK(
        lifecycle.ResourcesMatch(
            depthKey));

    CHECK(
        !lifecycle.ContentMatches(
            depthKey));

    CHECK(
        lifecycle.MarkContentReady(
            nextFrameDepthKey));

    CHECK(
        lifecycle.ContentMatches(
            nextFrameDepthKey));

    auto resizedDepthKey =
        nextFrameDepthKey;

    ++resizedDepthKey.width;

    CHECK(
        !lifecycle.ResourcesMatch(
            resizedDepthKey));

    CHECK(
        !lifecycle.MarkContentReady(
            resizedDepthKey));

    CHECK(
        lifecycle.ContentMatches(
            nextFrameDepthKey));

    lifecycle.MarkResourcesReady(
        resizedDepthKey);

    CHECK(
        lifecycle.ResourcesMatch(
            resizedDepthKey));

    CHECK(!lifecycle.contentReady);

    CHECK(
        !lifecycle.ContentMatches(
            nextFrameDepthKey));

    lifecycle.Reset();

    CHECK(!lifecycle.resourcesReady);
    CHECK(!lifecycle.contentReady);

    CHECK(
        !lifecycle.ResourcesMatch(
            resizedDepthKey));

    CHECK(
        !lifecycle.MarkContentReady(
            resizedDepthKey));

    // The source strings are production inputs to D3DCompile. Freeze the narrow
    // slot-3 ABI here so a future edit cannot silently add an unproven resource
    // or permutation while still satisfying the higher-level gate tests.
    const std::string_view vs = shaders::kVertexHlsl;
    const std::string_view ps = shaders::kPixelHlsl;
    auto has = [](std::string_view source, std::string_view token) {
        return source.find(token) != std::string_view::npos;
    };
    CHECK(has(vs, "float3 position : POSITION0;"));
    CHECK(has(vs, "float4 color    : COLOR0;"));
    CHECK(has(vs, "float2 uv0      : TEXCOORD0;"));
    CHECK(has(vs, "float2 uv1      : TEXCOORD1;"));
    CHECK(has(vs, "float4 C28 : register(c28);"));
    CHECK(has(vs, "o.t6 = r1;"));
    CHECK(has(vs, "o.color = v.color.xyz;"));

    for (auto token : {
        "sampler2D SceneColour : register(s0);",
        "sampler2D Reflection  : register(s1);",
        "sampler2D NormalWave  : register(s5);",
        "sampler2D LinearDepth : register(s6);",
        "sampler2D Chop        : register(s7);",
        "float4 B40 : register(c40);",
        "float4 B42 : register(c42);",
        "float4 B43 : register(c43);",
        "float4 B44 : register(c44);",
        "float4 B45 : register(c45);",
        "float4 B46 : register(c46);",
        "float4 B47 : register(c47);"})
        CHECK(has(ps, token));

    for (auto forbidden : {
        "register(s3)", "register(s4)", "register(s8)",
        "register(s13)", "register(s14)", "register(s15)",
        "sampler3D", "float4 B41 : register(c41);"})
        CHECK(!has(ps, forbidden));

    // Equation anchors from the exact recovered VS0/PS3 bodies. These are not
    // a substitute for Win32 D3DCompile validation; they guard the deliberate
    // equation translation and the two proven specializations.
    CHECK(has(vs, "r0 = v.position.y * C5;"));
    CHECK(has(vs, "r1 = r0.y * C9;"));
    CHECK(has(vs, "r0.x = sin(r0.x);"));
    CHECK(has(vs, "r0.y = cos(r0.y);"));
    CHECK(has(ps, "r1.z = tex2D(LinearDepth, r1.xy).x;"));
    CHECK(has(ps, "r1.z = r1.z - v.v1.z;"));
    CHECK(has(ps, "r9.xyz = tex2D(Reflection, r9.xy).xyz;"));
    CHECK(has(ps, "float3 sceneSample = tex2D(SceneColour, r1.xy).xyz;"));
    CHECK(has(ps, "const float chopScale0 = r0.x;"));
    CHECK(has(ps, "const float heightBlend = saturate((B40.y - distanceToOrigin) / (B40.y - B40.x));"));
    CHECK(has(ps, "outColor.xyz = r0.xyz;"));

    // Selector-5 atmospheric reduction used by the PS translation: B42/B43
    // colour is exactly zero, B44.x is one and B46.x is zero. The surviving
    // term is therefore only the B40 distance blend.
    const Float4 zero4{{0.f,0.f,0.f,0.f}};
    CHECK(kSelector5[2] == zero4);
    CHECK(kSelector5[3] == zero4);
    CHECK(kSelector5[4][0] == 1.f && kSelector5[6][0] == 0.f);
    auto selector5Blend = [](float distance) {
        const float value = (kSelector5[0][1] - distance) /
            (kSelector5[0][1] - kSelector5[0][0]);
        return std::clamp(value, 0.f, 1.f);
    };
    CHECK(selector5Blend(0.f) == 1.f);
    CHECK(std::abs(selector5Blend(5000000.f) - 0.5f) < 1e-7f);
    CHECK(selector5Blend(10000000.f) == 0.f);
    CHECK(selector5Blend(20000000.f) == 0.f);
    std::printf("R6 Slot3: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
