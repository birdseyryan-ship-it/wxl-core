// R6 P01/A0/B1/Q0 Classic-water shader translation. GPL-3.0-or-later.
//
// Governing source equations:
//   launch Classic 1.13.2.31650 ProcWater VS slot 0000
//   launch Classic 1.13.2.31650 ProcWaterAbove PS slot 0003
//
// This is an equation-preserving SM3 translation for the deliberately narrow
// selector-5 / disabled-VolumeFog specialization. It is not claimed to be
// byte-identical to the original SM5 DXBC. Constants retain their recovered
// CB1 row numbers as D3D9 c-register numbers. The selector-5 CB5 rows retain
// their recovered row numbers at c40..c47.
#pragma once

namespace wxl::water::slot3::shaders
{
inline constexpr const char kVertexHlsl[] = R"HLSL(
float4 C0  : register(c0);
float4 C1  : register(c1);
float4 C2  : register(c2);
float4 C3  : register(c3);
float4 C4  : register(c4);
float4 C5  : register(c5);
float4 C6  : register(c6);
float4 C7  : register(c7);
float4 C8  : register(c8);
float4 C9  : register(c9);
float4 C10 : register(c10);
float4 C11 : register(c11);
float4 C12 : register(c12);
float4 C13 : register(c13);
float4 C15 : register(c15);
float4 C16 : register(c16);
float4 C17 : register(c17);
float4 C19 : register(c19);
float4 C20 : register(c20);
float4 C21 : register(c21);
float4 C23 : register(c23);
float4 C24 : register(c24);
float4 C25 : register(c25);
float4 C27 : register(c27);
float4 C28 : register(c28);

struct VSIn
{
    float3 position : POSITION0;
    float4 color    : COLOR0;
    float2 uv0      : TEXCOORD0;
    float2 uv1      : TEXCOORD1;
};

struct VSOut
{
    float4 position : POSITION0;
    float3 t0       : TEXCOORD0;
    float4 t1       : TEXCOORD1;
    float4 t2       : TEXCOORD2;
    float4 t3       : TEXCOORD3;
    float4 t4       : TEXCOORD4;
    float3 t5       : TEXCOORD5;
    float4 t6       : TEXCOORD6;
    float3 color    : COLOR0;
};

VSOut main(VSIn v)
{
    VSOut o;
    float4 r0 = 0.0;
    float4 r1 = 0.0;
    float4 r2 = 0.0;

    r0 = v.position.y * C5;
    r0 = v.position.x * C4 + r0;
    r0 = v.position.z * C6 + r0;
    r0 = r0 + C7;

    r1 = r0.y * C9;
    r1 = r0.x * C8 + r1;
    r1 = r0.z * C10 + r1;
    r1 = r0.w * C11 + r1;

    o.t0 = r0.xyz;
    o.position = r1;
    o.t6 = r1;

    r0.xy = v.uv0 + float2(0.5, 0.5);
    r0.xy *= float2(-0.03125, -0.03125);
    r0.yz = r0.yy * float2(C17.x, C17.y);
    r0.xy = r0.xx * C16.xy + r0.yz;
    r0.xy += C19.xy;

    r0.zw = r0.yy * float2(C25.y, C25.x);
    r0.zw = r0.xx * float2(C24.y, C24.x) + r0.zw;
    o.t1.zw = r0.zw + float2(C27.y, C27.x);

    r0.zw = v.uv0 * float2(0.03125, 0.03125);
    r1.xy = r0.ww * C13.xy;
    r0.zw = r0.zz * C12.xy + r1.xy;
    r0.zw += C15.xy;

    r1.xy = r0.ww * float2(C25.y, C25.x);
    r1.xy = r0.zz * float2(C24.y, C24.x) + r1.xy;
    o.t1.xy = r1.xy + float2(C27.y, C27.x);

    r1.x = C28.w * 32.0;
    r0.xy *= r1.xx;
    r1.xy = r0.zw * r1.xx;
    r2.xy = r0.zw * float2(228.0, 228.0);

    r0.yz = r0.yy * float2(C25.y, C25.x);
    r0.xy = r0.xx * float2(C24.y, C24.x) + r0.yz;
    o.t2.zw = r0.xy + float2(C27.y, C27.x);

    r0.xy = r1.yy * float2(C25.y, C25.x);
    r0.xy = r1.xx * float2(C24.y, C24.x) + r0.xy;
    o.t2.xy = r0.xy + float2(C27.y, C27.x);

    r0.xy = C28.xx * float2(4.0, 2.0) + v.position.xy;
    r0.x = sin(r0.x);
    r0.y = cos(r0.y);
    r1.z = r0.x * 0.05 + r2.x;
    r1.xy = r0.xy * float2(0.1, 0.1);
    r1.w = r0.y * 0.05 + r2.y;
    r2.zw = float2(0.5, 0.5);
    o.t3 = r1 + r2;

    r0.xy = v.uv1.y * C21.xy;
    r0.xy = v.uv1.x * C20.xy + r0.xy;
    o.t4.xy = r0.xy + C23.xy;
    o.t4.zw = v.uv1;

    r0.xyz = v.position.y * C1.xyz;
    r0.xyz = v.position.x * C0.xyz + r0.xyz;
    r0.xyz = v.position.z * C2.xyz + r0.xyz;
    o.t5 = r0.xyz + C3.xyz;

    o.color = v.color.xyz;
    return o;
}
)HLSL";

