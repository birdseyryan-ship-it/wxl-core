// R8 Step 11B-02B1: first real structural WMO visual candidate.
//
// Scope is deliberately narrow.
//
// PROVEN selector identities from 11B-02A:
//   simple lit MapObjDiffuse_T1 selected VS:
//     868fbe1c...  -> raw/even 929f23fc...
//     b70c0dfa...  -> raw/even 1d29d928...
//
//   Diffuse PS:
//     pix5  24c2c665...
//     pix7  259824c4...
//
//   Opaque PS:
//     pix5  6b2c5652...
//     pix7  e761d8f2...
//
// UDiffuse, local-light VS permutations, Specular, pix15 and every
// unrecognised route fail open to native.
//
// The stock raw/even VS is patched only to:
//   * preserve native transform/shadow/fog outputs;
//   * preserve raw vertex RGB and odd-path saturated alpha;
//   * carry the exact native c10/c11/c12/c29 lighting constants through
//     otherwise-unused interpolator capacity.
//
// The native PS is patched only at the old vertex-lighting multiplication.
// Texture sampling, shadows, fog and family-specific alpha remain native.
//
// No direct SetVertexShader/SetPixelShader is used. Both wrappers are written
// through the engine's deferred GxState cache, matching native EffectBind.
//
// GPL-3.0-or-later.

#include "client/CWorldScene/WmoPerPixelCandidate.hpp"

