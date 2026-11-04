# Zicbom/Zicboz/Zicbop, hints, Zimop/Zcmop, Zawrs, Zabha, Zacas, Zfbfmin, scalar crypto (common to RV32 and RV64)
cbo.zero (a1)
cbo.clean (a1)
cbo.flush (a1)
cbo.inval (a1)
prefetch.i 0(a1)
prefetch.r 64(a1)
prefetch.w -64(a1)
pause
ntl.p1
ntl.pall
ntl.s1
ntl.all
mop.r.0 a0, a1
mop.r.5 a0, a1
mop.r.31 a0, a1
mop.rr.0 a0, a1, a2
mop.rr.7 a0, a1, a2
c.mop.1
c.mop.3
c.mop.15
wrs.nto
wrs.sto
amoswap.b a0, a2, (a1)
amoadd.b a0, a2, (a1)
amoxor.b a0, a2, (a1)
amoand.b a0, a2, (a1)
amoor.b a0, a2, (a1)
amomin.b a0, a2, (a1)
amomax.b a0, a2, (a1)
amominu.b a0, a2, (a1)
amomaxu.b a0, a2, (a1)
amoswap.h a0, a2, (a1)
amoadd.h a0, a2, (a1)
amoxor.h a0, a2, (a1)
amoand.h a0, a2, (a1)
amoor.h a0, a2, (a1)
amomin.h a0, a2, (a1)
amomax.h a0, a2, (a1)
amominu.h a0, a2, (a1)
amomaxu.h a0, a2, (a1)
amocas.b a0, a2, (a1)
amocas.h a0, a2, (a1)
amocas.w a0, a2, (a1)
amocas.w.aqrl a0, a2, (a1)
fcvt.bf16.s fa0, fa1
fcvt.s.bf16 fa0, fa1
fcvt.bf16.s fa0, fa1, rtz
fcvt.bf16.s fa0, fa1, rup
sha256sig0 a0, a1
sha256sig1 a0, a1
sha256sum0 a0, a1
sha256sum1 a0, a1
sm3p0 a0, a1
sm3p1 a0, a1
sm4ed a0, a1, a2, 1
sm4ks a0, a1, a2, 3
