# D extension: RV32 and RV64 differ in fcvt.d.w only by the width of a1
fcvt.d.w fa0, a1
fcvt.d.wu fa0, a1
fcvt.w.d a0, fa1
fcvt.wu.d a0, fa1
