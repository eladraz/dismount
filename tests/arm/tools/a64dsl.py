#!/usr/bin/env python3
"""A small language to write the A64 encoding tables, and the C++ emitter.

An encoding is written as a pattern of 32 bits from the most significant,
for example

    E('add', 'sf 0 0 100010 sh imm12 Rn Rd', [rs('Rd'), rs('Rn'), arith_imm()])

The tokens are bit strings ('0', '1', '100010') and field names
('sf', 'Rd', 'imm12', or 'name:width' for a field which isn't in FIELD_WIDTHS).
The mask and the value of the entry come from the bit strings, the operands
refer to the fields by name. A condition on the fields is a C++ expression
where $name is the field ('$Rd == 31'). tests/arm/tools/gen_a64_table.py runs
the groups and writes Source/dismount/proc/arm/Arm64EncodingTable.cpp.
"""
import re

FIELD_WIDTHS = {
    'sf': 1, 'op': 1, 'S': 1, 'sh': 1, 'N': 1, 'hw': 2, 'opc': 2, 'size': 2,
    'shift': 2, 'option': 3, 'imm3': 3, 'imm4': 4, 'imm5': 5, 'imm6': 6,
    'imm7': 7, 'imm8': 8, 'imm9': 9, 'imm10': 10, 'imm12': 12, 'imm14': 14,
    'imm16': 16, 'imm19': 19, 'imm26': 26, 'immr': 6, 'imms': 6, 'immlo': 2,
    'immhi': 19, 'cond': 4, 'nzcv': 4, 'Rd': 5, 'Rn': 5, 'Rm': 5, 'Ra': 5,
    'Rt': 5, 'Rt2': 5, 'Rs': 5, 'b5': 1, 'b40': 5, 'CRm': 4, 'CRn': 4,
    'op0': 2, 'op1': 3, 'op2': 3, 'L': 1, 'A': 1, 'R': 1, 'V': 1, 'Q': 1,
    'mask': 4, 'o0': 1, 'o1': 1, 'o2': 1, 'o3': 1, 'sz': 1, 'hint': 7,
}

NEXT_FEATURE_DEFAULT = 'FEAT_A64_BASE'


class Entry(object):
    def __init__(self, mnemonic, pattern, ops, feat, flags, flow, cond, consts):
        self.mnemonic = mnemonic
        self.pattern = pattern
        self.ops = ops
        self.feat = feat
        self.flags = flags
        self.flow = flow
        self.cond = cond
        self.consts = consts
        self.mask = 0
        self.value = 0
        self.fields = {}


ENTRIES = []
CONDITIONS = []          # (name, body)
_condition_names = {}

_current = None          # the fields of the pattern being defined


def parse_pattern(pattern):
    """Return (mask, value, fields); fields maps name -> (low bit, width)."""
    mask = 0
    value = 0
    fields = {}
    position = 32
    for token in pattern.split():
        if re.fullmatch(r'[01]+', token):
            for ch in token:
                position -= 1
                mask |= 1 << position
                if ch == '1':
                    value |= 1 << position
        else:
            # "Rs!" is a field which the decoder ignores and the assembler
            # fills with ones (the "should be one" bits)
            ones = token.endswith('!')
            name, _, width = token.rstrip('!').partition(':')
            width = int(width) if width else FIELD_WIDTHS[name]
            position -= width
            assert name not in fields, 'duplicate field %s in %s' % (name, pattern)
            fields[name] = (position, width)
            if ones:
                value |= ((1 << width) - 1) << position
    assert position == 0, 'pattern is %d bits: %s' % (32 - position, pattern)
    return mask, value, fields


def _condition_function(entry):
    """Return the name of the C++ function of the condition, None if none."""
    expr = entry.cond
    if expr is None:
        return None

    def field(match):
        name = match.group(1)
        if name in entry.consts:
            return str(entry.consts[name])
        low, width = entry.fields[name]
        return 'BITS(%d, %d)' % (low + width - 1, low)
    body = re.sub(r'\$(\w+)', field, expr)
    if body not in _condition_names:
        name = 'condition%d' % len(CONDITIONS)
        _condition_names[body] = name
        CONDITIONS.append((name, body))
    return _condition_names[body]


def E(mnemonic, pattern, ops, feat=NEXT_FEATURE_DEFAULT, flags=0, flow=0,
      cond=None, consts=None):
    """Add an encoding. 'ops' are callables which get the fields of the pattern
    (see the operand builders)."""
    global _current
    entry = Entry(mnemonic, pattern, None, feat, flags, flow, cond, consts or {})
    entry.mask, entry.value, entry.fields = parse_pattern(pattern)
    _current = entry.fields
    entry.ops = [op() if callable(op) else op for op in ops]
    assert len(entry.ops) <= 8, mnemonic
    ENTRIES.append(entry)
    return entry


# ---------------------------------------------------------------------------
# The operand builders. Each returns a function so the fields are looked up
# when the entry is added. The text is the S(kind, position, flags, extra)
# macro of the emitted file.

def _field(name):
    low, width = _current[name]
    return low, width


