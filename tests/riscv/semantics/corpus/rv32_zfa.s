# Zfa, RV32 only (fmvh.x.d / fmvp.d.x move the halves of a double)
fmvh.x.d a0, fa1
fmvp.d.x fa0, a1, a2
fcvtmod.w.d a0, fa1, rtz
