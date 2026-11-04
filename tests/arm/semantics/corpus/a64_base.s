// A64 base instructions (no x18, x28: reserved by the host runner). Run with
//   run_semantics.py --arch a64 --file a64_base
// @feature: tags are informative; @state: tags add directed states on top of random ones.

// --- arithmetic and flags
add x0, x1, x2
add w0, w1, w2
adds x0, x1, x2
adds w3, w4, w5
sub x0, x1, x2, lsl #3
subs x0, x1, x2, asr #5
subs w0, w1, w2, uxtb #2
add x0, x1, w2, sxtw #2
add x0, sp, #16
adds x0, x1, #4095
cmp x1, #1
cmn w1, #1
neg x0, x1
negs w0, w1
adc x0, x1, x2
adcs x0, x1, x2   // @state: nzcv=2
sbc x0, x1, x2    // @state: nzcv=0
sbcs w0, w1, w2   // @state: nzcv=2 x1=80000000 x2=1
// --- logic and shifts
and x0, x1, x2
orr x0, x1, #0xff00ff00ff00ff00
eor w0, w1, w2, ror #7
ands x0, x1, #0xf0
bic x0, x1, x2
bics w0, w1, w2
orn x0, x1, x2
eon w0, w1, w2
lsl x0, x1, x2
lsr w0, w1, w2
asr x0, x1, #63
ror x0, x1, #17
// --- bitfields and moves
ubfx x0, x1, #5, #20
sbfx w0, w1, #3, #9
bfi x0, x1, #8, #16
bfxil w0, w1, #4, #8
sxtb x0, w1
uxth w0, w1
sxtw x0, w1
movz x0, #0x1234, lsl #32
movn w0, #5
movk x0, #0xbeef, lsl #16
mov x0, x1
mov w0, #-2
extr x0, x1, x2, #13
// --- multiply, divide, count
mul x0, x1, x2
madd w0, w1, w2, w3
msub x0, x1, x2, x3
smull x0, w1, w2
umulh x0, x1, x2
smulh x0, x1, x2
umaddl x0, w1, w2, x3
sdiv x0, x1, x2
udiv w0, w1, w2
sdiv x0, x1, x2   // @state: x1=8000000000000000 x2=ffffffffffffffff
clz x0, x1
cls w0, w1
rbit x0, x1
rev x0, x1
rev16 w0, w1
rev32 x0, x1
// --- conditional
csel x0, x1, x2, eq
csinc w0, w1, w2, hi
csinv x0, x1, x2, lt
csneg x0, x1, x2, vs
cset w0, ne
ccmp x1, x2, #4, eq
ccmn w1, #5, #9, hs
// --- memory
ldr x0, [x4]
ldr w0, [x4, #8]
ldrb w0, [x4, #1]
ldrsb x0, [x4, #1]
ldrsh w0, [x4, #2]
ldrsw x0, [x4, #4]
str x0, [x4, #16]
strb w0, [x4]
strh w0, [x4, #2]
ldr x0, [x4, x5, lsl #3]
ldr x0, [x4, #8]!
ldr x0, [x4], #8
str w0, [x4, #-4]!
ldur x0, [x4, #-3]
stur w0, [x4, #5]
ldp x0, x1, [x4]
ldp x0, x1, [x4, #16]!
stp x0, x1, [x4], #-16
stp w0, w1, [x4, #8]
ldpsw x0, x1, [x4, #8]
ldr x0, #64
ldar x0, [x4]
stlr w0, [x4]
ldxr x0, [x4]
stxr w1, x2, [x4]
ldaxr w0, [x4]
ldadd x1, x0, [x4]
ldclr w1, w0, [x4]
swp x1, x0, [x4]
cas x0, x1, [x4]
// unaligned atomics (FEAT_LSE2 on the host, an alignment fault on Unicorn): oracle_bugs.txt
ldxr x0, [x6]    // @state: x6=10100004001
ldadd x1, x0, [x6]    // @state: x6=10100004005
swp w1, w0, [x6]    // @state: x6=10100004002
// --- branches
b #8
bl #16
br x4
blr x4
ret
ret x4
cbz x0, #12
cbnz w0, #12
tbz x0, #3, #12
tbnz x0, #40, #12
b.eq #8
b.lt #-8
// --- misc
nop
adr x0, #20
adrp x0, #0x3000
crc32b w0, w1, w2
crc32cx w0, w1, x2
brk #7
udf #0