def _spec(kind, position, flags, extra):
    return 'S(%s, %d, %s, %s)' % (kind, position, flags, extra)


def _gpr(name, flags):
    return lambda: _spec('ARM_KIND_GPR', _field(name)[0], flags, 5)


def X(f):
    return _gpr(f, 'ARM_GPR_X')


def W(f):
    return _gpr(f, 'ARM_GPR_W')


def R(f):
    return _gpr(f, 'ARM_GPR_SF')


def XS(f):
    return _gpr(f, 'ARM_GPR_X | ARM_GPR_SP')


def WS(f):
    return _gpr(f, 'ARM_GPR_W | ARM_GPR_SP')


def RS(f):
    return _gpr(f, 'ARM_GPR_SF | ARM_GPR_SP')


def O22(f):
    return _gpr(f, 'ARM_GPR_OPC22')


def XP(f):
    return _gpr(f, 'ARM_GPR_X | ARM_GPR_PAIR')


def WP(f):
    return _gpr(f, 'ARM_GPR_W | ARM_GPR_PAIR')


def XPZ(f):
    return _gpr(f, 'ARM_GPR_X | ARM_GPR_PAIR | ARM_GPR_PAIR_ZERO')


def XWB(f):
    return _gpr(f, 'ARM_GPR_X | ARM_GPR_WRITEBACK')


def sized(size, f):
    """A general purpose register of a size: 'x', 'w' or 'r' (the sf bit)."""
    return {'x': X, 'w': W, 'r': R}[size](f)


def sized_sp(size, f):
    return {'x': XS, 'w': WS, 'r': RS}[size](f)


def fpr(f, scheme):
    """A scalar SIMD & FP register: 'b', 'h', 's', 'd', 'q', 'ftype',
    'size', 'sz', 'load_store', 'long'."""
    schemes = {'b': 'ARM_FPR_B', 'h': 'ARM_FPR_H', 's': 'ARM_FPR_S', 'd': 'ARM_FPR_D',
               'q': 'ARM_FPR_Q', 'ftype': 'ARM_FPR_FTYPE', 'size': 'ARM_FPR_SIZE',
               'sz': 'ARM_FPR_SZ', 'load_store': 'ARM_FPR_LOAD_STORE',
               'long': 'ARM_FPR_LONG_SIZE'}
    return lambda: _spec('ARM_KIND_A64_FPR', _field(f)[0], '0', schemes[scheme])


def _imm_flags(scale, hex_, plus, decimal_zero):
    flags = []
    if scale:
        flags.append(str(scale))
    if plus:
        flags.append('ARM_SPEC_PLUS_ONE')
    if hex_:
        flags.append('ARM_SPEC_HEX')
    if decimal_zero:
        flags.append('ARM_SPEC_DECIMAL_ZERO')
    return ' | '.join(flags) if flags else '0'


def imm(f, scale=0, hex=False, plus=False, decimal_zero=False):
    return lambda: _spec('ARM_KIND_IMMEDIATE', _field(f)[0],
                         _imm_flags(scale, hex, plus, decimal_zero), _field(f)[1])


def simm(f, scale=0, hex=False, decimal_zero=False):
    return lambda: _spec('ARM_KIND_SIGNED_IMMEDIATE', _field(f)[0],
                         _imm_flags(scale, hex, False, decimal_zero), _field(f)[1])


def split_imm(index, hex=False, scale=0):
    return lambda: _spec('ARM_KIND_SPLIT_IMMEDIATE', 0, _imm_flags(scale, hex, False, False), index)


def arith_imm():
    return lambda: _spec('ARM_KIND_A64_ARITHMETIC_IMMEDIATE', 10, '0', 0)


def logical_imm(size, signed=False):
    flags = {'x': 'ARM_GPR_X', 'w': 'ARM_GPR_W', 'r': 'ARM_GPR_SF'}[size]
    return lambda: _spec('ARM_KIND_A64_LOGICAL_IMMEDIATE', 10, flags, 1 if signed else 0)


def move_wide(size, scheme='ARM_MOVE_WIDE_PLAIN'):
    flags = {'x': 'ARM_GPR_X', 'w': 'ARM_GPR_W', 'r': 'ARM_GPR_SF'}[size]
    return lambda: _spec('ARM_KIND_A64_MOVE_WIDE_IMMEDIATE', 5, flags, scheme)


def shreg(f, size, no_ror=False):
    flags = {'x': 'ARM_GPR_X', 'w': 'ARM_GPR_W', 'r': 'ARM_GPR_SF'}[size]
    if no_ror:
        flags += ' | ARM_SPEC_NO_ROR'
    return lambda: _spec('ARM_KIND_SHIFTED_REGISTER', _field(f)[0], flags, 0)


def extreg(f):
    return lambda: _spec('ARM_KIND_A64_EXTENDED_REGISTER', _field(f)[0], '0', 0)


def bitfield(scheme, size):
    flags = {'x': 'ARM_GPR_X', 'w': 'ARM_GPR_W', 'r': 'ARM_GPR_SF'}[size]
    return lambda: _spec('ARM_KIND_A64_BITFIELD_AMOUNT', 0, flags, scheme)


