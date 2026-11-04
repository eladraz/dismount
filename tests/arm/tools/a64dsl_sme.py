#!/usr/bin/env python3
"""The language of the A64 SME encoding table, and the C++ emitter.

It is the language of a64dsl_sve.py (an encoding is a pattern of 32 bits from
the most significant, the tokens are bit strings and field names, the
operands refer to the fields by name) with the operand builders of the SME
kinds (ARM_KIND_SME_*, see ArmEncodingTable.h and ArmOperandCodec.h), the
second feature of an entry (feat2: the instructions which LLVM guards with two
features), and the table of the index layouts which the generated file
defines (gArmSmeIndexLayouts).

    E('smopa', '10100000 100 Zm:5 Pm:3 Pn:3 Zn:5 0 0 0 ZAda:2',
      [TILE('ZAda', 's', 2), P('Pn', q='m', w=3), P('Pm', q='m', w=3),
       Z('Zn', 'B'), Z('Zm', 'B')], feat='SME')

The rows of the learned instructions (a64groups_sme_learned.py) are added with
add_row(): the mask, the value and the operand specs of the C++ table.
"""
import os
import re

import a64dsl_sve as base
from a64dsl_sve import (ENTRIES, FIELD_WIDTHS, REPO, Z, ZL, ZI, P, PN, X, XS, W, IMM, OPT, KIND, MEM, E as _base_E,
                        order_aliases, _field, _spec, size_name, _check_name)

