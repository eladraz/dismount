#!/usr/bin/env python3
"""Write Source/dismount/proc/arm/ArmSemanticTable.cpp, the semantic table of the AArch32 integer,
branch, load / store and system instructions (gArmSemanticTable, A32 and T32 share it): one entry
per mnemonic of gArmA32EncodingTable, gArmT16EncodingTable and gArmT32EncodingTable which the
lifters (ArmSemanticLift32.cpp, ArmSemanticLift32Memory.cpp) lift, with the handler and the
classification of the mnemonic (category, units, properties, flags).

The mnemonics which are not lifted are not in the table: they are lifted to OP_UNDEFINED and
classified as CATEGORY_UNKNOWN, and ArmFeatures::getSupport() of their feature has no SUPPORT_LIFT
(see UNLIFTED below for the reason of each group).

    gen_a32_semantic_table.py          # rewrite the table
    gen_a32_semantic_table.py --check  # exit 1 if the file differs
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

ARM = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm')
TABLES = ['ArmEncodingTableA32.cpp', 'ArmEncodingTableT16.cpp', 'ArmEncodingTableT32.cpp']
OUTPUT = os.path.join(ARM, 'ArmSemanticTable.cpp')

# The mnemonics which are NOT lifted, and why (regular expressions, full match, on the mnemonic
# without the T32 ".w")
UNLIFTED = [
    (r'cps|setpan|setend', 'system state: the interrupt masks, PAN and the data endianness (E) are not modeled'),
    (r'eret|rfe[a-z]+|srs[a-z]+|smc|hvc|dcps[123]', 'privileged: exception return, other exception levels, monitor calls'),
    (r'mcr2|mrc2|mcrr2?|mrrc2?|cdp2?|ldc2?l?|stc2?l?', 'the coprocessors (but the thread id registers of mrc / mcr): not modeled'),
    (r'swpb?', 'FEAT_V8: SWP / SWPB are UNDEFINED before the extension is enabled'),
    (r'bti|pac|aut|pacbti', 'FEAT_PACBTI (M profile): the keys are not modeled'),
    (r'subs', 'T32 subs pc, lr, #imm: an exception return (the SPSR is not modeled)'),
    (r'__brkdiv0', 'LLVM pseudo instruction'),
]

F_NZCV = 'SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW'
F_NZC = 'SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_CARRY'
F_NZ = 'SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO'
F_C = 'SemanticFlags::FLAG_CARRY'

# (regular expression, handler, category, units, properties, flags read, flags written). First
# match wins. The names are CATEGORY_x, UNIT_x, PROPERTY_x of OpcodeClassification
RULES = [
    (r'adc|sbc|rsc', 'liftDataProcessing', 'ARITHMETIC', ['INTEGER_ALU'], [], F_C, F_NZCV),
    (r'add|sub|rsb|addw|subw', 'liftDataProcessing', 'ARITHMETIC', ['INTEGER_ALU'], [], None, F_NZCV),
    (r'cmp|cmn', 'liftDataProcessing', 'COMPARE', ['INTEGER_ALU'], [], None, F_NZCV),
    (r'tst|teq', 'liftDataProcessing', 'COMPARE', ['INTEGER_ALU'], [], F_C, F_NZC),
    (r'and|eor|orr|bic|orn', 'liftDataProcessing', 'LOGIC', ['INTEGER_ALU'], [], None, F_NZC),
    (r'mov|movs|mvn', 'liftDataProcessing', 'MOVE', ['INTEGER_ALU'], [], None, F_NZC),
    (r'rrx', 'liftShift', 'SHIFT', ['SHIFTER'], [], F_C, F_NZC),
    (r'lsl|lsr|asr|ror', 'liftShift', 'SHIFT', ['SHIFTER'], [], None, F_NZC),
    (r'adr', 'liftAdr', 'ARITHMETIC', ['INTEGER_ALU'], [], None, None),
    (r'movw|movt', 'liftMoveWide', 'MOVE', ['INTEGER_ALU'], [], None, None),
    (r'mul|mla|mls', 'liftMultiply', 'MULTIPLY', ['INTEGER_MULTIPLY'], [], None, F_NZ),
    (r'umull|smull|umlal|smlal|umaal', 'liftLongMultiply', 'MULTIPLY', ['INTEGER_MULTIPLY'], [], None, F_NZ),
    (r'smul[bt][bt]|smla[bt][bt]|smulw[bt]|smlaw[bt]|smlal[bt][bt]', 'liftHalfwordMultiply', 'MULTIPLY',
     ['INTEGER_MULTIPLY'], [], None, None),
    (r'smuadx?|smusdx?|smladx?|smlsdx?|smlaldx?|smlsldx?', 'liftDualMultiply', 'MULTIPLY',
     ['INTEGER_MULTIPLY'], [], None, None),
    (r'smmulr?|smmlar?|smmlsr?', 'liftMostSignificantMultiply', 'MULTIPLY', ['INTEGER_MULTIPLY'], [], None, None),
    (r'usad8|usada8', 'liftSumAbsoluteDifferences', 'ARITHMETIC', ['INTEGER_ALU'], [], None, None),
    (r'sdiv|udiv', 'liftDivide', 'DIVIDE', ['INTEGER_DIVIDE'], [], None, None),
    (r'qadd|qsub|qdadd|qdsub', 'liftSaturatingAddSub', 'ARITHMETIC', ['INTEGER_ALU'], [], None, None),
    (r'ssat|usat', 'liftSaturate', 'ARITHMETIC', ['INTEGER_ALU'], [], None, None),
    (r'ssat16|usat16', 'liftSaturate16', 'ARITHMETIC', ['INTEGER_ALU'], [], None, None),
    (r'(?:s|q|sh|u|uq|uh)(?:add16|add8|sub16|sub8|asx|sax)', 'liftParallel', 'ARITHMETIC', ['INTEGER_ALU'], [],
     None, None),
    (r'sel', 'liftSelect', 'CONDITIONAL_MOVE', ['INTEGER_ALU'], [], None, None),
    (r'[su]xta?(?:b|h|b16)', 'liftExtend', 'CONVERT', ['INTEGER_ALU'], [], None, None),
    (r'pkhbt|pkhtb', 'liftPack', 'BIT_MANIPULATION', ['INTEGER_ALU'], [], None, None),
    (r'bfc|bfi', 'liftBitFieldInsert', 'BIT_MANIPULATION', ['INTEGER_ALU'], [], None, None),
    (r'ubfx|sbfx', 'liftBitFieldExtract', 'BIT_MANIPULATION', ['INTEGER_ALU'], [], None, None),
    (r'rev|rev16|revsh|rbit|clz', 'liftReverse', 'BIT_MANIPULATION', ['INTEGER_ALU'], [], None, None),
    (r'crc32c?[bhw]', 'liftCrc32', 'CRYPTO', ['INTEGER_ALU'], [], None, None),
    (r'bl', 'liftBranchLink', 'CALL', ['BRANCH'], [], None, None),
    (r'blx', 'liftBlx', 'CALL', ['BRANCH'], ['CHANGES_MODE'], None, None),
    (r'b', 'liftBranch', 'BRANCH', ['BRANCH'], [], None, None),
    (r'bx|bxj', 'liftBranchExchange', 'BRANCH', ['BRANCH'], ['CHANGES_MODE'], None, None),
    (r'cbz|cbnz', 'liftCompareBranch', 'BRANCH', ['BRANCH'], [], None, None),
    (r'tbb|tbh', 'liftTableBranch', 'BRANCH', ['BRANCH', 'LOAD'], ['READS_MEMORY'], None, None),
    (r'it|nop|hint|dbg|esb|csdb|tsb', 'liftNop', 'NOP', ['SYSTEM'], [], None, None),
    (r'yield|wfe|wfi|sev|sevl', 'liftHint', 'NOP', ['SYSTEM'], [], None, None),
    (r'dmb|dsb|isb|ssbb|pssbb|sb', 'liftBarrier', 'SYSTEM', ['SYSTEM'], ['SERIALIZING'], None, None),
    (r'svc|bkpt|udf|hlt|trap', 'liftException', 'INTERRUPT', ['SYSTEM'], [], None, None),
    (r'mcr|mrc', 'liftCoprocessorMove', 'SYSTEM', ['SYSTEM'], [], None, None),
    (r'mrs', 'liftReadApsr', 'SYSTEM', ['SYSTEM'], [], None, None),
    (r'msr', 'liftWriteApsr', 'SYSTEM', ['SYSTEM'], [], None, None),
    (r'clrex', 'liftClearExclusive', 'SYSTEM', ['SYSTEM'], [], None, None),
    (r'ldr|ldrb|ldrh|ldrsb|ldrsh|ldrt|ldrbt|ldrht|ldrsbt|ldrsht', 'liftLoadStore', 'LOAD', ['LOAD'],
     ['READS_MEMORY'], None, None),
    (r'str|strb|strh|strt|strbt|strht', 'liftLoadStore', 'STORE', ['STORE'], ['WRITES_MEMORY'], None, None),
    (r'lda|ldab|ldah', 'liftLoadStore', 'LOAD', ['LOAD'], ['READS_MEMORY', 'SERIALIZING'], None, None),
    (r'stl|stlb|stlh', 'liftLoadStore', 'STORE', ['STORE'], ['WRITES_MEMORY', 'SERIALIZING'], None, None),
    (r'ldrd', 'liftLoadStoreDouble', 'LOAD', ['LOAD'], ['READS_MEMORY'], None, None),
    (r'strd', 'liftLoadStoreDouble', 'STORE', ['STORE'], ['WRITES_MEMORY'], None, None),
    (r'push', 'liftLoadStoreMultiple', 'STACK', ['STORE'], ['WRITES_MEMORY', 'CHANGES_STACK'], None, None),
    (r'pop', 'liftLoadStoreMultiple', 'STACK', ['LOAD'], ['READS_MEMORY', 'CHANGES_STACK'], None, None),
    (r'ldm(?:ia|ib|da|db)?', 'liftLoadStoreMultiple', 'LOAD', ['LOAD'], ['READS_MEMORY'], None, None),
    (r'stm(?:ia|ib|da|db)?', 'liftLoadStoreMultiple', 'STORE', ['STORE'], ['WRITES_MEMORY'], None, None),
    (r'ldrex[bhd]?|ldaex[bhd]?', 'liftExclusive', 'ATOMIC', ['LOAD'], ['READS_MEMORY', 'EXCLUSIVE'], None, None),
    (r'strex[bhd]?|stlex[bhd]?', 'liftExclusive', 'ATOMIC', ['STORE'], ['WRITES_MEMORY', 'EXCLUSIVE'], None, None),
    (r'pld|pldw|pli', 'liftPrefetch', 'LOAD', ['LOAD'], [], None, None),
]


def mnemonics():
    """The distinct mnemonics of the three tables, as the tables spell them, in order."""
    seen = []
    for name in TABLES:
        for line in open(os.path.join(ARM, name)):
            m = re.match(r'\s*E\("([^"]+)",', line)
            if m and m.group(1) not in seen:
                seen.append(m.group(1))
    return seen


def base_name(mnemonic):
    return mnemonic[:-2] if mnemonic.endswith('.w') else mnemonic


def classify(mnemonic):
    base = base_name(mnemonic)
    for rx, reason in UNLIFTED:
        if re.fullmatch(rx, base):
            return None
    for rule in RULES:
        if re.fullmatch(rule[0], base):
            return rule
    raise SystemExit('no rule for %s' % mnemonic)


def build():
    lifted, unlifted = [], []
    for mnemonic in mnemonics():
        rule = classify(mnemonic)
        (unlifted if rule is None else lifted).append((mnemonic, rule))
    out = []
    out.append('''/*
 * ArmSemanticTable.cpp
 *
 * Implementation file
 *
 * The semantic table of the AArch32 integer, branch, load / store and system
 * instructions (A32, T16 and T32 share it, see ArmSemantics.h): the mnemonics
 * of the three encoding tables which the lifters lift (ArmSemanticLift32.cpp,
 * ArmSemanticLift32Memory.cpp), each with its handler and its classification.
 * The wide T32 encodings are named "add.w", "ldr.w": the semantic search
 * ignores the suffix. The condition of an instruction (the cond field, the
 * it block, the T16 / T32 branch condition) wraps the handler
 * (ArmSemanticLifter::lift()).
 *
 * GENERATED by tests/arm/tools/gen_a32_semantic_table.py: edit the generator, not this
 * file. The mnemonics which are not here are not lifted (OP_UNDEFINED):''')
    for rx, reason in UNLIFTED:
        out.append(' *   - %s: %s' % (rx.replace('|', ' '), reason))
    out.append(''' *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/ArmSemanticInternal.h"

namespace armdis {

namespace {

#define U_(x) OpcodeClassification::UNIT_##x
#define P_(x) OpcodeClassification::PROPERTY_##x

} // namespace

using namespace a32;

const ArmSemanticEntry gArmSemanticTable[] =
{''')
    for mnemonic, rule in lifted:
        rx, handler, category, units, props, read, written = rule
        out.append('    ARM32_ENTRY("%s", %s, %s, %s, %s, %s, %s),' % (
            mnemonic, handler, category,
            ' | '.join('U_(%s)' % u for u in units) or '0',
            ' | '.join('P_(%s)' % p for p in props) or '0',
            read or '0', written or '0'))
    out.append('    ARM32_END')
    out.append('};')
    out.append('')
    out.append('} // namespace armdis')
    return '\n'.join(out) + '\n', lifted, unlifted


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    text, lifted, unlifted = build()
    if args.check:
        if open(OUTPUT).read() != text:
            print('%s is out of date' % OUTPUT)
            return 1
        return 0
    open(OUTPUT, 'w').write(text)
    print('wrote %s: %d lifted, %d not lifted: %s' % (OUTPUT, len(lifted), len(unlifted),
                                                      ' '.join(m for m, r in unlifted)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