def lsl_reg(f, bits=3):
    return lambda: _spec('ARM_KIND_A64_LSL_REGISTER', _field(f)[0], '0', bits)


def tied(f, other):
    return lambda: _spec('ARM_KIND_A64_TIED_REGISTER', _field(f)[0], '0', _field(other)[0])


def mem(scheme, scale=0, base='Rn'):
    return lambda: _spec('ARM_KIND_A64_MEMORY', _field(base)[0], str(scale), scheme)


def pcrel(scheme):
    return lambda: _spec('ARM_KIND_A64_PC_RELATIVE', 0, '0', scheme)


def branch(f):
    return lambda: _spec('ARM_KIND_BRANCH_TARGET', _field(f)[0], '0', _field(f)[1])


def cond_op(f, invert=False):
    return lambda: _spec('ARM_KIND_CONDITION', _field(f)[0],
                         'ARM_SPEC_INVERT' if invert else '0', 0)


def cond_mnemonic(f, print_al=False):
    return lambda: _spec('ARM_KIND_CONDITION_MNEMONIC', _field(f)[0],
                         'ARM_SPEC_PRINT_AL' if print_al else '0', 0)


def nzcv(f='nzcv'):
    return lambda: _spec('ARM_KIND_A64_NZCV', _field(f)[0], '0', 0)


def sysreg():
    return lambda: _spec('ARM_KIND_A64_SYSTEM_REGISTER', 5, '0', 0)


def opt(scheme):
    return lambda: _spec('ARM_KIND_OPTION', 0, '0', scheme)


# ---------------------------------------------------------------------------
# The emitter

PREAMBLE = r'''#include "dismount/dismount.h"
/*
 * Arm64EncodingTable.cpp
 *
 * The encoding table of the A64 integer, branch, load / store and system
 * instructions (see ArmEncodingTable.h): the data processing, the branches,
 * the exceptions, the system instructions, the loads and stores with their
 * atomic, memory tagging and memory copy extensions.
 *
 * GENERATED by tests/arm/tools/gen_a64_table.py: edit the generator, not this
 * file. The order of the entries is the order of the decoder: an alias comes
 * before the general encoding it prints for.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/Opcode.h"
#include "dismount/proc/arm/ArmEncodingTable.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"

namespace armdis {

namespace {

// The bits hi..lo of the encoding
#define BITS(hi, lo) (((e) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
// An operand spec: the kind, the position of the field, the flags, the extra
#define S(kind, position, flags, extra) { kind, position, flags, extra }
// An entry: the mnemonic, the mask and the value, the feature, the flags, the
// flow, the condition and the operands
#define E(mnemonic, mask, value, feature, flags, flow, condition, ...) \
    { mnemonic, mask, value, { __VA_ARGS__ }, ArmFeatures::feature, flags, flow, condition }
// The common flows
#define FLOW_JUMP (Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_RELATIVE_JMP)
#define FLOW_CALL (Opcode::FLOW_STACK_CHANGE | Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_RELATIVE_JMP)
#define FLOW_COND (Opcode::FLOW_COND_ZERO | Opcode::FLOW_RELATIVE_JMP)
#define FLOW_JUMP_REGISTER (Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION)
#define FLOW_CALL_REGISTER (Opcode::FLOW_STACK_CHANGE | Opcode::FLOW_COND_ALWAYS | Opcode::FLOW_ACTION)
#define FLOW_RETURN (Opcode::FLOW_RET | Opcode::FLOW_ACTION)
#define FLOW_TRAP (Opcode::FLOW_DEBUG)

%(helpers)s

%(conditions)s

#undef BITS
} // anonymous namespace

const ArmEncodingEntry gArm64BaseEncodingTable[] = {
'''

POSTAMBLE = r'''    // The end of the table
    { NULL, 0, 0, {{ ARM_KIND_NONE, 0, 0, 0 }}, 0, 0, 0, NULL }
};

} // namespace armdis
'''


def emit(helpers, path):
    """Write the C++ file. finalize_conditions() has to be called first."""
    conditions = []
    for name, body in CONDITIONS:
        conditions.append('bool %s(uint32 e)\n{\n    return %s;\n}\n' % (name, body))
    out = [PREAMBLE % {'helpers': helpers, 'conditions': '\n'.join(conditions)}]
    lines = []
    for entry in ENTRIES:
        name = _condition_function(entry)
        ops = ', '.join(entry.ops)
        text = '    E("%s", 0x%08x, 0x%08x, %s, %s, %s, %s%s),' % (
            entry.mnemonic, entry.mask, entry.value, entry.feat, entry.flags or 0,
            entry.flow or 0, name if name else 'NULL', (', ' + ops) if ops else '')
        lines.append('    // %s' % entry.pattern)
        lines.append(text)
    out.append('\n'.join(lines) + '\n')
    out.append(POSTAMBLE)
    open(path, 'w').write(''.join(out))


def finalize_conditions():
    """Create the condition functions of all the entries. Must be called after
    all the entries were added and before emit()."""
    for entry in ENTRIES:
        _condition_function(entry)
