// R8 Weather W1: stock-12340 weather + precipitation observer.
// Diagnostic only: no replacement rendering, no weather scheduling, no D3D state mutation.
// Enabled only with WXL_R8_WEATHER_DIAG=1.
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/events/EventScript.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "game/Gx.hpp"
#include "offsets/engine/Gx.hpp"
#include "water/WaterDiagCore.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wxl::scripts::render_modern::weather_w1
{
namespace
{
namespace ev = wxl::events;
namespace gx = wxl::game::gx;
namespace gxoff = wxl::offsets::engine::gx;
namespace hash = wxl::waterdiag;

constexpr uintptr_t kWeatherGlobal   = 0x00CD7544;
constexpr uintptr_t kWeatherSetType  = 0x007846A0;
constexpr uintptr_t kWeatherClear    = 0x0078D0B0;
constexpr uintptr_t kWeatherGetType  = 0x00783B60;
constexpr uintptr_t kSmsWeatherPath  = 0x00526A96; // landmark only; W1 does not hook packet parsing.

constexpr const char* kCanonicalExeSha256 =
    "57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da";

constexpr uint32_t kDefaultMaxDrawRecords = 8192;
constexpr uint32_t kHardMaxDrawRecords = 32768;
constexpr size_t kMaxOutputBytes = 24u * 1024u * 1024u;

using WeatherSetTypeFn =
    void(__fastcall*)(void* self, void* edx, int type, float intensity,
                      void* weatherRec, int abrupt, float transition);
using WeatherClearFn =
    void(__fastcall*)(void* self, void* edx);
using WeatherGetTypeFn =
    int(__fastcall*)(void* self, void* edx);

struct WeatherRecSnapshot
{
    bool valid = false;
    int32_t id = -1;
    int32_t ambienceId = -1;
    int32_t effectType = -1;
    float transitionSkybox = 0.0f;
    float colour[3] = {};
    char texture[260] = {};
};

struct WeatherState
{
    uint64_t epoch = 0;
    int32_t requestedType = 0;
    int32_t observedGetType = -1;
    float requestedIntensity = 0.0f;
    int32_t abrupt = 0;
    float transition = 0.0f;
    WeatherRecSnapshot rec{};
};

struct Config
{
    bool enabled = false;
    bool allDraws = false;
    uint32_t maxDrawRecords = kDefaultMaxDrawRecords;
    uint32_t minPrimitives = 16;
};

struct ShaderCacheEntry
{
    IUnknown* object = nullptr;
    std::string sha256;
};

enum class DirectDrawKind : uint32_t
{
    Primitive = 1,
    IndexedPrimitive = 2,
    PrimitiveUP = 3,
    IndexedPrimitiveUP = 4
};

using DrawPrimitiveFn =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn =
    HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                 const void*, D3DFORMAT, const void*, UINT);

struct DirectCandidateKey
{
    DirectDrawKind kind{};
    D3DPRIMITIVETYPE primitiveType{};
    IDirect3DBaseTexture9* texture = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    UINT width = 0;
    UINT height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    UINT stride = 0;
    DWORD zEnable = 0;
    DWORD zWrite = 0;
    DWORD zFunc = 0;
    DWORD alphaBlend = 0;
    DWORD srcBlend = 0;
    DWORD dstBlend = 0;
    DWORD blendOp = 0;
    DWORD alphaTest = 0;
    DWORD alphaRef = 0;
    DWORD alphaFunc = 0;
    DWORD cull = 0;
    DWORD colorWrite = 0;
};

bool SameKey(const DirectCandidateKey& a, const DirectCandidateKey& b)
{
    return a.kind == b.kind &&
           a.primitiveType == b.primitiveType &&
           a.texture == b.texture &&
           a.vs == b.vs &&
           a.ps == b.ps &&
           a.width == b.width &&
           a.height == b.height &&
           a.format == b.format &&
           a.stride == b.stride &&
           a.zEnable == b.zEnable &&
           a.zWrite == b.zWrite &&
           a.zFunc == b.zFunc &&
           a.alphaBlend == b.alphaBlend &&
           a.srcBlend == b.srcBlend &&
           a.dstBlend == b.dstBlend &&
           a.blendOp == b.blendOp &&
           a.alphaTest == b.alphaTest &&
           a.alphaRef == b.alphaRef &&
           a.alphaFunc == b.alphaFunc &&
           a.cull == b.cull &&
           a.colorWrite == b.colorWrite;
}

struct DirectCandidateStats
{
    DirectCandidateKey key{};
    uint64_t calls = 0;
    uint64_t totalPrimitives = 0;
    uint32_t minPrimitives = std::numeric_limits<uint32_t>::max();
    uint32_t maxPrimitives = 0;
    uint64_t firstFrame = 0;
    uint64_t lastFrame = 0;
    uint64_t firstOrdinal = 0;
    uint32_t textureLevels = 0;
    DWORD textureUsage = 0;
    D3DPOOL texturePool = D3DPOOL_DEFAULT;
    DWORD fvf = 0;
    DWORD samplerAddressU = 0;
    DWORD samplerAddressV = 0;
    DWORD samplerMag = 0;
    DWORD samplerMin = 0;
    DWORD samplerMip = 0;
    std::string firstPhase;
    std::string lastPhase;
    std::string vsSha;
    std::string psSha;
    std::string declSha;
};

Config g_config{};
WeatherState g_weather{};
std::ofstream g_sink;
size_t g_outputBytes = 0;
uint64_t g_dropped = 0;
uint64_t g_frame = 0;
uint64_t g_drawOrdinal = 0;
uint64_t g_drawRecords = 0;
uint64_t g_candidateDraws = 0;
uint64_t g_weatherEvents = 0;
const char* g_phase = "startup";
WeatherSetTypeFn g_origSetType = nullptr;
WeatherClearFn g_origClear = nullptr;
DrawPrimitiveFn g_origDrawPrimitive = nullptr;
DrawIndexedPrimitiveFn g_origDrawIndexedPrimitive = nullptr;
DrawPrimitiveUPFn g_origDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUPFn g_origDrawIndexedPrimitiveUP = nullptr;
void* g_directDrawTargets[4] = {};
std::vector<ShaderCacheEntry> g_shaderCache;
std::vector<DirectCandidateStats> g_directCandidates;

void FlushDirectCandidates(const char* reason);

bool TruthyEnv(const char* name)
{
    char raw[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, raw, sizeof(raw));
    if (n == 0 || n >= sizeof(raw))
        return false;

    const char c = raw[0];
    return c != '0' &&
           c != 'n' && c != 'N' &&
           c != 'f' && c != 'F';
}

uint32_t UintEnv(const char* name, uint32_t fallback, uint32_t lo, uint32_t hi)
{
    char raw[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, raw, sizeof(raw));
    if (n == 0 || n >= sizeof(raw))
        return fallback;

    char* end = nullptr;
    const unsigned long v = std::strtoul(raw, &end, 10);
    if (end == raw || (end && *end != '\0'))
        return fallback;

    return static_cast<uint32_t>(std::clamp<unsigned long>(v, lo, hi));
}

std::string Pointer(const void* p)
{
    char b[24] = {};
    std::snprintf(
        b, sizeof(b), "0x%08llx",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(p)));
    return b;
}

