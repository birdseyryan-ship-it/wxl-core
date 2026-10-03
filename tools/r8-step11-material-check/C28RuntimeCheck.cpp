// Executes the actual diagnostic translation unit against fake Win32 memory,
// D3D9 COM vtables, event subscribers and logger. No graphics driver or game.
#include <windows.h>
#include <d3d9.h>
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

static unsigned checks = 0;
static void Check(bool ok) { ++checks; if (!ok) { std::fprintf(stderr, "FAIL runtime check %u\n", checks); std::exit(1); } }
static std::map<std::uintptr_t, std::vector<unsigned char>> memory;
static std::vector<std::string> logs;
static std::string throwLog;
static unsigned protects = 0, failProtectAt = 0;
static BOOL WINAPI FakeReadProcessMemory(HANDLE, LPCVOID address, LPVOID dest, SIZE_T bytes, SIZE_T* copied)
{
    auto found = memory.find(reinterpret_cast<std::uintptr_t>(address));
    if (found == memory.end() || found->second.size() != bytes) { *copied = 0; return FALSE; }
    std::memcpy(dest, found->second.data(), bytes); *copied = bytes; return TRUE;
}
static BOOL WINAPI FakeVirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD old)
{ ++protects; *old = PAGE_READWRITE; return protects == failProtectAt ? FALSE : TRUE; }
static void ProofLog(const char* fmt, ...)
{
    char buffer[4096]; va_list args; va_start(args, fmt); std::vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args);
    if (!throwLog.empty() && std::string(buffer).find(throwLog) != std::string::npos) { throwLog.clear(); throw 9; }
    Check(std::strlen(buffer) < 1024);
    logs.emplace_back(buffer);
}
struct Subscription { wxl::events::Event event; wxl::events::Handler handler; void* user; };
static std::vector<Subscription> subscriptions;
namespace wxl::events { void Subscribe(Event e, Handler h, void* u) { subscriptions.push_back({e,h,u}); } }
#undef WLOG_INFO
#undef WLOG_WARN
#undef WLOG_ERROR
#define WLOG_INFO(...) ProofLog(__VA_ARGS__)
#define WLOG_WARN(...) ProofLog(__VA_ARGS__)
#define WLOG_ERROR(...) ProofLog(__VA_ARGS__)
#define ReadProcessMemory FakeReadProcessMemory
#define VirtualProtect FakeVirtualProtect
#include "client/CWorldScene/WmoC28Proof.cpp"
#undef ReadProcessMemory
#undef VirtualProtect

