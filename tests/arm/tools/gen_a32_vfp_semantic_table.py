#!/usr/bin/env python3
"""Write Source/dismount/proc/arm/ArmSemanticTableVfp.inc, the semantic entries of the AArch32
floating point instructions (the first half of gArmSimdSemanticTable, ArmSemanticTableSimd.cpp):
one entry per mnemonic of gArmA32SimdEncodingTable which this lifter owns, with its lifter
(ArmSemanticLiftVfp.cpp) and its classification.

The ownership is by the full mnemonic of the encoding entry:
  * every mnemonic whose data type is floating point (f16, f32, f64, bf16 in the text after the
    base: vadd.f32, vcvt.s32.f32, vcmp.f64, vdot.bf16...)
  * vmov (all its forms), vmrs, vmsr, vldr, vstr, vldmia, vldmdb, vstmia, vstmdb, vpush, vpop,
    fldmiax, fldmdbx, fstmiax, fstmdbx
The other mnemonics of the table (the integer Advanced SIMD, the structure loads and stores, the
cryptographic extensions) are ArmSemanticTableNeon.inc.

    gen_a32_vfp_semantic_table.py          # rewrite the file
    gen_a32_vfp_semantic_table.py --check  # exit 1 if the file differs
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

TABLE = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'ArmEncodingTableA32Simd.cpp')
OUTPUT = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'ArmSemanticTableVfp.inc')

OWNED_BASES = set('vmov vmrs vmsr vldr vstr vldmia vldmdb vstmia vstmdb vpush vpop '
                  'fldmdbx fldmiax fstmdbx fstmiax'.split())

# The mnemonics which are not lifted, and why (full match on the mnemonic)
UNLIFTED = []

# base mnemonic (regular expression, full match) -> lifter
HANDLERS = [
    (r'vadd|vsub|vmul|vdiv|vnmul|vabd|vmax|vmin|vmaxnm|vminnm|vmla|vmls|vnmla|vnmls|vfma|vfms|'
     r'vfnma|vfnms|vrecps|vrsqrts', 'liftFpArithmetic'),
    (r'vpadd|vpmax|vpmin', 'liftFpPairwise'),
    (r'vabs|vneg|vsqrt|vrecpe|vrsqrte|vrint[amnpxzr]', 'liftFpUnary'),
    (r'vcmp|vcmpe', 'liftFpCompare'),
    (r'vceq|vcge|vcgt|vcle|vclt|vacge|vacgt', 'liftFpVectorCompare'),
    (r'vsel(eq|ge|gt|vs)', 'liftFpSelect'),
    (r'vjcvt', 'liftFpJavascript'),
    (r'vcvt|vcvtr|vcvta|vcvtn|vcvtp|vcvtm|vcvtb|vcvtt', 'liftFpConvert'),
    (r'vins|vmovx', 'liftFpHalfMove'),
    (r'vcadd|vcmla', 'liftFpComplex'),
    (r'vfmal|vfmsl', 'liftFpWidenMultiply'),
    (r'vdot|vmmla|vfmab|vfmat', 'liftBfloat'),
    (r'vmov', 'liftFpMove'),
    (r'vmrs|vmsr', 'liftFpStatus'),
    (r'vldr|vstr', 'liftFpMemory'),
    (r'vldmia|vldmdb|vstmia|vstmdb|vpush|vpop|fldmdbx|fldmiax|fstmdbx|fstmiax', 'liftFpMultiple'),
]

FLAGS = {
    'vseleq': 'SemanticFlags::FLAG_ZERO',
    'vselge': 'SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_OVERFLOW',
    'vselgt': 'SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_OVERFLOW',
    'vselvs': 'SemanticFlags::FLAG_OVERFLOW',
}
NZCV = ('SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_CARRY | '
        'SemanticFlags::FLAG_OVERFLOW')


def entries():
    out = []
    for line in open(TABLE):
        m = re.match(r'\s*E\("([^"]+)",\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(\w+),\s*([^,]+),\s*([^,]+),\s*(\w+)(.*)\),?\s*$', line)
        if not m:
            continue
        kinds = tuple(k.replace('ARM_KIND_', '') for k in re.findall(r'S\((\w+),', m.group(8)))
        out.append((m.group(1), kinds))
    return out


def owned(mnemonic):
    base = mnemonic.split('.')[0]
    suffix = mnemonic[len(base):]
    return base in OWNED_BASES or any(t in suffix for t in ('f16', 'f32', 'f64', 'bf16'))


def classify(mnemonic, shapes):
    base = mnemonic.split('.')[0]
    suffix = mnemonic[len(base):]
    vector = any(('DREG' in s or 'QREG' in s) and 'f64' not in suffix for s in shapes) and not \
        (base in ('vldr', 'vstr', 'vldmia', 'vldmdb', 'vstmia', 'vstmdb', 'vpush', 'vpop') or base.startswith('f'))
    category = 'VECTOR_FLOAT' if vector else 'FLOAT'
    units = 'U_(VECTOR_FLOAT)' if vector else 'U_(FLOAT_ADD)'
    props = '0'
    read = '0'
    written = '0'
    if re.match(r'vmul|vnmul|vfm|vfnm|vmla|vmls|vnmla|vnmls|vdot|vmmla|vcmla|vrecps|vrsqrts', base):
        units = 'U_(VECTOR_FLOAT)' if vector else 'U_(FLOAT_MULTIPLY)'
    elif base == 'vdiv' or base == 'vsqrt':
        units = 'U_(FLOAT_DIVIDE)'
    elif base.startswith('vcvt') or base == 'vjcvt':
        category = 'CONVERT'
        units = 'U_(FLOAT_CONVERT)'
    elif base == 'vmov' or base in ('vins', 'vmovx'):
        category = 'MOVE'
        units = 'U_(FLOAT_ADD)'
    elif base in ('vcmp', 'vcmpe'):
        category = 'COMPARE'
    elif base.startswith('vsel'):
        category = 'CONDITIONAL_MOVE'
        read = FLAGS[base]
    elif base in ('vmrs', 'vmsr'):
        category = 'SYSTEM'
        units = 'U_(SYSTEM)'
        if base == 'vmrs':
            written = NZCV
    elif base in ('vldr', 'vldmia', 'vldmdb', 'fldmdbx', 'fldmiax'):
        category = 'LOAD'
        units = 'U_(LOAD)'
        props = 'P_(READS_MEMORY)'
    elif base in ('vstr', 'vstmia', 'vstmdb', 'fstmdbx', 'fstmiax'):
        category = 'STORE'
        units = 'U_(STORE)'
        props = 'P_(WRITES_MEMORY)'
    elif base == 'vpop':
        category = 'STACK'
        units = 'U_(LOAD)'
        props = 'P_(READS_MEMORY) | P_(CHANGES_STACK)'
    elif base == 'vpush':
        category = 'STACK'
        units = 'U_(STORE)'
        props = 'P_(WRITES_MEMORY) | P_(CHANGES_STACK)'
    return category, units, props, read, written


def data_type(mnemonic):
    """(kind, format, bits) of the floating point data of a mnemonic, or None."""
    parts = mnemonic.split('.')[1:]
    for part in reversed(parts):
        if part in ('f16', 'f32', 'f64', 'bf16'):
            fmt = {'f16': 'FLOAT_HALF', 'f32': 'FLOAT_SINGLE', 'f64': 'FLOAT_DOUBLE', 'bf16': 'FLOAT_BFLOAT16'}[part]
            return 'SemanticType::KIND_FLOAT', 'SemanticType::' + fmt, 64 if part == 'f64' else (32 if part == 'f32' else 16)
    return None


def render():
    shapes = {}
    order = []
    for mnemonic, kinds in entries():
        if not owned(mnemonic):
            continue
        if mnemonic not in shapes:
            order.append(mnemonic)
            shapes[mnemonic] = set()
        shapes[mnemonic].add(' '.join(kinds))
    lines = ['// The entries of the floating point instructions of AArch32 (included by ArmSemanticTableSimd.cpp)',
             '// GENERATED by tests/arm/tools/gen_a32_vfp_semantic_table.py: edit the generator, not this file.',
             '// %d mnemonics are lifted (ArmSemanticLiftVfp.cpp).' % (len(order) - len(UNLIFTED))]
    if UNLIFTED:
        lines.append('// Not lifted:')
        for name, reason in UNLIFTED:
            lines.append('//   %s: %s' % (name, reason))
    else:
        lines.append('// Every mnemonic of the floating point part of the table is lifted.')
    for mnemonic in order:
        base = mnemonic.split('.')[0]
        if any(mnemonic == u[0] for u in UNLIFTED):
            continue
        handler = None
        for pattern, name in HANDLERS:
            if re.fullmatch(pattern, base):
                handler = name
                break
        if handler is None:
            raise SystemExit('no lifter for %s' % mnemonic)
        category, units, props, read, written = classify(mnemonic, shapes[mnemonic])
        data = data_type(mnemonic)
        if data is None or category in ('LOAD', 'STORE', 'STACK', 'SYSTEM'):
            lines.append('ARM32_ENTRY("%s", a32::%s, %s, %s, %s, %s, %s),' % (
                mnemonic, handler, category, units, props, read, written))
        else:
            lines.append('ARM32_ENTRY_DATA("%s", a32::%s, %s, %s, %s, %s, %s, %s, %s, %d, 0),' % (
                mnemonic, handler, category, units, props, read, written, data[0], data[1], data[2]))
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    text = render()
    if args.check:
        return 0 if open(OUTPUT).read() == text else 1
    open(OUTPUT, 'w').write(text)
    print('wrote %s' % OUTPUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
