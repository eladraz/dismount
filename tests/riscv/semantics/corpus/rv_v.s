# V extension, a sample (common). The registers are aligned for LMUL 8; the states pick LMUL from the register numbers.
# @mode: vector is implied by the v* mnemonics. Unicorn has no V: use --oracle qemu / spike.
vsetvli a0, a1, e8, m1, ta, ma
vsetvli a0, a1, e32, m2, tu, mu
vsetvli a0, zero, e64, m8, ta, ma
vsetvli zero, a1, e16, mf2, ta, mu
vsetvli zero, zero, e32, m1, ta, ma
vsetivli a0, 5, e16, m1, ta, ma
vsetivli zero, 31, e8, m8, tu, ma
vsetvl a0, a1, a2
vsetvl zero, zero, a2
vadd.vv v8, v16, v24
vadd.vv v8, v16, v24, v0.t
vadd.vx v8, v16, a1
vsub.vv v8, v16, v24
vsub.vv v8, v16, v24, v0.t
vsub.vx v8, v16, a1
vand.vv v8, v16, v24
vand.vv v8, v16, v24, v0.t
vand.vx v8, v16, a1
vor.vv v8, v16, v24
vor.vv v8, v16, v24, v0.t
vor.vx v8, v16, a1
vxor.vv v8, v16, v24
vxor.vv v8, v16, v24, v0.t
vxor.vx v8, v16, a1
vminu.vv v8, v16, v24
vminu.vv v8, v16, v24, v0.t
vminu.vx v8, v16, a1
vmin.vv v8, v16, v24
vmin.vv v8, v16, v24, v0.t
vmin.vx v8, v16, a1
vmaxu.vv v8, v16, v24
vmaxu.vv v8, v16, v24, v0.t
vmaxu.vx v8, v16, a1
vmax.vv v8, v16, v24
vmax.vv v8, v16, v24, v0.t
vmax.vx v8, v16, a1
vsll.vv v8, v16, v24
vsll.vv v8, v16, v24, v0.t
vsll.vx v8, v16, a1
vsrl.vv v8, v16, v24
vsrl.vv v8, v16, v24, v0.t
vsrl.vx v8, v16, a1
vsra.vv v8, v16, v24
vsra.vv v8, v16, v24, v0.t
vsra.vx v8, v16, a1
vmul.vv v8, v16, v24
vmul.vv v8, v16, v24, v0.t
vmul.vx v8, v16, a1
vmulh.vv v8, v16, v24
vmulh.vv v8, v16, v24, v0.t
vmulh.vx v8, v16, a1
vmulhu.vv v8, v16, v24
vmulhu.vv v8, v16, v24, v0.t
vmulhu.vx v8, v16, a1
vmulhsu.vv v8, v16, v24
vmulhsu.vv v8, v16, v24, v0.t
vmulhsu.vx v8, v16, a1
vdivu.vv v8, v16, v24
vdivu.vv v8, v16, v24, v0.t
vdivu.vx v8, v16, a1
vdiv.vv v8, v16, v24
vdiv.vv v8, v16, v24, v0.t
vdiv.vx v8, v16, a1
vremu.vv v8, v16, v24
vremu.vv v8, v16, v24, v0.t
vremu.vx v8, v16, a1
vrem.vv v8, v16, v24
vrem.vv v8, v16, v24, v0.t
vrem.vx v8, v16, a1
vsaddu.vv v8, v16, v24
vsaddu.vv v8, v16, v24, v0.t
vsaddu.vx v8, v16, a1
vsadd.vv v8, v16, v24
vsadd.vv v8, v16, v24, v0.t
vsadd.vx v8, v16, a1
vssubu.vv v8, v16, v24
vssubu.vv v8, v16, v24, v0.t
vssubu.vx v8, v16, a1
vssub.vv v8, v16, v24
vssub.vv v8, v16, v24, v0.t
vssub.vx v8, v16, a1
vaaddu.vv v8, v16, v24
vaaddu.vv v8, v16, v24, v0.t
vaaddu.vx v8, v16, a1
vaadd.vv v8, v16, v24
vaadd.vv v8, v16, v24, v0.t
vaadd.vx v8, v16, a1
vasubu.vv v8, v16, v24
vasubu.vv v8, v16, v24, v0.t
vasubu.vx v8, v16, a1
vasub.vv v8, v16, v24
vasub.vv v8, v16, v24, v0.t
vasub.vx v8, v16, a1
vsmul.vv v8, v16, v24
vsmul.vv v8, v16, v24, v0.t
vsmul.vx v8, v16, a1
vssrl.vv v8, v16, v24
vssrl.vv v8, v16, v24, v0.t
vssrl.vx v8, v16, a1
vssra.vv v8, v16, v24
vssra.vv v8, v16, v24, v0.t
vssra.vx v8, v16, a1
vadd.vi v8, v16, 5
vand.vi v8, v16, 5
vor.vi v8, v16, 5
vxor.vi v8, v16, 5
vsll.vi v8, v16, 5
vsrl.vi v8, v16, 5
vsra.vi v8, v16, 5
vsaddu.vi v8, v16, 5
vsadd.vi v8, v16, 5
vssrl.vi v8, v16, 5
vssra.vi v8, v16, 5
vrsub.vx v8, v16, a1
vrsub.vi v8, v16, -3
vadd.vi v8, v16, -16
vadd.vi v8, v16, 15
vmacc.vv v8, v16, v24
vnmsac.vv v8, v16, v24
vmadd.vx v8, a1, v24
vnmsub.vv v8, v16, v24
vmseq.vv v8, v16, v24
vmsne.vx v8, v16, a1
vmsltu.vv v8, v16, v24
vmslt.vx v8, v16, a1
vmsleu.vi v8, v16, 3
vmsgt.vi v8, v16, -2
vmsgtu.vx v8, v16, a1
vmerge.vvm v8, v16, v24, v0
vmerge.vxm v8, v16, a1, v0
vmerge.vim v8, v16, 7, v0
vmv.v.v v8, v16
vmv.v.x v8, a1
vmv.v.i v8, -5
vadc.vvm v8, v16, v24, v0
vsbc.vvm v8, v16, v24, v0
vmadc.vvm v8, v16, v24, v0
vmadc.vv v8, v16, v24
vmsbc.vv v8, v16, v24
vmv.x.s a0, v16
vmv.s.x v8, a1
vslideup.vx v8, v16, a1
vslidedown.vi v8, v16, 3
vslide1up.vx v8, v16, a1
vslide1down.vx v8, v16, a1
vrgather.vv v8, v16, v24
vrgather.vx v8, v16, a1
vrgather.vi v8, v16, 2
vrgatherei16.vv v8, v16, v24
vcompress.vm v8, v16, v24
vid.v v8
viota.m v8, v16
vcpop.m a0, v16
vfirst.m a0, v16
vmsbf.m v8, v16
vmsif.m v8, v16
vmsof.m v8, v16
vmand.mm v8, v16, v24
vmnand.mm v8, v16, v24
vmandn.mm v8, v16, v24
vmxor.mm v8, v16, v24
vmor.mm v8, v16, v24
vmnor.mm v8, v16, v24
vmorn.mm v8, v16, v24
vmxnor.mm v8, v16, v24
vredsum.vs v8, v16, v24
vredmax.vs v8, v16, v24
vredminu.vs v8, v16, v24
vredand.vs v8, v16, v24
vredor.vs v8, v16, v24
vredxor.vs v8, v16, v24
vwadd.vv v8, v16, v24
vwaddu.vx v8, v16, a1
vwmul.vv v8, v16, v24
vwmaccu.vv v8, v16, v24
vwadd.wv v8, v16, v24
vwsubu.wx v8, v16, a1
vnsrl.wv v8, v16, v24
vnsra.wi v8, v16, 3
vnclipu.wv v8, v16, v24
vnclip.wx v8, v16, a1
vzext.vf2 v8, v16
vsext.vf2 v8, v16
vzext.vf4 v8, v16
vsext.vf8 v8, v16
vle8.v v8, (a1)
vle8.v v8, (a1), v0.t
vse8.v v8, (a1)
vse8.v v8, (a1), v0.t
vlse8.v v8, (a1), a2
vsse8.v v8, (a1), a2
vle8ff.v v8, (a1)
vluxei8.v v8, (a1), v16
vloxei8.v v8, (a1), v16
vsuxei8.v v8, (a1), v16
vsoxei8.v v8, (a1), v16
vle16.v v8, (a1)
vle16.v v8, (a1), v0.t
vse16.v v8, (a1)
vse16.v v8, (a1), v0.t
vlse16.v v8, (a1), a2
vsse16.v v8, (a1), a2
vle16ff.v v8, (a1)
vluxei16.v v8, (a1), v16
vloxei16.v v8, (a1), v16
vsuxei16.v v8, (a1), v16
vsoxei16.v v8, (a1), v16
vle32.v v8, (a1)
vle32.v v8, (a1), v0.t
vse32.v v8, (a1)
vse32.v v8, (a1), v0.t
vlse32.v v8, (a1), a2
vsse32.v v8, (a1), a2
vle32ff.v v8, (a1)
vluxei32.v v8, (a1), v16
vloxei32.v v8, (a1), v16
vsuxei32.v v8, (a1), v16
vsoxei32.v v8, (a1), v16
vle64.v v8, (a1)
vle64.v v8, (a1), v0.t
vse64.v v8, (a1)
vse64.v v8, (a1), v0.t
vlse64.v v8, (a1), a2
vsse64.v v8, (a1), a2
vle64ff.v v8, (a1)
vlm.v v8, (a1)
vsm.v v8, (a1)
vl1re8.v v8, (a1)
vl2re16.v v8, (a1)
vl4re32.v v8, (a1)
vl8re64.v v8, (a1)
vs1r.v v8, (a1)
vs2r.v v8, (a1)
vs8r.v v8, (a1)
vlseg2e8.v v8, (a1)
vlseg3e16.v v8, (a1)
vsseg2e32.v v8, (a1)
vlsseg2e32.v v8, (a1), a2
vlseg4e64.v v8, (a1)
vfadd.vv v8, v16, v24
vfadd.vf v8, v16, fa1
vfsub.vv v8, v16, v24
vfsub.vf v8, v16, fa1
vfmul.vv v8, v16, v24
vfmul.vf v8, v16, fa1
vfdiv.vv v8, v16, v24
vfdiv.vf v8, v16, fa1
vfmin.vv v8, v16, v24
vfmin.vf v8, v16, fa1
vfmax.vv v8, v16, v24
vfmax.vf v8, v16, fa1
vfsgnj.vv v8, v16, v24
vfsgnj.vf v8, v16, fa1
vfsgnjn.vv v8, v16, v24
vfsgnjn.vf v8, v16, fa1
vfsgnjx.vv v8, v16, v24
vfsgnjx.vf v8, v16, fa1
vfrsub.vf v8, v16, fa1
vfrdiv.vf v8, v16, fa1
vfmacc.vv v8, v16, v24
vfmacc.vf v8, fa1, v24
vfnmacc.vv v8, v16, v24
vfmsac.vv v8, v16, v24
vfnmsub.vv v8, v16, v24
vfmadd.vf v8, fa1, v24
vfsqrt.v v8, v16
vfrsqrt7.v v8, v16
vfrec7.v v8, v16
vfclass.v v8, v16
vmfeq.vv v8, v16, v24
vmfne.vf v8, v16, fa1
vmflt.vv v8, v16, v24
vmfle.vf v8, v16, fa1
vmfgt.vf v8, v16, fa1
vmfge.vf v8, v16, fa1
vfmerge.vfm v8, v16, fa1, v0
vfmv.v.f v8, fa1
vfmv.f.s fa0, v16
vfmv.s.f v8, fa1
vfcvt.xu.f.v v8, v16
vfcvt.x.f.v v8, v16
vfcvt.f.xu.v v8, v16
vfcvt.f.x.v v8, v16
vfcvt.rtz.x.f.v v8, v16
vfcvt.rtz.xu.f.v v8, v16
vfwcvt.f.f.v v8, v16
vfncvt.f.f.w v8, v16
vfwadd.vv v8, v16, v24
vfwmul.vf v8, v16, fa1
vfwmacc.vv v8, v16, v24
vfncvt.rod.f.f.w v8, v16
vfredusum.vs v8, v16, v24
vfredosum.vs v8, v16, v24
vfredmax.vs v8, v16, v24
vfredmin.vs v8, v16, v24
vfslide1up.vf v8, v16, fa1
vfslide1down.vf v8, v16, fa1
vandn.vv v8, v16, v24
vandn.vx v8, v16, a1
vbrev8.v v8, v16
vrev8.v v8, v16
vclz.v v8, v16
vctz.v v8, v16
vcpop.v v8, v16
vrol.vv v8, v16, v24
vror.vv v8, v16, v24
vror.vi v8, v16, 3
vwsll.vv v8, v16, v24
vclmul.vv v8, v16, v24
vclmulh.vx v8, v16, a1