std::string FloatHex(float v)
{
    return hash::Hex(&v, sizeof(v));
}

void EmitLine(std::string line)
{
    if (!g_sink.is_open())
        return;

    line.push_back('\n');

    if (g_outputBytes > kMaxOutputBytes ||
        line.size() > kMaxOutputBytes - g_outputBytes)
    {
        ++g_dropped;
        return;
    }

    g_sink.write(line.data(), static_cast<std::streamsize>(line.size()));
    g_outputBytes += line.size();

    if ((g_drawRecords & 127u) == 0u)
        g_sink.flush();
}

bool FileSha256(const char* path, std::string& out)
{
    HANDLE h = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    hash::Sha256 sha;
    std::array<uint8_t, 64 * 1024> buffer{};

    bool ok = true;
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
        {
            ok = false;
            break;
        }

        if (got == 0)
            break;

        sha.Update(buffer.data(), got);
    }

    CloseHandle(h);

    if (!ok)
        return false;

    out = hash::Hex(sha.Finish());
    return true;
}

bool VerifyCanonicalExecutable()
{
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        WLOG_ERROR("wxl-r8-weather-w1: cannot resolve Wow.exe path");
        return false;
    }

    std::string sha;
    if (!FileSha256(path, sha))
    {
        WLOG_ERROR("wxl-r8-weather-w1: cannot hash Wow.exe");
        return false;
    }

    if (sha != kCanonicalExeSha256)
    {
        WLOG_ERROR(
            "wxl-r8-weather-w1: Wow.exe authority mismatch "
            "expected=%s actual=%s; observer remains disabled",
            kCanonicalExeSha256,
            sha.c_str());
        return false;
    }

    WLOG_INFO(
        "wxl-r8-weather-w1: Wow.exe authority PASS sha256=%s",
        sha.c_str());
    return true;
}

bool SafeWeatherRec(void* raw, WeatherRecSnapshot& out) noexcept
{
    if (!raw)
        return false;

    __try
    {
        const uint8_t* p = static_cast<const uint8_t*>(raw);

        out.id = *reinterpret_cast<const int32_t*>(p + 0x00);
        out.ambienceId = *reinterpret_cast<const int32_t*>(p + 0x04);
        out.effectType = *reinterpret_cast<const int32_t*>(p + 0x08);
        out.transitionSkybox = *reinterpret_cast<const float*>(p + 0x0C);
        out.colour[0] = *reinterpret_cast<const float*>(p + 0x10);
        out.colour[1] = *reinterpret_cast<const float*>(p + 0x14);
        out.colour[2] = *reinterpret_cast<const float*>(p + 0x18);

        const char* texture =
            *reinterpret_cast<const char* const*>(p + 0x1C);

        if (texture)
        {
            size_t i = 0;
            for (; i + 1 < sizeof(out.texture); ++i)
            {
                const char c = texture[i];
                out.texture[i] = c;
                if (c == '\0')
                    break;
            }
            out.texture[sizeof(out.texture) - 1] = '\0';
        }

        out.valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out = {};
        return false;
    }
}

