# F extension (common)
fadd.s fa0, fa1, fa2
fadd.s fa0, fa1, fa2, rne
fadd.s fa0, fa1, fa2, rtz
fadd.s fa0, fa1, fa2, rdn
fadd.s fa0, fa1, fa2, rup
fadd.s fa0, fa1, fa2, rmm
fsub.s fa0, fa1, fa2
fsub.s fa0, fa1, fa2, rne
fsub.s fa0, fa1, fa2, rtz
fsub.s fa0, fa1, fa2, rdn
fsub.s fa0, fa1, fa2, rup
fsub.s fa0, fa1, fa2, rmm
fmul.s fa0, fa1, fa2
fmul.s fa0, fa1, fa2, rne
fmul.s fa0, fa1, fa2, rtz
fmul.s fa0, fa1, fa2, rdn
fmul.s fa0, fa1, fa2, rup
fmul.s fa0, fa1, fa2, rmm
fdiv.s fa0, fa1, fa2
fdiv.s fa0, fa1, fa2, rne
fdiv.s fa0, fa1, fa2, rtz
fdiv.s fa0, fa1, fa2, rdn
fdiv.s fa0, fa1, fa2, rup
fdiv.s fa0, fa1, fa2, rmm
fsqrt.s fa0, fa1
fsqrt.s fa0, fa1, rtz
fsqrt.s fa0, fa1, rdn
fsqrt.s fa0, fa1, rup
fsqrt.s fa0, fa1, rmm
fmadd.s fa0, fa1, fa2, fa3
fmadd.s fa0, fa1, fa2, fa3, rtz
fmadd.s fa0, fa1, fa2, fa3, rdn
fmsub.s fa0, fa1, fa2, fa3
fmsub.s fa0, fa1, fa2, fa3, rtz
fmsub.s fa0, fa1, fa2, fa3, rdn
fnmsub.s fa0, fa1, fa2, fa3
fnmsub.s fa0, fa1, fa2, fa3, rtz
fnmsub.s fa0, fa1, fa2, fa3, rdn
fnmadd.s fa0, fa1, fa2, fa3
fnmadd.s fa0, fa1, fa2, fa3, rtz
fnmadd.s fa0, fa1, fa2, fa3, rdn
fsgnj.s fa0, fa1, fa2
fsgnjn.s fa0, fa1, fa2
fsgnjx.s fa0, fa1, fa2
fmin.s fa0, fa1, fa2
fmax.s fa0, fa1, fa2
fsgnj.s fa0, fa1, fa1
fsgnjn.s fa0, fa1, fa1
fsgnjx.s fa0, fa1, fa1
feq.s a0, fa1, fa2
flt.s a0, fa1, fa2
fle.s a0, fa1, fa2
fclass.s a0, fa1
fcvt.w.s a0, fa1
fcvt.w.s a0, fa1, rne
fcvt.w.s a0, fa1, rtz
fcvt.w.s a0, fa1, rdn
fcvt.w.s a0, fa1, rup
fcvt.w.s a0, fa1, rmm
fcvt.s.w fa0, a1
fcvt.s.w fa0, a1, rne
fcvt.wu.s a0, fa1
fcvt.wu.s a0, fa1, rne
fcvt.wu.s a0, fa1, rtz
fcvt.wu.s a0, fa1, rdn
fcvt.wu.s a0, fa1, rup
fcvt.wu.s a0, fa1, rmm
fcvt.s.wu fa0, a1
fcvt.s.wu fa0, a1, rne
flw fa0, 0(a1)
flw fa0, 4(a1)
flw fa0, -4(a1)
fsw fa2, 0(a1)
fsw fa2, 4(a1)
fmv.x.w a0, fa1
fmv.w.x fa0, a1
fmv.s fa0, fa1
fneg.s fa0, fa1
fabs.s fa0, fa1
frcsr a0
fsrm a0, a1
fsflags a0, a1
frflags a0
frrm a0
