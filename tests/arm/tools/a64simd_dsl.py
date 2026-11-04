#!/usr/bin/env python3
"""The operand builders of the A64 scalar floating point, Advanced SIMD and
cryptographic encodings (gArm64SimdEncodingTable), on top of a64dsl.py.

The groups are in a64groups_fp.py, a64groups_simd.py, a64groups_crypto.py and
a64groups_simdmem.py; tests/arm/tools/gen_a64_simd_table.py runs them and
writes Source/dismount/proc/arm/Arm64EncodingTableSimd.cpp. The schemes are
documented in Include/dismount/proc/arm/ArmOperandCodec.h.
"""
import a64dsl
from a64dsl import E, _spec, _field

a64dsl.FIELD_WIDTHS.update({
    'U': 1, 'M': 1, 'H': 1, 'ftype': 2, 'rmode': 2, 'scale': 6, 'imm8': 8,
    'immh': 4, 'immb': 3, 'cmode': 4, 'len': 2, 'rot': 2, 'a': 1, 'o': 1,
    'S': 1, 'rot1': 1, 'ig': 1, 'Rm4': 4, 'imm4': 4, 'abc': 3, 'defgh': 5, 'c': 1,
    'hx': 1, 'imm2': 2, 'i': 1,
})


def mask_of(allow):
    """The m_flags mask of the sizes in 'allow' (a list of size field values)."""
    if allow is None:
        return '0'
    return '0x%x' % sum(1 << s for s in allow)


def vec(f, scheme, allow=None, flags=None, no2s=False):
    """A vector register vN.<T>; the scheme is the suffix of ARM_ARR_*."""
    if flags is None:
        flags = mask_of(allow)
        if no2s:
            flags = '%s | ARM_ARR_FLAG_NO_2S' % flags
    return lambda: _spec('ARM_KIND_A64_VECTOR', _field(f)[0], flags,
                         'ARM_ARR_' + scheme.upper())


def vecf(f, arrangement):
    """A vector register with a fixed arrangement ('4S', '16B', '1Q'...)."""
    return lambda: _spec('ARM_KIND_A64_VECTOR', _field(f)[0],
                         'ArmDecodedOperand::ARRANGEMENT_%s' % arrangement.upper(),
                         'ARM_ARR_FIXED')


def sfp(f, scheme, allow=None, flags=None):
    """A scalar register with one of the schemes of ArmFprSizeScheme."""
    return lambda: _spec('ARM_KIND_A64_FPR', _field(f)[0],
                         flags if flags is not None else mask_of(allow),
                         'ARM_FPR_' + scheme.upper())


def elem(f, scheme, allow=None, flags=None):
    """A vector element vN.<T>[index]; the scheme is the suffix of ARM_ELEM_*."""
    return lambda: _spec('ARM_KIND_A64_VECTOR_ELEMENT', _field(f)[0],
                         flags if flags is not None else mask_of(allow),
                         'ARM_ELEM_' + scheme.upper())


def vlist(f, count, scheme, element=False, size10=False):
    """A list of 'count' vector registers; size10: the size field is at bit 10."""
    flags = str(count)
    if element:
        flags += ' | ARM_SPEC_LIST_ELEMENT'
    if size10:
        flags += ' | ARM_ARR_FLAG_SIZE10'
    return lambda: _spec('ARM_KIND_A64_VECTOR_LIST', _field(f)[0], flags,
                         'ARM_ARR_' + scheme.upper())


def fimm(size, f='imm8', split=False):
    """The 8 bit floating point immediate of 'size' ('h', 's', 'd')."""
    extra = {'h': 0, 's': 1, 'd': 2}[size]
    if split:
        return lambda: _spec('ARM_KIND_A64_FLOAT_IMMEDIATE', 5, 'ARM_FIMM_SPLIT', extra)
    return lambda: _spec('ARM_KIND_A64_FLOAT_IMMEDIATE', _field(f)[0], '0', extra)


def fzero(size='s', integer=False):
    """The #0.0 operand of fcmp / fcmeq; integer: #0 is read too (fcmeq)."""
    extra = {'h': 0, 's': 1, 'd': 2}[size]
    flags = 'ARM_FIMM_ZERO | ARM_FIMM_ZERO_INTEGER' if integer else 'ARM_FIMM_ZERO'
    return lambda: _spec('ARM_KIND_A64_FLOAT_IMMEDIATE', 0, flags, extra)