int SafeGetType(void* self) noexcept
{
    if (!self)
        return -1;

    __try
    {
        auto fn = reinterpret_cast<WeatherGetTypeFn>(kWeatherGetType);
        return fn(self, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

std::string WeatherJson(const char* event)
{
    std::string s = "{";
    s += "\"schema\":1";
    s += ",\"event\":" + hash::Quote(event);
    s += ",\"frame\":" + std::to_string(g_frame);
    s += ",\"weather_epoch\":" + std::to_string(g_weather.epoch);
    s += ",\"requested_type\":" + std::to_string(g_weather.requestedType);
    s += ",\"observed_get_type\":" + std::to_string(g_weather.observedGetType);
    s += ",\"requested_intensity\":" + std::to_string(g_weather.requestedIntensity);
    s += ",\"requested_intensity_le_hex\":" + hash::Quote(FloatHex(g_weather.requestedIntensity));
    s += ",\"abrupt\":" + std::to_string(g_weather.abrupt);
    s += ",\"transition\":" + std::to_string(g_weather.transition);
    s += ",\"transition_le_hex\":" + hash::Quote(FloatHex(g_weather.transition));
    s += ",\"weather_rec_valid\":" + std::string(g_weather.rec.valid ? "true" : "false");

    if (g_weather.rec.valid)
    {
        s += ",\"weather_rec_id\":" + std::to_string(g_weather.rec.id);
        s += ",\"ambience_id\":" + std::to_string(g_weather.rec.ambienceId);
        s += ",\"effect_type\":" + std::to_string(g_weather.rec.effectType);
        s += ",\"transition_skybox\":" + std::to_string(g_weather.rec.transitionSkybox);
        s += ",\"effect_colour\":[";
        s += std::to_string(g_weather.rec.colour[0]) + ",";
        s += std::to_string(g_weather.rec.colour[1]) + ",";
        s += std::to_string(g_weather.rec.colour[2]) + "]";
        s += ",\"effect_texture\":" + hash::Quote(g_weather.rec.texture);
    }

    s += "}";
    return s;
}

void __fastcall hkWeatherSetType(
    void* self,
    void* edx,
    int type,
    float intensity,
    void* weatherRec,
    int abrupt,
    float transition)
{
    FlushDirectCandidates("before_weather_set_type");

    WeatherRecSnapshot rec{};
    SafeWeatherRec(weatherRec, rec);

    ++g_weather.epoch;
    g_weather.requestedType = type;
    g_weather.requestedIntensity = intensity;
    g_weather.abrupt = abrupt;
    g_weather.transition = transition;
    g_weather.rec = rec;

    if (g_origSetType)
        g_origSetType(
            self, edx, type, intensity, weatherRec, abrupt, transition);

    g_weather.observedGetType = SafeGetType(self);
    ++g_weatherEvents;

    EmitLine(WeatherJson("weather_set_type"));
    g_sink.flush();
}

void __fastcall hkWeatherClear(void* self, void* edx)
{
    FlushDirectCandidates("before_weather_clear");

    ++g_weather.epoch;
    g_weather.requestedType = 0;
    g_weather.requestedIntensity = 0.0f;
    g_weather.abrupt = 0;
    g_weather.transition = 0.0f;
    g_weather.rec = {};

    if (g_origClear)
        g_origClear(self, edx);

    g_weather.observedGetType = SafeGetType(self);
    ++g_weatherEvents;

    EmitLine(WeatherJson("weather_clear"));
    g_sink.flush();
}

std::string ShaderSha(IUnknown* shader, bool vertex)
{
    if (!shader)
        return "null";

    for (const auto& e : g_shaderCache)
        if (e.object == shader)
            return e.sha256;

    if (g_shaderCache.size() >= 192)
        return "cache_full";

    UINT bytes = 0;
    HRESULT hr = vertex
        ? static_cast<IDirect3DVertexShader9*>(shader)->GetFunction(nullptr, &bytes)
        : static_cast<IDirect3DPixelShader9*>(shader)->GetFunction(nullptr, &bytes);

    if (FAILED(hr) || bytes < 4 || bytes > 65536)
        return "query_failed";

    std::vector<uint8_t> code(bytes);
    UINT actual = bytes;
    hr = vertex
        ? static_cast<IDirect3DVertexShader9*>(shader)->GetFunction(code.data(), &actual)
        : static_cast<IDirect3DPixelShader9*>(shader)->GetFunction(code.data(), &actual);

    if (FAILED(hr) || actual != bytes)
        return "read_failed";

    const std::string digest =
        hash::Hex(hash::Sha256::Of(code.data(), code.size()));

    shader->AddRef();
    g_shaderCache.push_back({ shader, digest });
    return digest;
}

std::string VertexDeclSha(IDirect3DDevice9* d)
{
    IDirect3DVertexDeclaration9* decl = nullptr;
    const HRESULT hr = d->GetVertexDeclaration(&decl);
    if (FAILED(hr) || !decl)
        return "null";

    UINT count = 0;
    HRESULT a = decl->GetDeclaration(nullptr, &count);
    if (FAILED(a) || count == 0 || count > 64)
    {
        decl->Release();
        return "query_failed";
    }

    std::vector<D3DVERTEXELEMENT9> elements(count);
    HRESULT b = decl->GetDeclaration(elements.data(), &count);
    decl->Release();

    if (FAILED(b))
        return "read_failed";

    return hash::Hex(
        hash::Sha256::Of(
            elements.data(),
            count * sizeof(D3DVERTEXELEMENT9)));
}

std::string TextureJson(IDirect3DBaseTexture9* t)
{
    std::string s = "{";
    s += "\"bound\":" + std::string(t ? "true" : "false");

    if (!t)
    {
        s += "}";
        return s;
    }

    s += ",\"token\":" + hash::Quote(Pointer(t));
    s += ",\"resource_type\":" + std::to_string(static_cast<unsigned>(t->GetType()));
    s += ",\"levels\":" + std::to_string(t->GetLevelCount());

    D3DSURFACE_DESC d{};
    HRESULT hr = D3DERR_INVALIDCALL;

    if (t->GetType() == D3DRTYPE_TEXTURE)
        hr = static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
    else if (t->GetType() == D3DRTYPE_CUBETEXTURE)
        hr = static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &d);

    s += ",\"desc_hr\":" + std::to_string(static_cast<int32_t>(hr));

    if (SUCCEEDED(hr))
    {
        s += ",\"width\":" + std::to_string(d.Width);
        s += ",\"height\":" + std::to_string(d.Height);
        s += ",\"format\":" + std::to_string(static_cast<unsigned>(d.Format));
        s += ",\"usage\":" + std::to_string(d.Usage);
        s += ",\"pool\":" + std::to_string(static_cast<unsigned>(d.Pool));
    }

    s += "}";
    return s;
}

std::string SurfaceJson(IDirect3DSurface9* surf)
{
    std::string s = "{";
    s += "\"bound\":" + std::string(surf ? "true" : "false");

    if (!surf)
    {
        s += "}";
        return s;
    }

    s += ",\"token\":" + hash::Quote(Pointer(surf));

    D3DSURFACE_DESC d{};
    const HRESULT hr = surf->GetDesc(&d);
    s += ",\"desc_hr\":" + std::to_string(static_cast<int32_t>(hr));

    if (SUCCEEDED(hr))
    {
        s += ",\"width\":" + std::to_string(d.Width);
        s += ",\"height\":" + std::to_string(d.Height);
        s += ",\"format\":" + std::to_string(static_cast<unsigned>(d.Format));
        s += ",\"msaa\":" + std::to_string(static_cast<unsigned>(d.MultiSampleType));
        s += ",\"msaa_quality\":" + std::to_string(d.MultiSampleQuality);
    }

    s += "}";
    return s;
}

DWORD GetRs(IDirect3DDevice9* d, D3DRENDERSTATETYPE state)
{
    DWORD value = 0;
    d->GetRenderState(state, &value);
    return value;
}

DWORD GetSs(IDirect3DDevice9* d, DWORD sampler, D3DSAMPLERSTATETYPE state)
{
    DWORD value = 0;
    d->GetSamplerState(sampler, state, &value);
    return value;
}

const char* DrawKindName(DirectDrawKind kind)
{
    switch (kind)
    {
    case DirectDrawKind::Primitive: return "DrawPrimitive";
    case DirectDrawKind::IndexedPrimitive: return "DrawIndexedPrimitive";
    case DirectDrawKind::PrimitiveUP: return "DrawPrimitiveUP";
    case DirectDrawKind::IndexedPrimitiveUP: return "DrawIndexedPrimitiveUP";
    default: return "unknown";
    }
}

UINT CurrentStreamStride(IDirect3DDevice9* d)
{
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0;
    UINT stride = 0;
    if (SUCCEEDED(d->GetStreamSource(0, &vb, &offset, &stride)) && vb)
        vb->Release();
    return stride;
}

bool PlausibleDirectWeatherDraw(
    IDirect3DDevice9* d,
    UINT primitiveCount,
    DirectCandidateKey& key)
{
    if (!d)
        return false;

    if (!g_config.allDraws && primitiveCount < g_config.minPrimitives)
        return false;

    key.zEnable = GetRs(d, D3DRS_ZENABLE);
    key.zWrite = GetRs(d, D3DRS_ZWRITEENABLE);
    key.zFunc = GetRs(d, D3DRS_ZFUNC);
    key.alphaBlend = GetRs(d, D3DRS_ALPHABLENDENABLE);
    key.srcBlend = GetRs(d, D3DRS_SRCBLEND);
    key.dstBlend = GetRs(d, D3DRS_DESTBLEND);
    key.blendOp = GetRs(d, D3DRS_BLENDOP);
    key.alphaTest = GetRs(d, D3DRS_ALPHATESTENABLE);
    key.alphaRef = GetRs(d, D3DRS_ALPHAREF);
    key.alphaFunc = GetRs(d, D3DRS_ALPHAFUNC);
    key.cull = GetRs(d, D3DRS_CULLMODE);
    key.colorWrite = GetRs(d, D3DRS_COLORWRITEENABLE);

    if (!g_config.allDraws)
    {
        // Native precipitation in the strongest donor evidence is world-space,
        // depth-tested, alpha-blended, and does not write depth.  This is only
        // a discovery filter: exact stock-12340 texture/shader signatures are
        // still learned from this observer rather than copied from Ascension.
        if (key.zEnable == FALSE ||
            key.zWrite != FALSE ||
            key.alphaBlend == FALSE ||
            key.colorWrite == 0)
            return false;
    }

    IDirect3DBaseTexture9* base = nullptr;
    if (FAILED(d->GetTexture(0, &base)) || !base)
        return false;

    if (base->GetType() != D3DRTYPE_TEXTURE)
    {
        base->Release();
        return false;
    }

    D3DSURFACE_DESC desc{};
    auto* texture = static_cast<IDirect3DTexture9*>(base);
    const HRESULT hr = texture->GetLevelDesc(0, &desc);
    if (FAILED(hr) || (desc.Usage & D3DUSAGE_RENDERTARGET))
    {
        base->Release();
        return false;
    }

    key.texture = base; // temporary AddRef held until ObserveDirectDraw finishes.
    key.width = desc.Width;
    key.height = desc.Height;
    key.format = desc.Format;
    return true;
}

void ObserveDirectDraw(
    IDirect3DDevice9* d,
    DirectDrawKind kind,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    UINT explicitStride)
{
    ++g_drawOrdinal;

    DirectCandidateKey key{};
    key.kind = kind;
    key.primitiveType = primitiveType;
    key.stride = explicitStride ? explicitStride : CurrentStreamStride(d);

    if (!PlausibleDirectWeatherDraw(d, primitiveCount, key))
        return;

    ++g_candidateDraws;

    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    key.vs = vs;
    key.ps = ps;

    DirectCandidateStats* hit = nullptr;
    for (auto& candidate : g_directCandidates)
    {
        if (SameKey(candidate.key, key))
        {
            hit = &candidate;
            break;
        }
    }

    if (!hit)
    {
        const size_t hardUniqueLimit =
            static_cast<size_t>(std::min<uint32_t>(g_config.maxDrawRecords, 2048u));

        if (g_directCandidates.size() >= hardUniqueLimit)
        {
            ++g_dropped;
        }
        else
        {
            DirectCandidateStats fresh{};
            fresh.key = key;
            fresh.firstFrame = g_frame;
            fresh.lastFrame = g_frame;
            fresh.firstOrdinal = g_drawOrdinal;
            fresh.textureLevels = key.texture ? key.texture->GetLevelCount() : 0;
            fresh.textureUsage = 0;
            fresh.texturePool = D3DPOOL_DEFAULT;

            if (key.texture && key.texture->GetType() == D3DRTYPE_TEXTURE)
            {
                D3DSURFACE_DESC desc{};
                if (SUCCEEDED(
                        static_cast<IDirect3DTexture9*>(key.texture)->GetLevelDesc(0, &desc)))
                {
                    fresh.textureUsage = desc.Usage;
                    fresh.texturePool = desc.Pool;
                }
            }

            d->GetFVF(&fresh.fvf);
            fresh.samplerAddressU = GetSs(d, 0, D3DSAMP_ADDRESSU);
            fresh.samplerAddressV = GetSs(d, 0, D3DSAMP_ADDRESSV);
            fresh.samplerMag = GetSs(d, 0, D3DSAMP_MAGFILTER);
            fresh.samplerMin = GetSs(d, 0, D3DSAMP_MINFILTER);
            fresh.samplerMip = GetSs(d, 0, D3DSAMP_MIPFILTER);
            fresh.firstPhase = g_phase ? g_phase : "";
            fresh.lastPhase = fresh.firstPhase;
            fresh.vsSha = ShaderSha(vs, true);
            fresh.psSha = ShaderSha(ps, false);
            fresh.declSha = VertexDeclSha(d);

            // Candidate keys keep raw object identity only for this segment;
            // no additional COM reference is required because the engine owns
            // these resources throughout their draws.
            g_directCandidates.push_back(std::move(fresh));
            hit = &g_directCandidates.back();
        }
    }

    if (hit)
    {
        ++hit->calls;
        hit->totalPrimitives += primitiveCount;
        hit->minPrimitives = std::min(hit->minPrimitives, primitiveCount);
        hit->maxPrimitives = std::max(hit->maxPrimitives, primitiveCount);
        hit->lastFrame = g_frame;
        hit->lastPhase = g_phase ? g_phase : "";
    }

    if (key.texture)
        key.texture->Release();
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
}

void FlushDirectCandidates(const char* reason)
{
    if (g_directCandidates.empty())
        return;

    for (const auto& c : g_directCandidates)
    {
        if (g_drawRecords >= g_config.maxDrawRecords)
        {
            ++g_dropped;
            break;
        }

        std::string s = "{";
        s += "\"schema\":2";
        s += ",\"event\":\"direct_draw_aggregate\"";
        s += ",\"reason\":" + hash::Quote(reason ? reason : "");
        s += ",\"weather_epoch\":" + std::to_string(g_weather.epoch);
        s += ",\"requested_type\":" + std::to_string(g_weather.requestedType);
        s += ",\"observed_get_type\":" + std::to_string(g_weather.observedGetType);
        s += ",\"requested_intensity\":" + std::to_string(g_weather.requestedIntensity);
        s += ",\"weather_rec_id\":" +
            std::to_string(g_weather.rec.valid ? g_weather.rec.id : -1);
        s += ",\"draw_kind\":" + hash::Quote(DrawKindName(c.key.kind));
        s += ",\"prim_type\":" +
            std::to_string(static_cast<unsigned>(c.key.primitiveType));
        s += ",\"calls\":" + std::to_string(c.calls);
        s += ",\"total_primitives\":" + std::to_string(c.totalPrimitives);
        s += ",\"min_primitives\":" +
            std::to_string(c.minPrimitives == std::numeric_limits<uint32_t>::max()
                               ? 0u
                               : c.minPrimitives);
        s += ",\"max_primitives\":" + std::to_string(c.maxPrimitives);
        s += ",\"first_frame\":" + std::to_string(c.firstFrame);
        s += ",\"last_frame\":" + std::to_string(c.lastFrame);
        s += ",\"first_ordinal\":" + std::to_string(c.firstOrdinal);
        s += ",\"first_phase\":" + hash::Quote(c.firstPhase);
        s += ",\"last_phase\":" + hash::Quote(c.lastPhase);

        s += ",\"texture\":{";
        s += "\"token\":" + hash::Quote(Pointer(c.key.texture));
        s += ",\"width\":" + std::to_string(c.key.width);
        s += ",\"height\":" + std::to_string(c.key.height);
        s += ",\"format\":" + std::to_string(static_cast<unsigned>(c.key.format));
        s += ",\"levels\":" + std::to_string(c.textureLevels);
        s += ",\"usage\":" + std::to_string(c.textureUsage);
        s += ",\"pool\":" + std::to_string(static_cast<unsigned>(c.texturePool));
        s += "}";

        s += ",\"vs_sha256\":" + hash::Quote(c.vsSha);
        s += ",\"ps_sha256\":" + hash::Quote(c.psSha);
        s += ",\"vertex_decl_sha256\":" + hash::Quote(c.declSha);
        s += ",\"fvf\":" + std::to_string(c.fvf);
        s += ",\"stream0_stride\":" + std::to_string(c.key.stride);

        s += ",\"render_state\":{";
        s += "\"zenable\":" + std::to_string(c.key.zEnable);
        s += ",\"zwrite\":" + std::to_string(c.key.zWrite);
        s += ",\"zfunc\":" + std::to_string(c.key.zFunc);
        s += ",\"alpha_blend\":" + std::to_string(c.key.alphaBlend);
        s += ",\"src_blend\":" + std::to_string(c.key.srcBlend);
        s += ",\"dst_blend\":" + std::to_string(c.key.dstBlend);
        s += ",\"blend_op\":" + std::to_string(c.key.blendOp);
        s += ",\"alpha_test\":" + std::to_string(c.key.alphaTest);
        s += ",\"alpha_ref\":" + std::to_string(c.key.alphaRef);
        s += ",\"alpha_func\":" + std::to_string(c.key.alphaFunc);
        s += ",\"cull\":" + std::to_string(c.key.cull);
        s += ",\"color_write\":" + std::to_string(c.key.colorWrite);
        s += "}";

        s += ",\"sampler0\":{";
        s += "\"address_u\":" + std::to_string(c.samplerAddressU);
        s += ",\"address_v\":" + std::to_string(c.samplerAddressV);
        s += ",\"mag_filter\":" + std::to_string(c.samplerMag);
        s += ",\"min_filter\":" + std::to_string(c.samplerMin);
        s += ",\"mip_filter\":" + std::to_string(c.samplerMip);
        s += "}";

        s += "}";
        EmitLine(std::move(s));
        ++g_drawRecords;
    }

    g_directCandidates.clear();
    g_sink.flush();
}

HRESULT STDMETHODCALLTYPE hkDrawPrimitive(
    IDirect3DDevice9* d,
    D3DPRIMITIVETYPE primitiveType,
    UINT startVertex,
    UINT primitiveCount)
{
    ObserveDirectDraw(
        d, DirectDrawKind::Primitive, primitiveType, primitiveCount, 0);
    return g_origDrawPrimitive
        ? g_origDrawPrimitive(d, primitiveType, startVertex, primitiveCount)
        : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitive(
    IDirect3DDevice9* d,
    D3DPRIMITIVETYPE primitiveType,
    INT baseVertexIndex,
    UINT minVertexIndex,
    UINT numVertices,
    UINT startIndex,
    UINT primitiveCount)
{
    ObserveDirectDraw(
        d, DirectDrawKind::IndexedPrimitive, primitiveType, primitiveCount, 0);
    return g_origDrawIndexedPrimitive
        ? g_origDrawIndexedPrimitive(
              d, primitiveType, baseVertexIndex, minVertexIndex,
              numVertices, startIndex, primitiveCount)
        : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(
    IDirect3DDevice9* d,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    const void* vertexData,
    UINT stride)
{
    ObserveDirectDraw(
        d, DirectDrawKind::PrimitiveUP, primitiveType, primitiveCount, stride);
    return g_origDrawPrimitiveUP
        ? g_origDrawPrimitiveUP(
              d, primitiveType, primitiveCount, vertexData, stride)
        : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE hkDrawIndexedPrimitiveUP(
    IDirect3DDevice9* d,
    D3DPRIMITIVETYPE primitiveType,
    UINT minVertexIndex,
    UINT numVertices,
    UINT primitiveCount,
    const void* indexData,
    D3DFORMAT indexFormat,
    const void* vertexData,
    UINT stride)
{
    ObserveDirectDraw(
        d, DirectDrawKind::IndexedPrimitiveUP, primitiveType, primitiveCount, stride);
    return g_origDrawIndexedPrimitiveUP
        ? g_origDrawIndexedPrimitiveUP(
              d, primitiveType, minVertexIndex, numVertices,
              primitiveCount, indexData, indexFormat, vertexData, stride)
        : D3DERR_INVALIDCALL;
}

bool ContainsWeatherToken(const char* name)
{
    if (!name)
        return false;

    std::string n(name);
    std::transform(
        n.begin(), n.end(), n.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return n.find("weather") != std::string::npos ||
           n.find("rain") != std::string::npos ||
           n.find("snow") != std::string::npos ||
           n.find("sand") != std::string::npos ||
           n.find("mist") != std::string::npos;
}

class ObserverEvents : public ev::EventScript
{
public:
    ObserverEvents()
    {
        on<&ObserverEvents::OnFrame>(ev::Event::OnFrame);
        on<&ObserverEvents::OnWorldSceneBegin>(ev::Event::OnWorldSceneBegin);
        on<&ObserverEvents::OnLiquid>(ev::Event::OnLiquidRender);
        on<&ObserverEvents::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
        on<&ObserverEvents::OnWorldRenderEnd>(ev::Event::OnWorldRenderEnd);
        on<&ObserverEvents::OnEndScene>(ev::Event::OnEndScene);
        on<&ObserverEvents::OnBlpLoad>(ev::Event::OnBlpLoad);
    }

private:
    void OnFrame(const ev::FrameArgs&)
    {
        ++g_frame;
        g_drawOrdinal = 0;
        g_phase = "present_boundary";
    }

    void OnWorldSceneBegin(const ev::WorldSceneBeginArgs&)
    {
        g_phase = "world_scene";
    }

    void OnLiquid(const ev::LiquidRenderArgs&)
    {
        g_phase = "liquid";
    }

    void OnWorldSceneEnd(const ev::WorldSceneEndArgs&)
    {
        g_phase = "world_scene_end";
    }

    void OnWorldRenderEnd(const ev::WorldRenderEndArgs&)
    {
        g_phase = "world_render_end";
    }

    void OnEndScene(const ev::EndSceneArgs&)
    {
        g_phase = "end_scene";

        static uint64_t lastSummaryFrame = 0;
        static uint64_t lastAggregateFlushFrame = 0;
        if (g_frame >= lastAggregateFlushFrame + 900)
        {
            lastAggregateFlushFrame = g_frame;
            FlushDirectCandidates("periodic_900_frames");
        }

        if (g_frame >= lastSummaryFrame + 300)
        {
            lastSummaryFrame = g_frame;

            std::string s = "{";
            s += "\"schema\":1,\"event\":\"summary\"";
            s += ",\"frame\":" + std::to_string(g_frame);
            s += ",\"weather_events\":" + std::to_string(g_weatherEvents);
            s += ",\"candidate_draws\":" + std::to_string(g_candidateDraws);
            s += ",\"draw_records\":" + std::to_string(g_drawRecords);
            s += ",\"dropped\":" + std::to_string(g_dropped);
            s += "}";
            EmitLine(std::move(s));
            g_sink.flush();
        }
    }

    void OnBlpLoad(const ev::BlpLoadArgs& a)
    {
        if (!ContainsWeatherToken(a.name))
            return;

        std::string s = "{";
        s += "\"schema\":1,\"event\":\"weather_blp\"";
        s += ",\"frame\":" + std::to_string(g_frame);
        s += ",\"name\":" + hash::Quote(a.name ? a.name : "");
        s += ",\"handle\":" + hash::Quote(Pointer(a.handle));
        s += "}";
        EmitLine(std::move(s));
    }
};

ObserverEvents* g_events = nullptr;

void ReleaseShaderCache()
{
    for (auto& e : g_shaderCache)
        if (e.object)
            e.object->Release();
    g_shaderCache.clear();
}

bool Install()
{
    g_config.enabled = TruthyEnv("WXL_R8_WEATHER_DIAG");

    if (!g_config.enabled)
    {
        WLOG_INFO(
            "wxl-r8-weather-w1: observer disabled "
            "(set WXL_R8_WEATHER_DIAG=1 to enable)");
        return true;
    }

    g_config.allDraws =
        TruthyEnv("WXL_R8_WEATHER_DIAG_ALL_DRAWS");

    g_config.maxDrawRecords =
        UintEnv(
            "WXL_R8_WEATHER_DIAG_MAX_DRAWS",
            kDefaultMaxDrawRecords,
            64,
            kHardMaxDrawRecords);

    g_config.minPrimitives =
        UintEnv(
            "WXL_R8_WEATHER_DIAG_MIN_PRIMS",
            16,
            1,
            65535);

    if (!VerifyCanonicalExecutable())
        return true;

    IDirect3DDevice9* rawDevice =
        static_cast<IDirect3DDevice9*>(gx::RawDevice());
    if (!rawDevice)
    {
        WLOG_ERROR("wxl-r8-weather-w1: no live D3D9 device for direct draw observer");
        return true;
    }

    void** d3dVtable = *reinterpret_cast<void***>(rawDevice);
    g_directDrawTargets[0] = d3dVtable[gxoff::vt::kDrawPrimitive];
    g_directDrawTargets[1] = d3dVtable[gxoff::vt::kDrawIndexedPrimitive];
    g_directDrawTargets[2] = d3dVtable[gxoff::vt::kDrawPrimitiveUP];
    g_directDrawTargets[3] = d3dVtable[gxoff::vt::kDrawIndexedPrimitiveUP];

    CreateDirectoryA("Logs", nullptr);
    g_sink.open(
        "Logs\\weather-w1.ndjson",
        std::ios::binary | std::ios::trunc);

    if (!g_sink.is_open())
    {
        WLOG_ERROR(
            "wxl-r8-weather-w1: failed to open Logs\\weather-w1.ndjson");
        return true;
    }

    std::string header = "{";
    header += "\"schema\":1,\"event\":\"observer_start\"";
    header += ",\"wow_sha256\":" + hash::Quote(kCanonicalExeSha256);
    header += ",\"weather_global\":" + hash::Quote(Pointer(reinterpret_cast<void*>(kWeatherGlobal)));
    header += ",\"weather_set_type\":" + hash::Quote(Pointer(reinterpret_cast<void*>(kWeatherSetType)));
    header += ",\"weather_clear\":" + hash::Quote(Pointer(reinterpret_cast<void*>(kWeatherClear)));
    header += ",\"weather_get_type\":" + hash::Quote(Pointer(reinterpret_cast<void*>(kWeatherGetType)));
    header += ",\"smsg_weather_landmark\":" + hash::Quote(Pointer(reinterpret_cast<void*>(kSmsWeatherPath)));
    header += ",\"d3d_draw_primitive\":" + hash::Quote(Pointer(g_directDrawTargets[0]));
    header += ",\"d3d_draw_indexed_primitive\":" + hash::Quote(Pointer(g_directDrawTargets[1]));
    header += ",\"d3d_draw_primitive_up\":" + hash::Quote(Pointer(g_directDrawTargets[2]));
    header += ",\"d3d_draw_indexed_primitive_up\":" + hash::Quote(Pointer(g_directDrawTargets[3]));
    header += ",\"max_draw_records\":" + std::to_string(g_config.maxDrawRecords);
    header += ",\"min_primitives\":" + std::to_string(g_config.minPrimitives);
    header += ",\"candidate_filter\":" + hash::Quote(
        g_config.allDraws
            ? "all_direct_d3d_draws_with_stage0_texture"
            : "direct_d3d + prims>=min + zenable + !zwrite + alphablend + colorwrite + nonRT_stage0");
    header += ",\"mutates_render_state\":false";
    header += ",\"changes_weather\":false";
    header += "}";
    EmitLine(std::move(header));
    g_sink.flush();

    bool ok = true;

    ok &= wxl::hook::Install(
        "R8WeatherW1SetTypeObserver",
        kWeatherSetType,
        &hkWeatherSetType,
        &g_origSetType);

    ok &= wxl::hook::Install(
        "R8WeatherW1ClearObserver",
        kWeatherClear,
        &hkWeatherClear,
        &g_origClear);

    ok &= wxl::hook::Install(
        "R8WeatherW1D3DDrawPrimitiveObserver",
        g_directDrawTargets[0],
        reinterpret_cast<void*>(&hkDrawPrimitive),
        reinterpret_cast<void**>(&g_origDrawPrimitive));

    ok &= wxl::hook::Install(
        "R8WeatherW1D3DDrawIndexedPrimitiveObserver",
        g_directDrawTargets[1],
        reinterpret_cast<void*>(&hkDrawIndexedPrimitive),
        reinterpret_cast<void**>(&g_origDrawIndexedPrimitive));

    ok &= wxl::hook::Install(
        "R8WeatherW1D3DDrawPrimitiveUPObserver",
        g_directDrawTargets[2],
        reinterpret_cast<void*>(&hkDrawPrimitiveUP),
        reinterpret_cast<void**>(&g_origDrawPrimitiveUP));

    ok &= wxl::hook::Install(
        "R8WeatherW1D3DDrawIndexedPrimitiveUPObserver",
        g_directDrawTargets[3],
        reinterpret_cast<void*>(&hkDrawIndexedPrimitiveUP),
        reinterpret_cast<void**>(&g_origDrawIndexedPrimitiveUP));

    if (!ok)
    {
        WLOG_ERROR(
            "wxl-r8-weather-w1: hook registration failed; observer inactive");
        return false;
    }

    g_events = new ObserverEvents();

    WLOG_INFO(
        "wxl-r8-weather-w1: observer enabled "
        "output=Logs\\weather-w1.ndjson maxDraws=%u minPrims=%u allDraws=%u "
        "direct_d3d=1 visual_changes=0",
        g_config.maxDrawRecords,
        g_config.minPrimitives,
        g_config.allDraws ? 1u : 0u);

    return true;
}

} // namespace

WXL_REGISTER_FEATURE(
    "render-modern-r8-weather-w1r4-direct-d3d-observer",
    true,
    Install);

} // namespace wxl::scripts::render_modern::weather_w1