using namespace wxl::r8::materials;
struct ShaderStub { void** table; std::vector<unsigned char> code; unsigned refs = 0; };
static ULONG WINAPI ReleaseShader(void* ptr) { auto* s = static_cast<ShaderStub*>(ptr); Check(s->refs > 0); return --s->refs; }
static HRESULT WINAPI ShaderBytes(void* ptr, void* data, UINT* bytes)
{
    auto& code = static_cast<ShaderStub*>(ptr)->code;
    if (data) { if (*bytes < code.size()) return D3DERR_INVALIDCALL; std::memcpy(data, code.data(), code.size()); }
    *bytes = static_cast<UINT>(code.size()); return S_OK;
}
static void* shaderTable[5]{nullptr, nullptr, reinterpret_cast<void*>(&ReleaseShader), nullptr, reinterpret_cast<void*>(&ShaderBytes)};
struct DeviceStub
{
    void** table;
    ShaderStub* vs; ShaderStub* ps;
    float vertex[256][4]{}; float pixel[256][4]{};
    unsigned reads = 0, calls = 0;
    int failedReg = -1;
    HRESULT result = S_OK;
    std::vector<std::uintptr_t> arguments;
};
static HRESULT WINAPI ReadVS(IDirect3DDevice9* ptr, UINT r, float* out, UINT n)
{
    auto* d = reinterpret_cast<DeviceStub*>(ptr); ++d->reads; Check(n == 1 && r < 256);
    if (int(r) == d->failedReg) return D3DERR_INVALIDCALL;
    std::memcpy(out, d->vertex[r], 16); return S_OK;
}
static HRESULT WINAPI ReadPS(IDirect3DDevice9* ptr, UINT r, float* out, UINT n)
{
    auto* d = reinterpret_cast<DeviceStub*>(ptr); ++d->reads; Check(n == 1 && r == 31);
    if (int(r) == d->failedReg) return D3DERR_INVALIDCALL;
    std::memcpy(out, d->pixel[r], 16); return S_OK;
}
static HRESULT WINAPI GetVS(IDirect3DDevice9* ptr, IDirect3DVertexShader9** out)
{ auto* s = reinterpret_cast<DeviceStub*>(ptr)->vs; ++s->refs; *out = reinterpret_cast<IDirect3DVertexShader9*>(s); return S_OK; }
static HRESULT WINAPI GetPS(IDirect3DDevice9* ptr, IDirect3DPixelShader9** out)
{ auto* s = reinterpret_cast<DeviceStub*>(ptr)->ps; ++s->refs; *out = reinterpret_cast<IDirect3DPixelShader9*>(s); return S_OK; }
static HRESULT WINAPI OriginalDP(IDirect3DDevice9* ptr, D3DPRIMITIVETYPE t, UINT start, UINT n)
{ auto* d = reinterpret_cast<DeviceStub*>(ptr); ++d->calls; d->arguments = {t,start,n}; return d->result; }
static HRESULT WINAPI OriginalDIP(IDirect3DDevice9* ptr, D3DPRIMITIVETYPE t, INT base, UINT min, UINT vertices, UINT start, UINT n)
{ auto* d = reinterpret_cast<DeviceStub*>(ptr); ++d->calls; d->arguments = {t,static_cast<UINT>(base),min,vertices,start,n}; return d->result; }
static HRESULT WINAPI OriginalDPUP(IDirect3DDevice9* ptr, D3DPRIMITIVETYPE t, UINT n, const void* data, UINT stride)
{ auto* d = reinterpret_cast<DeviceStub*>(ptr); ++d->calls; d->arguments = {t,n,reinterpret_cast<std::uintptr_t>(data),stride}; return d->result; }
static HRESULT WINAPI OriginalDIPUP(IDirect3DDevice9* ptr, D3DPRIMITIVETYPE t, UINT min, UINT vertices, UINT n, const void* indices, D3DFORMAT fmt, const void* data, UINT stride)
{ auto* d = reinterpret_cast<DeviceStub*>(ptr); ++d->calls; d->arguments = {t,min,vertices,n,reinterpret_cast<std::uintptr_t>(indices),fmt,reinterpret_cast<std::uintptr_t>(data),stride}; return d->result; }
template<class T> static void Put(std::uintptr_t address, const T& value)
{ auto* b = reinterpret_cast<const unsigned char*>(&value); memory[address] = std::vector<unsigned char>(b, b + sizeof(value)); }
static std::vector<unsigned char> Load(const std::string& file)
{ std::ifstream stream(file, std::ios::binary); Check(bool(stream)); return {std::istreambuf_iterator<char>(stream), {}}; }
static bool Has(const std::string& text)
{ for (const auto& l : logs) if (l.find(text) != std::string::npos) return true; return false; }
static void EmitEvent(wxl::events::Event event)
{ wxl::events::DeviceResetArgs args{}; for (auto& s : subscriptions) if (s.event == event) s.handler(s.user, &args); }

