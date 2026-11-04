#!/usr/bin/env python3
"""Write Source/dismount/proc/arm/ArmSemanticTableNeon.inc, the entries of the semantic table of the
AArch32 Advanced SIMD integer instructions, the structure loads and stores and the cryptographic
extensions (included by ArmSemanticTableSimd.cpp): one entry per mnemonic of
gArmA32SimdEncodingTable which the lifters of ArmSemanticLiftNeon*.cpp / ArmSemanticLiftCrypto.cpp lift,
with the classification of the mnemonic.

The floating point entries (a floating point data type in the mnemonic, vmov, vmrs, vmsr, vldr, vstr,
vldm, vstm, vpush, vpop) belong to the other lifter (ArmSemanticTableVfp.inc) and are skipped here.

    gen_a32_neon_semantic_table.py          # rewrite the table
    gen_a32_neon_semantic_table.py --check  # exit 1 if the file differs
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

TABLE = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'ArmEncodingTableA32Simd.cpp')
OUTPUT = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'ArmSemanticTableNeon.inc')

# The base mnemonics of the floating point lifter (vmov, the transfers and the loads and stores of
# the VFP registers)
VFP_BASES = set('vmov vmrs vmsr vldr vstr vldmia vldmdb vstmia vstmdb vpush vpop fldmdbx fldmiax '
                'fstmdbx fstmiax'.split())

# base mnemonic -> (handler, category, unit, properties)
HANDLERS = {}


def add(bases, handler, category, unit, properties='0'):
    for b in bases.split():
        HANDLERS[b] = (handler, category, unit, properties)


add('vadd vsub vmul vmla vmls vabd vaba vhadd vhsub vrhadd vmax vmin vqadd vqsub vtst vceq vcge vcgt vcle vclt',
    'a32::liftNeonBinary', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vaddl vaddw vsubl vsubw vabdl vabal vmull vmlal vmlsl', 'a32::liftNeonLong', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vand vbic vorr vorn veor vbsl vbit vbif', 'a32::liftNeonLogic', 'LOGIC', 'VECTOR_INTEGER')
add('vmvn vabs vneg vqabs vqneg vcls vclz vcnt', 'a32::liftNeonUnary', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vaddhn vraddhn vsubhn vrsubhn', 'a32::liftNeonNarrowHigh', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vqdmulh vqrdmulh vqrdmlah vqrdmlsh vqdmull vqdmlal vqdmlsl', 'a32::liftNeonSaturatingDouble',
    'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vshr vsra vrshr vrsra vshl vqshl vqshlu vsli vsri', 'a32::liftNeonShiftImmediate', 'SHIFT', 'VECTOR_INTEGER')
add('vrshl vqrshl', 'a32::liftNeonShiftRegister', 'SHIFT', 'VECTOR_INTEGER')
add('vmovl vmovn vqmovn vqmovun vshll vshrn vrshrn vqshrn vqrshrn vqshrun vqrshrun',
    'a32::liftNeonNarrowMove', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vpadd vpmax vpmin vpaddl vpadal', 'a32::liftNeonPairwise', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vrev16 vrev32 vrev64 vswp vtrn vuzp vzip vext', 'a32::liftNeonPermute', 'VECTOR_PERMUTE', 'VECTOR_PERMUTE')
add('vtbl vtbx', 'a32::liftNeonTable', 'VECTOR_PERMUTE', 'VECTOR_PERMUTE')
add('vdup', 'a32::liftNeonDuplicate', 'VECTOR_PERMUTE', 'VECTOR_PERMUTE')
add('vrecpe vrsqrte', 'a32::liftNeonEstimate', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vld1 vld2 vld3 vld4', 'a32::liftNeonStructure', 'LOAD', 'LOAD', 'P_(READS_MEMORY)')
add('vst1 vst2 vst3 vst4', 'a32::liftNeonStructure', 'STORE', 'STORE', 'P_(WRITES_MEMORY)')
add('vsdot vudot vusdot vsudot', 'a32::liftNeonDot', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('vsmmla vummla vusmmla', 'a32::liftNeonMatrixMultiply', 'VECTOR_INTEGER', 'VECTOR_INTEGER')
add('aese aesd aesmc aesimc', 'a32::liftNeonCryptoAes', 'CRYPTO', 'CRYPTO')
add('sha1c sha1m sha1p sha1h sha1su0 sha1su1 sha256h sha256h2 sha256su0 sha256su1', 'a32::liftNeonCryptoSha',
    'CRYPTO', 'CRYPTO')

# The mnemonics which are NOT lifted (none today); base -> reason
UNLIFTED = {}


def is_float(mnemonic):
    base = mnemonic.split('.')[0]
    suffix = mnemonic[len(base):]
    return base in VFP_BASES or any(x in suffix for x in ('f16', 'f32', 'f64', 'bf16'))


def mnemonics():
    seen = []
    for line in open(TABLE):
        m = re.match(r'\s*E\("([^"]+)"', line)
        if m and m.group(1) not in seen:
            seen.append(m.group(1))
    return seen


def render():
    out = []
    lifted = []
    skipped = {}
    for mnemonic in mnemonics():
        if is_float(mnemonic):
            continue
        base = mnemonic.split('.')[0]
        if mnemonic == 'vmull.p64':
            handler = HANDLERS['vmull']
        elif base in HANDLERS and base not in UNLIFTED:
            handler = HANDLERS[base]
        else:
            skipped.setdefault(UNLIFTED.get(base, 'no lifter'), []).append(mnemonic)
            continue
        suffix = mnemonic[len(base):]
        size = re.search(r'(\d+)$', suffix)
        bits = int(size.group(1)) if size else 0
        if bits > 64:
            bits = 0
        fn, category, unit, properties = handler
        props = properties
        out.append('    ARM32_ENTRY_DATA("%s", %s, %s, U_(%s), %s, 0, 0, SemanticType::KIND_INTEGER, 0, %d, 0),' %
                   (mnemonic, fn, category, unit, props, bits))
        lifted.append(mnemonic)
    header = ['// The entries of the Advanced SIMD integer, structure load / store and cryptographic instructions of',
              '// AArch32 (included by ArmSemanticTableSimd.cpp).',
              '// GENERATED by tests/arm/tools/gen_a32_neon_semantic_table.py: edit the generator, not this file.',
              '// %d mnemonics are lifted (ArmSemanticLiftNeon.cpp, ArmSemanticLiftNeonMemory.cpp,' % len(lifted),
              '// ArmSemanticLiftCrypto.cpp); every mnemonic of the Advanced SIMD tables which is not a floating point',
              '// one has an entry.']
    for reason, names in sorted(skipped.items()):
        header.append('// NOT lifted (%s): %s' % (reason, ' '.join(names)))
    return '\n'.join(header + out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    text = render()
    if args.check:
        return 0 if open(OUTPUT).read() == text else 1
    with open(OUTPUT, 'w') as f:
        f.write(text)
    print('wrote %s' % OUTPUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