inline constexpr const char kPixelHlsl[] = R"HLSL(
sampler2D SceneColour : register(s0);
sampler2D Reflection  : register(s1);
sampler2D NormalWave  : register(s5);
sampler2D LinearDepth : register(s6);
sampler2D Chop        : register(s7);

float4 C0  : register(c0);
float4 C1  : register(c1);
float4 C2  : register(c2);
float4 C3  : register(c3);
float4 C4  : register(c4);
float4 C5  : register(c5);
float4 C6  : register(c6);
float4 C7  : register(c7);
float4 C13 : register(c13);
float4 C14 : register(c14);
float4 C15 : register(c15);
float4 C16 : register(c16);
float4 C17 : register(c17);
float4 C18 : register(c18);
float4 C21 : register(c21);
float4 C28 : register(c28);
float4 C31 : register(c31);
float4 C32 : register(c32);

// Exact selector-5 CB5 rows. Row 41 is intentionally retained by the CPU
// bank but is not read by this permutation.
float4 B40 : register(c40);
float4 B42 : register(c42);
float4 B43 : register(c43);
float4 B44 : register(c44);
float4 B45 : register(c45);
float4 B46 : register(c46);
float4 B47 : register(c47);

struct PSIn
{
    float2 position : VPOS;
    float3 v1 : TEXCOORD0;
    float4 v2 : TEXCOORD1;
    float4 v3 : TEXCOORD2;
    float4 v4 : TEXCOORD3;
    float4 v5 : TEXCOORD4;
    float3 v6 : TEXCOORD5;
    float4 v7 : TEXCOORD6;
    float3 v8 : COLOR0;
};

