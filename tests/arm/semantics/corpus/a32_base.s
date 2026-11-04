// A32 (ARM state) instructions, assembled for armv8.2-a (CRC32, crypto, VFPv4, Advanced SIMD)
// --- data processing
add r0, r1, r2
adds r0, r1, r2
adc r0, r1, r2       // @state: apsr=20000000
adcs r0, r1, #255
sub r0, r1, r2, lsl #3
subs r0, r1, r2, asr #5
sbc r0, r1, r2       // @state: apsr=00000000
rsb r0, r1, #0
rsbs r0, r1, r2, lsr #7
rsc r0, r1, r2
and r0, r1, r2
orr r0, r1, #0xff00
eor r0, r1, r2, ror #9
bic r0, r1, r2
bics r0, r1, r2, lsl r3
mov r0, r1
movs r0, r1, lsl #31
movw r0, #0x1234
movt r0, #0xbeef
mvn r0, r1
mvns r0, #5
cmp r1, r2
cmn r1, #1
tst r1, r2
teq r1, #0x80000000
lsl r0, r1, r2
lsrs r0, r1, #32
asr r0, r1, #31
rrx r0, r1
// --- multiply, divide, saturate, bit fields
mul r0, r1, r2
muls r0, r1, r2
mla r0, r1, r2, r3
mls r0, r1, r2, r3
umull r0, r1, r2, r3
smull r0, r1, r2, r3
umlal r0, r1, r2, r3
smlal r0, r1, r2, r3
smmul r0, r1, r2
smulbb r0, r1, r2
smlabt r0, r1, r2, r3
smlawb r0, r1, r2, r3
smuad r0, r1, r2
usad8 r0, r1, r2
sdiv r0, r1, r2
udiv r0, r1, r2
qadd r0, r1, r2
qsub r0, r1, r2
qdadd r0, r1, r2
ssat r0, #8, r1
usat r0, #5, r1, lsl #3
sadd8 r0, r1, r2
uadd16 r0, r1, r2
shadd8 r0, r1, r2
uqsub16 r0, r1, r2
sel r0, r1, r2          // @state: apsr=000a0000
clz r0, r1
rbit r0, r1
rev r0, r1
rev16 r0, r1
revsh r0, r1
sxtb r0, r1, ror #8
uxtah r0, r1, r2
sxtab16 r0, r1, r2
bfi r0, r1, #4, #8
bfc r0, #3, #12
ubfx r0, r1, #5, #10
sbfx r0, r1, #3, #20
pkhbt r0, r1, r2, lsl #8
crc32b r0, r1, r2
crc32cw r0, r1, r2
// --- conditional
addeq r0, r1, r2
movne r0, #5
subgt r0, r1, #2
addscs r0, r1, r2
// --- memory
ldr r0, [r4]
ldr r0, [r4, #8]
ldr r0, [r4, #-8]!
ldr r0, [r4], #4
ldr r0, [r4, r5, lsl #2]
ldrb r0, [r4, #1]
ldrsb r0, [r4, #1]
ldrh r0, [r4, #2]
ldrsh r0, [r4, #2]
ldrd r0, r1, [r4, #8]
str r0, [r4, #16]
strb r0, [r4]
strh r0, [r4, #2]
strd r0, r1, [r4]
ldm r4, {r0, r1, r2}
ldm r4!, {r0, r1, r2}
ldmdb r4, {r0, r1}
stm r4!, {r0, r1, r2}
stmdb r4!, {r1, r2, r3}
push {r0, r1}
pop {r2, r3}
ldrex r0, [r4]
strex r1, r0, [r4]
ldrexd r0, r1, [r4]
// --- VFP and Advanced SIMD
vadd.f32 s0, s1, s2
vsub.f64 d0, d1, d2
vmul.f32 q0, q1, q2
vdiv.f64 d0, d1, d2
vsqrt.f32 s0, s1
vfma.f32 q0, q1, q2
vmla.f32 s0, s1, s2
vcmp.f32 s0, s1
vcvt.s32.f32 s0, s1
vcvt.f64.s32 d0, s1
vcvt.f32.f64 s0, d1
vcvtr.s32.f64 s0, d1
vrintz.f32 s0, s1
vmaxnm.f32 q0, q1, q2
vmov.f32 s0, #1.5
vmov r0, s1
vmov s2, r1
vmov d0, r0, r1
vmov.32 d0[1], r1
vmov.u16 r0, d1[2]
vadd.i32 q0, q1, q2
vsub.i8 d0, d1, d2
vmul.i16 q0, q1, q2
vand q0, q1, q2
veor d0, d1, d2
vbsl q0, q1, q2
vceq.i32 q0, q1, q2
vcgt.s16 d0, d1, d2
vshl.i32 q0, q1, #3
vshr.u64 q0, q1, #7
vqadd.s16 q0, q1, q2
vmax.u8 q0, q1, q2
vabd.s32 d0, d1, d2
vpadd.i32 d0, d1, d2
vzip.8 d0, d1
vtrn.16 q0, q1
vext.8 q0, q1, q2, #5
vdup.32 q0, r1
vmovl.s16 q0, d1
vmovn.i32 d0, q1
vcnt.8 d0, d1
vrev64.32 q0, q1
vld1.32 {d0, d1}, [r4]
vst1.8 {d0}, [r4]
vldr d0, [r4, #8]
vstr s0, [r4, #4]
vldmia r4!, {d0, d1}
vpush {d8, d9}
vmrs r0, fpscr
vmsr fpscr, r1
vmrs APSR_nzcv, fpscr
vsdot.s8 q0, q1, q2
sha1h.32 q0, q1
aese.8 q0, q1
// --- branches
b #8
bl #16
bx r5
blx r5
bne #8
// --- misc
nop
mrs r0, apsr
msr apsr_nzcvq, r1
svc #5
bkpt #3
udf #1
