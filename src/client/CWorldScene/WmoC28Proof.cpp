// R8 Step 11B-02B2A. Observation only; no shader/constant/render-state setters.
// The only process writes install forwarding dispatch taps while explicitly enabled.
// The exact timing result remains LIVE-UNQUALIFIED until a capture is audited.
// GPL-3.0-or-later.
#include "client/CWorldScene/WmoC28Proof.hpp"
#include "common/Log.hpp"
#include "engine/events/EventScript.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Shader.hpp"
#include "water/WaterDiagCore.hpp"
#include <windows.h>
#include <d3d9.h>
#include <array>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace wxl::r8::materials
{
namespace
{
namespace gx = wxl::offsets::engine::gx;
namespace sh = wxl::offsets::engine::shader;
namespace diag = wxl::waterdiag;
namespace ev = wxl::events;

constexpr unsigned kMaxIdentities = 64;
constexpr unsigned kSamplesPerIdentity = 8;
constexpr unsigned kFrameGap = 60;
constexpr unsigned kDrawsPerBind = 2;
bool enabled = false, quarantined = false;
DWORD renderThread = 0;
std::uint64_t frame = 1, generation = 1, bindSerial = 0, scopeSerial = 0;
unsigned rejected = 0;

struct Snapshot
{
    std::array<c28::Constant, 6> constants{};
    std::array<std::array<float, 4>, 6> cache{};
    std::array<bool, 6> cacheValid{};
    std::array<std::uint32_t, 4> dirty{}; // VS start/end, PS start/end
    bool dirtyValid = false;
    std::string vs, ps;
};
struct Pending
{
    bool active = false;
    std::uint64_t bind = 0, scope = 0, epoch = 0;
    std::uint32_t collection = 0;
    unsigned vtx = 0, pix = 0, draws = 0;
    WmoFamily family = WmoFamily::Unknown;
    IDirect3DDevice9* device = nullptr; // borrowed, never retained across a scope/reset
    std::string vs, raw, ps;
};
thread_local Pending pending{};
thread_local unsigned scopeDepth = 0;
thread_local std::array<std::uint64_t, 16> scopes{};
thread_local bool inspecting = false;

struct SampleBudget
{
    std::string identity;
    std::uint64_t lastFrame = 0;
    unsigned samples = 0;
};
std::array<SampleBudget, kMaxIdentities> budgets{};
unsigned budgetCount = 0;

bool ReadMemory(std::uintptr_t address, void* output, std::size_t bytes)
{
    SIZE_T copied = 0;
    return address && output && bytes &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, bytes, &copied) && copied == bytes;
}
template<class T> bool Read(std::uintptr_t address, T& out)
{
    return ReadMemory(address, &out, sizeof(out));
}
IDirect3DDevice9* Device()
{
    std::uint32_t graphics = 0, device = 0;
    if (!Read(gx::kGxDevicePtr, graphics) || !graphics || !Read(graphics + gx::kD3DDeviceField, device)) return nullptr;
    return reinterpret_cast<IDirect3DDevice9*>(static_cast<std::uintptr_t>(device));
}
std::ostringstream Json()
{
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(9);
    return s;
}
void Emit(const std::string& s)
{
    // common/Log.cpp has a 1024-byte body. Never silently truncate JSON evidence.
    if (s.size() > 980) throw 1;
    WLOG_INFO("r8-step11-c28: %s", s.c_str());
}
void Failure() noexcept
{
    pending.active = false;
    quarantined = true;
    WLOG_ERROR("r8-step11-c28: {\"event\":\"quarantine\",\"mutation\":false}");
}
void Reject(const char* reason)
{
    pending.active = false;
    ++rejected;
    if (rejected <= 32)
        Emit("{\"event\":\"reject\",\"reason\":" + diag::Quote(reason) + "}");
}
bool Admit(const std::string& key)
{
    unsigned i = 0;
    for (; i < budgetCount; ++i) if (budgets[i].identity == key) break;
    if (i == budgetCount)
    {
        if (budgetCount == kMaxIdentities) return false;
        budgets[i].identity = key;
        ++budgetCount;
    }
    auto& b = budgets[i];
    if (b.samples == kSamplesPerIdentity || (b.samples && (frame < b.lastFrame || frame - b.lastFrame < kFrameGap))) return false;
    ++b.samples;
    b.lastFrame = frame;
    return true;
}
template<class T> struct Com
{
    T* value = nullptr;
    ~Com() { if (value) value->Release(); }
};
template<class T> std::string ShaderHash(T* shader, std::uint32_t version)
{
    UINT bytes = 0;
    if (!shader || FAILED(shader->GetFunction(nullptr, &bytes)) || bytes < 4 || bytes > 0x20000) return "UNAVAILABLE";
    std::vector<std::uint8_t> data(bytes);
    UINT actual = bytes;
    if (FAILED(shader->GetFunction(data.data(), &actual)) || actual != bytes) return "UNAVAILABLE";
    std::uint32_t found = 0;
    std::memcpy(&found, data.data(), sizeof(found));
    if (found != version) return "UNAVAILABLE";
    // No pointer-keyed cache: object addresses may be recycled after reset/reload.
    return diag::Hex(diag::Sha256::Of(data.data(), data.size()));
}
Snapshot Capture(IDirect3DDevice9* device)
{
    Snapshot s;
    s.constants = c28::ReadConstants(*device);
    for (unsigned i = 0; i < 6; ++i)
        s.cacheValid[i] = ReadMemory((i == 5 ? gx::kPsConstCache : gx::kVsConstCache) + c28::kRegisters[i] * 16,
                                     s.cache[i].data(), 16);
    s.dirtyValid = Read(gx::kVsDirtyRegStart, s.dirty[0]) && Read(gx::kVsDirtyRegEnd, s.dirty[1]) &&
                   Read(gx::kPsDirtyRegStart, s.dirty[2]) && Read(gx::kPsDirtyRegEnd, s.dirty[3]);
    Com<IDirect3DVertexShader9> vs;
    Com<IDirect3DPixelShader9> ps;
    const HRESULT vsHr = device->GetVertexShader(&vs.value);
    const HRESULT psHr = device->GetPixelShader(&ps.value);
    s.vs = SUCCEEDED(vsHr) ? ShaderHash(vs.value, 0xfffe0300u) : "UNAVAILABLE";
    s.ps = SUCCEEDED(psHr) ? ShaderHash(ps.value, 0xffff0300u) : "UNAVAILABLE";
    return s;
}
std::string Bits(const std::array<float, 4>& value)
{
    auto s = Json(); s << '[';
    for (unsigned i = 0; i < 4; ++i)
    {
        std::uint32_t bits = 0; std::memcpy(&bits, &value[i], 4);
        if (i) s << ',';
        s << bits;
    }
    s << ']'; return s.str();
}
void LogSnapshot(const Pending& p, unsigned draw, const char* phase, const Snapshot& snapshot)
{
    auto s = Json();
    s << "{\"event\":\"snapshot\",\"bind\":" << p.bind << ",\"draw\":" << draw
      << ",\"phase\":" << diag::Quote(phase) << ",\"vs\":" << diag::Quote(snapshot.vs)
      << ",\"ps\":" << diag::Quote(snapshot.ps) << ",\"dirty_valid\":" << (snapshot.dirtyValid ? "true" : "false")
      << ",\"dirty\":[" << snapshot.dirty[0] << ',' << snapshot.dirty[1] << ',' << snapshot.dirty[2] << ',' << snapshot.dirty[3] << "]}";
    Emit(s.str());
    for (unsigned i = 0; i < 6; ++i)
    {
        const auto& c = snapshot.constants[i];
        auto r = Json();
        r << "{\"event\":\"constant\",\"bind\":" << p.bind << ",\"draw\":" << draw
          << ",\"phase\":" << diag::Quote(phase) << ",\"stage\":\"" << (i == 5 ? "ps" : "vs")
          << "\",\"reg\":" << c28::kRegisters[i] << ",\"hr\":" << c.hr << ",\"valid\":" << (c.Valid() ? "true" : "false")
          << ",\"bits\":" << (c.hr >= 0 ? Bits(c.value) : "null") << ",\"value\":";
        if (c.Valid()) r << '[' << c.value[0] << ',' << c.value[1] << ',' << c.value[2] << ',' << c.value[3] << ']';
        else r << "null";
        r << ",\"cache_bits\":" << (snapshot.cacheValid[i] ? Bits(snapshot.cache[i]) : "null") << '}';
        Emit(r.str());
    }
}

using DP = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DIP = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DPUP = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DIPUP = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
struct Chain { void** table = nullptr; DP dp = nullptr; DIP dip = nullptr; DPUP dpup = nullptr; DIPUP dipup = nullptr; bool complete = false; };
std::array<Chain, 8> chains{};
Chain* Find(IDirect3DDevice9* device)
{
    void** table = *reinterpret_cast<void***>(device);
    for (auto& c : chains) if (c.table == table) return &c;
    return nullptr;
}
template<class Forward> HRESULT Draw(IDirect3DDevice9* device, const char* api, Forward forward)
{
    // Any unscoped call (including M2) follows its original chain without getters.
    if (!enabled || quarantined || inspecting || !scopeDepth || GetCurrentThreadId() != renderThread || !pending.active)
        return forward();
    diag::InspectionScope inspection(inspecting);
    Pending ticket;
    unsigned ordinal = 0;
    return c28::ObserveDraw([&]() {
        std::uint32_t active = 0;
        if (pending.epoch != generation || pending.device != device || scopeDepth > scopes.size() ||
            pending.scope != scopes[scopeDepth - 1] || !Read(sh::kActiveCollection, active) || active != pending.collection)
        { Reject("scope_device_or_collection_changed"); return false; }
        ticket = pending;
        ordinal = ++pending.draws;
        if (ordinal == kDrawsPerBind) pending.active = false;
        Snapshot before = Capture(device);
        if (before.vs != ticket.vs || before.ps != ticket.ps)
        { Reject("draw_shader_identity_mismatch"); return false; }
        LogSnapshot(ticket, ordinal, "pre_draw", before);
        Emit("{\"event\":\"draw_begin\",\"bind\":" + std::to_string(ticket.bind) + ",\"draw\":" +
             std::to_string(ordinal) + ",\"api\":" + diag::Quote(api) + "}");
        return true;
    }, forward, [&](HRESULT hr) {
        // Read again after the existing downstream chain; no restore is needed.
        Snapshot after = Capture(device);
        LogSnapshot(ticket, ordinal, "post_draw", after);
        Emit("{\"event\":\"draw_end\",\"bind\":" + std::to_string(ticket.bind) + ",\"draw\":" +
             std::to_string(ordinal) + ",\"hr\":" + std::to_string(static_cast<std::int32_t>(hr)) +
             ",\"shader_match\":" + (after.vs == ticket.vs && after.ps == ticket.ps ? "true" : "false") + "}");
    }, Failure);
}
HRESULT WINAPI HookDP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT n)
{
    Chain* c = Find(d); if (!c || !c->dp) { Failure(); return D3DERR_INVALIDCALL; }
    return Draw(d, "DrawPrimitive", [&] { return c->dp(d, t, start, n); });
}
HRESULT WINAPI HookDIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT min, UINT vertices, UINT start, UINT n)
{
    Chain* c = Find(d); if (!c || !c->dip) { Failure(); return D3DERR_INVALIDCALL; }
    return Draw(d, "DrawIndexedPrimitive", [&] { return c->dip(d, t, base, min, vertices, start, n); });
}
HRESULT WINAPI HookDPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* data, UINT stride)
{
    Chain* c = Find(d); if (!c || !c->dpup) { Failure(); return D3DERR_INVALIDCALL; }
    return Draw(d, "DrawPrimitiveUP", [&] { return c->dpup(d, t, n, data, stride); });
}
HRESULT WINAPI HookDIPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT min, UINT vertices, UINT n,
                       const void* indices, D3DFORMAT fmt, const void* data, UINT stride)
{
    Chain* c = Find(d); if (!c || !c->dipup) { Failure(); return D3DERR_INVALIDCALL; }
    return Draw(d, "DrawIndexedPrimitiveUP", [&] { return c->dipup(d, t, min, vertices, n, indices, fmt, data, stride); });
}
template<class Fn> bool Tap(void** table, unsigned slot, Fn hook, Fn& previous)
{
    if (table[slot] == reinterpret_cast<void*>(hook)) return previous != nullptr;
    DWORD old = 0;
    if (!table[slot] || !VirtualProtect(&table[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    // Publish the forwarding target before exposing the hook to another caller.
    previous = reinterpret_cast<Fn>(table[slot]);
    InterlockedExchangePointer(&table[slot], reinterpret_cast<void*>(hook));
    DWORD ignored = 0;
    return VirtualProtect(&table[slot], sizeof(void*), old, &ignored) != FALSE;
}
bool EnsureTaps(IDirect3DDevice9* device)
{
    if (auto* found = Find(device)) return found->complete;
    Chain* c = nullptr;
    for (auto& entry : chains) if (!entry.table) { c = &entry; break; }
    if (!c) return false;
    c->table = *reinterpret_cast<void***>(device);
    bool ok = Tap(c->table, gx::vt::kDrawPrimitive, &HookDP, c->dp);
    ok &= Tap(c->table, gx::vt::kDrawIndexedPrimitive, &HookDIP, c->dip);
    ok &= Tap(c->table, gx::vt::kDrawPrimitiveUP, &HookDPUP, c->dpup);
    ok &= Tap(c->table, gx::vt::kDrawIndexedPrimitiveUP, &HookDIPUP, c->dipup);
    c->complete = ok;
    // Never rewrap a known table: another owner's later wrapper may already call us.
    auto s = Json();
    s << "{\"event\":\"device_taps\",\"complete\":" << (ok ? "true" : "false")
      << ",\"table\":" << reinterpret_cast<std::uintptr_t>(c->table)
      << ",\"downstream_dip\":" << reinterpret_cast<std::uintptr_t>(c->dip)
      << ",\"boundary\":\"d3d9_dispatch_chain\",\"mutation\":false}";
    Emit(s.str());
    return ok;
}
class ProofEvents final : public ev::EventScript
{
public:
    ProofEvents()
    {
        on<&ProofEvents::EndFrame>(ev::Event::OnEndScene);
        on<&ProofEvents::Lost>(ev::Event::OnDeviceLost);
        on<&ProofEvents::Reset>(ev::Event::OnDeviceReset);
    }
private:
    void EndFrame(const ev::EndSceneArgs&)
    {
        pending.active = false;
        ++frame;
    }
    void Lost(const ev::DeviceResetArgs&)
    {
        pending.active = false;
        ++generation;
        WLOG_INFO("r8-step11-c28: {\"event\":\"device_lost\"}");
    }
    void Reset(const ev::DeviceResetArgs&)
    {
        pending.active = false;
        WLOG_INFO("r8-step11-c28: {\"event\":\"device_reset\"}");
    }
};
}

void InitializeWmoC28Proof()
{
    static ProofEvents events; // constructed only through the validated opt-in path
    enabled = true;
    WLOG_INFO("r8-step11-c28: {\"event\":\"startup\",\"step\":\"R8_STEP11B02B2A\",\"read_only\":true,\"shader_substitution\":false,\"constant_mutation\":false,\"gx_device_draw_owner\":false,\"m2_changed\":false,\"live_qualified\":false}");
}
void EnterWmoC28ProofScope(std::uintptr_t) noexcept
{
    if (!enabled || quarantined) return;
    pending.active = false;
    if (!renderThread) renderThread = GetCurrentThreadId();
    if (GetCurrentThreadId() != renderThread) return;
    ++scopeDepth;
    if (scopeDepth > scopes.size()) { Failure(); return; }
    scopes[scopeDepth - 1] = ++scopeSerial;
}
void LeaveWmoC28ProofScope() noexcept
{
    pending.active = false;
    if (scopeDepth) --scopeDepth;
}
void InvalidateWmoC28Proof() noexcept { pending.active = false; }
void ObserveWmoC28Bind(WmoFamily family, const char* path, unsigned vtx, unsigned pix,
                       std::uint32_t collection, const char* vs, const char* raw, const char* ps) noexcept
{
    if (!enabled || quarantined || inspecting || !scopeDepth || GetCurrentThreadId() != renderThread) return;
    diag::InspectionScope inspection(inspecting);
    try
    {
        pending.active = false;
        if (!vs || !raw || !ps || !collection || !c28::Qualified(family, vtx, pix, vs, raw, ps)) return;
        IDirect3DDevice9* device = Device();
        if (!device || !EnsureTaps(device)) { Failure(); return; }
        std::string safePath = path ? path : "UNKNOWN";
        // Keep path JSON safely below the core logger's 1024-byte limit even for
        // pathological control characters. Ordinary WMO asset paths are ASCII.
        if (safePath.size() > 260) safePath = "UNAVAILABLE";
        for (char& x : safePath) if (static_cast<unsigned char>(x) < 32) x = '?';
        const std::string key = std::to_string(static_cast<int>(family)) + ':' + std::to_string(vtx) + ':' +
                                std::to_string(pix) + ':' + vs + ':' + raw + ':' + ps + ':' + safePath;
        if (!Admit(key)) return;
        Pending p;
        p.bind = ++bindSerial; p.scope = scopes[scopeDepth - 1]; p.epoch = generation;
        p.collection = collection; p.vtx = vtx; p.pix = pix; p.family = family;
        p.vs = vs; p.raw = raw; p.ps = ps; p.device = device;
        auto s = Json();
        s << "{\"event\":\"identity\",\"bind\":" << p.bind << ",\"scope\":" << p.scope << ",\"frame\":" << frame
          << ",\"generation\":" << generation << ",\"family\":\"" << (family == WmoFamily::Diffuse ? "Diffuse" : "Opaque")
          << "\",\"vtx\":" << vtx << ",\"pix\":" << pix << ",\"paired_raw_vtx\":" << (vtx & ~1u)
          << ",\"selected_vs\":" << diag::Quote(vs) << ",\"raw_vs\":" << diag::Quote(raw)
          << ",\"ps\":" << diag::Quote(ps) << ",\"collection\":" << collection << '}';
        Emit(s.str());
        Emit("{\"event\":\"path\",\"bind\":" + std::to_string(p.bind) + ",\"path\":" + diag::Quote(safePath) + "}");
        LogSnapshot(p, 0, "post_bind", Capture(device));
        p.active = true;
        pending = std::move(p);
    }
    catch (...) { Failure(); }
}
}
