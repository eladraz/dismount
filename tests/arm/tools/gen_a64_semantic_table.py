#!/usr/bin/env python3
"""Write Source/dismount/proc/arm/Arm64SemanticTable.cpp, the semantic table of the
A64 base instructions (gArm64SemanticTable): one entry per mnemonic of
gArm64BaseEncodingTable which the lifter (Arm64SemanticLift*.cpp) lifts, with the
classification of the mnemonic (category, units, properties, flags).

The mnemonics which are not lifted are not in the table: they are lifted to OP_UNDEFINED
and classified as CATEGORY_UNKNOWN, and ArmFeatures::getSupport() of their feature has no
SUPPORT_LIFT (see UNLIFTED below for the reason of each group).

    gen_a64_semantic_table.py          # rewrite the table
    gen_a64_semantic_table.py --check  # exit 1 if the file differs
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

TABLE = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64EncodingTable.cpp')
OUTPUT = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64SemanticTable.cpp')

# The mnemonics which are NOT lifted, and why (regular expressions, full match)
UNLIFTED = [
    # FEAT_PAuth: the keys and the QARMA algorithm are not modeled. The hint space forms
    # (paciasp...) are lifted to a helper, xpaci / xpacd / xpaclri strip the code exactly
    (r'pac[a-z0-9]*|aut[a-z0-9]*|braa[a-z]*|brab[a-z]*|blraa[a-z]*|blrab[a-z]*|retaa[a-z0-9]*|retab[a-z0-9]*|'
     r'eretaa|eretab|ldraa|ldrab', 'pauth'),
    # FEAT_MTE: the allocation tags are not part of the memory of the IR
    (r'addg|subg|irg|gmi|ldg|ldgm|stg|stgm|stzg|stz2g|st2g|stgp|stzgm|subp|subps|cmpp', 'mte'),
    # FEAT_MOPS: the memory copy and set instructions
    (r'cpy[a-z]+|set[pmeg][a-z]*', 'mops'),
    # FEAT_THE (RCW), FEAT_LSFE: the read check write and floating point atomics
    (r'rcw[a-z]+|ldb?f(add|max|min)[a-z]*|stb?f(add|max|min)[a-z]*', 'atomic-extensions'),
    # FEAT_LS64 with a status register
    (r'st64bv0?', 'ls64'),
    # FEAT_D128 (128 bit system registers), FEAT_GCS, FEAT_TME, FEAT_POE2, FEAT_BRBE, FEAT_TRF
    (r'mrrs|msrr|sysp|tlbip|gcs[a-z0-9]*|tstart|tcommit|ttest|tcancel|tenter|texit|tchange[bf]|trcit|brb|'
     r'plbi|cfp|cpp|dvp|apas|at|tlbi|sys|sysl', 'system-extensions'),
    # Privileged: exception return, debug state, calls to the other exception levels
    (r'eret|drps|dcps[123]|hvc|smc', 'privileged'),
    # FEAT_CPA checked pointer arithmetic
    (r'addpt|subpt|maddpt|msubpt', 'cpa'),
]

# The mnemonics which are lifted though a rule above matches them
LIFTED_EXCEPTIONS = {'cbz', 'cbnz', 'xpaci', 'xpacd', 'xpaclri', 'dc', 'ic', 'cfp', 'dvp', 'cpp', 'cosp',
                     'paciasp', 'pacibsp', 'pacia1716', 'pacib1716', 'paciaz', 'pacibz',
                     'autiasp', 'autibsp', 'autia1716', 'autib1716', 'autiaz', 'autibz'}

# (regular expression, category, units, properties). First match wins. The names are
# CATEGORY_x, UNIT_x, PROPERTY_x of OpcodeClassification
RULES = [
    (r'adds?|subs?|negs?|ngcs?|adcs?|sbcs?|adr|adrp', 'ARITHMETIC', ['INTEGER_ALU'], []),
    (r'cmp|cmn|tst|ccmp|ccmn', 'COMPARE', ['INTEGER_ALU'], []),
    (r'and|ands|orr|eor|bic|bics|orn|eon|mvn', 'LOGIC', ['INTEGER_ALU'], []),
    (r'lsl|lsr|asr|ror|extr', 'SHIFT', ['SHIFTER'], []),
    (r'mov|movz|movn|movk', 'MOVE', ['INTEGER_ALU'], []),
    (r'sxtb|sxth|sxtw|uxtb|uxth', 'CONVERT', ['INTEGER_ALU'], []),
    (r'bfm|sbfm|ubfm|bfi|bfxil|bfc|sbfiz|sbfx|ubfiz|ubfx|rbit|rev|rev16|rev32|clz|cls|cnt|ctz|abs',
     'BIT_MANIPULATION', ['INTEGER_ALU'], []),
    (r'smax|smin|umax|umin', 'ARITHMETIC', ['INTEGER_ALU'], []),
    (r'madd|msub|mul|mneg|smaddl|smsubl|umaddl|umsubl|smull|umull|smnegl|umnegl|smulh|umulh',
     'MULTIPLY', ['INTEGER_MULTIPLY'], []),
    (r'udiv|sdiv', 'DIVIDE', ['INTEGER_DIVIDE'], []),
    (r'crc32[a-z]+', 'CRYPTO', ['INTEGER_ALU'], []),
    (r'csel|csinc|csinv|csneg|cset|csetm|cinc|cinv|cneg', 'CONDITIONAL_MOVE', ['INTEGER_ALU'], []),
    (r'rmif|setf8|setf16|cfinv|axflag|xaflag', 'BIT_MANIPULATION', ['INTEGER_ALU'], []),
    (r'b|bc|cbz|cbnz|br|cb[a-z]+', 'BRANCH', ['BRANCH'], []),
    (r'bl|blr', 'CALL', ['BRANCH'], ['CHANGES_STACK_NO']),
    (r'ret', 'RETURN', ['BRANCH'], []),
    (r'svc|brk|hlt|udf', 'INTERRUPT', ['SYSTEM'], []),
    (r'tbz|tbnz', 'BRANCH', ['BRANCH'], []),
    (r'nop|yield|wfe|wfi|wfet|wfit|sev|sevl|dgh|csdb|esb|psb|tsb|clrbhb|hint|stshh|chkfeat',
     'NOP', ['SYSTEM'], []),
    (r'bti', 'SECURITY', ['SYSTEM'], ['LANDING_PAD']),
    (r'xpaci|xpacd|xpaclri|paci[a-z0-9]*|autia[a-z0-9]*|autib[a-z0-9]*', 'SECURITY', ['INTEGER_ALU'], []),
    (r'dmb|dsb|isb|sb|ssbb|pssbb|clrex', 'SYSTEM', ['SYSTEM'], ['SERIALIZING']),
    (r'smstart|smstop', 'SYSTEM', ['SYSTEM'], ['CHANGES_MODE']),
    (r'mrs|msr', 'SYSTEM', ['SYSTEM'], []),
    (r'dc|ic', 'CACHE', ['SYSTEM'], []),
    (r'cfp|dvp|cpp|cosp', 'NOP', ['SYSTEM'], []),
    (r'prfm|prfum|rprfm', 'CACHE', ['LOAD'], []),
    (r'(ld|st)a?l?t?x[rp][bh]?', 'ATOMIC', ['LOAD', 'STORE'], ['EXCLUSIVE']),
    (r'(cas|swp|ld(add|clr|eor|set|smax|smin|umax|umin)|ldt(add|clr|set)|st(add|clr|eor|set|smax|smin|umax|umin))'
     r'[a-z]*', 'ATOMIC', ['LOAD', 'STORE'], ['ATOMIC', 'READS_MEMORY', 'WRITES_MEMORY', 'SERIALIZING']),
    (r'ld(ar|lar|apr|apur|iapp)[a-z]*', 'LOAD', ['LOAD'], ['READS_MEMORY', 'SERIALIZING']),
    (r'st(lr|llr|lur|ilp)[a-z]*', 'STORE', ['STORE'], ['WRITES_MEMORY', 'SERIALIZING']),
    (r'ld64b', 'LOAD', ['LOAD'], ['READS_MEMORY', 'IMPLICIT_OPERANDS']),
    (r'st64b', 'STORE', ['STORE'], ['WRITES_MEMORY', 'IMPLICIT_OPERANDS']),
    (r'ld[a-z]*', 'LOAD', ['LOAD'], ['READS_MEMORY']),
    (r'st[a-z]*', 'STORE', ['STORE'], ['WRITES_MEMORY']),
]

# The flags (SemanticFlags) each mnemonic reads and writes
NZCV = 'SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_CARRY | SemanticFlags::FLAG_OVERFLOW'
FLAGS_WRITTEN = r'adds|subs|ands|bics|cmp|cmn|tst|negs|ngcs|adcs|sbcs|ccmp|ccmn|msr'
FLAGS_READ = r'adc|adcs|sbc|sbcs|ngc|ngcs|csel|csinc|csinv|csneg|cset|csetm|cinc|cinv|cneg|ccmp|ccmn|bc|mrs'
FLAGS_BOTH = r'rmif|setf8|setf16|cfinv|axflag|xaflag'


def base_mnemonics():
    text = open(TABLE).read()
    names = []
    for m in re.finditer(r'\bE\("([^"]+)"', text):
        if m.group(1) not in names:
            names.append(m.group(1))
    return names


def is_lifted(name):
    if name in LIFTED_EXCEPTIONS:
        return True
    for pattern, _ in UNLIFTED:
        if re.fullmatch(pattern, name):
            return False
    return True


def classify(name):
    for pattern, category, units, props in RULES:
        if re.fullmatch(pattern, name):
            props = [p for p in props if p != 'CHANGES_STACK_NO']
            if name in ('b', 'bc'):
                props = props + ['CONDITIONAL']
            if name.startswith('ld') or name.startswith('st') or name in ('prfm', 'prfum', 'rprfm'):
                pass
            return category, units, props
    return None


def flags_of(name):
    read = written = '0'
    if re.fullmatch(FLAGS_WRITTEN, name):
        written = NZCV
    if re.fullmatch(FLAGS_READ, name):
        read = NZCV
    if re.fullmatch(FLAGS_BOTH, name):
        read = written = NZCV
    if name == 'b':
        read = NZCV
    return read, written


def generate():
    out = []
    w = out.append
    w('''/*
 * Arm64SemanticTable.cpp
 *
 * Implementation file
 *
 * The semantic table of the A64 integer, branch, load / store and system
 * instructions (see ArmSemantics.h): the mnemonics of gArm64BaseEncodingTable
 * which the lifter lifts, with their classification. Every entry is lifted by
 * liftArm64Base(), which decodes the bits of the instruction (Arm64SemanticLift.cpp,
 * Arm64SemanticLiftMemory.cpp), so an alias (cmp, mov, lsl...) has the entry of its
 * name and the same semantics as the instruction it is an alias of.
 *
 * GENERATED by tests/arm/tools/gen_a64_semantic_table.py: edit the generator, not this
 * file. The mnemonics which are not here are not lifted (OP_UNDEFINED):
 *   - FEAT_PAuth: the pac / aut instructions and the branches with authentication
 *     (the keys are not modeled; the hint space forms are lifted to a helper, xpac
 *     strips the code exactly)
 *   - FEAT_MTE: the allocation tags are not part of the memory of the IR
 *   - FEAT_MOPS: the memory copy and set instructions
 *   - FEAT_THE, FEAT_LSFE, st64bv: not lifted
 *   - FEAT_D128, FEAT_GCS, FEAT_TME, FEAT_POE2, the privileged system instructions (at,
 *     tlbi, sys, eret, hvc, smc...), FEAT_CPA
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticInternal.h"

namespace armdis {

namespace {

// An entry: the mnemonic, the category, the units, the properties, the flags
// read and written
#define A64(name, category, units, properties, read, written) \\
    { ArmSemanticEntry::ANY_OPERANDS_COUNT, \\
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, units, properties, \\
        read, written, 0, 0 }, \\
      liftArm64Base, 0, 0, 0, 0 }
#define U_(x) OpcodeClassification::UNIT_##x
#define P_(x) OpcodeClassification::PROPERTY_##x

} // namespace

const ArmSemanticEntry gArm64SemanticTable[] =
{''')
    skipped = []
    for name in base_mnemonics():
        if not is_lifted(name):
            skipped.append(name)
            continue
        c = classify(name)
        if c is None:
            skipped.append(name)
            continue
        category, units, props = c
        read, written = flags_of(name)
        units_text = ' | '.join('U_(%s)' % u for u in units) if units else '0'
        props_text = ' | '.join('P_(%s)' % p for p in props) if props else '0'
        w('    A64("%s", %s, %s, %s, %s, %s),' % (name, category, units_text, props_text, read, written))
    w('    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }')
    w('};')
    w('')
    w('} // namespace armdis')
    return '\n'.join(out) + '\n', skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--list-skipped', action='store_true')
    args = parser.parse_args()
    text, skipped = generate()
    if args.list_skipped:
        print(' '.join(skipped))
        return 0
    if args.check:
        return 0 if open(OUTPUT).read() == text else 1
    open(OUTPUT, 'w').write(text)
    print('wrote %s (%d not lifted)' % (OUTPUT, len(skipped)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
