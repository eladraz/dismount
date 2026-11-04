#!/usr/bin/env python3
"""The operand tokens and the entry builder of gen_encoding_tables.py.

An entry of the encoding tables (RiscvEncodingTable.h) is described by its
mnemonic, its operand tokens and its features; the mask and the value are
derived by assembling an example of it with llvm-mc: the mask is every bit
which is not a field of an operand, the value is the bits LLVM gives for the
example. A second example (other operand values) is checked to give the same
bits outside the fields, so a wrong field description is found at once.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# The bits (mask) of the immediate schemes, and example values (base, alt)
IMM = {
    'I': (0xfff00000, 1365, -1366),
    'S': (0xfe000f80, 1365, -1366),
    'B': (0xfe000f80, 2730, -2732),
    'U': (0xfffff000, 0x12345, 0xabcde),
    'J': (0xfffff000, 2730, -2732),
    'CSR_UIMM5': (0x000f8000, 5, 21),
    'PREFETCH': (0xfe000000, 32, -64),
    'AES_BS': (0xc0000000, 1, 2),
    'AES_RNUM': (0x00f00000, 5, 10),
    'LPAD': (0xfffff000, 0x12345, 0xabcde),
    'V_SIMM5': (0x000f8000, 5, -6),
    'V_UIMM5': (0x000f8000, 5, 21),
    'V_UIMM6': (0x040f8000, 33, 7),
    'V_AVL': (0x000f8000, 5, 21),
    'CI': (0x107c, 21, -22),
    'CI_LUI': (0x107c, 21, 0xfffea),
    'CI_ADDI16SP': (0x107c, 336, -352),
    'CIW': (0x1fe0, 84, 168),
    'CI_LWSP': (0x107c, 84, 168),
    'CI_LDSP': (0x107c, 168, 336),
    'CSS_SWSP': (0x1f80, 84, 168),
    'CSS_SDSP': (0x1f80, 168, 336),
    'CL_W': (0x1c60, 84, 40),
    'CL_D': (0x1c60, 168, 80),
    'CLB': (0x60, 1, 2),
    'CLH': (0x20, 0, 2),
    'CB': (0x1c7c, 42, -44),
    'CJ': (0x1ffc, 682, -684),
}

# The x registers: (position, flags) -> example names
XREG = {7: ('a0', 's5'), 15: ('a1', 's6'), 20: ('a2', 's7')}
FREG = {7: ('fa0', 'fs5'), 15: ('fa1', 'fs6'), 20: ('fa2', 'fs7'), 27: ('fa3', 'fs8')}


class Token(object):
    def __init__(self, cpp, fields=0, base='', alt='', hidden=False, part=False, role=None):
        # role: 'vd', 'vs1', 'vs2', 'vm' for the vector tokens whose text the
        # constraint probes of gen_encoding_tables.py replace
        self.role = role
        self.cpp = cpp
        self.fields = fields  # a mask of the bits the operand occupies
        self.base = base
        self.alt = alt
        self.hidden = hidden
        self.part = part


def S(kind, pos, flags, extra):
    return 'S(RISCV_KIND_%s, %s, %s, %s)' % (kind, pos, flags, extra)


def build_tokens():
    t = {}
    for name, pos in (('rd', 7), ('rs1', 15), ('rs2', 20)):
        b, a = XREG[pos]
        t[name] = Token(S('GPR', pos, '0', '0'), 0x1f << pos, b, a)
        t[name + '_nz'] = Token(S('GPR', pos, 'RISCV_GPR_NOT_ZERO', '0'), 0x1f << pos, b, a)
        t[name + '_nzsp'] = Token(S('GPR', pos, 'RISCV_GPR_NOT_ZERO | RISCV_GPR_NOT_SP', '0'),
                                  0x1f << pos, b, a)
        t[name + '_even'] = Token(S('GPR', pos, 'RISCV_GPR_EVEN', '0'), 0x1f << pos, 'a0', 's4')
    for name, pos in (('frd', 7), ('frs1', 15), ('frs2', 20), ('frs3', 27)):
        b, a = FREG[pos]
        t[name] = Token(S('FPR', pos, '0', '0'), 0x1f << pos, b, a)
    # the compressed registers
    t['rdp'] = Token(S('GPR_C', 2, '0', '0'), 7 << 2, 'a0', 'a3')
    t['rs2p'] = Token(S('GPR_C', 2, '0', '0'), 7 << 2, 'a0', 'a3')
    t['rs1p'] = Token(S('GPR_C', 7, '0', '0'), 7 << 7, 'a1', 'a4')
    t['frdp'] = Token(S('FPR_C', 2, '0', '0'), 7 << 2, 'fa0', 'fa3')
    t['frs2p'] = Token(S('FPR_C', 2, '0', '0'), 7 << 2, 'fa0', 'fa3')
    # the 5 bit registers of the compressed formats
    t['crd'] = Token(S('GPR', 7, '0', '0'), 0x1f << 7, 'a0', 's5')
    t['crd_nz'] = Token(S('GPR', 7, 'RISCV_GPR_NOT_ZERO', '0'), 0x1f << 7, 'a0', 's5')
    t['crd_nzsp'] = Token(S('GPR', 7, 'RISCV_GPR_NOT_ZERO | RISCV_GPR_NOT_SP', '0'), 0x1f << 7, 'a0', 's5')
    t['crs2'] = Token(S('GPR', 2, '0', '0'), 0x1f << 2, 'a1', 's6')
    t['crs2_nz'] = Token(S('GPR', 2, 'RISCV_GPR_NOT_ZERO', '0'), 0x1f << 2, 'a1', 's6')
    t['cfrd'] = Token(S('FPR', 7, '0', '0'), 0x1f << 7, 'fa0', 'fs5')
    t['cfrs2'] = Token(S('FPR', 2, '0', '0'), 0x1f << 2, 'fa1', 'fs6')
    # fixed registers
    for reg, number in (('zero', 0), ('ra', 1), ('sp', 2), ('t0', 5)):
        t[reg] = Token(S('GPR_FIXED', 0, '0', number), 0, reg, reg)
    # the immediates
    for scheme, (mask, base, alt) in IMM.items():
        t['imm_' + scheme.lower()] = Token(S('IMMEDIATE', 0, '0', 'RISCV_IMM_' + scheme), mask,
                                           str(base), str(alt))
        t['imm_' + scheme.lower() + '_nz'] = Token(
            S('IMMEDIATE', 0, 'RISCV_IMMEDIATE_NOT_ZERO', 'RISCV_IMM_' + scheme), mask,
            str(base), str(alt))
    t['imm_u_hex'] = t['imm_u']
    t['shamt6'] = Token(S('SHAMT', 20, '0', 6), 0x3f << 20, '21', '10')
    t['shamt5'] = Token(S('SHAMT', 20, '0', 5), 0x1f << 20, '21', '10')
    t['cshamt'] = Token(S('SHAMT', 2, '0', 6), 0x107c, '21', '10')
    t['cshamt_nz'] = Token(S('SHAMT', 2, 'RISCV_IMMEDIATE_NOT_ZERO', 6), 0x107c, '21', '10')
    t['fli'] = Token(S('FLOAT_IMMEDIATE', 15, '0', 0), 0x1f << 15, '1.0', '0.5')
    # memory: offset(base)
    def mem(name, base_pos, flags, scheme, regs, mask_base):
        mask, b, a = IMM[scheme]
        t[name] = Token(S('MEMORY', base_pos, flags, 'RISCV_IMM_' + scheme), mask | mask_base,
                        '%d(%s)' % (b, regs[0]), '%d(%s)' % (a, regs[1]))
    mem('mem_i', 15, '0', 'I', ('a1', 's6'), 0x1f << 15)
    mem('mem_s', 15, '0', 'S', ('a1', 's6'), 0x1f << 15)
    mem('mem_prefetch', 15, '0', 'PREFETCH', ('a1', 's6'), 0x1f << 15)
    mem('mem_cw', 7, 'RISCV_MEMORY_COMPRESSED', 'CL_W', ('a1', 'a4'), 7 << 7)
    mem('mem_cd', 7, 'RISCV_MEMORY_COMPRESSED', 'CL_D', ('a1', 'a4'), 7 << 7)
    mem('mem_clb', 7, 'RISCV_MEMORY_COMPRESSED', 'CLB', ('a1', 'a4'), 7 << 7)
    mem('mem_clh', 7, 'RISCV_MEMORY_COMPRESSED', 'CLH', ('a1', 'a4'), 7 << 7)
    mem('mem_lwsp', 0, 'RISCV_MEMORY_SP', 'CI_LWSP', ('sp', 'sp'), 0)
    mem('mem_ldsp', 0, 'RISCV_MEMORY_SP', 'CI_LDSP', ('sp', 'sp'), 0)
    mem('mem_swsp', 0, 'RISCV_MEMORY_SP', 'CSS_SWSP', ('sp', 'sp'), 0)
    mem('mem_sdsp', 0, 'RISCV_MEMORY_SP', 'CSS_SDSP', ('sp', 'sp'), 0)
    t['mem0'] = Token(S('MEMORY', 15, 'RISCV_MEMORY_NO_OFFSET', 0), 0x1f << 15, '(a1)', '(s6)')
    for name, scheme in (('br_b', 'B'), ('br_j', 'J'), ('br_cb', 'CB'), ('br_cj', 'CJ')):
        mask, b, a = IMM[scheme]
        t[name] = Token(S('BRANCH_TARGET', 0, '0', 'RISCV_IMM_' + scheme), mask, str(b), str(a))
    t['csr'] = Token(S('CSR', 20, '0', 0), 0xfff << 20, '0x555', '0x2aa')
    t['rm'] = Token(S('ROUNDING_MODE', 12, '0', 0), 7 << 12, 'rtz', 'rdn')
    t['rm_rne'] = Token(S('ROUNDING_MODE', 12, 'RISCV_ROUNDING_DEFAULT_RNE', 0), 7 << 12, 'rtz', 'rdn')
    # a rounding mode which is part of the encoding (fcvtmod.w.d rtz)
    t['rm_fixed'] = Token(S('ROUNDING_MODE', 12, '0', 0), 0, 'rtz', 'rtz')
    t['fence_p'] = Token(S('FENCE_SET', 24, '0', 0), 0xf << 24, 'ir', 'ow')
    t['fence_s'] = Token(S('FENCE_SET', 20, '0', 0), 0xf << 20, 'rw', 'io')
    t['aqrl'] = Token(S('AQRL', 25, '0', 0), 3 << 25, '.aq', '.rl', part=True)
    return t


def add_vector_tokens(t):
    """The operands of the vector instructions. The flags of the destination
    (VDFLAGS) are what llvm-mc checks when it assembles the entry: the generator
    derives them (RISCV_VR_* of RiscvOperandCodec.h) and fills them in; the
    m_extra of a vector register is its alignment (log2 of the number of
    registers) | (the registers of a segment tuple << 3)."""
    def vr(pos, flags, extra, base, alt, role):
        return Token(S('VR', pos, flags, extra), 0x1f << pos, base, alt, role=role)
    t['vd'] = vr(7, 'VDFLAGS', 0, 'v8', 'v24', 'vd')
    t['vs2'] = vr(20, 0, 0, 'v4', 'v20', 'vs2')
    t['vs1'] = vr(15, 0, 0, 'v2', 'v18', 'vs1')
    t['vs3'] = vr(7, 0, 0, 'v8', 'v24', None)
    # the registers of a group: vmv<n>r.v, vl<n>re<eew>.v, vs<n>r.v
    for log2, n in ((1, 2), (2, 4), (3, 8)):
        t['vd_m%d' % n] = vr(7, 0, log2, 'v8', 'v24', None)
        t['vs2_m%d' % n] = vr(20, 0, log2, 'v16', 'v8', None)
        t['vs3_m%d' % n] = vr(7, 0, log2, 'v8', 'v24', None)
    # the tuples of the segment instructions (nf registers)
    for n in range(2, 9):
        t['vd_n%d' % n] = vr(7, 'VDFLAGS', n << 3, 'v8', 'v24', 'vd')
        t['vs3_n%d' % n] = vr(7, 0, n << 3, 'v8', 'v24', None)
    t['v0'] = Token(S('VR_V0', 0, 0, 0), 0, 'v0', 'v0')
    t['vm'] = Token(S('VMASK', 25, 0, 0), 1 << 25, '', 'v0.t', role='vm')
    t['vtype11'] = Token(S('VTYPE', 20, 0, 11), 0x7ff << 20, 'e32, m1, ta, ma', 'e8, mf2, tu, mu')
    t['vtype10'] = Token(S('VTYPE', 20, 0, 10), 0x3ff << 20, 'e32, m1, ta, ma', 'e8, mf2, tu, mu')
    t['vmem0'] = t['mem0']


TOKENS = build_tokens()
add_vector_tokens(TOKENS)


def tie(pos, other):
    return Token(S('GPR_TIED', pos, '0', other), 0x1f << pos, '', '', hidden=True)


class Entry(object):
    def __init__(self, mn, ops, feat, flags, flow, feat2, rv, probe, cond, width):
        self.mn = mn
        self.ops = ops
        self.feat = feat
        self.flags = flags
        self.flow = flow
        self.feat2 = feat2
        self.rv = rv
        self.probe = probe
        self.cond = cond
        self.width = width
        self.mask = None
        self.value = None
        # The flags of the destination vector register (VDFLAGS)
        self.vflags = '0'

    def tokens(self):
        return [t if isinstance(t, Token) else TOKENS[t] for t in self.ops]

    def fields(self):
        mask = 0
        for t in self.tokens():
            mask |= t.fields
        return mask

    def probe_text(self, alt):
        parts = []
        suffix = ''
        for t in self.tokens():
            if t.part:
                suffix += t.alt if alt else t.base
            elif not t.hidden:
                text = t.alt if alt else t.base
                # (an operand which prints nothing: the unmasked vm)
                if text != '':
                    parts.append(text)
        if self.probe:
            return self.probe.format(*parts)
        return self.mn + suffix + ((' ' + ', '.join(parts)) if parts else '')

    def cpp(self):
        flags = list(self.flags)
        if self.rv == 32:
            flags.append('RISCV_ENTRY_RV32_ONLY')
        elif self.rv == 64:
            flags.append('RISCV_ENTRY_RV64_ONLY')
        flag_text = ' | '.join(flags) if flags else '0'
        ops = ', '.join(t.cpp.replace('VDFLAGS', self.vflags) for t in self.tokens())
        head = 'E("%s", 0x%08x, 0x%08x, %s, ' % (self.mn, self.mask, self.value, self.feat)
        if self.feat2:
            head = 'E2("%s", 0x%08x, 0x%08x, %s, %s, ' % (self.mn, self.mask, self.value,
                                                        self.feat, self.feat2)
        return '%s%s, %s, %s%s)' % (head, flag_text, self.flow or '0',
                                    self.cond or 'NULL', (', ' + ops) if ops else '')