#include "common/Log.hpp"
#include "game/Gx.hpp"
#include "offsets/engine/Shader.hpp"
#include "water/WaterDiagCore.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wxl::r8::materials
{
    namespace
    {
        namespace diag = wxl::waterdiag;
        namespace sh   = wxl::offsets::engine::shader;

        constexpr const char* kVsSimple30 =
            "868fbe1c6807c6dd978ae18f7c7176ac1764c40eb1b7c72f1b2e0fe28c598ca7";

        constexpr const char* kVsRaw30 =
            "929f23fcaa265eb0a40f22dd58774669132043d98bcd111e09a058806c1f380e";

        constexpr const char* kVsSimple60 =
            "b70c0dfa2206d332576336e0f166832101aa7cff9f5fddfb8343aff6a4677328";

        constexpr const char* kVsRaw60 =
            "1d29d928f3416de9e21eca61fd77bb1e8054a2d541e9cd84b2b9c145f6803fe7";

        constexpr const char* kPsDiffuse5 =
            "24c2c665054205b408bba34848906b73fadb33aba72abff3a1545d8804ff3a9b";

        constexpr const char* kPsDiffuse7 =
            "259824c48d1717aef69a51a09b04bd519a01e75afe4c3eec34b719d1b01f5bb1";

        constexpr const char* kPsOpaque5 =
            "6b2c56523812cdffe77383c1dbc481abe73d96bef4ef12808d996f1d5221def1";

        constexpr const char* kPsOpaque7 =
            "e761d8f215688ac549a49ca4faf20ec15d0ea3e0c8084985f0a8732bcdfa7937";

        thread_local bool g_restorePending = false;
        thread_local void* g_restoreVertex = nullptr;
        thread_local void* g_restorePixel = nullptr;

        std::unordered_map<void*, std::string>
            g_wrapperHashes;

        std::unordered_map<void*, void*>
            g_vertexPatches;

        std::unordered_map<void*, void*>
            g_pixelPatches;

        std::unordered_set<std::string>
            g_loggedFallbacks;

        std::unordered_set<std::string>
            g_loggedSubstitutions;

        typedef HRESULT(WINAPI* PFN_D3DAssemble)(
            LPCVOID,
            SIZE_T,
            LPCSTR,
            const D3D_SHADER_MACRO*,
            ID3DInclude*,
            UINT,
            ID3DBlob**,
            ID3DBlob**);

        typedef HRESULT(WINAPI* PFN_D3DDisassemble)(
            LPCVOID,
            SIZE_T,
            UINT,
            LPCSTR,
            ID3DBlob**);

        bool ReadMemory(
            std::uintptr_t address,
            void* destination,
            std::size_t bytes)
        {
            if (
                !address ||
                !destination ||
                bytes == 0)
            {
                return false;
            }

            SIZE_T copied = 0;

            return
                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(address),
                    destination,
                    bytes,
                    &copied) &&
                copied == bytes;
        }

        template<class T>
        bool ReadValue(
            std::uintptr_t address,
            T& value)
        {
            return
                ReadMemory(
                    address,
                    &value,
                    sizeof(value));
        }

        void* CollectionWrapper(
            std::uintptr_t collection,
            std::size_t slotsOffset,
            std::uint32_t index,
            std::uint32_t count)
        {
            if (
                !collection ||
                index >= count)
            {
                return nullptr;
            }

            std::uint32_t raw = 0;

            if (
                !ReadValue(
                    collection +
                        slotsOffset +
                        static_cast<std::uintptr_t>(index) *
                            sizeof(std::uint32_t),
                    raw) ||
                !raw)
            {
                return nullptr;
            }

            return
                reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(raw));
        }

        bool ReadWrapperBytecode(
            void* wrapper,
            std::vector<std::uint8_t>& bytes,
            std::uint32_t& version)
        {
            bytes.clear();
            version = 0;

            if (!wrapper)
                return false;

            const std::uintptr_t base =
                reinterpret_cast<std::uintptr_t>(
                    wrapper);

            std::uint32_t length = 0;
            std::uint32_t bytecode = 0;

            if (
                !ReadValue(
                    base +
                        sh::kCgxShaderByteLen,
                    length) ||
                !ReadValue(
                    base +
                        sh::kCgxShaderBytePtr,
                    bytecode) ||
                !bytecode ||
                length < sizeof(std::uint32_t) ||
                length > 0x20000u)
            {
                return false;
            }

            bytes.resize(
                length);

            if (
                !ReadMemory(
                    bytecode,
                    bytes.data(),
                    bytes.size()))
            {
                bytes.clear();
                return false;
            }

            std::memcpy(
                &version,
                bytes.data(),
                sizeof(version));

            return true;
        }

        std::string WrapperHash(
            void* wrapper)
        {
            const auto found =
                g_wrapperHashes.find(
                    wrapper);

            if (
                found !=
                    g_wrapperHashes.end())
            {
                return found->second;
            }

            std::vector<std::uint8_t> bytes;
            std::uint32_t version = 0;

            std::string hash =
                "UNAVAILABLE";

            if (
                ReadWrapperBytecode(
                    wrapper,
                    bytes,
                    version))
            {
                hash =
                    diag::Hex(
                        diag::Sha256::Of(
                            bytes.data(),
                            bytes.size()));
            }

            g_wrapperHashes.emplace(
                wrapper,
                hash);

            return hash;
        }

        std::string Trim(
            const std::string& input)
        {
            std::size_t begin = 0;
            std::size_t end = input.size();

            while (
                begin < end &&
                std::isspace(
                    static_cast<unsigned char>(
                        input[begin])))
            {
                ++begin;
            }

            while (
                end > begin &&
                std::isspace(
                    static_cast<unsigned char>(
                        input[end - 1])))
            {
                --end;
            }

            return
                input.substr(
                    begin,
                    end - begin);
        }

        void NormalizeAbsoluteOperands(
            std::string& line)
        {
            std::size_t open =
                line.find('|');

            while (
                open !=
                    std::string::npos)
            {
                const std::size_t close =
                    line.find(
                        '|',
                        open + 1);

                if (
                    close ==
                        std::string::npos)
                {
                    break;
                }

                std::string operand =
                    line.substr(
                        open + 1,
                        close - open - 1);

                if (
                    !operand.empty() &&
                    operand[0] == '-')
                {
                    operand.erase(
                        0,
                        1);
                }

                if (operand.empty())
                    break;

                const std::size_t swizzle =
                    operand.find('.');

                if (
                    swizzle ==
                        std::string::npos)
                {
                    operand +=
                        "_abs";
                }
                else
                {
                    operand.insert(
                        swizzle,
                        "_abs");
                }

                line.replace(
                    open,
                    close - open + 1,
                    operand);

                open =
                    line.find(
                        '|',
                        open +
                            operand.size());
            }
        }

        HMODULE ShaderCompiler()
        {
            HMODULE compiler =
                GetModuleHandleA(
                    "d3dcompiler_47.dll");

            return
                compiler
                    ? compiler
                    : LoadLibraryA(
                          "d3dcompiler_47.dll");
        }

        bool NormalizeDisassembly(
            const std::vector<std::uint8_t>& bytes,
            const char* profile,
            std::string& output)
        {
            output.clear();

            if (
                bytes.empty() ||
                !profile)
            {
                return false;
            }

            HMODULE const compiler =
                ShaderCompiler();

            const auto disassemble =
                reinterpret_cast<
                    PFN_D3DDisassemble>(
                    compiler
                        ? GetProcAddress(
                              compiler,
                              "D3DDisassemble")
                        : nullptr);

            if (!disassemble)
                return false;

            ID3DBlob* blob = nullptr;

            if (
                FAILED(
                    disassemble(
                        bytes.data(),
                        bytes.size(),
                        D3D_DISASM_INSTRUCTION_ONLY,
                        nullptr,
                        &blob)) ||
                !blob)
            {
                return false;
            }

            std::string text(
                static_cast<const char*>(
                    blob->GetBufferPointer()),
                blob->GetBufferSize());

            blob->Release();

            const std::size_t profileAt =
                text.find(
                    profile);

            if (
                profileAt ==
                    std::string::npos)
            {
                return false;
            }

            text.erase(
                0,
                profileAt);

            for (
                std::size_t i = 0;
                i < text.size();
                ++i)
            {
                const unsigned char value =
                    static_cast<unsigned char>(
                        text[i]);

                const bool valid =
                    value == '\n' ||
                    value == '\r' ||
                    value == '\t' ||
                    (
                        value >= 0x20 &&
                        value <= 0x7E
                    );

                if (!valid)
                {
                    text.resize(i);
                    break;
                }
            }

            std::istringstream stream(
                text);

            std::string line;

            while (
                std::getline(
                    stream,
                    line))
            {
                line =
                    Trim(
                        line);

                if (
                    line.empty() ||
                    line.compare(
                        0,
                        2,
                        "//") == 0 ||
                    line[0] == ';')
                {
                    continue;
                }

                if (
                    line.compare(
                        0,
                        4,
                        "def ") == 0 ||
                    line.compare(
                        0,
                        5,
                        "defi ") == 0 ||
                    line.compare(
                        0,
                        5,
                        "defb ") == 0)
                {
                    const std::size_t equals =
                        line.find(
                            " = ");

                    if (
                        equals !=
                            std::string::npos)
                    {
                        line.replace(
                            equals,
                            3,
                            ", ");
                    }
                }

                if (
                    line.compare(
                        0,
                        4,
                        "def ") == 0)
                {
                    const std::size_t comma =
                        line.find(',');

                    if (
                        comma !=
                            std::string::npos)
                    {
                        double a = 0.0;
                        double b = 0.0;
                        double c = 0.0;
                        double d = 0.0;

                        if (
                            std::sscanf(
                                line.c_str() +
                                    comma + 1,
                                " %lf, %lf, %lf, %lf",
                                &a,
                                &b,
                                &c,
                                &d) == 4)
                        {
                            char fixed[512] = {};

                            std::snprintf(
                                fixed,
                                sizeof(fixed),
                                "%s, %.9f, %.9f, %.9f, %.9f",
                                line.substr(
                                    0,
                                    comma).c_str(),
                                a,
                                b,
                                c,
                                d);

                            line =
                                fixed;
                        }
                    }
                }

                if (
                    line.compare(
                        0,
                        14,
                        "dcl_specular0 ") == 0)
                {
                    line.replace(
                        0,
                        14,
                        "dcl_color1 ");
                }

                NormalizeAbsoluteOperands(
                    line);

                output += line;
                output += '\n';
            }

            return
                !output.empty();
        }

        bool ReplaceExactlyOnce(
            std::string& text,
            const std::string& before,
            const std::string& after)
        {
            const std::size_t at =
                text.find(
                    before);

            if (
                at ==
                    std::string::npos)
            {
                return false;
            }

            if (
                text.find(
                    before,
                    at +
                        before.size()) !=
                    std::string::npos)
            {
                return false;
            }

            text.replace(
                at,
                before.size(),
                after);

            return true;
        }

        std::size_t AfterLastDeclaration(
            const std::string& text)
        {
            std::size_t position = 0;
            std::size_t best = 0;

            while (
                position <
                    text.size())
            {
                std::size_t end =
                    text.find(
                        '\n',
                        position);

                if (
                    end ==
                        std::string::npos)
                {
                    end =
                        text.size();
                }

                const std::string line =
                    Trim(
                        text.substr(
                            position,
                            end - position));

                if (
                    line.compare(
                        0,
                        3,
                        "dcl") == 0)
                {
                    best =
                        end <
                            text.size()
                                ? end + 1
                                : end;
                }

                position =
                    end <
                        text.size()
                            ? end + 1
                            : end;
            }

            return best;
        }

        bool PatchVertexAssembly(
            std::string& text)
        {
            if (
                !ReplaceExactlyOnce(
                    text,
                    "dcl_texcoord o2.xy\n",
                    "dcl_texcoord o2\n") ||
                !ReplaceExactlyOnce(
                    text,
                    "dcl_texcoord1 o3.xyz\n",
                    "dcl_texcoord1 o3\n") ||
                !ReplaceExactlyOnce(
                    text,
                    "dcl_texcoord2 o4.xyz\n",
                    "dcl_texcoord2 o4\n"))
            {
                return false;
            }

            if (
                text.find(
                    "dcl_texcoord6 ") !=
                    std::string::npos ||
                text.find(
                    "dcl_texcoord7 ") !=
                    std::string::npos)
            {
                return false;
            }

            const std::size_t declarationEnd =
                AfterLastDeclaration(
                    text);

            if (!declarationEnd)
                return false;

            text.insert(
                declarationEnd,
                "dcl_texcoord6 o9\n"
                "dcl_texcoord7 o10\n");

            const std::string stockTail =
                "mov o1, v2\n"
                "mov o2.xy, v3\n";

            const std::string patchedTail =
                "mov o1.xyz, v2\n"
                "mov_sat o1.w, v2.w\n"
                "mov o2.xy, v3\n"
                "mov o9.xyz, c12\n"
                "mov o9.w, c29.x\n"
                "mov o10.xyz, c10\n"
                "mov o10.w, c29.y\n"
                "mov o2.z, c11.x\n"
                "mov o2.w, c11.y\n"
                "mov o3.w, c11.z\n"
                "mov o4.w, c29.z\n";

            return
                ReplaceExactlyOnce(
                    text,
                    stockTail,
                    patchedTail);
        }

        bool PatchPixelAssembly(
            std::string& text)
        {
            if (
                !ReplaceExactlyOnce(
                    text,
                    "dcl_texcoord v1.xy\n",
                    "dcl_texcoord v1\n") ||
                !ReplaceExactlyOnce(
                    text,
                    "dcl_texcoord1 v2.xyz\n",
                    "dcl_texcoord1 v2\n") ||
                !ReplaceExactlyOnce(
                    text,
                    "dcl_texcoord2 v3.xyz\n",
                    "dcl_texcoord2 v3\n"))
            {
                return false;
            }

            if (
                text.find("r30") !=
                    std::string::npos ||
                text.find("r31") !=
                    std::string::npos ||
                text.find(
                    "dcl_texcoord6 ") !=
                    std::string::npos ||
                text.find(
                    "dcl_texcoord7 ") !=
                    std::string::npos)
            {
                return false;
            }

            const std::size_t declarationEnd =
                AfterLastDeclaration(
                    text);

            if (!declarationEnd)
                return false;

            text.insert(
                declarationEnd,
                "dcl_texcoord6 v8\n"
                "dcl_texcoord7 v9\n");

            const std::string nativeLightingUse =
                "mul r1.xyz, r0.w, v0\n";

            const std::string perPixelLighting =
                "nrm r30.xyz, v3\n"
                "dp3_sat r31.x, -v8, r30\n"
                "mov r31.y, v1.z\n"
                "mov r31.z, v1.w\n"
                "mov r31.w, v2.w\n"
                "mad_sat r30.xyz, r31.x, r31.yzww, v9\n"
                "mov r31.x, v8.w\n"
                "mov r31.y, v9.w\n"
                "mov r31.z, v3.w\n"
                "mad_sat r30.xyz, v0, r30, r31\n"
                "mul r1.xyz, r0.w, r30\n";

            return
                ReplaceExactlyOnce(
                    text,
                    nativeLightingUse,
                    perPixelLighting);
        }

        void* MakeShaderWrapper(
            const void* bytecode,
            std::uint32_t length,
            bool vertex)
        {
            auto* const device =
                static_cast<IDirect3DDevice9*>(
                    wxl::game::gx::RawDevice());

            if (
                !device ||
                !bytecode ||
                length == 0)
            {
                return nullptr;
            }

            void* shaderHandle = nullptr;

            if (vertex)
            {
                IDirect3DVertexShader9* shader =
                    nullptr;

                if (
                    FAILED(
                        device->CreateVertexShader(
                            static_cast<const DWORD*>(
                                bytecode),
                            &shader)) ||
                    !shader)
                {
                    return nullptr;
                }

                shaderHandle =
                    shader;
            }
            else
            {
                IDirect3DPixelShader9* shader =
                    nullptr;

                if (
                    FAILED(
                        device->CreatePixelShader(
                            static_cast<const DWORD*>(
                                bytecode),
                            &shader)) ||
                    !shader)
                {
                    return nullptr;
                }

                shaderHandle =
                    shader;
            }

            auto* const copy =
                new std::uint8_t[
                    length];

            std::memcpy(
                copy,
                bytecode,
                length);

            auto* const wrapper =
                new std::uint8_t[
                    sh::kCgxShaderWrapBytes]();

            *reinterpret_cast<void**>(
                wrapper +
                    sh::kCgxShaderHandle) =
                shaderHandle;

            *reinterpret_cast<std::uint32_t*>(
                wrapper +
                    sh::kCgxShaderCreated) =
                1;

            *reinterpret_cast<std::uint32_t*>(
                wrapper +
                    sh::kCgxShaderByteLen) =
                length;

            *reinterpret_cast<const void**>(
                wrapper +
                    sh::kCgxShaderBytePtr) =
                copy;

            return wrapper;
        }

        void* AssemblePatchedShader(
            const std::string& assembly,
            bool vertex,
            const char* label)
        {
            HMODULE const compiler =
                ShaderCompiler();

            const auto assemble =
                reinterpret_cast<
                    PFN_D3DAssemble>(
                    compiler
                        ? GetProcAddress(
                              compiler,
                              "D3DAssemble")
                        : nullptr);

            if (!assemble)
            {
                WLOG_WARN(
                    "r8-step11-perpixel: "
                    "D3DAssemble unavailable");

                return nullptr;
            }

            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;

            const HRESULT hr =
                assemble(
                    assembly.c_str(),
                    assembly.size(),
                    label,
                    nullptr,
                    nullptr,
                    0,
                    &code,
                    &errors);

            if (
                FAILED(hr) ||
                !code)
            {
                WLOG_WARN(
                    "r8-step11-perpixel: "
                    "assembly failed label=%s error=%s",
                    label ? label : "?",
                    errors
                        ? static_cast<const char*>(
                              errors->GetBufferPointer())
                        : "?");

                if (errors)
                    errors->Release();

                if (code)
                    code->Release();

                return nullptr;
            }

            if (errors)
                errors->Release();

            void* const wrapper =
                MakeShaderWrapper(
                    code->GetBufferPointer(),
                    static_cast<std::uint32_t>(
                        code->GetBufferSize()),
                    vertex);

            code->Release();

            return wrapper;
        }

        void* BuildPatchedVertex(
            void* stock)
        {
            std::vector<std::uint8_t> bytes;
            std::uint32_t version = 0;

            if (
                !ReadWrapperBytecode(
                    stock,
                    bytes,
                    version) ||
                version != 0xFFFE0300u)
            {
                return nullptr;
            }

            std::string assembly;

            if (
                !NormalizeDisassembly(
                    bytes,
                    "vs_3_0",
                    assembly) ||
                !PatchVertexAssembly(
                    assembly))
            {
                WLOG_WARN(
                    "r8-step11-perpixel: "
                    "vertex patch transform refused stock=%p",
                    stock);

                return nullptr;
            }

            return
                AssemblePatchedShader(
                    assembly,
                    true,
                    "wxlR8WmoPerPixelVS");
        }

        void* BuildPatchedPixel(
            void* stock)
        {
            std::vector<std::uint8_t> bytes;
            std::uint32_t version = 0;

            if (
                !ReadWrapperBytecode(
                    stock,
                    bytes,
                    version) ||
                version != 0xFFFF0300u)
            {
                return nullptr;
            }

            std::string assembly;

            if (
                !NormalizeDisassembly(
                    bytes,
                    "ps_3_0",
                    assembly) ||
                !PatchPixelAssembly(
                    assembly))
            {
                WLOG_WARN(
                    "r8-step11-perpixel: "
                    "pixel patch transform refused stock=%p",
                    stock);

                return nullptr;
            }

            return
                AssemblePatchedShader(
                    assembly,
                    false,
                    "wxlR8WmoPerPixelPS");
        }

        void* PatchedVertex(
            void* stock)
        {
            const auto found =
                g_vertexPatches.find(
                    stock);

            if (
                found !=
                    g_vertexPatches.end())
            {
                return found->second;
            }

            void* const patched =
                BuildPatchedVertex(
                    stock);

            g_vertexPatches.emplace(
                stock,
                patched);

            return patched;
        }

        void* PatchedPixel(
            void* stock)
        {
            const auto found =
                g_pixelPatches.find(
                    stock);

            if (
                found !=
                    g_pixelPatches.end())
            {
                return found->second;
            }

            void* const patched =
                BuildPatchedPixel(
                    stock);

            g_pixelPatches.emplace(
                stock,
                patched);

            return patched;
        }

        bool ExpectedRawVs(
            const std::string& selected,
            const std::string& raw)
        {
            if (
                selected ==
                    kVsSimple30)
            {
                return
                    raw ==
                        kVsRaw30;
            }

            if (
                selected ==
                    kVsSimple60)
            {
                return
                    raw ==
                        kVsRaw60;
            }

            return false;
        }

        bool ExpectedPixel(
            WmoFamily family,
            std::uint32_t pixIdx,
            const std::string& hash)
        {
            if (
                family ==
                    WmoFamily::Diffuse)
            {
                if (pixIdx == 5)
                    return hash == kPsDiffuse5;

                if (pixIdx == 7)
                    return hash == kPsDiffuse7;

                return false;
            }

            if (
                family ==
                    WmoFamily::Opaque)
            {
                if (pixIdx == 5)
                    return hash == kPsOpaque5;

                if (pixIdx == 7)
                    return hash == kPsOpaque7;

                return false;
            }

            return false;
        }

        void LogFallbackOnce(
            const char* reason,
            WmoFamily family,
            std::uint32_t vtxIdx,
            std::uint32_t pixIdx,
            const std::string& selectedVs,
            const std::string& rawVs,
            const std::string& ps)
        {
            std::string key =
                std::string(reason ? reason : "?") +
                "|" +
                std::to_string(
                    static_cast<int>(family)) +
                "|" +
                std::to_string(vtxIdx) +
                "|" +
                std::to_string(pixIdx) +
                "|" +
                selectedVs +
                "|" +
                rawVs +
                "|" +
                ps;

            if (
                !g_loggedFallbacks.insert(
                    key).second)
            {
                return;
            }

            WLOG_INFO(
                "r8-step11-perpixel: "
                "stock fallback reason=%s family=%d "
                "vtx=%u pix=%u "
                "selected_vs_sha=%s "
                "raw_vs_sha=%s ps_sha=%s",
                reason ? reason : "?",
                static_cast<int>(family),
                static_cast<unsigned>(vtxIdx),
                static_cast<unsigned>(pixIdx),
                selectedVs.c_str(),
                rawVs.c_str(),
                ps.c_str());
        }

        void LogSubstitutionOnce(
            WmoFamily family,
            std::uint32_t vtxIdx,
            std::uint32_t pixIdx,
            const std::string& selectedVs,
            const std::string& rawVs,
            const std::string& ps,
            void* customVs,
            void* customPs)
        {
            std::string key =
                std::to_string(
                    static_cast<int>(family)) +
                "|" +
                std::to_string(vtxIdx) +
                "|" +
                std::to_string(pixIdx) +
                "|" +
                selectedVs +
                "|" +
                ps;

            if (
                !g_loggedSubstitutions.insert(
                    key).second)
            {
                return;
            }

            WLOG_INFO(
                "r8-step11-perpixel: "
                "SUBSTITUTION PASS family=%d "
                "vtx=%u pix=%u "
                "selected_vs_sha=%s "
                "raw_vs_sha=%s ps_sha=%s "
                "custom_vs=%p custom_ps=%p "
                "lighting=simple_directional_per_pixel "
                "native_shadows=preserved "
                "native_fog=preserved native_alpha=preserved",
                static_cast<int>(family),
                static_cast<unsigned>(vtxIdx),
                static_cast<unsigned>(pixIdx),
                selectedVs.c_str(),
                rawVs.c_str(),
                ps.c_str(),
                customVs,
                customPs);
        }
    }


    void NoteWmoPerPixelNativeBind()
    {
        // Native EffectBind has just replaced any previous custom wrappers.
        // Therefore a restore belonging to the preceding draw is obsolete.
        g_restorePending = false;
        g_restoreVertex = nullptr;
        g_restorePixel = nullptr;
    }


    bool TryBindWmoPerPixelCandidate(
        WmoFamily family,
        std::uint32_t vtxIdx,
        std::uint32_t pixIdx)
    {
        if (
            family != WmoFamily::Diffuse &&
            family != WmoFamily::Opaque)
        {
            return false;
        }

        // Only native lighting-enabled permutations are candidates.
        if ((vtxIdx & 1u) == 0)
            return false;

        // 11B-02B1 deliberately leaves pix15 / alpha-test and every
        // unobserved PS permutation stock.
        if (
            pixIdx != 5 &&
            pixIdx != 7)
        {
            return false;
        }

        std::uintptr_t active = 0;

        if (
            !ReadValue(
                sh::kActiveCollection,
                active) ||
            !active)
        {
            return false;
        }

        void* const selectedVs =
            CollectionWrapper(
                active,
                sh::kCollectionVtxSlots,
                vtxIdx,
                90);

        const std::uint32_t rawVtxIdx =
            vtxIdx & ~1u;

        void* const rawVs =
            CollectionWrapper(
                active,
                sh::kCollectionVtxSlots,
                rawVtxIdx,
                90);

        void* const selectedPs =
            CollectionWrapper(
                active,
                sh::kCollectionPixSlots,
                pixIdx,
                16);

        if (
            !selectedVs ||
            !rawVs ||
            !selectedPs)
        {
            return false;
        }

        const std::string selectedVsHash =
            WrapperHash(
                selectedVs);

        const std::string rawVsHash =
            WrapperHash(
                rawVs);

        const std::string psHash =
            WrapperHash(
                selectedPs);

        if (
            !ExpectedRawVs(
                selectedVsHash,
                rawVsHash))
        {
            LogFallbackOnce(
                "selected_vs_not_simple_exact_pair",
                family,
                vtxIdx,
                pixIdx,
                selectedVsHash,
                rawVsHash,
                psHash);

            return false;
        }

        if (
            !ExpectedPixel(
                family,
                pixIdx,
                psHash))
        {
            LogFallbackOnce(
                "pixel_hash_not_exact_family_slot",
                family,
                vtxIdx,
                pixIdx,
                selectedVsHash,
                rawVsHash,
                psHash);

            return false;
        }

        void* const customVs =
            PatchedVertex(
                rawVs);

        void* const customPs =
            PatchedPixel(
                selectedPs);

        if (
            !customVs ||
            !customPs)
        {
            LogFallbackOnce(
                "patch_build_failed",
                family,
                vtxIdx,
                pixIdx,
                selectedVsHash,
                rawVsHash,
                psHash);

            return false;
        }

        void* const gxDevice =
            wxl::game::gx::RawGraphicsDevice();

        const auto setState =
            reinterpret_cast<
                sh::GxStateSetFn>(
                sh::kGxStateSet);

        if (
            !gxDevice ||
            !setState)
        {
            return false;
        }

        // Store the exact native pair selected by the just-completed
        // EffectBind. This is restored if this remains the final WMO bind
        // when the enclosing WMO render returns.
        g_restoreVertex =
            selectedVs;

        g_restorePixel =
            selectedPs;

        setState(
            gxDevice,
            sh::kStateVertexShader,
            customVs);

        setState(
            gxDevice,
            sh::kStatePixelShader,
            customPs);

        g_restorePending = true;

        LogSubstitutionOnce(
            family,
            vtxIdx,
            pixIdx,
            selectedVsHash,
            rawVsHash,
            psHash,
            customVs,
            customPs);

        return true;
    }


    void RestoreWmoPerPixelIfPending()
    {
        if (!g_restorePending)
            return;

        void* const gxDevice =
            wxl::game::gx::RawGraphicsDevice();

        const auto setState =
            reinterpret_cast<
                sh::GxStateSetFn>(
                sh::kGxStateSet);

        if (
            gxDevice &&
            setState)
        {
            setState(
                gxDevice,
                sh::kStateVertexShader,
                g_restoreVertex);

            setState(
                gxDevice,
                sh::kStatePixelShader,
                g_restorePixel);
        }

        g_restorePending = false;
        g_restoreVertex = nullptr;
        g_restorePixel = nullptr;
    }
}
