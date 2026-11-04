#!/usr/bin/env python3
"""Write tests/arm/semantics/corpus/a32_neon.s and t32_neon.s: the AArch32 Advanced SIMD integer
instructions, the structure loads and stores and the cryptographic extensions, one line per mnemonic
(every data type) and operand shape (d and q registers, by scalar, immediates, the list forms and
the writeback forms of vld / vst), run by tests/arm/semantics/run_semantics.py against Unicorn.

    gen_corpus_a32_neon.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

CORPUS = os.path.join(armlib.REPO, 'tests', 'arm', 'semantics', 'corpus')

lines = []


def emit(text, state=None):
    lines.append(text + ('       // @state: ' + state if state else ''))


def section(title):
    lines.append('// --- ' + title)


S = ['s8', 's16', 's32']
U = ['u8', 'u16', 'u32']
SU = S + U
I = ['i8', 'i16', 'i32']
S64 = S + ['s64']
SU64 = SU + ['s64', 'u64']


def dq(mnemonic, types, three=True, extra=''):
    """The d and q register forms."""
    for t in types:
        if three:
            emit('%s.%s d0, d1, d2%s' % (mnemonic, t, extra))
            emit('%s.%s q4, q5, q6%s' % (mnemonic, t, extra))
        else:
            emit('%s.%s d0, d1%s' % (mnemonic, t, extra))
            emit('%s.%s q4, q5%s' % (mnemonic, t, extra))


def bits_of(t):
    return int(t[1:])


# ---------------------------------------------------------------------------
section('three registers of the same size')
for m, types in (('vadd', I + ['i64']), ('vsub', I + ['i64']), ('vmul', I + ['p8']), ('vmla', I), ('vmls', I),
                 ('vabd', SU), ('vaba', SU), ('vhadd', SU), ('vhsub', SU), ('vrhadd', SU),
                 ('vmax', SU), ('vmin', SU), ('vqadd', SU64), ('vqsub', SU64),
                 ('vtst', ['8', '16', '32']), ('vceq', I), ('vcge', SU), ('vcgt', SU)):
    dq(m, types)
emit('vadd.i8 d0, d0, d1')
emit('vadd.i32 q4, q4, q5')
emit('vmla.i16 d0, d0, d0')
emit('vqadd.s8 d0, d1, d2', 'q1=7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f q2=01010101010101010101010101010101')
emit('vqadd.s8 q4, q5, q6', 'q5=80808080808080808080808080808080 q6=ffffffffffffffffffffffffffffffff')
emit('vqsub.u8 d0, d1, d2', 'q1=00010203040506070809000000000000 q2=01010101010101010101010101010101')
emit('vqadd.s64 q4, q5, q6', 'q5=7fffffffffffffff7fffffffffffffff q6=00000000000000010000000000000001')
emit('vqsub.s64 q4, q5, q6', 'q5=80000000000000008000000000000000 q6=00000000000000010000000000000001')
emit('vqadd.u64 q4, q5, q6', 'q5=ffffffffffffffffffffffffffffffff q6=00000000000000010000000000000001')
emit('vhsub.u8 d0, d1, d2', 'q1=00000000000000000000000000000000 q2=ffffffffffffffffffffffffffffffff')
emit('vrhadd.s32 q4, q5, q6', 'q5=7fffffff7fffffff7fffffff7fffffff q6=7fffffff7fffffff7fffffff7fffffff')

section('compare with zero, one register')
for m, types in (('vceq', I), ('vcge', S), ('vcgt', S), ('vcle', S), ('vclt', S)):
    for t in types:
        emit('%s.%s d0, d1, #0' % (m, t))
        emit('%s.%s q4, q5, #0' % (m, t))

section('by scalar')
for m in ('vmul', 'vmla', 'vmls'):
    for t in ('i16', 'i32'):
        idx = '3' if t == 'i16' else '1'
        emit('%s.%s d0, d1, d2[%s]' % (m, t, idx))
        emit('%s.%s q4, q5, d6[%s]' % (m, t, idx))
        emit('%s.%s d0, d1, d7[0]' % (m, t))
for m in ('vqdmulh', 'vqrdmulh', 'vqrdmlah', 'vqrdmlsh'):
    for t in ('s16', 's32'):
        idx = '3' if t == 's16' else '1'
        emit('%s.%s d0, d1, d2[%s]' % (m, t, idx))
        emit('%s.%s q4, q5, d6[%s]' % (m, t, idx))
        emit('%s.%s d0, d1, d2' % (m, t))
        emit('%s.%s q4, q5, q6' % (m, t))
emit('vqdmulh.s16 d0, d1, d2', 'q1=80008000800080008000800080008000 q2=80008000800080008000800080008000')
emit('vqrdmulh.s32 q4, q5, q6', 'q5=80000000800000008000000080000000 q6=80000000800000008000000080000000')
emit('vqrdmlah.s16 d0, d1, d2', 'q0=7fff7fff7fff7fff7fff7fff7fff7fff q1=7fff7fff7fff7fff7fff7fff7fff7fff q2=7fff7fff7fff7fff7fff7fff7fff7fff')
emit('vqrdmlsh.s32 q4, q5, q6', 'q4=80000000800000008000000080000000 q5=7fffffff7fffffff7fffffff7fffffff q6=7fffffff7fffffff7fffffff7fffffff')

section('logic')
for m in ('vand', 'vbic', 'vorr', 'vorn', 'veor', 'vbsl', 'vbit', 'vbif'):
    emit('%s d0, d1, d2' % m)
    emit('%s q4, q5, q6' % m)
for t, imm in (('i16', '#0x12'), ('i16', '#0x3400'), ('i32', '#0x56'), ('i32', '#0x7800'), ('i32', '#0x90000'),
               ('i32', '#0xab000000')):
    for m in ('vorr', 'vbic', 'vmvn'):
        emit('%s.%s d0, %s' % (m, t, imm))
        emit('%s.%s q4, %s' % (m, t, imm))
emit('vmvn.i32 d0, #0x12ff')
emit('vmvn.i32 q4, #0x12ffff')
emit('vmvn d0, d1')
emit('vmvn q4, q5')

section('two registers')
for m, types in (('vabs', S), ('vneg', S), ('vqabs', S), ('vqneg', S), ('vcls', S), ('vclz', I)):
    dq(m, types, three=False)
dq('vcnt', ['8'], three=False)
emit('vqabs.s8 d0, d1', 'q1=80808080808080808080808080808080')
emit('vqneg.s16 q4, q5', 'q5=80008000800080008000800080008000')
emit('vqneg.s32 q4, q5', 'q5=80000000800000008000000080000000')
for m, types in (('vrev16', ['8']), ('vrev32', ['8', '16']), ('vrev64', ['8', '16', '32'])):
    dq(m, types, three=False)
for t in ('8', '16', '32'):
    for m in ('vtrn', 'vuzp', 'vzip'):
        emit('%s.%s d0, d1' % (m, t))
        emit('%s.%s q4, q5' % (m, t))
emit('vswp d0, d1')
emit('vswp q4, q5')
for t in S + U:
    emit('vpaddl.%s d0, d1' % t)
    emit('vpaddl.%s q4, q5' % t)
    emit('vpadal.%s d0, d1' % t)
    emit('vpadal.%s q4, q5' % t)
emit('vrecpe.u32 d0, d1')
emit('vrecpe.u32 q4, q5')
emit('vrsqrte.u32 d0, d1')
emit('vrsqrte.u32 q4, q5')
for s in ('80000000', '7fffffff', '40000000', '3fffffff', 'ffffffff', '80ffffff', '90000000', 'b0000000', 'c0000000'):
    emit('vrecpe.u32 d0, d1', 'q1=%s%s%s%s' % (s, s, s, s))
    emit('vrsqrte.u32 d0, d1', 'q1=%s%s%s%s' % (s, s, s, s))
for t in ('8', '16', '32'):
    emit('vext.%s d0, d1, d2, #1' % t)
for i in (0, 3, 7):
    emit('vext.8 d0, d1, d2, #%d' % i)
    emit('vext.8 q4, q5, q6, #%d' % (i + 8))
emit('vext.8 q4, q5, q6, #0')
emit('vext.16 d0, d1, d2, #3')
emit('vext.16 q4, q5, q6, #7')
emit('vext.32 d0, d1, d2, #1')
emit('vext.32 q4, q5, q6, #3')
emit('vext.64 q4, q5, q6, #1')
for t in ('8', '16', '32'):
    emit('vdup.%s d0, r1' % t)
    emit('vdup.%s q4, r2' % t)
emit('vdup.8 d0, d1[5]')
emit('vdup.16 q4, d5[2]')
emit('vdup.32 d0, d1[1]')
emit('vdup.32 q4, d5[0]')

section('tables')
for n in (1, 2, 3, 4):
    lst = ', '.join('d%d' % (2 + i) for i in range(n))
    emit('vtbl.8 d0, {%s}, d10' % lst)
    emit('vtbx.8 d0, {%s}, d10' % lst)
emit('vtbl.8 d0, {d2, d3}, d1', 'q1=0f0e0d0c0b0a09080706050403020100')

section('long and wide')
for m in ('vaddl', 'vsubl', 'vabdl', 'vabal', 'vmull', 'vmlal', 'vmlsl'):
    for t in SU:
        emit('%s.%s q4, d1, d2' % (m, t))
for m in ('vaddw', 'vsubw'):
    for t in SU:
        emit('%s.%s q4, q5, d2' % (m, t))
for m in ('vmull', 'vmlal', 'vmlsl'):
    for t in ('s16', 's32', 'u16', 'u32'):
        idx = '3' if '16' in t else '1'
        emit('%s.%s q4, d1, d2[%s]' % (m, t, idx))
for t in ('p8', 'p64'):
    emit('vmull.%s q4, d1, d2' % t)
emit('vmull.s32 q4, d1, d2', 'q1=80000000800000008000000080000000 q2=80000000800000008000000080000000')
for m in ('vqdmull', 'vqdmlal', 'vqdmlsl'):
    for t in ('s16', 's32'):
        idx = '3' if '16' in t else '1'
        emit('%s.%s q4, d1, d2' % (m, t))
        emit('%s.%s q4, d1, d2[%s]' % (m, t, idx))
emit('vqdmull.s16 q4, d1, d2', 'q1=80008000800080008000800080008000 q2=80008000800080008000800080008000')
emit('vqdmull.s32 q4, d1, d2', 'q1=80000000800000008000000080000000 q2=80000000800000008000000080000000')
emit('vqdmlal.s32 q4, d1, d2', 'q4=7fffffffffffffff7fffffffffffffff q1=7fffffff7fffffff7fffffff7fffffff q2=7fffffff7fffffff7fffffff7fffffff')
emit('vqdmlsl.s16 q4, d1, d2', 'q4=80000000800000008000000080000000 q1=7fff7fff7fff7fff7fff7fff7fff7fff q2=7fff7fff7fff7fff7fff7fff7fff7fff')
for m in ('vaddhn', 'vraddhn', 'vsubhn', 'vrsubhn'):
    for t in ('i16', 'i32', 'i64'):
        emit('%s.%s d0, q5, q6' % (m, t))

section('shifts by an immediate')
for t in SU64:
    n = bits_of(t)
    for m in ('vshr', 'vrshr', 'vsra', 'vrsra'):
        for sh in (1, n // 2, n):
            emit('%s.%s d0, d1, #%d' % (m, t, sh))
        emit('%s.%s q4, q5, #%d' % (m, t, n // 2 + 1))
for t in I + ['i64']:
    n = bits_of(t)
    for sh in (0, 1, n - 1):
        emit('vshl.%s d0, d1, #%d' % (t, sh))
    emit('vshl.%s q4, q5, #%d' % (t, n // 2))
for t in SU64:
    n = bits_of(t)
    for sh in (0, 1, n - 1):
        emit('vqshl.%s d0, d1, #%d' % (t, sh))
    emit('vqshl.%s q4, q5, #%d' % (t, n // 2))
for t in S64:
    n = bits_of(t)
    for sh in (0, 1, n - 1):
        emit('vqshlu.%s d0, d1, #%d' % (t, sh))
    emit('vqshlu.%s q4, q5, #%d' % (t, n // 2))
for t in ('8', '16', '32', '64'):
    n = int(t)
    for sh in (0, n - 1):
        emit('vsli.%s d0, d1, #%d' % (t, sh))
        emit('vsli.%s q4, q5, #%d' % (t, sh))
    for sh in (1, n):
        emit('vsri.%s d0, d1, #%d' % (t, sh))
        emit('vsri.%s q4, q5, #%d' % (t, sh))
emit('vqshl.s8 d0, d1, #3', 'q1=10203040506070809080ff0001ff7f7f')
emit('vqshl.u8 q4, q5, #1', 'q5=10203040506070809080ff0001ff7f7f')
emit('vqshlu.s8 d0, d1, #2', 'q1=10203040506070809080ff0001ff7f7f')
emit('vqshl.s64 q4, q5, #1', 'q5=c000000000000000bfffffffffffffff')
emit('vqshl.u64 q4, q5, #1', 'q5=80000000000000007fffffffffffffff')

section('narrowing and widening shifts')
for t in ('i16', 'i32', 'i64'):
    n = bits_of(t)
    for sh in (1, n // 4, n // 2):
        emit('vshrn.%s d0, q5, #%d' % (t, sh))
        emit('vrshrn.%s d0, q5, #%d' % (t, sh))
for t in ('s16', 's32', 's64', 'u16', 'u32', 'u64'):
    n = bits_of(t)
    for sh in (1, n // 4, n // 2):
        emit('vqshrn.%s d0, q5, #%d' % (t, sh))
        emit('vqrshrn.%s d0, q5, #%d' % (t, sh))
for t in ('s16', 's32', 's64'):
    n = bits_of(t)
    for sh in (1, n // 2):
        emit('vqshrun.%s d0, q5, #%d' % (t, sh))
        emit('vqrshrun.%s d0, q5, #%d' % (t, sh))
for t in SU:
    n = bits_of(t)
    emit('vshll.%s q4, d1, #%d' % (t, 1))
    emit('vshll.%s q4, d1, #%d' % (t, n - 1))
    emit('vmovl.%s q4, d1' % t)
for t in ('i8', 'i16', 'i32'):
    emit('vshll.%s q4, d1, #%d' % (t, bits_of(t)))
for t in ('i16', 'i32', 'i64'):
    emit('vmovn.%s d0, q5' % t)
for t in ('s16', 's32', 's64', 'u16', 'u32', 'u64'):
    emit('vqmovn.%s d0, q5' % t)
for t in ('s16', 's32', 's64'):
    emit('vqmovun.%s d0, q5' % t)
emit('vqshrn.s16 d0, q5, #4', 'q5=7fff80007fff80007fff80007fff8000')
emit('vqshrun.s32 d0, q5, #8', 'q5=ffffff00ffffff000100000001000000')
emit('vqrshrn.u32 d0, q5, #4', 'q5=ffffffffffffffffffffffffffffffff')
emit('vqmovn.s64 d0, q5', 'q5=800000000000000000000000ffffffff')

section('shifts by a register')
for t in SU64:
    for m in ('vshl', 'vrshl', 'vqshl', 'vqrshl'):
        emit('%s.%s d0, d1, d2' % (m, t))
        emit('%s.%s q4, q5, q6' % (m, t))
for t in ('s8', 'u8', 's64', 'u64', 's32'):
    emit('vqrshl.%s d0, d1, d2' % t, 'q1=8070605040302010ff7f01ff7f7f0101 q2=0001020708fffefdfcf9f8f7c0801020')
    emit('vqshl.%s q4, q5, q6' % t, 'q5=7f80ff0102fe7f807f80ff0102fe7f80 q6=01020304f8fefd0140ff00800100ff01')

section('pairwise')
for m in ('vpadd',):
    for t in I:
        emit('%s.%s d0, d1, d2' % (m, t))
for m in ('vpmax', 'vpmin'):
    for t in SU:
        emit('%s.%s d0, d1, d2' % (m, t))

section('dot products and matrix multiplies')
for t in ('s8', 'u8'):
    m = 'vsdot' if t == 's8' else 'vudot'
    emit('%s.%s d0, d1, d2' % (m, t))
    emit('%s.%s q4, q5, q6' % (m, t))
    emit('%s.%s d0, d1, d2[1]' % (m, t))
    emit('%s.%s q4, q5, d6[0]' % (m, t))

section('cryptography')
for m in ('aese', 'aesd', 'aesmc', 'aesimc'):
    emit('%s.8 q4, q5' % m)
emit('sha1h.32 q4, q5')
emit('sha1su1.32 q4, q5')
emit('sha256su0.32 q4, q5')
for m in ('sha1c', 'sha1m', 'sha1p', 'sha1su0', 'sha256h', 'sha256h2', 'sha256su1'):
    emit('%s.32 q4, q5, q6' % m)
emit('aese.8 q0, q1', 'q0=00112233445566778899aabbccddeeff q1=000102030405060708090a0b0c0d0e0f')

section('vld / vst: multiple structures')
for m in ('vld1', 'vst1'):
    for t in ('8', '16', '32', '64'):
        for lst in ('{d0}', '{d0, d1}', '{d0, d1, d2}', '{d0, d1, d2, d3}'):
            emit('%s.%s %s, [r4]' % (m, t, lst))
        emit('%s.%s {d0, d1}, [r4:128]' % (m, t))
        emit('%s.%s {d0}, [r4:64]' % (m, t))
        emit('%s.%s {d0, d1, d2, d3}, [r4:256]' % (m, t))
        emit('%s.%s {d0, d1}, [r4]!' % (m, t))
        emit('%s.%s {d0, d1, d2}, [r4], r2' % (m, t))
        emit('%s.%s {d0}, [r4:64]!' % (m, t))
for m in ('vld2', 'vst2'):
    for t in ('8', '16', '32'):
        for lst in ('{d0, d1}', '{d0, d2}', '{d0, d1, d2, d3}'):
            emit('%s.%s %s, [r4]' % (m, t, lst))
        emit('%s.%s {d0, d1}, [r4:128]' % (m, t))
        emit('%s.%s {d0, d1, d2, d3}, [r4:256]!' % (m, t))
        emit('%s.%s {d0, d2}, [r4], r2' % (m, t))
        emit('%s.%s {d0, d1}, [r4]!' % (m, t))
for m in ('vld3', 'vst3'):
    for t in ('8', '16', '32'):
        emit('%s.%s {d0, d1, d2}, [r4]' % (m, t))
        emit('%s.%s {d0, d2, d4}, [r4]!' % (m, t))
        emit('%s.%s {d0, d1, d2}, [r4:64]' % (m, t))
        emit('%s.%s {d0, d1, d2}, [r4], r2' % (m, t))
for m in ('vld4', 'vst4'):
    for t in ('8', '16', '32'):
        emit('%s.%s {d0, d1, d2, d3}, [r4]' % (m, t))
        emit('%s.%s {d0, d2, d4, d6}, [r4]!' % (m, t))
        emit('%s.%s {d0, d1, d2, d3}, [r4:256]' % (m, t))
        emit('%s.%s {d0, d1, d2, d3}, [r4:64], r2' % (m, t))

section('vld / vst: one lane')
for m in ('vld1', 'vst1'):
    for t, idx in (('8', 7), ('16', 3), ('32', 1)):
        emit('%s.%s {d0[%d]}, [r4]' % (m, t, idx))
        emit('%s.%s {d0[0]}, [r4]!' % (m, t))
        emit('%s.%s {d1[%d]}, [r4], r2' % (m, t, idx))
    emit('%s.16 {d0[2]}, [r4:16]' % m)
    emit('%s.32 {d0[1]}, [r4:32]' % m)
for m in ('vld2', 'vst2'):
    for t, idx in (('8', 7), ('16', 3), ('32', 1)):
        emit('%s.%s {d0[%d], d1[%d]}, [r4]' % (m, t, idx, idx))
        emit('%s.%s {d0[0], d1[0]}, [r4]!' % (m, t))
        emit('%s.%s {d0[%d], d1[%d]}, [r4], r2' % (m, t, idx, idx))
    emit('%s.16 {d0[1], d2[1]}, [r4]' % m)
    emit('%s.32 {d0[1], d2[1]}, [r4]!' % m)
    emit('%s.8 {d0[2], d1[2]}, [r4:16]' % m)
    emit('%s.32 {d0[1], d1[1]}, [r4:64]' % m)
for m in ('vld3', 'vst3'):
    for t, idx in (('8', 5), ('16', 3), ('32', 1)):
        emit('%s.%s {d0[%d], d1[%d], d2[%d]}, [r4]' % (m, t, idx, idx, idx))
        emit('%s.%s {d0[0], d1[0], d2[0]}, [r4]!' % (m, t))
        emit('%s.%s {d0[%d], d1[%d], d2[%d]}, [r4], r2' % (m, t, idx, idx, idx))
    emit('%s.16 {d0[1], d2[1], d4[1]}, [r4]' % m)
    emit('%s.32 {d0[1], d2[1], d4[1]}, [r4]!' % m)
for m in ('vld4', 'vst4'):
    for t, idx in (('8', 5), ('16', 3), ('32', 1)):
        emit('%s.%s {d0[%d], d1[%d], d2[%d], d3[%d]}, [r4]' % (m, t, idx, idx, idx, idx))
        emit('%s.%s {d0[0], d1[0], d2[0], d3[0]}, [r4]!' % (m, t))
        emit('%s.%s {d0[%d], d1[%d], d2[%d], d3[%d]}, [r4], r2' % (m, t, idx, idx, idx, idx))
    emit('%s.16 {d0[1], d2[1], d4[1], d6[1]}, [r4:64]' % m)
    emit('%s.32 {d0[1], d2[1], d4[1], d6[1]}, [r4:128]!' % m)
    emit('%s.8 {d0[1], d1[1], d2[1], d3[1]}, [r4:32]' % m)

section('vld: all lanes')
for t in ('8', '16', '32'):
    emit('vld1.%s {d0[]}, [r4]' % t)
    emit('vld1.%s {d0[], d1[]}, [r4]!' % t)
    emit('vld1.%s {d0[], d1[]}, [r4], r2' % t)
    emit('vld2.%s {d0[], d1[]}, [r4]' % t)
    emit('vld2.%s {d0[], d2[]}, [r4]!' % t)
    emit('vld3.%s {d0[], d1[], d2[]}, [r4]' % t)
    emit('vld3.%s {d0[], d2[], d4[]}, [r4], r2' % t)
    emit('vld4.%s {d0[], d1[], d2[], d3[]}, [r4]' % t)
    emit('vld4.%s {d0[], d2[], d4[], d6[]}, [r4]!' % t)
emit('vld1.16 {d0[]}, [r4:16]')
emit('vld2.8 {d0[], d1[]}, [r4:16]')
emit('vld4.32 {d0[], d1[], d2[], d3[]}, [r4:128]')


def write(name, text_lines, header):
    with open(os.path.join(CORPUS, name), 'w') as f:
        f.write(header + '\n')
        f.write('\n'.join(text_lines) + '\n')


write('a32_neon.s', lines,
      '// A32 Advanced SIMD integer instructions, structure loads / stores and cryptography. GENERATED by\n'
      '// tests/arm/tools/gen_corpus_a32_neon.py: edit the generator, not this file.')

# The T32 spellings: the same instructions as T32 encodings (the SIMD encodings are translated to A32 by the
# decoder). One line per mnemonic family and shape: the A32 lines are the T32 ones, unchanged text
seen = set()
t32 = []
for text in lines:
    if text.startswith('//'):
        t32.append(text)
        continue
    code = text.split('//')[0].strip()
    key = code.split(',')[0].split()[0].split('.')[0] + ' ' + ('q' if ' q' in code else 'd') + ('[' if '[' in code else '') + \
        ('#' if '#' in code else '') + ('!' if '!' in code else '')
    if key in seen:
        continue
    seen.add(key)
    t32.append(text)
write('t32_neon.s', t32,
      '// T32 Advanced SIMD integer instructions, structure loads / stores and cryptography: one line per mnemonic\n'
      '// and operand shape of a32_neon.s. GENERATED by tests/arm/tools/gen_corpus_a32_neon.py.')
print('wrote a32_neon.s (%d lines) and t32_neon.s (%d lines)' % (len(lines), len(t32)))
