; x87 in real mode (bits 16) for the semantics tests (dismount IR vs. Unicorn): the 16 bit operand and
; address sizes, the 14 byte environment images. Same format as tests/ia32/corpus. The x87 state is
; random (x87_adjust in states_v2.py), the memory operands are absolute.
; @file-feature: fpu
bits 16
fld1
fldpi
fld dword [0x2000]
fld qword [0x2000]
fld tword [0x2000]
fild word [0x2000]
fild dword [0x2000]
fild qword [0x2000]
fst dword [0x2000]
fstp qword [0x2000]
fstp tword [0x2000]
fist word [0x2000]
fistp dword [0x2000]
fistp qword [0x2000]
fadd dword [0x2000]
fsub qword [0x2000]
fmul st0, st1
fdiv st1, st0
faddp st1, st0
fcom st1
fcomp dword [0x2000]
fcomi st0, st1
fxch st1
fchs
fsqrt
frndint
fscale
fprem
fxam
ftst
fnstcw [0x2000]
fldcw [0x2000]
fnstsw [0x2000]
fnstsw ax
fnstenv [0x2000]
o32 fnstenv [0x2000]
fldenv [0x2000]
fnsave [0x2000]
o32 fnsave [0x2000]
frstor [0x2000]
fninit
fnclex
fxsave [0x2010]
fxrstor [0x2010]
fbld tword [0x2000]
fbstp tword [0x2000]
fwait
fnop
fcmovb st0, st1
ffree st1
fincstp
fdecstp