int main(int argc, char** argv)
{
    Check(argc == 2);
    std::string fixtures = argv[1];
    ShaderStub vs{shaderTable, Load(fixtures + '/' + std::string(c28::kVs30) + ".sm3")};
    ShaderStub ps{shaderTable, Load(fixtures + '/' + std::string(c28::kDiffuse5) + ".sm3")};
    ShaderStub wrong{shaderTable, Load(fixtures + '/' + std::string(c28::kRaw30) + ".sm3")};
    void* table[119]{};
    table[81] = reinterpret_cast<void*>(&OriginalDP); table[82] = reinterpret_cast<void*>(&OriginalDIP);
    table[83] = reinterpret_cast<void*>(&OriginalDPUP); table[84] = reinterpret_cast<void*>(&OriginalDIPUP);
    table[93] = reinterpret_cast<void*>(&GetVS); table[95] = reinterpret_cast<void*>(&ReadVS);
    table[108] = reinterpret_cast<void*>(&GetPS); table[110] = reinterpret_cast<void*>(&ReadPS);
    DeviceStub d{table, &vs, &ps}; auto* dev = reinterpret_cast<IDirect3DDevice9*>(&d);
    for (unsigned r : c28::kRegisters) for (unsigned i = 0; i < 4; ++i) { d.vertex[r][i] = float(r + i); d.pixel[r][i] = -float(r + i); }
    constexpr std::uint32_t collection = 0x12340000, graphics = 0x11000000;
    Put(gx::kGxDevicePtr, graphics); Put(graphics + gx::kD3DDeviceField, reinterpret_cast<std::uintptr_t>(dev));
    Put(sh::kActiveCollection, collection);
    for (unsigned i = 0; i < 6; ++i)
    {
        unsigned r = c28::kRegisters[i];
        if (i == 5) Put(gx::kPsConstCache + r * 16, d.pixel[r]);
        else Put(gx::kVsConstCache + r * 16, d.vertex[r]);
    }
    Put(gx::kVsDirtyRegStart, 28u); Put(gx::kVsDirtyRegEnd, 28u);
    Put(gx::kPsDirtyRegStart, 255u); Put(gx::kPsDirtyRegEnd, 0u);
    auto arm = [&] {
        frame += 60; InvalidateWmoC28Proof();
        ObserveWmoC28Bind(WmoFamily::Diffuse, "WORLD\\TEST.WMO", 31, 5, collection,
                         c28::kVs30.data(), c28::kRaw30.data(), c28::kDiffuse5.data());
    };
    // No configuration means no callbacks, dispatch writes, or reads.
    EnterWmoC28ProofScope(1); arm();
    Check(subscriptions.empty() && protects == 0 && d.reads == 0 && table[82] == reinterpret_cast<void*>(&OriginalDIP));
    InitializeWmoC28Proof(); Check(subscriptions.size() == 3);
    EnterWmoC28ProofScope(1);
    arm(); Check(pending.active && protects == 8);
    unsigned calls = d.calls;
    // Hardware at EffectBind is intentionally stale: emulate native deferred upload.
    d.vertex[28][0] = 0.25f; Put(gx::kVsConstCache + 28 * 16, d.vertex[28]);
    Put(gx::kVsDirtyRegStart, 255u); Put(gx::kVsDirtyRegEnd, 0u);
    d.result = static_cast<HRESULT>(0x8876086c);
    auto hr = dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, -7, 9, 13, 17, 19);
    Check(hr == d.result && d.calls == calls + 1);
    Check(d.arguments == std::vector<std::uintptr_t>({4, static_cast<UINT>(-7),9,13,17,19}));
    Check(Has("\"phase\":\"pre_draw\"") && Has("\"value\":[0.25,29,30,31]"));
    Check(vs.refs == 0 && ps.refs == 0);
    // Second draw under one bind is captured; third stays native without readback.
    dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 3, 2); unsigned reads = d.reads;
    Check(d.arguments == std::vector<std::uintptr_t>({5,3,2}));
    dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 3, 2); Check(d.reads == reads);
    // Scope exit and unqualified bind invalidate attribution, including nested scopes.
    arm(); EnterWmoC28ProofScope(2); Check(!pending.active); LeaveWmoC28ProofScope();
    arm(); InvalidateWmoC28Proof(); reads = d.reads; dev->DrawPrimitive(D3DPT_TRIANGLELIST,0,1); Check(d.reads == reads);
    arm(); LeaveWmoC28ProofScope(); reads = d.reads; dev->DrawPrimitive(D3DPT_TRIANGLELIST,0,1); Check(d.reads == reads);
    EnterWmoC28ProofScope(3);
    arm(); d.vs = &wrong; calls = d.calls;
    dev->DrawPrimitive(D3DPT_TRIANGLELIST,0,1); Check(d.calls == calls + 1 && !pending.active && Has("draw_shader_identity_mismatch")); d.vs = &vs;
    // Reset destroys pending attribution without touching the native dispatch chain.
    arm(); EmitEvent(ev::Event::OnDeviceLost); Check(!pending.active); EmitEvent(ev::Event::OnDeviceReset);
    // Reset sample budget only in the test to exercise additional independent failures.
    budgets = {}; budgetCount = 0;
    arm(); d.failedReg = 28; dev->DrawPrimitive(D3DPT_TRIANGLELIST,0,1); Check(Has("\"reg\":28,\"hr\":-2005530516,\"valid\":false")); d.failedReg = -1;
    arm(); throwLog = "draw_begin"; calls = d.calls; dev->DrawPrimitive(D3DPT_TRIANGLELIST,0,1);
    Check(d.calls == calls + 1 && quarantined); quarantined = false;
    // Verify UP ABI forwards all original pointer/format/stride arguments verbatim.
    arm(); int data = 1, indices = 2;
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 7, &data, 36);
    Check(d.arguments == std::vector<std::uintptr_t>({4,7,reinterpret_cast<std::uintptr_t>(&data),36}));
    dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 8,9,10,&indices,D3DFMT_INDEX16,&data,40);
    Check(d.arguments == std::vector<std::uintptr_t>({4,8,9,10,reinterpret_cast<std::uintptr_t>(&indices),101,reinterpret_cast<std::uintptr_t>(&data),40}));
    Check(protects == 8); // no rewrapping / cycles on repeated binds
    Check(vs.refs == 0 && ps.refs == 0 && wrong.refs == 0);
    // Failed installation keeps every already-installed hook's original forwarding.
    void* table2[119]; std::memcpy(table2, table, sizeof(table));
    table2[81] = reinterpret_cast<void*>(&OriginalDP); table2[82] = reinterpret_cast<void*>(&OriginalDIP);
    table2[83] = reinterpret_cast<void*>(&OriginalDPUP); table2[84] = reinterpret_cast<void*>(&OriginalDIPUP);
    d.table = table2; failProtectAt = protects + 3; arm(); Check(quarantined);
    calls = d.calls; dev->DrawPrimitive(D3DPT_TRIANGLELIST,0,1); dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,3,0,1);
    Check(d.calls == calls + 2);
    std::printf("C28_RUNTIME_CHECK: PASS checks=%u (actual production translation unit, fake COM/Win32 only)\n", checks);
}