float4 main(PSIn v) : COLOR0
{
    float4 r0 = 0.0;
    float4 r1 = 0.0;
    float4 r2 = 0.0;
    float4 r3 = 0.0;
    float4 r4 = 0.0;
    float4 r5 = 0.0;
    float4 r6 = 0.0;
    float4 r7 = 0.0;
    float4 r8 = 0.0;
    float4 r9 = 0.0;
    float4 r10 = 0.0;
    float4 outColor = 0.0;

    r0.x = dot(v.v6, v.v6);
    r0.x = sqrt(r0.x);
    r0.yzw = v.v6 / r0.xxx;

    r1.xy = v.v7.xy / v.v7.ww;
    r1.xy = r1.xy * C31.xy + C31.zw;
    r1.z = tex2D(LinearDepth, r1.xy).x;
    r1.z = r1.z - v.v1.z;
    r1.w = r1.z / v.v1.z;

    r2.xy = tex2D(NormalWave, v.v2.xy).xy;
    r2.zw = tex2D(NormalWave, v.v2.zw).xy;
    r2.xy = r2.zw + r2.xy;
    r2.xy += float2(-1.0, -1.0);

    r2.zw = tex2D(NormalWave, v.v3.xy).xy;
    r3.xy = tex2D(NormalWave, v.v3.zw).xy;
    r2.zw = r2.zw + r3.xy;
    r2.zw += float2(-1.0, -1.0);
    r2.zw *= float2(1.4, 1.4);

    r3.x = r0.x * 0.006667;
    r0.x = r0.x * 0.001 + 0.2;
    r0.x = min(r0.x, 1.0);
    r2.xy = r2.xy * float2(2.0, 2.0) - r2.zw;
    r2.xy = r0.xx * r2.xy + r2.zw;
    r2.xy *= C28.ww;
    r2.z = 1.0;

    r0.x = dot(r2.xyz, r2.xyz);
    r0.x = rsqrt(r0.x);
    r2.xy = r0.xx * r2.xy;
    r4.xy = r2.xy * C28.yy;
    r4.z = 1.0;
    r0.x = dot(r4.xyz, r4.xyz);
    r0.x = rsqrt(r0.x);
    r2.xzw = r0.xxx * r4.xyz;

    r3.y = saturate(dot(-r0.yzw, r2.xzw));
    r3.y = 1.0 - r3.y;
    r3.z = r3.y * r3.y;
    r3.w = saturate(C0.z);

    r5.xyz = C2.xyz * r3.www + C1.xyz;
    r5.xyz *= v.v8;
    r2.y *= 0.1;
    r2.y = abs(r2.y) + v.v5.y;

    r6 = C18 - C17;
    r6 = r2.yyyy * r6 + C17;
    r7.xyz = r5.xyz * r6.xyz;
    r8.xy = r2.xz * float2(0.1, 0.1);

    r1.w *= 100.0;
    r2.y = 1.2 - r3.y * r3.y;
    r1.w = saturate(r1.w * r2.y);
    r3.y = r1.w * r8.x;
    r3.w = r1.w * r8.y;
    r8.xy = r8.xy * r1.ww + r1.xy;
    r8.zw = r3.zz * float2(0.5, 0.5) + float2(0.1, 0.2);

    r9.xyz = v.v6.y * float3(C14.x, C14.y, C14.w);
    r9.xyz = v.v6.x * float3(C13.x, C13.y, C13.w) + r9.xyz;
    r9.xyz = v.v6.z * float3(C15.x, C15.y, C15.w) + r9.xyz;
    r9.xyz += float3(C16.x, C16.y, C16.w);
    r9.w = -r9.y;
    r9.xy = float2(r9.x, r9.w) / r9.zz;
    r9.xy += float2(1.0, 1.0);
    r9.xy *= float2(0.5, 0.5);
    r9.xyz = tex2D(Reflection, r9.xy).xyz;

    r10.xyz = r6.xyz * r5.xyz - r9.xyz;
    r9.xyz = r10.xyz * float3(0.6, 0.6, 0.6) + r9.xyz;
    r10.xyz = r7.xyz * C5.xyz;
    r10.xyz *= C4.www;
    r1.w = saturate(dot(r2.xzw, -C4.xyz));
    r9.xyz = r10.xyz * r1.www + r9.xyz;

    r1.w = tex2D(LinearDepth, r8.xy).x;
    r1.w = (v.v1.z < r1.w) ? 1.0 : 0.0;
    r1.xy = r1.ww * r3.yw + r1.xy;

    float3 sceneSample = tex2D(SceneColour, r1.xy).xyz;
    r1.x = sceneSample.x;
    r1.y = sceneSample.y;
    r1.w = sceneSample.z;

    r2.y = 1.0 - r3.z * 0.1;
    r2.y = r6.w * r2.y;
    r5.xyz = r6.xyz * r5.xyz - float3(r1.x, r1.y, r1.w);
    float3 refracted = r2.yyy * r5.xyz + float3(r1.x, r1.y, r1.w);
    r1.x = refracted.x;
    r1.y = refracted.y;
    r1.w = refracted.z;

    r5.xyz = r9.xyz - float3(r1.x, r1.y, r1.w);
    refracted = r8.zzz * r5.xyz + float3(r1.x, r1.y, r1.w);
    r1.x = refracted.x;
    r1.y = refracted.y;
    r1.w = refracted.z;

    r5.xyz = C0.xyz - r0.yzw;
    r0.y = dot(r5.xyz, r5.xyz);
    r0.y = rsqrt(r0.y);
    r5.xyz = r0.yyy * r5.xyz;
    r0.y = saturate(dot(r5.xyz, r2.xzw));
    r0.y = log2(r0.y);
    r0.y *= C3.w;
    r0.y = exp2(r0.y);

    r0.z = r4.z * r0.x - 0.8;
    r0.z = max(r0.z, 0.0);
    r0.y = r0.z + r0.y;
    r5.xyz = r0.yyy * C3.xyz;
    r3.yzw = r3.zzz * r5.xyz;

    r0.yz = r8.ww * float2(1.2, 2.5);
    r0.yz = float2(1.0, 1.0) / r0.yz;
    r0.y = saturate(r0.y * r1.z);
    r2.y = r0.y * -2.0 + 3.0;
    r0.y = r0.y * r0.y;
    r4.z = r0.y * r2.y;

    r0.w = 1.0 - r0.w;
    r0.w *= r1.z;
    r0.z = saturate(r0.z * r0.w);
    r0.w = r0.z * -2.0 + 3.0;
    r0.z = r0.z * r0.z;
    r0.z = 1.0 - r0.w * r0.z;
    r0.z *= C21.y;

    const float chopScale0 = r0.x;
    r0.x = v.v4.x - r4.x * chopScale0;
    r0.w = v.v4.y - r4.y * chopScale0;
    float3 chop0 = tex2D(Chop, float2(r0.x, r0.w)).xyz;
    r4.x = chop0.x;
    r4.y = chop0.y;
    r4.w = chop0.z;

    r0.x = v.v4.z + r2.z * 0.2;
    r0.w = v.v4.w + r2.x * 0.2;
    float3 chop1 = tex2D(Chop, float2(r0.x, r0.w)).xyz;
    r5.xyz = chop1;
    r4.x += r5.x;
    r4.y += r5.y;
    r4.w += r5.z;

    r0.x = min(r3.x, 1.0);
    r0.x = 1.1 - r0.x;
    r0.x *= r0.z;
    r0.x *= r2.w;

    float3 waterRgb;
    waterRgb.x = r0.x * r4.x + r1.x;
    waterRgb.y = r0.x * r4.y + r1.y;
    waterRgb.z = r0.x * r4.w + r1.w;

    r1.z = saturate(-r1.z);
    r0.y = 1.0 - r2.y * r0.y;
    outColor.w = r1.z * r0.y + r4.z;

    r0.y = saturate(dot(C0.xyz, r2.xzw));
    r0.y *= C32.z;
    r1.xyz = r0.yyy * C2.xyz;
    r0.xyz = r1.xyz * r7.xyz + waterRgb;
    r0.xyz = C32.www * r3.yzw + r0.xyz;

    // Exact selector-5 CB5 atmospheric specialization. With the proven bank:
    // B42.xyz = 0, B43.yzw = 0, B44.x = 1, B46.x = 0. Therefore the
    // recovered directional-colour path cannot be selected and the atmospheric
    // colour is exactly zero. The full block algebraically reduces to the
    // distance blend below; this avoids evaluating the original dead 1/(1-1)
    // path while preserving the selected result exactly.
    const float distanceToOrigin = sqrt(dot(v.v1, v.v1));
    const float heightBlend = saturate((B40.y - distanceToOrigin) / (B40.y - B40.x));
    r0.xyz *= heightBlend;

    // Exact disabled/unsupported VolumeFog specialization: the proven 1x1x1
    // zero t16 makes the recovered fog tail algebraically return pre-fog RGB.
    outColor.xyz = r0.xyz;
    return outColor;
}
)HLSL";
}