OUTPUT = os.path.join(REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64EncodingTableSme.cpp')

FIELD_WIDTHS.update({
    'ZAda': 2, 'ZAt': 4, 'Rs': 2, 'V': 1, 'Zk': 2, 'off': 3, 'Pn': 4, 'Pm': 4,
})

# The size letters of the SME specs: the order of ArmSveSizeScheme ARM_SVE_NONE ... Q
SIZES = {'none': 0, 'b': 1, 'h': 2, 's': 3, 'd': 4, 'q': 5}
SIZE_NAMES = {0: 'ARM_SVE_NONE', 1: 'ARM_SVE_B', 2: 'ARM_SVE_H', 3: 'ARM_SVE_S', 4: 'ARM_SVE_D', 5: 'ARM_SVE_Q'}


# The instructions which also run outside of the streaming mode (they need ZA or ZT0
# enabled only): the others are valid only in the streaming mode
STREAMING = 'ARM_ENTRY_STREAMING_ONLY'
NON_STREAMING_MNEMONICS = ('ldr', 'str', 'zero')


def E(mnemonic, pattern, ops, feat='SME', feat2=None, streaming=None, **kwargs):
    """Add an encoding (see a64dsl_sve.E). feat2 is a second feature which the
    instruction needs; a list of features in feat (alternatives) adds one
    entry for each of them; streaming: the instruction is valid only in the streaming
    mode (the default but ldr, str and zero)"""
    if streaming is None:
        streaming = mnemonic not in NON_STREAMING_MNEMONICS
    if streaming and 'flags' not in kwargs:
        kwargs['flags'] = STREAMING
    if isinstance(feat, (list, tuple)):
        entries = []
        for i, alternative in enumerate(feat):
            f2 = feat2[i] if isinstance(feat2, (list, tuple)) else feat2
            entries.append(E(mnemonic, pattern, ops, feat=alternative, feat2=f2, streaming=streaming, **kwargs))
        return entries[0]
    entry = _base_E(mnemonic, pattern, ops, feat=feat, **kwargs)
    entry.feat2 = feat2
    return entry


# ---------------------------------------------------------------------------
# The operand builders

def TILE(f, size, width):
    """za<n>.<T>: the tile number is the field f of 'width' bits"""
    return lambda: _spec('ARM_KIND_SME_TILE', _field(f)[0] if width else 0, str(width), SIZE_NAMES[SIZES[size]])


def SLICE(f, size, count=1, braces=False):
    """za<n><h|v>.<T>[wV, offset]: f is the field of the tile and the offset (the
    low bit of it); size is b h s d q or 'field' (the size field of mova);
    count is the number of slices (1, 2 or 4)"""
    selector = 5 if size == 'field' else SIZES[size] - 1
    flags = selector | ({1: 0, 2: 1, 4: 2}[count] << 3) | (0x20 if braces else 0)
    return lambda: _spec('ARM_KIND_SME_TILE_SLICE', _field(f)[0], str(flags), 0)


def ARRAY(f, size, group=0, rng=1, width=3, w12=False, optional_group=False):
    """za.<T>[wV, offset{:last}{, vgxN}]: f is the offset field of 'width' bits
    (the offset is the field times the length of the range), group is 0, 2
    or 4 (vgx), w12: the vector select register is w12-w15"""
    flags = {0: 0, 2: 1, 4: 2}[group] | ({1: 0, 2: 1, 4: 2}[rng] << 2) | (width << 4) | (0x80 if w12 else 0)
    extra = SIZE_NAMES[SIZES[size]] + (' | ARM_SME_ARRAY_OPTIONAL_GROUP' if optional_group else '')
    return lambda: _spec('ARM_KIND_SME_ARRAY', _field(f)[0] if f else 0, str(flags), extra)


def TILELIST(f):
    """{za0.d, za2.d}: the mask of the 64 bit tiles is the 8 bit field f"""
    return lambda: _spec('ARM_KIND_SME_TILE_LIST', _field(f)[0], '0', 0)


def ZT0(scheme='PLAIN', f=None):
    """zt0, zt0[imm3], zt0[imm2, mul vl]"""
    return lambda: _spec('ARM_KIND_SME_ZT0', _field(f)[0] if f else 0, '0', 'ARM_SME_ZT0_' + scheme)


def ZAMEM(scheme, base='Rn', scale=0):
    """[xN{, #imm4, mul vl}] of ldr / str za (ZA), [xN] (BASE), [xN{, xM{, lsl #scale}}]
    of the loads and stores of the tiles (SCALAR_OPTIONAL)"""
    return lambda: _spec('ARM_KIND_SME_MEMORY', _field(base)[0], str(scale), 'ARM_SME_MEM_' + scheme)


LAYOUTS = []     # (fields [(position, width)], list count, log2 multiple, base, split bits, split step)


def _layout(fields, list_count, log2_multiple, base=0, split_bits=0, split_step=0):
    key = (tuple(fields), list_count, log2_multiple, base, split_bits, split_step)
    if key not in LAYOUTS:
        LAYOUTS.append(key)
    return LAYOUTS.index(key)


def ZIDX(f, width, size, index, list_count=0, log2_multiple=0, base=0):
    """z<n>.<T>[index], z<n>[index] (size 'none') or { z<n>, z<n+1> }[index]:
    f is the register field of 'width' bits, index is a list of the names of
    the fields of the index, from the most significant"""
    def build():
        fields = [_field(name) for name in index]
        layout = _layout([(pos, w) for pos, w in fields], list_count, log2_multiple, base)
        code = 7 if size == 'none' else SIZES[size] - 1
        flags = width | (code << 4)
        return _spec('ARM_KIND_SME_Z_INDEXED', _field(f)[0], str(flags), layout)
    return build


def _spec_string(spec):
    kind, position, flags, extra = spec
    if isinstance(extra, tuple):
        # ('LAYOUT', [(position, width)], list count, log2 multiple, base)
        extra = _layout(list(extra[1]), *extra[2:])
    return 'S(%s, %d, %d, %s)' % (kind, position, flags, extra)


def add_row(mnemonic, mask, value, features, specs, comment):
    """Add an encoding made of its bits (the rows of a64groups_sme_learned.py):
    features is a list of alternatives (feature, second feature)"""
    ops = [_spec_string(spec) for spec in specs]
    flags = 0 if mnemonic in NON_STREAMING_MNEMONICS else STREAMING
    for feat, feat2 in features:
        entry = base.Entry(mnemonic, comment, ops, feat, flags, 0, None)
        entry.mask = mask
        entry.value = value
        entry.fields = {}
        entry.alias = False
        entry.feat2 = feat2
        ENTRIES.append(entry)


# ---------------------------------------------------------------------------
# The emitter

PREAMBLE = r'''#include "dismount/dismount.h"
/*
 * Arm64EncodingTableSme.cpp
 *
 * The encoding table of the A64 SME, SME2 and SME2.1 instructions
 * (see ArmEncodingTable.h): the outer products, the loads, stores and moves
 * of the za array, the multi vector instructions with the vector groups of
 * za, the lookup tables of zt0, and the extensions of them (the FP8, BFloat16,
 * 16 bit floating point and 64 bit parts, SME2.1, SME2.2, the 4 way outer
 * products and the structured sparse ones).
 *
 * GENERATED by tests/arm/tools/gen_a64_sme_table.py: edit the generator, not
 * this file. The order of the entries is the order of the decoder.
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
// flow, the condition, the second feature and the operands
#define E(mnemonic, mask, value, feature, flags, flow, condition, feature2, ...) \
    { mnemonic, mask, value, { __VA_ARGS__ }, ArmFeatures::feature, flags, flow, condition, feature2 }

%(conditions)s

#undef BITS
} // anonymous namespace

// The layouts of the indexes of the indexed z registers
const ArmSmeIndexLayout gArmSmeIndexLayouts[] = {
%(layouts)s
};
const uint gArmSmeIndexLayoutsCount = sizeof(gArmSmeIndexLayouts) / sizeof(gArmSmeIndexLayouts[0]);

const ArmEncodingEntry gArm64SmeEncodingTable[] = {
'''

POSTAMBLE = r'''    // The end of the table
    { NULL, 0, 0, {{ ARM_KIND_NONE, 0, 0, 0 }}, 0, 0, 0, NULL, 0 }
};

} // namespace armdis
'''

FEATURES = dict(base.FEATURES)
FEATURES.update({
    'SME': 'FEAT_SME', 'SME2': 'FEAT_SME2', 'SME2P1': 'FEAT_SME2P1', 'SME2P2': 'FEAT_SME2P2',
    'SME2P3': 'FEAT_SME2P3', 'F64F64': 'FEAT_SME_F64F64', 'I16I64': 'FEAT_SME_I16I64',
    'F16F16': 'FEAT_SME_F16F16', 'SME_B16B16': 'FEAT_SME_B16B16', 'F8F16': 'FEAT_SME_F8F16',
    'F8F32': 'FEAT_SME_F8F32', 'LUTV2': 'FEAT_SME_LUTV2', 'MOP4': 'FEAT_SME_MOP4',
    'TMOP': 'FEAT_SME_TMOP',
})


def feature_name(feat):
    return FEATURES.get(feat, feat)


def emit(path=OUTPUT):
    """Write the C++ file"""
    for entry in ENTRIES:
        base._condition_function(entry)
    conditions = []
    for name, body in base.CONDITIONS:
        conditions.append('bool %s(uint32 e)\n{\n    return %s;\n}\n' % (name, body))
    layouts = []
    for fields, list_count, log2_multiple, base_register, split_bits, split_step in LAYOUTS:
        positions = [p for p, _ in fields] + [0] * (3 - len(fields))
        widths = [w for _, w in fields] + [0] * (3 - len(fields))
        assert len(fields) <= 3
        layouts.append('    { %d, { %s }, { %s }, %d, %d, %d, %d, %d },' % (
            len(fields), ', '.join(str(p) for p in positions), ', '.join(str(w) for w in widths),
            list_count, log2_multiple, base_register, split_bits, split_step))
    if not layouts:
        layouts.append('    { 0, { 0, 0, 0 }, { 0, 0, 0 }, 0, 0, 0, 0, 0 }')
    out = [PREAMBLE % {'conditions': '\n'.join(conditions), 'layouts': '\n'.join(layouts)}]
    lines = []
    for entry in ENTRIES:
        name = base._condition_function(entry)
        ops = ', '.join(entry.ops)
        feat2 = getattr(entry, 'feat2', None)
        lines.append('    // %s' % entry.pattern)
        lines.append('    E("%s", 0x%08x, 0x%08x, %s, %s, %s, %s, %s%s),' % (
            entry.mnemonic, entry.mask, entry.value, feature_name(entry.feat), entry.flags or 0,
            entry.flow or 0, name if name else 'NULL',
            ('ArmFeatures::' + feature_name(feat2)) if feat2 else '0',
            (', ' + ops) if ops else ''))
    out.append('\n'.join(lines) + '\n')
    out.append(POSTAMBLE)
    open(path, 'w').write(''.join(out))
