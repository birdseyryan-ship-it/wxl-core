    ps_3_0
    def c0, -3.4482758, 3.41379309, 0, 1
    def c1, 10, 0.5, 1, 0
    def c6, 0.200000003, -11.1111107, 11, -1
    def c8, 1.20000005, 0.300000012, 0.699999988, 2
    dcl_color v0
    dcl_texcoord v1.xy
    dcl_texcoord1 v2.xyz
    dcl_texcoord2 v3.xyz
    dcl_texcoord3 v4
    dcl_texcoord4 v5
    dcl_texcoord5 v6
    dcl_fog v7.x
    dcl_2d s0
    dcl_2d s4
    dcl_2d s5
    dcl_2d s6
    dcl_2d s7
    texld r0, v1, s0
    mul oC0.w, r0.w, v0.w
    max r0.w, v4_abs.x, v4_abs.y
    mad_sat r0.w, r0.w, c0.x, c0.y
    cmp r0.w, -r0.w, c0.z, c0.w
    if_ne r0.w, -r0.w
      if_lt c1.x, v2.z
        mad r1, v4.xyzx, c1.yyzw, c1.yyww
        texldl r1, r1, s4
        mad r1.yz, v4.xxyw, c1.y, c1.y
        add r2.xy, r1.yzzw, c5
        mul r2.zw, c0.xywz, v4.xyzx
        texldl r2, r2, s4
        add r0.w, r1.x, r2.x
        add r2.xy, r1.yzzw, c7
        mul r2.zw, c0.xywz, v4.xyzx
        texldl r2, r2, s4
        add r0.w, r0.w, r2.x
        add r2.xy, r1.yzzw, c9
        mul r2.zw, c0.xywz, v4.xyzx
        texldl r2, r2, s4
        add r0.w, r0.w, r2.x
        add r1.xy, r1.yzzw, c11
        mul r1.zw, c0.xywz, v4.xyzx
        texldl r1, r1, s4
        add r0.w, r0.w, r1.x
        mul r0.w, r0.w, c6.x
      else
        mad r1, v4.xyzx, c1.yyzw, c1.yyww
        texldl r1, r1, s4
        mad r1.yz, v4.xxyw, c1.y, c1.y
        add r2.xy, r1.yzzw, c5
        mul r2.zw, c0.xywz, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        add r2.xy, r1.yzzw, c7
        mul r2.zw, c0.xywz, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        add r2.xy, r1.yzzw, c9
        mul r2.zw, c0.xywz, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        add r2.xy, r1.yzzw, c11
        mul r2.zw, c0.xywz, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        mul r0.w, r1.x, c6.x
      endif
    else
      mov r0.w, c0.w
    endif
    dp3 r1.x, v2, c3
    add_sat r1.x, r1.x, c3.w
    lrp r2.x, r1.x, c0.w, r0.w
    max r0.w, v5_abs.x, v5_abs.y
    if_lt r0.w, c0.w
      mad r1, v5.xyzx, c1.yyzw, c1.yyww
      texldl r1, r1, s5
      mad r1.yz, v5.xxyw, c1.y, c1.y
      add r3.xy, r1.yzzw, c5
      mul r3.zw, c0.xywz, v5.xyzx
      texldl r3, r3, s5
      add r0.w, r1.x, r3.x
      add r3.xy, r1.yzzw, c7
      mul r3.zw, c0.xywz, v5.xyzx
      texldl r3, r3, s5
      add r0.w, r0.w, r3.x
      add r3.xy, r1.yzzw, c9
      mul r3.zw, c0.xywz, v5.xyzx
      texldl r3, r3, s5
      add r0.w, r0.w, r3.x
      add r1.xy, r1.yzzw, c11
      mul r1.zw, c0.xywz, v5.xyzx
      texldl r1, r1, s5
      add r0.w, r0.w, r1.x
      mul r0.w, r0.w, c6.x
    else
      max r1.x, v6_abs.x, v6_abs.y
      if_lt r1.x, c0.w
        mad r1, v6.xyzx, c1.yyzw, c1.yyww
        texldl r1, r1, s6
        mad r1.yz, v6.xxyw, c1.y, c1.y
        add r3.xy, r1.yzzw, c5
        mul r3.zw, c0.xywz, v6.xyzx
        texldl r3, r3, s6
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c7
        mul r3.zw, c0.xywz, v6.xyzx
        texldl r3, r3, s6
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c9
        mul r3.zw, c0.xywz, v6.xyzx
        texldl r3, r3, s6
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c11
        mul r3.zw, c0.xywz, v6.xyzx
        texldl r3, r3, s6
        add r1.x, r1.x, r3.x
        mul r0.w, r1.x, c6.x
      else
        mad r1.xw, v4.w, c1.yyzw, c1.yyzw
        mad r1.y, v5.w, c1.y, c1.y
        mov r1.z, v6.w
        texldl r3, r1, s7
        add r4.xy, r1, c5
        mov r4.zw, r1
        texldl r5, r4, s7
        add r1.z, r3.x, r5.x
        add r4.xy, r1, c7
        texldl r3, r4, s7
        add r1.z, r1.z, r3.x
        add r4.xy, r1, c9
        texldl r3, r4, s7
        add r1.z, r1.z, r3.x
        add r4.xy, r1, c11
        texldl r3, r4, s7
        add r1.x, r1.z, r3.x
        mov r1.y, v4.w
        mov r1.z, v5.w
        max r2.y, r1_abs.y, r1_abs.z
        mad_sat r1.y, r2.y, c6.y, c6.z
        mad r1.x, r1.x, c6.x, c6.w
        mad r0.w, r1.y, r1.x, c0.w
      endif
    endif
    min r1.x, r0.w, r2.x
    dp3 r0.w, c4, v3
    add r0.w, -r0_abs.w, c8.x
    mul r0.w, r0.w, r0.w
    mul_sat r0.w, r0.w, r0.w
    lrp r2.x, r0.w, c0.w, r1.x
    mad r0.w, r2.x, c8.y, c8.z
    mul r1.xyz, r0.w, v0
    mul r0.xyz, r0, r1
    mov r0.w, c8.w
    mad r0.xyz, r0, r0.w, -c2
    mad oC0.xyz, v7.x, r0, c2

// approximately 171 instruction slots used (51 texture, 120 arithmetic)