def modimm(scheme):
    """The modified immediate; the scheme is the suffix of ARM_MODIMM_*."""
    return lambda: _spec('ARM_KIND_A64_SIMD_MODIFIED_IMMEDIATE', 5, '0',
                         'ARM_MODIMM_' + scheme.upper())


def shiftimm(right=False, esize=False, hex_=False, xar=None):
    flags = []
    if right:
        flags.append('ARM_SIMD_SHIFT_RIGHT')
    if esize:
        flags.append('ARM_SIMD_SHIFT_ESIZE')
    if hex_:
        flags.append('ARM_SIMD_SHIFT_HEX')
    if xar:
        flags.append('ARM_SIMD_SHIFT_XAR')
    position = (lambda: _field(xar)[0]) if xar else (lambda: 16)
    return lambda: _spec('ARM_KIND_A64_SIMD_SHIFT_IMMEDIATE', position(),
                         ' | '.join(flags) if flags else '0', 0)


def fbits_gpr(f='scale'):
    return lambda: _spec('ARM_KIND_A64_FIXED_POINT_FBITS', _field(f)[0], '0', 0)


def fbits_vec():
    return lambda: _spec('ARM_KIND_A64_FIXED_POINT_FBITS', 16, 'ARM_FBITS_VECTOR', 0)


def rotation(f, scheme):
    return lambda: _spec('ARM_KIND_A64_SIMD_ROTATION', _field(f)[0], '0',
                         'ARM_ROT_' + scheme.upper())


def post_reg():
    return lambda: _spec('ARM_KIND_A64_MEMORY', _field('Rn')[0], '0', 'ARM_ADDR_SIMD_POST_REG')


def post_multi(count):
    return lambda: _spec('ARM_KIND_A64_MEMORY', _field('Rn')[0], str(count),
                         'ARM_ADDR_SIMD_POST_IMM_MULTI')


def post_fixed(bytes_):
    return lambda: _spec('ARM_KIND_A64_MEMORY', _field('Rn')[0], str(bytes_),
                         'ARM_ADDR_SIMD_POST_IMM_FIXED')


def base_mem():
    return lambda: _spec('ARM_KIND_A64_MEMORY', _field('Rn')[0], '0', 'ARM_ADDR_BASE')


def tied(f, other):
    return a64dsl.tied(f, other)


# ---------------------------------------------------------------------------
# The emitter of the Simd table

PREAMBLE = r'''#include "dismount/dismount.h"
/*
 * Arm64EncodingTableSimd.cpp
 *
 * The encoding table of the A64 scalar floating point, Advanced SIMD and
 * cryptographic instructions (see ArmEncodingTable.h): the scalar floating
 * point, the Advanced SIMD data processing (vector and scalar), the structure
 * loads and stores, the cryptographic extensions and the floating point,
 * dot product, matrix multiply, BFloat16, FP8 and lookup table extensions.
 *
 * GENERATED by tests/arm/tools/gen_a64_simd_table.py: edit the generator, not
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

const ArmEncodingEntry gArm64SimdEncodingTable[] = {
'''


def emit(helpers, path):
    """Write the C++ file. a64dsl.finalize_conditions() has to be called first."""
    conditions = []
    for name, body in a64dsl.CONDITIONS:
        conditions.append('bool %s(uint32 e)\n{\n    return %s;\n}\n' % (name, body))
    out = [PREAMBLE % {'helpers': helpers, 'conditions': '\n'.join(conditions)}]
    lines = []
    for entry in a64dsl.ENTRIES:
        name = a64dsl._condition_function(entry)
        ops = ', '.join(entry.ops)
        text = '    E("%s", 0x%08x, 0x%08x, %s, %s, %s, %s%s),' % (
            entry.mnemonic, entry.mask, entry.value, entry.feat, entry.flags or 0,
            entry.flow or 0, name if name else 'NULL', (', ' + ops) if ops else '')
        lines.append('    // %s' % entry.pattern)
        lines.append(text)
    out.append('\n'.join(lines) + '\n')
    out.append(a64dsl.POSTAMBLE)
    open(path, 'w').write(''.join(out))
