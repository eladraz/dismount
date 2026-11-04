#!/usr/bin/env python3
"""The vector corpora of gen_corpus.py (tests/riscv/corpus/<arch>/v_*.s).

Every vector instruction of riscvvector.py gets a line per value of each of its
operands (the other operands at their base value): the 32 vector registers
(rejected ones included: the register groups of vmv2r.v, vl4re8.v...), the
scalar registers, the edge values of the immediates and just outside of them,
masked and unmasked (the masked destination of v0), and for the instructions
with constraints on the destination (RISCV_VR_*, probed from llvm-mc) the
combinations of vd, vs2 and vs1 which overlap. vsetvli / vsetivli get every
vtype value (numbers and the spelled forms, the reserved ones too), and a
file of odd syntax and of the pseudo instructions of more than one
instruction (vmsge.vx...).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

VREGS = ['v%d' % i for i in range(32)]
# The registers which are enough for the families of instructions whose
# encoding code is the one of another entry (the segments of 3 to 7 fields)
VREGS_REDUCED = ['v0', 'v1', 'v2', 'v3', 'v7', 'v8', 'v15', 'v16', 'v24', 'v30', 'v31']
# The registers of the overlap combinations
OVERLAP = ['v0', 'v1', 'v2', 'v3', 'v4', 'v8', 'v9', 'v16', 'v31']
OVERLAP_WIDE = ['v0', 'v1', 'v2', 'v3', 'v4', 'v5', 'v6', 'v7', 'v8', 'v9', 'v10', 'v11', 'v24',
                'v31']

XREGS = ['zero', 'ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1', 'a0', 'a1', 'a2', 'a3', 'a4',
         'a5', 'a6', 'a7', 's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9', 's10', 's11', 't3', 't4',
         't5', 't6', 'x0', 'x31', 'fp']
FREGS = ['ft0', 'ft1', 'ft2', 'ft3', 'ft4', 'ft5', 'ft6', 'ft7', 'fs0', 'fs1', 'fa0', 'fa1', 'fa2',
         'fa3', 'fa4', 'fa5', 'fa6', 'fa7', 'fs2', 'fs3', 'fs4', 'fs5', 'fs6', 'fs7', 'fs8', 'fs9',
         'fs10', 'fs11', 'ft8', 'ft9', 'ft10', 'ft11', 'f0', 'f31']

IMMEDIATES = {
    'imm_v_simm5': [-100, -17, -16, -15, -2, -1, 0, 1, 2, 15, 16, 17, 100],
    'imm_v_uimm5': [-1, 0, 1, 2, 15, 16, 30, 31, 32, 33],
    'imm_v_uimm6': [-1, 0, 1, 31, 32, 33, 62, 63, 64, 65],
    'imm_v_avl': [-1, 0, 1, 2, 15, 16, 30, 31, 32, 33],
}

# The spelled vtype words
SEWS = ['e8', 'e16', 'e32', 'e64', 'e8alt', 'e16alt']
LMULS = ['m1', 'm2', 'm4', 'm8', 'mf2', 'mf4', 'mf8']


def vtype_texts(bits_wide):
    """The vtype operands: every valid combination, the partial forms, the
    numbers (all of them: the reserved ones too) and the invalid spellings."""
    texts = []
    for sew in SEWS:
        for lmul in LMULS:
            for ta in ('ta', 'tu'):
                for ma in ('ma', 'mu'):
                    texts.append('%s, %s, %s, %s' % (sew, lmul, ta, ma))
    for sew in ('e8', 'e32', 'e64', 'e16alt'):
        texts += [sew, sew + ',', sew + ', m1', sew + ', m1,', sew + ', ta', sew + ', tu',
                  sew + ', ma', sew + ', mu', sew + ', ta, ma', sew + ', m2, ma', sew + ', mf2, ta',
                  sew + ',m8,ta,mu', sew + ' , m1 , ta , ma', sew + ',tu,mu']
    texts += ['e32alt', 'e64alt, m1', 'e128', 'e4', 'e0', 'e', 'e07', 'e008, m01', 'e8, mf02',
              'e8, mf1', 'e8, m3', 'e8, m0', 'e8, mf16', 'e8, m16', 'E8', 'e8, M1', 'e8, TA',
              'e8, m1, TA, ma', 'e+8', 'e8, ta, m1', 'e8, mu, ta', 'e8, ta, ta', 'e8, ma, ma',
              'e8, m1, m1', 'e8,, m1', 'e8, m1,,', 'e8, m1, ta, ma, mu', 'e8 m1', 'e8, m1 ta',
              'e8, tama', 'e8, m1, t', 'e8, m1, ta, m', 'm1, e8', 'ta', 'mf2', '(e8)', 'e8alt, ',
              '0', '1', '7', '8', '0x10', '0b101', '1+2', '1<<4', '-0', '-1', 'sym', '0X10']
    count = 1 << bits_wide
    texts += [str(v) for v in range(count)]
    texts += [str(count), str(count + 1), str(count + 256), '4096', '65536']
    return texts


def values(name, token, entry_name):
    """The values (texts) of a vector token."""
    reduced = 'seg3' in entry_name or 'seg4' in entry_name or 'seg5' in entry_name or \
        'seg6' in entry_name or 'seg7' in entry_name
    if name.startswith('vd') or name.startswith('vs'):
        return VREGS_REDUCED if reduced else VREGS
    if name == 'v0':
        return ['v0', 'v1', 'v31']
    if name == 'vm':
        return ['', 'v0.t']
    if name in IMMEDIATES:
        return [str(v) for v in IMMEDIATES[name]]
    if name in ('rd', 'rs1', 'rs2'):
        return XREGS if not reduced else XREGS[:8] + ['a0', 'a1', 'x31']
    if name in ('frd', 'frs1'):
        return FREGS
    if name == 'mem0':
        regs = XREGS if not reduced else XREGS[:8] + ['a0', 'a1', 'x31']
        return ['(%s)' % r for r in regs] + ['0(a1)', '0 (a1)', '( a1 )', '4(a1)', '-4(a1)',
                                              '1(a1)', '(a1', 'a1', '0(v1)', '(v1)', '(fa1)',
                                              '(1)', '()', '0+0(a1)', '(0+a1)']
    if name == 'vtype11':
        return vtype_texts(11)
    if name == 'vtype10':
        return vtype_texts(10)
    return [token.base]


# Operand texts of the wrong class, by the kind of the token
WRONG = {
    'v': ['a0', 'fa0', '5', '(a0)', 'v0.t', 'x', 'v32', 'v-1', 'v01', 'V1', 'v 1'],
    'x': ['v1', 'fa0', '5', '(a0)', 'v0.t', 'x32', 'a8', 'a0.t'],
    'f': ['v1', 'a0', '5', '(a0)', 'v0.t', 'f32'],
    'imm': ['v1', 'a0', 'fa0', '(a0)', 'v0.t', '1.5', 'x'],
    'mem': ['a0', 'v1', 'fa0', '5', '4(a0)v0.t'],
    'm': ['v0', 'v1.t', 'v0.T', 'V0.t', 'v0.t ', 'v0. t', 'a0', '5', 'v0.t.t', 'v31.t', 'm'],
}


def kind_of(name):
    if name.startswith('vd') or name.startswith('vs') or name == 'v0':
        return 'v'
    if name in ('rd', 'rs1', 'rs2'):
        return 'x'
    if name in ('frd', 'frs1'):
        return 'f'
    if name.startswith('imm_'):
        return 'imm'
    if name == 'mem0':
        return 'mem'
    if name == 'vm':
        return 'm'
    return None


def render(entry, texts):
    """texts: one per token (hidden tokens and the empty ones are skipped)."""
    parts = []
    for token, text in zip(entry.tokens(), texts):
        if token.hidden:
            continue
        if text != '':
            parts.append(text)
    if entry.probe:
        return entry.probe.format(*parts)
    return entry.mn + ((' ' + ', '.join(parts)) if parts else '')


def entry_lines(entry):
    names = entry.ops
    tokens = entry.tokens()
    base = [t.base for t in tokens]
    lines = [render(entry, base)]
    # a mask on the base
    roles = {t.role: i for i, t in enumerate(tokens) if t.role}
    for i, (token, name) in enumerate(zip(tokens, names)):
        if token.hidden or not isinstance(name, str):
            continue
        for value in values(name, token, entry.mn):
            current = list(base)
            current[i] = value
            lines.append(render(entry, current))
    # the masked destination of every register
    if 'vm' in roles and 'vd' in roles:
        for reg in ('v0', 'v1', 'v2', 'v3', 'v7', 'v8', 'v31'):
            current = list(base)
            current[roles['vm']] = 'v0.t'
            current[roles['vd']] = reg
            lines.append(render(entry, current))
    # the overlaps
    flags = entry.vflags
    if 'vd' in roles:
        pool = OVERLAP_WIDE if ('seg' in entry.mn and 'ei' in entry.mn) else OVERLAP
        for key, role in (('VS2', 'vs2'), ('VS1', 'vs1')):
            if 'NO_%s_OVERLAP' % key in flags and role in roles:
                for vd in pool:
                    for vs in pool:
                        current = list(base)
                        current[roles['vd']] = vd
                        current[roles[role]] = vs
                        lines.append(render(entry, current))
                        if 'vm' in roles:
                            current[roles['vm']] = 'v0.t'
                            lines.append(render(entry, current))
        if 'NOT_V0' in flags or 'NOT_MASK' in flags:
            for vs in ('v0', 'v1'):
                for role in ('vs2', 'vs1'):
                    if role in roles:
                        current = list(base)
                        current[roles['vd']] = 'v0'
                        current[roles[role]] = vs
                        lines.append(render(entry, current))
    return lines


def shape_lines(entry):
    """Operands of the wrong class and a wrong number of operands."""
    names = entry.ops
    tokens = entry.tokens()
    base = [t.base for t in tokens]
    lines = []
    for i, (token, name) in enumerate(zip(tokens, names)):
        if token.hidden or not isinstance(name, str):
            continue
        kind = kind_of(name)
        if kind is None or kind not in WRONG:
            continue
        for value in WRONG[kind]:
            current = list(base)
            current[i] = value
            lines.append(render(entry, current))
    text = render(entry, base)
    lines.append(text + ', v1')
    lines.append(text + ', v0.t')
    lines.append(text + ', a0')
    lines.append(text + ', 0')
    shown = [t for t in tokens if not t.hidden]
    if len(shown) >= 2:
        # an operand fewer
        mn = text.split(' ', 1)
        ops = mn[1].split(', ') if len(mn) > 1 else []
        if len(ops) >= 2:
            lines.append(mn[0] + ' ' + ', '.join(ops[:-1]))
            lines.append(mn[0] + ' ' + ', '.join(ops[1:]))
            lines.append(mn[0] + ' ' + ', '.join(reversed(ops)))
    lines.append(text.upper())
    lines.append(text.replace(' ', '  ', 1))
    return lines


def pseudo_lines():
    """The pseudo instructions of more than one instruction and the odd
    forms (vmsge.vx...)."""
    lines = []
    for mn in ('vmsge', 'vmsgeu'):
        for vd in ('v1', 'v0', 'v3', 'v2'):
            for va in ('v2', 'v1', 'v0'):
                lines.append('%s.vx %s, %s, a0' % (mn, vd, va))
                lines.append('%s.vx %s, %s, a0, v0.t' % (mn, vd, va))
                for vt in ('v1', 'v2', 'v3', 'v0', 'v4'):
                    lines.append('%s.vx %s, %s, a0, v0.t, %s' % (mn, vd, va, vt))
                    lines.append('%s.vx %s, %s, a0, %s' % (mn, vd, va, vt))
                    lines.append('%s.vx %s, %s, a0, v0.t, %s, v0' % (mn, vd, va, vt))
        for reg in ('zero', 'a0', 's11', 'x31'):
            lines.append('%s.vx v1, v2, %s' % (mn, reg))
            lines.append('%s.vx v1, v2, %s, v0.t' % (mn, reg))
            lines.append('%s.vx v1, v2, %s, v0.t, v3' % (mn, reg))
        lines += ['%s.vx v1, v2, 5' % mn, '%s.vx v1, v2, fa0' % mn, '%s.vx v1, v2' % mn,
                  '%s.vx v1, v2, a0, v0' % mn, '%s.vx v1, v2, a0, v0.t, v3, v4' % mn,
                  '%s.vx v1, v2, a0, v0.t, a1' % mn, '%s.vx v1, v2, a0, v3, v0.t' % mn]
        for imm in (-17, -16, -15, -1, 0, 1, 2, 15, 16, 17):
            for vd in ('v1', 'v0'):
                lines.append('%s.vi %s, v2, %d' % (mn, vd, imm))
                lines.append('%s.vi %s, v2, %d, v0.t' % (mn, vd, imm))
                lines.append('%s.vi %s, v2, %d, v0' % (mn, vd, imm))
        lines += ['%s.vi v1, v1, 3' % mn, '%s.vi v1, v2, a0' % mn, '%s.vi v1, v2, 0x10' % mn,
                  '%s.vi v1, v2, 1+1' % mn]
    for mn in ('vmslt', 'vmsltu', 'vmsle', 'vmsleu'):
        for imm in (-17, -16, -15, 0, 15, 16, 17):
            lines.append('%s.vi v1, v2, %d' % (mn, imm))
    # the carries and merges with a missing or wrong v0
    for text in ('vadc.vvm v1, v2, v3', 'vadc.vvm v1, v2, v3, v0', 'vadc.vvm v1, v2, v3, v0.t',
                 'vadc.vvm v1, v2, v3, v1', 'vmerge.vvm v1, v2, v3, v0',
                 'vmerge.vim v1, v2, 3, v0', 'vmerge.vxm v1, v2, a0, v0',
                 'vfmerge.vfm v1, v2, fa0, v0', 'vfmerge.vfm v1, v2, fa0, v0.t',
                 'vmv.v.v v1, v2, v0.t', 'vmv.v.x v1, a0, v0.t', 'vmv.v.i v1, 1, v0.t',
                 'vfmv.v.f v1, fa0, v0.t', 'vmv.s.x v1, a0, v0.t', 'vmv.x.s a0, v1, v0.t',
                 'vfmv.f.s fa0, v1, v0.t', 'vfmv.s.f v1, fa0, v0.t', 'vid.v v1, v0.t',
                 'vid.v v0, v0.t', 'vid.v v0', 'vmand.mm v1, v2, v3, v0.t'):
        lines.append(text)
    # the spaces and the case
    for text in ('vadd.vv  v1,v2,v3', 'vadd.vv v1 , v2 , v3 , v0.t', 'vadd.vv v1,v2,v3,v0.t',
                 'VADD.VV v1, v2, v3', 'vadd.VV v1, v2, v3', 'vadd.vv V1, v2, v3',
                 'vadd.vv v1, v2, v3, V0.T', 'vadd.vv v1, v2, v3, v0.T', 'vadd.vv v1, v2, v3,',
                 'vadd.vv v1, v2, v3 # c', 'vadd.vv v1, v2, v3 ; vadd.vv v4, v5, v6',
                 'vadd.vi v1, v2, 0x5', 'vadd.vi v1, v2, 0X5', 'vadd.vi v1, v2, 0b101',
                 'vadd.vi v1, v2, 1+2', 'vadd.vi v1, v2, -(3)', 'vadd.vi v1, v2, +3',
                 'vadd.vi v1, v2, 010', 'vadd.vi v1, v2, 3 + 3 * 2', 'vadd.vi v1, v2, 1 << 3',
                 'vadd.vi v1, v2, sym', 'vadd.vx v1, v2, x1', 'vadd.vx v1, v2, fp',
                 'vadd.vx v1, v2, X1', 'vle8.v v1, 0(a0)', 'vle8.v v1, 0x0(a0)',
                 'vle8.v v1, (a0), v0.t', 'vle8.v v1, (A0)', 'vle8.v v1,(a0),v0.t',
                 'vle8.v v1, ( a0 ), v0.t', 'vle8.v v1, +0(a0)', 'vle8.v v1, -0(a0)',
                 'vle8.v v1, 1-1(a0)', 'vle8.v v1, 1(a0)', 'vle8.v v1, (a0) , v0.t',
                 'vsetvli a0, a1, e8, m1, ta, ma # c', 'vsetvli  a0,a1,e8,m1',
                 'vsetvli a0 , a1 , e8 , m1', 'vsetvli a0, a1, e8 ,m1', 'vsetvli a0, a1, e8,\tm1',
                 'vsetvli a0, a1, E8', 'vsetvli A0, a1, e8', 'VSETVLI a0, a1, e8'):
        lines.append(text)
    return lines


def config_lines():
    """vsetvli / vsetivli with every vtype value: the rd, rs1 and avl are
    swept by entry_lines; here the vtype is."""
    return []
