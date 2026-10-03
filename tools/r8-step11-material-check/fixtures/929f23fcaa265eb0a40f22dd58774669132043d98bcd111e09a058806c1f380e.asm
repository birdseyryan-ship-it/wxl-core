    vs_3_0
    def c0, 1, 0, 0, 0
    dcl_position v0
    dcl_normal v1
    dcl_color v2
    dcl_texcoord v3
    dcl_position o0
    dcl_color o1
    dcl_texcoord o2.xy
    dcl_texcoord1 o3.xyz
    dcl_texcoord2 o4.xyz
    dcl_texcoord3 o5.xyz
    dcl_fog o6.x
    dp3 r0.x, c31, v1
    dp3 r0.y, c32, v1
    dp3 r0.z, c33, v1
    dp3 r0.w, r0, r0
    rsq r0.w, r0.w
    mul o4.xyz, r0, r0.w
    mov r0.w, c0.x
    dp4 r0.x, c31, v0
    dp4 r0.y, c32, v0
    dp4 r0.z, c33, v0
    dp4 o0.x, c2, r0
    dp4 o0.y, c3, r0
    dp4 o0.z, c4, r0
    dp4 o0.w, c5, r0
    mad r1.x, r0.z, c30.x, c30.y
    max r1.x, r1.x, c0.y
    pow r2.x, r1.x, c30.z
    min o6.x, r2.x, c0.x
    dp4 o5.x, r0, c224
    dp4 o5.y, r0, c225
    dp4 o5.z, r0, c226
    mov o3.xyz, r0
    mov o1, v2
    mov o2.xy, v3

// approximately 26 instruction slots used
