# Vector sample 2: whole register moves, vector cryptography (Zvk*), Zvfbf*, extra shapes
vmv1r.v v8, v16
vmv2r.v v8, v16
vmv4r.v v8, v16
vmv8r.v v8, v16
vaesdf.vv v8, v16
vaesdf.vs v8, v16
vaesef.vv v8, v16
vaesem.vv v8, v16
vaesdm.vs v8, v16
vaesz.vs v8, v16
vaeskf1.vi v8, v16, 3
vaeskf2.vi v8, v16, 9
vsha2ms.vv v8, v16, v24
vsha2ch.vv v8, v16, v24
vsha2cl.vv v8, v16, v24
vsm3c.vi v8, v16, 5
vsm3me.vv v8, v16, v24
vsm4k.vi v8, v16, 3
vsm4r.vv v8, v16
vsm4r.vs v8, v16
vghsh.vv v8, v16, v24
vgmul.vv v8, v16
vwsll.vi v8, v16, 3
vwsll.vx v8, v16, a1
vbrev.v v8, v16
vfwcvtbf16.f.f.v v8, v16
vfncvtbf16.f.f.w v8, v16
vfwmaccbf16.vv v8, v16, v24
vfwmaccbf16.vf v8, fa1, v24
vfwcvt.rtz.x.f.v v8, v16
vfwcvt.f.xu.v v8, v16
vfwcvt.f.x.v v8, v16
vfncvt.x.f.w v8, v16
vfncvt.f.x.w v8, v16
vfncvt.rtz.xu.f.w v8, v16
vfwadd.wv v8, v16, v24
vfwsub.wf v8, v16, fa1
vfwmsac.vv v8, v16, v24
vfwnmacc.vf v8, fa1, v24
vfwredusum.vs v8, v16, v24
vfwredosum.vs v8, v16, v24
vwredsumu.vs v8, v16, v24
vwredsum.vs v8, v16, v24
vsext.vf4 v8, v16
vzext.vf8 v8, v16
vle8.v v8, (a1), v0.t
vle8ff.v v8, (a1), v0.t
vlseg2e8ff.v v8, (a1)
vlsseg3e16.v v8, (a1), a2
vsseg2e8.v v8, (a1), v0.t
vluxseg2ei8.v v8, (a1), v16
vsoxseg2ei16.v v8, (a1), v16
vl2re8.v v8, (a1)
vs4r.v v8, (a1)
vfmin.vv v8, v16, v24, v0.t
vfmax.vf v8, v16, fa1, v0.t
vmfeq.vf v8, v16, fa1
vmfle.vv v8, v16, v24, v0.t
vfsqrt.v v8, v16, v0.t
vfclass.v v8, v16, v0.t
vfmerge.vfm v8, v16, fa1, v0
vfmv.f.s fa0, v16
vmslt.vv v8, v16, v24, v0.t
vmsltu.vx v8, v16, a1, v0.t
vmsle.vi v8, v16, 7, v0.t
vsll.vi v8, v16, 31
vsra.vi v8, v16, 7, v0.t
vnsrl.wi v8, v16, 7
vnclip.wi v8, v16, 5
vnclipu.wx v8, v16, a1, v0.t
vwmaccsu.vv v8, v16, v24
vwmaccus.vx v8, a1, v24
vredmaxu.vs v8, v16, v24, v0.t
vredmin.vs v8, v16, v24
vdiv.vx v8, v16, a1, v0.t
vrem.vv v8, v16, v24, v0.t
vslideup.vi v8, v16, 1, v0.t
vslidedown.vx v8, v16, a1, v0.t
vrgather.vv v8, v16, v24, v0.t
vcompress.vm v8, v16, v24
viota.m v8, v16, v0.t
vid.v v8, v0.t
vmv.s.x v8, a1
vmv.x.s a0, v16
vcpop.m a0, v16, v0.t
vfirst.m a0, v16, v0.t
vmsbf.m v8, v16, v0.t
vmsif.m v8, v16
vmsof.m v8, v16, v0.t
