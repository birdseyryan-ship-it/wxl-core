    ps_3_0
    def c0, -3.4482758, 3.41379309, 0.00999999978, 10
    def c1, 0.111111112, -1, 1.20000005, 2
    def c13, 0.300000012, 0.699999988, 0, 0
    def c14, 0, 1, 0.5, -0.200000003
    dcl_color v0
    dcl_texcoord v1.xy
    dcl_texcoord1 v2.xyz
    dcl_texcoord2 v3.xyz
    dcl_texcoord3 v4.xyz
    dcl_fog v5.x
    dcl_2d s0
    dcl_2d s4
    texld r0, v1, s0
    max r0.w, v4_abs.x, v4_abs.y
    mad_sat r0.w, r0.w, c0.x, c0.y
    add r1.x, -r0.w, c0.z
    cmp r1.x, r1.x, c14.x, c14.y
    if_ne r1.x, -r1.x
      if_lt c0.w, v2.z
        mad r1, v4.xyzx, c14.zzyx, c14.zzxx
        texldl r1, r1, s4
        mad r1.yz, v4.xxyw, c14.z, c14.z
        add r2.xy, r1.yzzw, c5
        mul r2.zw, c14.xyyx, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        add r2.xy, r1.yzzw, c7
        mul r2.zw, c14.xyyx, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        add r2.xy, r1.yzzw, c9
        mul r2.zw, c14.xyyx, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        add r2.xy, r1.yzzw, c11
        mul r2.zw, c14.xyyx, v4.xyzx
        texldl r2, r2, s4
        add r1.x, r1.x, r2.x
        mad r1.x, r1.x, -c14.w, -c14.y
        mad r1.x, r0.w, r1.x, c14.y
        min r2.x, r1.x, c14.y
      else
        mad r1, v4.xyzx, c14.zzyx, c14.zzxx
        texldl r1, r1, s4
        mad r1.yz, v4.xxyw, c14.z, c14.z
        add r3.xy, r1.yzzw, c5
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c6
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c7
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c8
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c9
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c10
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c11
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        add r3.xy, r1.yzzw, c12
        mul r3.zw, c14.xyyx, v4.xyzx
        texldl r3, r3, s4
        add r1.x, r1.x, r3.x
        mad r1.x, r1.x, c1.x, c1.y
        mad r0.w, r0.w, r1.x, c14.y
        min r2.x, r0.w, c14.y
      endif
    else
      mov r2.x, c14.y
    endif
    dp3 r0.w, v2, c3
    add_sat r0.w, r0.w, c3.w
    lrp r1.x, r0.w, c14.y, r2.x
    dp3 r0.w, c4, v3
    add r0.w, -r0_abs.w, c1.z
    mul r0.w, r0.w, r0.w
    mul_sat r0.w, r0.w, r0.w
    lrp r2.x, r0.w, c14.y, r1.x
    mad r0.w, r2.x, c13.x, c13.y
    mul r1.xyz, r0.w, v0
    mul r0.xyz, r0, r1
    mov r0.w, c1.w
    mad r0.xyz, r0, r0.w, -c2
    mad oC0.xyz, v5.x, r0, c2
    mov oC0.w, v0.w

// approximately 105 instruction slots used (29 texture, 76 arithmetic)
