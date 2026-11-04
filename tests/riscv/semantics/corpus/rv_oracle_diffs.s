# Directed lines which document the differences between the oracles (oracle_bugs.txt has the verdicts).
# Run with: run_semantics.py --cross qemu,unicorn --file rv_oracle_diffs   (also qemu,spike and unicorn,spike)
# --- fmin / fmax with one NaN operand: the number is returned (minimumNumber / maximumNumber, spec 2.2)
fmax.d fa0, fa1, fa2        # @state: f11=7ff8000000000000 f12=3ff0000000000000
fmax.d fa0, fa1, fa2        # @state: f11=3ff0000000000000 f12=7ff8000000000000
fmin.d fa0, fa1, fa2        # @state: f11=7ff8000000000000 f12=3ff0000000000000
fmin.s fa0, fa1, fa2        # @state: f11=ffffffff7fc00000 f12=ffffffff3f800000
fmax.s fa0, fa1, fa2        # @state: f11=ffffffff7f800001 f12=ffffffff3f800000
fmax.s fa0, fa1, fa2        # @state: f11=ffffffff7f800001 f12=ffffffff7fc00000
fmin.d fa0, fa1, fa2        # @state: f11=8000000000000000 f12=0000000000000000
fmax.d fa0, fa1, fa2        # @state: f11=8000000000000000 f12=0000000000000000
# --- single precision results are NaN boxed
fadd.s fa0, fa1, fa2        # @state: f11=ffffffff3f800000 f12=ffffffff40000000
fcvt.s.d fa0, fa1           # @state: f11=3ff0000000000000
fmv.w.x fa0, a1             # @state: x11=3f800000
flw fa0, 0(a1)
# --- a single operand which is not NaN boxed reads as the canonical NaN
fadd.s fa0, fa1, fa2        # @state: f11=000000003f800000 f12=ffffffff40000000
fsgnj.s fa0, fa1, fa2       # @state: f11=000000003f800000 f12=ffffffff40000000
fcvt.d.s fa0, fa1           # @state: f11=000000003f800000
fmv.x.w a0, fa1             # @state: f11=000000003f800000
# --- misaligned atomics (Zama16b: complete inside an aligned 16 byte granule)
amoadd.w a0, a2, (a1)       # @state: x11=win+1001
amoadd.w a0, a2, (a1)       # @state: x11=win+100d
amoadd.w a0, a2, (a1)       # @state: x11=win+1004
amoswap.w a0, a2, (a1)      # @state: x11=win+1003
lr.w a0, (a1)               # @state: x11=win+1002
# --- sc which fails (no reservation) to an address outside of the window
sc.w a0, a2, (a1)           # @state: x11=win+8000
sc.w a0, a2, (a1)           # @state: x11=win+1000
# --- fcvt of the boundary values: the saturation
fcvt.w.s a0, fa1            # @state: f11=ffffffff7fc00000
fcvt.w.s a0, fa1            # @state: f11=ffffffff4f000000
fcvt.wu.s a0, fa1           # @state: f11=ffffffffbf800000
fcvt.wu.d a0, fa1           # @state: f11=7ff8000000000000
fcvt.w.d a0, fa1, rtz       # @state: f11=c1e0000000000000
# --- the scalar operand of vfwmaccbf16.vf which is not NaN boxed as a 16 bit value reads as the canonical NaN
vfwmaccbf16.vf v8, fa1, v24  # @state: vtype=c8 vl=4 f11=ffffffff40000000 v24=3f803f803f803f80 v8=00000000000000000000000000000000
vfwmaccbf16.vf v8, fa1, v24  # @state: vtype=c8 vl=4 f11=ffffffffffff4000 v24=3f803f803f803f80 v8=00000000000000000000000000000000
vfadd.vf v8, v16, fa1        # @state: vtype=c8 vl=4 f11=ffffffff00004400 v16=3c003c003c003c00
