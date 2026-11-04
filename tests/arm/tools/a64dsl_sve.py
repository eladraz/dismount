#!/usr/bin/env python3
"""The language of the A64 SVE encoding table, and the C++ emitter.

It is the language of a64dsl.py (an encoding is a pattern of 32 bits from the
most significant, the tokens are bit strings and field names, the operands
refer to the fields by name) with the operand builders of the SVE kinds
(ARM_KIND_SVE_*, see ArmEncodingTable.h) and its own list of entries, so the
SVE table is generated independently of the base one.

    E('add', '00000100 size 0 00 000 000 Pg:3 Zm Zdn',
      [Z('Zdn', 'size'), P('Pg', q='m', w=3), Z('Zdn', 'size'), Z('Zm', 'size')],
      feat='SVE')

The size, index and immediate schemes are the names of the enumerators of
ArmOperandCodec.h without the prefix (ARM_SVE_SIZE_22 is 'SIZE_22'); the
emitter checks that they exist there.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
CODEC_HEADER = os.path.join(REPO, 'Include', 'dismount', 'proc', 'arm', 'ArmOperandCodec.h')

FIELD_WIDTHS = {
    'size': 2, 'sz': 1, 'Zd': 5, 'Zn': 5, 'Zm': 5, 'Za': 5, 'Zt': 5, 'Zda': 5,
    'Zdn': 5, 'Pd': 4, 'Pn': 4, 'Pm': 4, 'Pg': 4, 'Pt': 4, 'Pdn': 4, 'Rn': 5,
    'Rm': 5, 'Rd': 5, 'Vd': 5, 'Vn': 5, 'Vm': 5, 'Vdn': 5, 'Rdn': 5, 'Rt': 5, 'Rv': 2, 'imm3': 3, 'imm4': 4,
    'imm5': 5, 'imm6': 6, 'imm7': 7, 'imm8': 8, 'imm9': 9, 'pattern': 5,
    'prfop': 4, 'sh': 1, 'M': 1, 'S': 1, 'U': 1, 'op': 1, 'opc': 2,
    'Zk': 5, 'Pv': 4, 'Pdm': 4, 'imm2': 2, 'tsz': 5, 'tszh': 2, 'tszl': 2, 'i1': 1, 'i2': 2,
    'i3h': 1, 'i3l': 2, 'rot': 2, 'r': 1, 'PNd': 3, 'PNn': 3, 'PNg': 3, 'vl': 1, 'xs': 1,
    'imm13': 13,
}


class Entry(object):
    def __init__(self, mnemonic, pattern, ops, feat, flags, flow, cond):
        self.mnemonic = mnemonic
        self.pattern = pattern
        self.ops = ops
        self.feat = feat
        self.flags = flags
        self.flow = flow
        self.cond = cond
        self.mask = 0
        self.value = 0
        self.fields = {}


ENTRIES = []
CONDITIONS = []          # (name, body)
_condition_names = {}
_current = None          # the fields of the pattern being defined
_comment = None


def parse_pattern(pattern):
    """Return (mask, value, fields); fields maps name -> (low bit, width). A
    field may appear in several parts: 'name:width' for each part, the first
    part is the most significant (the position is the one of the low part,
    the others are returned as extra fields 'name#1', ...)."""
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
            ones = token.endswith('!')
            name, _, width = token.rstrip('!').partition(':')
            width = int(width) if width else FIELD_WIDTHS[name]
            position -= width
            key = name
            n = 1
            while key in fields:
                key = '%s#%d' % (name, n)
                n += 1
            fields[key] = (position, width)
            if ones:
                value |= ((1 << width) - 1) << position
    assert position == 0, 'pattern is %d bits: %s' % (32 - position, pattern)
    return mask, value, fields


def order_aliases():
    """The aliases come first (the order inside each kind is kept)."""
    ENTRIES[:] = [e for e in ENTRIES if e.alias] + [e for e in ENTRIES if not e.alias]


def _condition_function(entry):
    expr = entry.cond
    if expr is None:
        return None

    def field(match):
        name = match.group(1)
        low, width = entry.fields[name]
        return 'BITS(%d, %d)' % (low + width - 1, low)
    body = re.sub(r'\$(\w+)', field, expr)
    if body not in _condition_names:
        name = 'condition%d' % len(CONDITIONS)
        _condition_names[body] = name
        CONDITIONS.append((name, body))
    return _condition_names[body]


def E(mnemonic, pattern, ops, feat='SVE', flags=0, flow=0, cond=None, alias=False,
      assemble_only=False, decode_only=False):
    """Add an encoding. 'ops' are callables which get the fields of the
    pattern (see the operand builders). An alias is the preferred disassembly
    of a more general encoding (it comes before the general one in the table);
    an assemble_only entry is read by the assembler and never printed."""
    global _current
    names = [flags] if flags else []
    if alias:
        names.append('ARM_ENTRY_ALIAS')
    if assemble_only:
        names.append('ARM_ENTRY_ASSEMBLE_ONLY')
    if decode_only:
        names.append('ARM_ENTRY_DECODE_ONLY')
    flags = ' | '.join(str(n) for n in names) if names else 0
    entry = Entry(mnemonic, pattern, None, feat, flags, flow, cond)
    entry.alias = alias or assemble_only
    entry.mask, entry.value, entry.fields = parse_pattern(pattern)
    _current = entry.fields
    entry.ops = [op() if callable(op) else op for op in ops]
    assert len(entry.ops) <= 8, mnemonic
    ENTRIES.append(entry)
    return entry


# ---------------------------------------------------------------------------
# The operand builders. Each returns a function so the fields are looked up
# when the entry is added.

def _field(name):
    return _current[name]


def _spec(kind, position, flags, extra):
    return 'S(%s, %d, %s, %s)' % (kind, position, flags, extra)


_HEADER_NAMES = None


def _check_name(name):
    """The enumerator ARM_<name> has to exist in ArmOperandCodec.h."""
    global _HEADER_NAMES
    if _HEADER_NAMES is None:
        _HEADER_NAMES = set(re.findall(r'\b(ARM_[A-Z0-9_]+)\b', open(CODEC_HEADER).read()))
    assert name in _HEADER_NAMES, 'unknown enumerator ' + name
    return name


def size_name(size):
    """The ArmSveSizeScheme enumerator of a scheme name: 'b', 'h', 's', 'd',
    'q', 'none' or an enumerator name without the ARM_SVE_ prefix."""
    fixed = {'none': 'NONE', 'b': 'B', 'h': 'H', 's': 'S', 'd': 'D', 'q': 'Q'}
    return _check_name('ARM_SVE_' + fixed.get(size, size))


def _sizeflag(size):
    return {'b': 0, 'h': 1, 's': 2, 'd': 3, 'q': 4, 'none': 5}[size]


def Z(f, size):
    """zN.<T>: the register at field f, the element size by a scheme."""
    return lambda: _spec('ARM_KIND_SVE_Z', _field(f)[0], '0', size_name(size))


def ZI(f, size, scheme):
    """zN.<T>[index] (by element, dup): the scheme gives the index fields."""
    return lambda: _spec('ARM_KIND_SVE_Z_INDEXED', _field(f)[0], str(_sizeflag(size)),
                         _check_name('ARM_SVE_IDX_' + scheme))


def ZL(f, count, size, multiple=1, strided=False):
    """{ zN.<T>, zN+1.<T> ... }: count registers; the encoded field is the
    register number divided by 'multiple' (1, 2 or 4)."""
    flags = count | ({1: 0, 2: 1, 4: 2, 8: 3}[multiple] << 4) | (8 if strided else 0)
    return lambda: _spec('ARM_KIND_SVE_Z_LIST', _field(f)[0], str(flags), size_name(size))


PRED_QUALIFIERS = {None: 0, 'm': 1, 'z': 2, 'M4': 3, 'M13': 4, 'M16': 5, 'M10': 6, 'M20': 7}


def P(f, size='none', q=None, w=4, pair=False):
    """pN, pN/m, pN/z, pN.<T>: w is 3 or 4 bits; the qualifier is none, m, z
    or M4 / M13 / M16 / M10 / M20 (the bit gives /m when set, /z otherwise).
    pair is the list { p2.s, p3.s } of two predicates (the field is 3 bits)."""
    flags = (1 if w == 3 else 0) | (PRED_QUALIFIERS[q] << 1) | (0x10 if pair else 0)
    return lambda: _spec('ARM_KIND_SVE_PREDICATE', _field(f)[0], str(flags), size_name(size))


def PN(f, size='none', q=None, high=True):
    """pnN.<T>, a predicate as counter: pn8-pn15 with high (3 bits)."""
    flags = (1 if high else 0) | (PRED_QUALIFIERS[q] << 1)
    return lambda: _spec('ARM_KIND_SVE_PREDICATE_COUNTER', _field(f)[0], str(flags),
                         size_name(size))


def PIDX(f, scheme):
    """The predicate of psel / pext: pn.<T>[wv, #imm] / pnN[#imm]."""
    return lambda: _spec('ARM_KIND_SVE_PREDICATE_INDEXED', _field(f)[0], '0',
                         _check_name('ARM_SVE_PIDX_' + scheme))


def TIED(f, source, width):
    """A field which copies another one (width bits), not an operand."""
    return lambda: _spec('ARM_KIND_SVE_TIED', _field(f)[0], str(width), _field(source)[0])


def FFR():
    return lambda: _spec('ARM_KIND_SVE_FFR', 0, '0', '0')


def IMM(scheme, flags='0'):
    """An immediate of ArmSveImmediateScheme."""
    return lambda: _spec('ARM_KIND_SVE_IMMEDIATE', 0, flags, _check_name('ARM_SVE_IMM_' + scheme))


def OPT(scheme, f=None, flags='0'):
    """A named option of an ARM_OPT_SVE_* scheme at field f."""
    return lambda: _spec('ARM_KIND_OPTION', _field(f)[0] if f else 0, flags,
                         _check_name('ARM_OPT_SVE_' + scheme))


def MEM(scheme, scale=0, base='Rn'):
    """An SVE memory operand of ArmSveAddressScheme; scale is the shift (or
    the multiplier) of the form."""
    return lambda: _spec('ARM_KIND_SVE_MEMORY', _field(base)[0], str(scale),
                         _check_name('ARM_SVE_ADDR_' + scheme))


def KIND(kind, f, flags, extra):
    """A generic spec: the kind name, the field, the flags, the extra."""
    return lambda: _spec(kind, _field(f)[0] if f else 0, flags, extra)


# The general purpose and scalar SIMD registers (the kinds of the base)
def _gpr(name, flags):
    return lambda: _spec('ARM_KIND_GPR', _field(name)[0], flags, 5)


def X(f):
    return _gpr(f, 'ARM_GPR_X')


def W(f):
    return _gpr(f, 'ARM_GPR_W')


def XS(f):
    return _gpr(f, 'ARM_GPR_X | ARM_GPR_SP')


def WS(f):
    return _gpr(f, 'ARM_GPR_W | ARM_GPR_SP')


def fpr(f, scheme):
    schemes = {'b': 'ARM_FPR_B', 'h': 'ARM_FPR_H', 's': 'ARM_FPR_S', 'd': 'ARM_FPR_D',
               'q': 'ARM_FPR_Q', 'size': 'ARM_FPR_SIZE'}
    return lambda: _spec('ARM_KIND_A64_FPR', _field(f)[0], '0', schemes[scheme])


def GS(f, size='SIZE_22', sp=False):
    """A general purpose register of the size of the element: x for the double
    word, w otherwise (sp / wsp for number 31 with sp, else the zero one)."""
    return lambda: _spec('ARM_KIND_SVE_GPR', _field(f)[0], '1' if sp else '0', size_name(size))


def QV(f, size):
    """vN.<16b|8h|4s|2d>, the destination of the quadword reductions."""
    return lambda: _spec('ARM_KIND_SVE_V128', _field(f)[0], '0', size_name(size))


# ---------------------------------------------------------------------------
# The emitter

PREAMBLE = r'''#include "dismount/dismount.h"
/*
 * Arm64EncodingTableSve.cpp
 *
 * The encoding table of the A64 SVE, SVE2 and SVE2.1 instructions
 * (see ArmEncodingTable.h): the SVE integer, floating point, predicate,
 * permute and compare instructions, the loads, stores and prefetches, and
 * the extensions of them (SVE2, the crypto, bit permute, matrix multiply
 * and bfloat16 parts, SVE2.1, and the SME2 instructions which live in the
 * SVE encoding space).
 *
 * GENERATED by tests/arm/tools/gen_a64_sve_table.py: edit the generator, not
 * this file. The order of the entries is the order of the decoder: an alias
 * comes before the general encoding it prints for.
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

%(helpers)s

%(conditions)s

#undef BITS
} // anonymous namespace

const ArmEncodingEntry gArm64SveEncodingTable[] = {
'''

POSTAMBLE = r'''    // The end of the table
    { NULL, 0, 0, {{ ARM_KIND_NONE, 0, 0, 0 }}, 0, 0, 0, NULL }
};

} // namespace armdis
'''

FEATURES = {
    'SVE': 'FEAT_SVE', 'SVE2': 'FEAT_SVE2', 'AES': 'FEAT_SVE_AES', 'SHA3': 'FEAT_SVE_SHA3',
    'SM4': 'FEAT_SVE_SM4', 'BITPERM': 'FEAT_SVE_BITPERM', 'F32MM': 'FEAT_F32MM',
    'F64MM': 'FEAT_F64MM', 'I8MM': 'FEAT_I8MM', 'BF16': 'FEAT_BF16',
    'SVE2P1': 'FEAT_SVE2P1', 'B16B16': 'FEAT_SVE_B16B16', 'SVE2P2': 'FEAT_SVE2P2',
    'SVE2P3': 'FEAT_SVE2P3', 'SME': 'FEAT_SME', 'CPA': 'FEAT_CPA', 'FAMINMAX': 'FEAT_FAMINMAX',
    'FRINTTS': 'FEAT_FRINTTS', 'BFSCALE': 'FEAT_SVE_BFSCALE', 'AES2': 'FEAT_SVE_AES2',
    'LUT': 'FEAT_LUT', 'FP8DOT2': 'FEAT_FP8DOT2', 'FP8DOT4': 'FEAT_FP8DOT4',
    'FP8FMA': 'FEAT_FP8FMA', 'FP8': 'FEAT_FP8', 'F8F32MM': 'FEAT_F8F32MM',
    'F8F16MM': 'FEAT_F8F16MM',
}


def feature_name(feat):
    return FEATURES.get(feat, feat)


def emit(helpers, path):
    """Write the C++ file."""
    for entry in ENTRIES:
        _condition_function(entry)
    conditions = []
    for name, body in CONDITIONS:
        conditions.append('bool %s(uint32 e)\n{\n    return %s;\n}\n' % (name, body))
    out = [PREAMBLE % {'helpers': helpers, 'conditions': '\n'.join(conditions)}]
    lines = []
    for entry in ENTRIES:
        name = _condition_function(entry)
        ops = ', '.join(entry.ops)
        text = '    E("%s", 0x%08x, 0x%08x, %s, %s, %s, %s%s),' % (
            entry.mnemonic, entry.mask, entry.value, feature_name(entry.feat), entry.flags or 0,
            entry.flow or 0, name if name else 'NULL', (', ' + ops) if ops else '')
        lines.append('    // %s' % entry.pattern)
        lines.append(text)
    out.append('\n'.join(lines) + '\n')
    out.append(POSTAMBLE)
    open(path, 'w').write(''.join(out))
