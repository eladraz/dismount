#!/usr/bin/env python3
"""
Generate tests/semantics/corpus/x87_edge.asm, the directed x87 cases of the semantics tests: stack
overflow and underflow, rounding and precision control, the exceptions (masked and unmasked), NaNs and
the unsupported encodings, denormals, the conversions and their overflow, comparisons, fxam, the
environment images.

Every line is `instruction ; @x87: tokens` (see tests/semantics/x87states.py). `tag=` tokens name the
group of the case: the regexes of tests/semantics/oracle_bugs.txt use them.

Usage: python3 tests/semantics/tools/gen_x87_edge.py [--out FILE]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

lines = []


def add(text, tag='', **state):
    """One case: text is the instruction, state the @x87 tokens (hex values as strings)."""
    tokens = []
    for key, value in state.items():
        tokens.append('%s=%s' % (key, value))
    if tag:
        tokens.append('tag=' + tag)
    lines.append('%-40s ; @x87: %s' % (text, ' '.join(tokens)))


FCW_DEFAULT = '37f'
# exceptions unmasked: invalid, zero divide, denormal, overflow, underflow, precision
UNMASK_IE = '37e'
UNMASK_DE = '37d'
UNMASK_ZE = '37b'
UNMASK_OE = '377'
UNMASK_UE = '36f'
UNMASK_PE = '35f'


def fcw(pc=3, rc=0, masks=0x3f):
    return '%x' % (0x40 | masks | (pc << 8) | (rc << 10))


PCS = [0, 2, 3]
RCS = [0, 1, 2, 3]

ARITHMETIC = ['fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr']
ADDRESS = '[0x2000]'


def stack_faults():
    # --- overflow: the push instructions on a full stack
    for top in (0, 3):
        for masks, name in ((0x3f, 'masked'), (0x3e, 'unmasked')):
            tg = 'stackfault' if masks == 0x3f else 'stackfault-unmasked'
            kw = dict(fcw=fcw(masks=masks), stack=8, top=top, st0='two', st1='three', st7='f:5.0')
            add('fld1', tg, **kw)
            add('fldz', tg, **kw)
            add('fldpi', tg, **kw)
            add('fld st0', tg, **kw)
            add('fld st3', tg, **kw)
            add('fld dword ' + ADDRESS, tg, m='f32:1.5', **kw)
            add('fld qword ' + ADDRESS, tg, m='f64:1.5', **kw)
            add('fld tword ' + ADDRESS, tg, m='x80:two', **kw)
            add('fild word ' + ADDRESS, tg, m='i16:100', **kw)
            add('fild dword ' + ADDRESS, tg, m='i32:100', **kw)
            add('fild qword ' + ADDRESS, tg, m='i64:100', **kw)
            add('fbld tword ' + ADDRESS, tg, m='bcd:12345', **kw)
            add('fxtract', tg, **kw)
    # only the register below the top is free: fxtract needs one more
    add('fxtract', 'stackfault', stack=7, top=2, st0='f:12.0', st6='one')
    add('fld1', 'stackfault', stack=7, top=2, st0='f:12.0', st6='one')
    # --- underflow
    for masks in (0x3f, 0x3e):
        tg = 'stackfault' if masks == 0x3f else 'stackfault-unmasked'
        for stack in (0, 1):
            kw = dict(fcw=fcw(masks=masks), stack=stack, top=5, st0='f:1.5')
            for op in ARITHMETIC:
                add('%s st0, st1' % op, tg, **kw)
                add('%s st1, st0' % op, tg, **kw)
                add('%sp st1, st0' % op, tg, **kw)
            add('fxch st1', tg, **kw)
            add('fcom st1', tg, **kw)
            add('fcomp st1', tg, **kw)
            add('fucom st1', tg, **kw)
            add('fucomp st1', tg, **kw)
            add('fcomi st0, st1', tg, **kw)
            add('fcomip st0, st1', tg, **kw)
            add('fucomi st0, st1', tg, **kw)
            add('fucomip st0, st1', tg, **kw)
            add('fcmovb st0, st1', tg, **kw)
            add('fcmove st0, st1', tg, eflags='243', **kw)
            add('fscale', tg, **kw)
            add('fcompp', tg, **kw)
            add('fucompp', tg, **kw)
            add('fld st1', tg, **kw)
        kw = dict(fcw=fcw(masks=masks), stack=0, top=5)
        for text in ['fabs', 'fchs', 'fsqrt', 'frndint', 'ftst', 'fxtract', 'fst st1', 'fstp st1',
                     'fst dword ' + ADDRESS, 'fstp dword ' + ADDRESS, 'fst qword ' + ADDRESS,
                     'fstp qword ' + ADDRESS, 'fstp tword ' + ADDRESS, 'fist word ' + ADDRESS,
                     'fist dword ' + ADDRESS, 'fistp dword ' + ADDRESS, 'fistp qword ' + ADDRESS,
                     'fisttp word ' + ADDRESS, 'fisttp dword ' + ADDRESS, 'fisttp qword ' + ADDRESS,
                     'fbstp tword ' + ADDRESS, 'fcom dword ' + ADDRESS, 'fcomp qword ' + ADDRESS,
                     'ficom word ' + ADDRESS, 'ficomp dword ' + ADDRESS,
                     'fadd dword ' + ADDRESS, 'fsubr qword ' + ADDRESS, 'fidiv word ' + ADDRESS,
                     'fmul dword ' + ADDRESS]:
            add(text, tg, **kw)
    # fxam of an empty register is a class, not a fault
    add('fxam', 'stackfault', stack=0, top=3, r3='-one')
    add('fxam', 'stackfault', stack=0, top=3, r3='nan')
    # fxch with one register empty, the other valid
    add('fxch st1', 'stackfault', stack=1, top=0, st0='f:3.0')
    add('fxch st1', 'stackfault', stack=2, top=7, st0='f:3.0', st1='f:4.0')
    # ffree, fincstp, fdecstp, ffreep on any tag
    for top in (0, 7):
        add('ffree st0', 'stack', stack=3, top=top, st0='one', st1='two', st2='three')
        add('ffree st2', 'stack', stack=3, top=top, st0='one', st1='two', st2='three')
        add('ffreep st0', 'stack', stack=3, top=top, st0='one', st1='two', st2='three')
        add('ffreep st1', 'stack', stack=3, top=top, st0='one', st1='two', st2='three')
        add('fincstp', 'stack', stack=3, top=top, st0='one', st1='two', st2='three', fsw='200')
        add('fdecstp', 'stack', stack=3, top=top, st0='one', st1='two', st2='three', fsw='200')
        add('fincstp', 'stack', stack=0, top=top)
        add('fdecstp', 'stack', stack=8, top=top)


def rounding():
    third = dict(st0='one', st1='three')
    for pc in PCS:
        for rc in RCS:
            kw = dict(fcw=fcw(pc, rc), stack=2)
            add('fdiv st0, st1', 'round', st0='one', st1='three', **kw)
            add('fdivr st0, st1', 'round', st0='three', st1='one', **kw)
            add('fdiv st0, st1', 'round', st0='f:-2.0', st1='three', **kw)
            add('fadd st0, st1', 'round', st0='one', st1='3ffc8000000000000001', **kw)
            add('fsub st0, st1', 'round', st0='one', st1='3ffc8000000000000001', **kw)
            add('fadd st0, st1', 'round', st0='-one', st1='3ffc8000000000000001', **kw)
            add('fmul st0, st1', 'round', st0='3fffffffffffffffffff', st1='3fffffffffffffffffff', **kw)
            add('fmul st0, st1', 'round', st0='3fffc000000000000001', st1='f:-1.5', **kw)
            add('fsqrt', 'round', st0='two', **kw)
            add('fsqrt', 'round', st0='3fff8000000000000001', **kw)
            add('fsqrt', 'round', st0='f:9.0', **kw)
            add('fadd dword ' + ADDRESS, 'round', m='f32:0.1', st0='one', **kw)
            add('fmul qword ' + ADDRESS, 'round', m='f64:0.1', st0='three', **kw)
            add('fidiv dword ' + ADDRESS, 'round', m='i32:3', st0='one', **kw)
            add('fimul word ' + ADDRESS, 'round', m='i16:-7', st0='f:1.1', **kw)
            add('frndint', 'round', st0='f:2.5', **kw)
            add('frndint', 'round', st0='f:-2.5', **kw)
            add('frndint', 'round', st0='f:3.5', **kw)
            add('frndint', 'round', st0='f:0.3', **kw)
            add('frndint', 'round', st0='f:-0.3', **kw)
            add('frndint', 'round', st0='f:1e30', **kw)
            add('fscale', 'round', st0='f:3.0', st1='f:-1.5', **kw)
            add('fscale', 'round', st0='f:3.0', st1='f:2.9', **kw)
            add('fscale', 'round', st0='f:3.0', st1='f:-2.9', **kw)
            add('fscale', 'round', st0='min', st1='f:-2.0', **kw)
            add('fscale', 'round', st0='denormal', st1='f:2.0', **kw)
            add('fscale', 'round', st0='3fffc000000000000001', st1='f:-16385.0', **kw)
    for rc in RCS:
        for pc in PCS:
            kw = dict(fcw=fcw(pc, rc), stack=1)
            add('fst dword ' + ADDRESS, 'round', st0='f:0.1', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='3ffbccccccccccccccce', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='f:-0.1', **kw)
            add('fst qword ' + ADDRESS, 'round', st0='3ffbccccccccccccccce', **kw)
            add('fstp qword ' + ADDRESS, 'round', st0='bffbccccccccccccccce', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='f:1e39', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='f:-1e39', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='f:1e-40', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='f:1e-46', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='f:-1e-46', **kw)
            add('fst qword ' + ADDRESS, 'round', st0='f:1e-320', **kw)
            add('fst qword ' + ADDRESS, 'round', st0='f:1e-330', **kw)
            add('fst qword ' + ADDRESS, 'round', st0='max', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='denormal', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='min', **kw)
            add('fst dword ' + ADDRESS, 'round', st0='f:16777217.0', **kw)
            for value in ('f:2.5', 'f:-2.5', 'f:3.5', 'f:0.5', 'f:-0.5', 'f:1.5', 'f:0.3', 'f:-0.3',
                          'f:32767.5', 'f:32768.0', 'f:-32768.5', 'f:-32769.0', 'f:2147483647.5',
                          'f:2147483648.0', 'f:-2147483648.5', 'f:9223372036854775807.0',
                          'f:9223372036854775808.0', 'f:-9223372036854775808.0', 'f:1e30', 'inf', '-inf',
                          'nan', 'snan', 'indefinite', 'zero', '-zero', 'denormal'):
                for width, name in ((16, 'word'), (32, 'dword'), (64, 'qword')):
                    if width != 64:
                        add('fist %s %s' % (name, ADDRESS), 'convert', st0=value, **kw)
                    add('fistp %s %s' % (name, ADDRESS), 'convert', st0=value, **kw)
                    if rc == 0 and pc == 3:
                        add('fisttp %s %s' % (name, ADDRESS), 'convert-truncate', st0=value, **kw)


def exceptions():
    kw = dict(fcw=fcw(), stack=2)
    inputs = ['zero', '-zero', 'one', '-one', 'inf', '-inf', 'nan', '-nan', 'snan', '-snan', 'indefinite', 'denormal', '-denormal', 'max', 'min', 'tiny',
              'pseudo-denormal', 'unnormal', 'pseudo-inf', 'pseudo-nan']
    for op in ARITHMETIC:
        for a in inputs:
            for b in ('one', 'zero', 'inf', 'nan', 'snan', 'denormal', 'max', 'min'):
                add('%s st0, st1' % op, 'special', st0=a, st1=b, **kw)
    for op in ('fadd', 'fsub', 'fmul', 'fdiv'):
        for a in ('one', 'inf', '-inf', 'zero', 'nan'):
            for m in ('nan', 'snan', 'inf', '-inf', 'zero', 'denormal', 'max', 'min', 'one'):
                add('%s dword %s' % (op, ADDRESS), 'special', m='f32:' + m, st0=a, **kw)
                add('%s qword %s' % (op, ADDRESS), 'special', m='f64:' + m, st0=a, **kw)
    for a in inputs:
        add('fsqrt', 'special', st0=a, **kw)
        add('fabs', 'special', st0=a, **kw)
        add('fchs', 'special', st0=a, **kw)
        add('frndint', 'special', st0=a, **kw)
        add('fxtract', 'special', st0=a, stack=1, top=3)
        add('fld st0', 'special', st0=a, stack=1)
        add('fxam', 'special', st0=a, stack=1)
        add('fxam', 'special', st0=a, stack=1, fsw='200')
        add('ftst', 'special', st0=a, stack=1)
        add('fstp tword ' + ADDRESS, 'special', st0=a, stack=1)
        add('fst qword ' + ADDRESS, 'special', st0=a, stack=1)
        add('fst dword ' + ADDRESS, 'special', st0=a, stack=1)
        add('fbstp tword ' + ADDRESS, 'special', st0=a, stack=1)
    for m in ('nan', 'snan', 'inf', '-inf', 'denormal', '-denormal', 'max', 'min', 'zero', '-zero', 'one'):
        add('fld dword ' + ADDRESS, 'special', m='f32:' + m, **kw)
        add('fld qword ' + ADDRESS, 'special', m='f64:' + m, **kw)
    for m in ('nan', 'snan', 'inf', 'denormal', 'pseudo-denormal', 'unnormal', 'pseudo-inf', 'pseudo-nan',
              'indefinite', 'max', 'min', 'tiny'):
        add('fld tword ' + ADDRESS, 'special', m='x80:' + m, **kw)
    # two NaNs: the larger significand wins, a quiet NaN wins over a signaling one
    nans = ['7fffc000000000000001', '7fffc000000000000002', 'ffffc000000000000001', 'ffffc000000000000002',
            '7fffa000000000000001', '7fffa000000000000003', 'ffffa000000000000002', '7fffe000000000000000']
    for op in ('fadd', 'fmul', 'fsub', 'fdiv'):
        for a in nans:
            for b in nans:
                add('%s st0, st1' % op, 'nan-order', st0=a, st1=b, **kw)
    for a in nans:
        for b in nans:
            add('fcom st1', 'nan-order', st0=a, st1=b, **kw)
            add('fucom st1', 'nan-order', st0=a, st1=b, **kw)
            add('fcomi st0, st1', 'nan-order', st0=a, st1=b, **kw)
            add('fucomi st0, st1', 'nan-order', st0=a, st1=b, **kw)
    # overflow and underflow of the result
    for rc in RCS:
        for pc in PCS:
            k = dict(fcw=fcw(pc, rc), stack=2)
            add('fmul st0, st1', 'ovf', st0='max', st1='two', **k)
            add('fmul st0, st1', 'ovf', st0='-max', st1='two', **k)
            add('fadd st0, st1', 'ovf', st0='max', st1='max', **k)
            add('fdiv st0, st1', 'ovf', st0='max', st1='half', **k)
            add('fmul st0, st1', 'ovf', st0='min', st1='half', **k)
            add('fmul st0, st1', 'ovf', st0='min', st1='min', **k)
            add('fmul st0, st1', 'ovf', st0='min', st1='3ffe8000000000000001', **k)
            add('fdiv st0, st1', 'ovf', st0='min', st1='f:3.0', **k)
            add('fsub st0, st1', 'ovf', st0='min', st1='00018000000000000001', **k)
            add('fsub st0, st1', 'ovf', st0='00018000000000000001', st1='min', **k)
            add('fmul st0, st1', 'ovf', st0='denormal', st1='half', **k)
            add('fmul st0, st1', 'ovf', st0='denormal', st1='denormal', **k)
            add('fadd st0, st1', 'ovf', st0='denormal', st1='denormal', **k)
    # unmasked: the exception is pending, the instruction does not store (and does not pop)
    for fc, name in ((UNMASK_IE, 'ie'), (UNMASK_DE, 'de'), (UNMASK_ZE, 'ze'), (UNMASK_OE, 'oe'),
                     (UNMASK_UE, 'ue'), (UNMASK_PE, 'pe')):
        k = dict(fcw=fc, stack=3)
        add('fadd st0, st1', 'unmasked', st0='snan', st1='one', **k)
        add('faddp st1, st0', 'unmasked', st0='denormal', st1='one', **k)
        add('fdiv st0, st1', 'unmasked', st0='one', st1='zero', **k)
        add('fdivp st1, st0', 'unmasked', st0='zero', st1='one', **k)
        add('fmul st0, st1', 'unmasked', st0='max', st1='two', **k)
        add('fmul st0, st1', 'unmasked', st0='min', st1='half', **k)
        add('fdiv st0, st1', 'unmasked', st0='one', st1='three', **k)
        add('fsqrt', 'unmasked', st0='-one', **k)
        add('fcom st1', 'unmasked', st0='nan', st1='one', **k)
        add('fucom st1', 'unmasked', st0='snan', st1='one', **k)
        add('fcomi st0, st1', 'unmasked', st0='nan', st1='one', eflags='8d7', **k)
        add('fucomip st0, st1', 'unmasked', st0='snan', st1='one', eflags='8d7', **k)
        add('fstp dword ' + ADDRESS, 'unmasked', st0='f:1e39', **k)
        add('fstp dword ' + ADDRESS, 'unmasked', st0='f:0.1', **k)
        add('fistp dword ' + ADDRESS, 'unmasked', st0='f:1e10', **k)
        add('fistp word ' + ADDRESS, 'unmasked', st0='f:2.5', **k)
        add('fld dword ' + ADDRESS, 'unmasked', m='f32:snan', **k)
        add('fld dword ' + ADDRESS, 'unmasked', m='f32:denormal', **k)
        add('fxtract', 'unmasked', st0='zero', **k)
        add('frndint', 'unmasked', st0='f:2.5', **k)
        add('fscale', 'unmasked', st0='one', st1='snan', **k)
        add('fbstp tword ' + ADDRESS, 'unmasked', st0='f:1e20', **k)
    # a pending exception: the waiting instructions raise #MF, the others do not
    pending = dict(fsw='81', stack=2, st0='one', st1='two')
    for text in ('fwait', 'fnop', 'fadd st0, st1', 'fld1', 'fxch st1', 'fcom st1', 'fldcw ' + ADDRESS,
                 'fst qword ' + ADDRESS, 'fldenv ' + ADDRESS, 'frstor ' + ADDRESS, 'fxam', 'ffree st0',
                 'fincstp', 'fchs'):
        add(text, 'pending', m='raw:7f03' + '00' * 100, **pending)
    for text in ('fnstsw ax', 'fnstsw ' + ADDRESS, 'fnstcw ' + ADDRESS, 'fnclex', 'fninit',
                 'fnstenv ' + ADDRESS, 'fnsave ' + ADDRESS, 'fxsave ' + ADDRESS, 'fxrstor ' + ADDRESS):
        add(text, 'pending', **pending)


def compare():
    k = dict(fcw=fcw(), stack=2)
    values = ['zero', '-zero', 'one', '-one', 'f:2.0', 'f:-2.0', 'inf', '-inf', 'nan', 'snan', 'denormal',
              '-denormal', 'max', 'min', 'unnormal', 'pseudo-nan', 'pseudo-inf']
    for a in values:
        for b in values:
            for text in ('fcom st1', 'fcomp st1', 'fucom st1', 'fucomp st1'):
                add(text, 'compare', st0=a, st1=b, fsw='4700', **k)
            for text in ('fcomi st0, st1', 'fucomi st0, st1', 'fcomip st0, st1', 'fucomip st0, st1'):
                add(text, 'compare', st0=a, st1=b, eflags='8d7', **k)
        add('ftst', 'compare', st0=a, fsw='4700', stack=1)
        add('fxam', 'compare', st0=a, fsw='4700', stack=1)
    add('fcompp', 'compare', st0='one', st1='f:2.0', fsw='4700', **k)
    add('fucompp', 'compare', st0='nan', st1='f:2.0', fsw='4700', **k)
    add('fucompp', 'compare', st0='snan', st1='f:2.0', fsw='4700', **k)
    for m in ('f32:zero', 'f32:one', 'f32:nan', 'f32:snan', 'f32:denormal', 'f32:inf'):
        add('fcom dword ' + ADDRESS, 'compare', m=m, st0='one', fsw='4700', **k)
        add('fcomp dword ' + ADDRESS, 'compare', m=m, st0='denormal', fsw='4700', **k)
    for m in ('f64:zero', 'f64:one', 'f64:nan', 'f64:snan', 'f64:denormal', 'f64:-inf'):
        add('fcom qword ' + ADDRESS, 'compare', m=m, st0='one', fsw='4700', **k)
    for m in ('i16:0', 'i16:1', 'i16:-1', 'i16:32767'):
        add('ficom word ' + ADDRESS, 'compare', m=m, st0='one', fsw='4700', **k)
        add('ficomp word ' + ADDRESS, 'compare', m=m, st0='f:-1.0', fsw='4700', **k)
    for m in ('i32:0', 'i32:2', 'i32:-2147483648'):
        add('ficom dword ' + ADDRESS, 'compare', m=m, st0='f:2.0', fsw='4700', **k)
    # fcmovcc: every condition, the flags drawn from the eflags bits CF ZF PF
    for cc in ('b', 'e', 'be', 'u', 'nb', 'ne', 'nbe', 'nu'):
        for flags in ('202', '203', '242', '243', '206', '207', '246', '247'):
            add('fcmov%s st0, st1' % cc, 'cmov', st0='one', st1='f:2.0', eflags=flags, **k)
    add('fcmovb st0, st1', 'cmov', st0='one', st1='snan', eflags='203', **k)
    add('fcmovb st0, st1', 'stackfault', stack=1, st0='one', eflags='203', fcw=fcw())
    add('fcmovnb st0, st1', 'cmov-empty', tags='2', st1='f:2.0', eflags='203', fcw=fcw())
    add('fcmovb st0, st1', 'cmov-empty', tags='2', st1='f:2.0', eflags='203', fcw=fcw())


def constants():
    for rc in RCS:
        for pc in PCS:
            for text in ('fld1', 'fldz', 'fldl2t', 'fldl2e', 'fldpi', 'fldlg2', 'fldln2'):
                add(text, 'constant' if rc == 0 else 'constant-rc', fcw=fcw(pc, rc), stack=1, st0='two')


def bcd():
    k = dict(fcw=fcw(), stack=1)
    for value in (0, 1, -1, 12345678901234567, 999999999999999999, -999999999999999999, 100, 4660):
        add('fbld tword ' + ADDRESS, 'bcd', m='bcd:%d' % value, stack=0)
    for raw in ('ffffffffffffffffffff', 'ffffc000000000000000', '00000000000000000099', '000000000000000000a5',
                '8000000000000000000f', '1234567890abcdef1234'):
        add('fbld tword ' + ADDRESS, 'bcd', m='raw:' + raw, stack=0)
    for value in ('f:0.0', 'f:-0.0', 'f:1.0', 'f:-1.0', 'f:0.5', 'f:-0.5', 'f:1.5', 'f:2.5', 'f:-2.5',
                  'f:123456789012345678.0', 'f:999999999999999999.0', 'f:999999999999999999.5',
                  'f:1000000000000000000.0', 'f:-1000000000000000000.0', 'f:1e19', 'inf', '-inf', 'nan',
                  'snan', 'indefinite', 'f:0.1', 'f:-0.3', 'denormal'):
        for rc in RCS:
            add('fbstp tword ' + ADDRESS, 'bcd', st0=value, fcw=fcw(3, rc), stack=1)


def environment():
    # the tag word: every class of register
    values = ['one', 'zero', 'inf', 'nan', 'denormal', 'unnormal', 'pseudo-denormal', '-one', 'min']
    for top in (0, 5):
        add('fnstenv ' + ADDRESS, 'env', fcw='37f', fsw='225', stack=8, top=top,
            st0='one', st1='zero', st2='inf', st3='nan', st4='denormal', st5='unnormal',
            st6='pseudo-denormal', st7='-one')
        add('o16 fnstenv ' + ADDRESS, 'env', fcw='27f', fsw='1f', stack=3, top=top, st0='one', st1='zero',
            st2='denormal')
        add('fnsave ' + ADDRESS, 'env', fcw='37f', fsw='225', stack=8, top=top,
            st0='one', st1='zero', st2='inf', st3='nan', st4='denormal', st5='unnormal',
            st6='pseudo-denormal', st7='-one')
        add('o16 fnsave ' + ADDRESS, 'env', fcw='27f', fsw='1f', stack=3, top=top, st0='one', st1='zero',
            st2='denormal')
        add('fnstenv ' + ADDRESS, 'env', fcw='7f', fsw='0', stack=0, top=top)
        add('fnsave ' + ADDRESS, 'env', fcw='a7f', fsw='4100', stack=0, top=top)
        add('fxsave ' + ADDRESS.replace('2000', '2010'), 'env', fcw='37f', fsw='225', stack=5, top=top,
            st0='one', st1='zero', st2='inf', st3='nan', st4='denormal', xmm0='0123456789abcdef0123456789abcdef',
            xmm7='ffeeddccbbaa99887766554433221100', mxcsr='1f80')
        add('fxsave ' + ADDRESS.replace('2000', '2008'), 'env', fcw='37f', fsw='0', stack=1, top=top, st0='one')
    env16 = '7f' + '03' + '00' * 2 + '25' + '12' + '00' * 2 + 'a5' + '03' + '00' * 2
    img32 = '7f03' + '0000' + 'a512' + '0000' + 'ff3f' + '0000' + '00' * 16
    for tags in ('ffff', '0000', 'aaaa', '5555', 'fcff', '3fff'):
        for fsw in ('0000', '3800', '2a5f', '0041'):
            image = '7f03' + '0000' + fsw[2:] + fsw[:2] + '0000' + tags[2:] + tags[:2] + '0000' + '00' * 16
            add('fldenv ' + ADDRESS, 'env', m='raw:' + image, fcw='37f', stack=2, st0='one', st1='two')
            add('o16 fldenv ' + ADDRESS, 'env', m='raw:7f03' + fsw[2:] + fsw[:2] + tags[2:] + tags[:2] + '00' * 8,
                fcw='37f', stack=2, st0='one', st1='two')
    for cw in ('037f', '0000', 'ffff', '1f3f', '2000', '0300', '0c7f', '087f', '0040', '4000', '1f7f', '037e'):
        # the reserved bits (bit 6 reads 1, bits 7 and 13-15 read 0) and ES
        tag = 'env' if int(cw, 16) & ~0x1f7f == 0 and int(cw, 16) & 0x40 and int(cw, 16) & 0x3f == 0x3f else 'env-reserved'
        add('fldcw ' + ADDRESS, tag, m='raw:' + cw[2:] + cw[:2], fcw='37f', stack=1, st0='one')
        add('fldcw ' + ADDRESS, tag, m='raw:' + cw[2:] + cw[:2], fcw='37f', fsw='3f', stack=1, st0='one')
        add('fldcw ' + ADDRESS, tag, m='raw:' + cw[2:] + cw[:2], fcw='37e', fsw='1', stack=1, st0='one')
    add('fnstcw ' + ADDRESS, 'env', fcw='37f')
    add('fnstsw ' + ADDRESS, 'env', fcw='37f', fsw='4a45', stack=2, top=3, st0='one', st1='one')
    add('fnstsw ax', 'env', fcw='37f', fsw='4a45', stack=2, top=3, st0='one', st1='one', eax='12345678')
    add('fninit', 'env', fcw='a7f', fsw='7f7f', stack=8, top=3, st0='one')
    add('fnclex', 'env', fcw='a7f', fsw='ffff', stack=1, top=3, st0='one')
    add('fnclex', 'env', fcw='a7f', fsw='7f', stack=1, top=3, st0='one')
    # unaligned fxsave / fxrstor: #GP
    add('fxsave ' + ADDRESS.replace('2000', '2004'), 'env', stack=1, st0='one')
    add('fxrstor ' + ADDRESS.replace('2000', '2008'), 'env', stack=1, st0='one')
    add('fxrstor ' + ADDRESS.replace('2000', '2010'), 'env', m='raw:' + '7f03' + '00' * 22 + '807f0000',
        stack=1, st0='one')
    add('fxrstor ' + ADDRESS.replace('2000', '2010'), 'env', m='raw:' + '7f03' + '00' * 22 + '80ff0000',
        stack=1, st0='one')
    add('fxrstor ' + ADDRESS.replace('2000', '2010'), 'env-reserved', m='raw:' + '7f03' + '00' * 22 + '801f0100',
        stack=1, st0='one')


def remainder():
    k = dict(fcw=fcw(), stack=2)
    cases = [('f:10.0', 'three'), ('f:-10.0', 'three'), ('f:10.0', 'f:-3.0'), ('f:5.5', 'two'), ('f:1.0', 'f:4.0'),
             ('f:3.0', 'f:4.0'), ('f:2.0', 'f:4.0'), ('f:1.9', 'f:4.0'), ('f:0.5', 'f:4.0'),
             ('f:100.0', 'f:0.1'), ('zero', 'three'), ('-zero', 'three'), ('three', 'zero'),
             ('inf', 'three'), ('three', 'inf'), ('three', '-inf'), ('nan', 'three'), ('three', 'snan'),
             ('denormal', 'min'), ('min', 'denormal'), ('max', 'f:3.0'), ('f:7.0', 'f:7.0'), ('f:14.0', 'f:7.0'),
             ('f:-7.0', 'f:7.0'), ('f:3.5', 'f:1.0'), ('f:1e18', 'f:3.0'), ('f:9223372036854775808.0', 'three'),
             ('f:18446744073709551616.0', 'three'), ('f:36893488147419103232.0', 'three'),
             ('f:1.0', 'f:2.0e-5'), ('f:1.0', 'f:1.0e-18'), ('f:1.0', 'f:1.0e-19'),
             ('f:1.5', 'f:0.5'), ('f:2.5', 'f:1.0'), ('f:3.5', 'f:1.0'), ('f:-2.5', 'f:1.0')]
    for a, b in cases:
        for text in ('fprem', 'fprem1'):
            add(text, 'rem', st0=a, st1=b, fsw='4700', **k)
    for rc in RCS:
        add('fprem', 'rem', st0='f:10.0', st1='three', fcw=fcw(3, rc), stack=2)


def build():
    stack_faults()
    rounding()
    exceptions()
    compare()
    constants()
    bcd()
    environment()
    remainder()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', default=os.path.join(HERE, '..', 'corpus', 'x87_edge.asm'))
    args = parser.parse_args()
    build()
    with open(args.out, 'w') as f:
        f.write('; Directed x87 cases for the semantics tests (dismount IR vs. Unicorn), bits 32\n')
        f.write('; Generated by tests/semantics/tools/gen_x87_edge.py. Do not edit.\n')
        f.write('; The state of a line is the `@x87:` annotation, see tests/semantics/x87states.py\n')
        f.write('; @file-feature: fpu\n')
        f.write('bits 32\n')
        seen = set()
        for line in lines:
            if line not in seen:
                seen.add(line)
                f.write(line + '\n')
    print('%d lines -> %s' % (len(seen), args.out))


if __name__ == '__main__':
    sys.exit(main())
