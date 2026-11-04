// T32 (Thumb-2) instructions. `;` joins an IT block into one line (one run unit).
adds r0, r1, r2
adds r0, r1, #5
adds r0, #200
add r0, r1, r2
add.w r0, r1, r2, lsl #3
subs r0, r1, r2
sub.w r0, r1, #0x3f0000
sbc.w r0, r1, r2
rsb.w r0, r1, #0
rsbs r0, r1, #0
adc.w r0, r1, r2   // @state: apsr=20000000
and.w r0, r1, r2
ands r0, r1
orr.w r0, r1, r2, lsr #4
eors r0, r1
bic.w r0, r1, r2
orn r0, r1, r2
mvn r0, r1
mov.w r0, #0x00ff00ff
movs r0, r1
movw r0, #0x1234
movt r0, #0xbeef
cmp r0, r1
cmn.w r0, #4
tst.w r0, r1
teq r0, r1
lsls r0, r1, #3
lsrs r0, r1
asrs r0, r1, #31
ror.w r0, r1, r2
rrx r0, r1
muls r0, r1, r0
mul r0, r1, r2
mla r0, r1, r2, r3
mls r0, r1, r2, r3
umull r0, r1, r2, r3
smlal r0, r1, r2, r3
sdiv r0, r1, r2
udiv r0, r1, r2
clz r0, r1
rbit r0, r1
rev r0, r1
rev16 r0, r1
uxtb r0, r1
sxth.w r0, r1, ror #16
uxtab r0, r1, r2
ubfx r0, r1, #3, #9
sbfx r0, r1, #7, #5
bfi r0, r1, #4, #8
bfc r0, #2, #6
usat r0, #7, r1
ssat16 r0, #5, r1
qadd r0, r1, r2
sadd16 r0, r1, r2
uhsub8 r0, r1, r2
smulbb r0, r1, r2
smmla r0, r1, r2, r3
crc32b r0, r1, r2
ldr r0, [r4]
ldr r0, [r4, #8]
ldr.w r0, [r4, #-4]!
ldr r0, [r4], #4
ldr.w r0, [r4, r5, lsl #2]
ldrb r0, [r4, #1]
ldrsh.w r0, [r4, #2]
ldrd r0, r1, [r4, #8]
str r0, [r4, #8]
strb.w r0, [r4, #-1]
strh r0, [r4, #2]
strd r0, r1, [r4]
ldm r4, {r0, r1, r2}
ldm.w r4!, {r0, r1, r2}
stmdb r4!, {r0, r1}
push {r0, r1}
pop {r2, r3}
ldrex r0, [r4]
strex r1, r0, [r4]
vadd.f32 q0, q1, q2
vmla.f32 s0, s1, s2
vcvt.s32.f32 s0, s1
vld1.32 {d0, d1}, [r4]
it eq ; addeq r0, r1, r2
ite ne ; movne r0, #1 ; moveq r0, #2
itt gt ; addgt r0, r1, r2 ; subgt r3, r3, #1
ittee lt ; movlt r0, #1 ; movlt r1, #2 ; movge r0, #3 ; movge r1, #4
b #8
bl #16
bx r5
blx r5
cbz r0, #8
cbnz r1, #8
bne.w #8
tbb [r5, r6]
nop
bkpt #3
udf #1
