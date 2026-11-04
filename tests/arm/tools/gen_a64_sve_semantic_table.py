#!/usr/bin/env python3
"""Write Source/dismount/proc/arm/Arm64SemanticTableSve.cpp, the semantic table of the A64 SVE,
SVE2 and SVE2.1 instructions (gArm64SveSemanticTable): one entry per mnemonic of
gArm64SveEncodingTable which a lifter (Arm64SemanticLiftSve*.cpp) lifts, with the classification
of the mnemonic (category, units, properties, flags).

The lifters are the descriptor tables of the Arm64SemanticLiftSve*.cpp files: every mnemonic
named at the start of a table line is lifted by the function of its file (LIFTERS below). The
mnemonics of the encoding table which no lifter names are listed by --missing; they are lifted
to OP_UNDEFINED and ArmFeatures::getSupport() of their feature has no SUPPORT_LIFT.

    gen_a64_sve_semantic_table.py             # rewrite the table
    gen_a64_sve_semantic_table.py --check     # exit 1 if the file differs
    gen_a64_sve_semantic_table.py --missing   # the mnemonics which are not lifted
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

ARM = os.path.join(armlib.REPO, 'Source', 'dismount', 'proc', 'arm')
TABLE = os.path.join(ARM, 'Arm64EncodingTableSve.cpp')
OUTPUT = os.path.join(ARM, 'Arm64SemanticTableSve.cpp')

# (source file, lifter function): the first match of the mnemonic decides
LIFTERS = [
    ('Arm64SemanticLiftSve.cpp', 'liftSveInteger'),
    ('Arm64SemanticLiftSvePredicate.cpp', 'liftSvePredicate'),
    ('Arm64SemanticLiftSveFloat.cpp', 'liftSveFloat'),
    ('Arm64SemanticLiftSveMemory.cpp', 'liftSveMemory'),
    ('Arm64SemanticLiftSve2.cpp', 'liftSve2'),
    ('Arm64SemanticLiftSve2Wide.cpp', 'liftSve2Wide'),
    ('Arm64SemanticLiftSveFloat2.cpp', 'liftSveFloat2'),
    ('Arm64SemanticLiftSveQuad.cpp', 'liftSveQuad'),
    ('Arm64SemanticLiftSveCrypto.cpp', 'liftSveCrypto'),
    ('Arm64SemanticLiftSveMisc.cpp', 'liftSveMisc'),
]

NZCV = ('SemanticFlags::FLAG_SIGN | SemanticFlags::FLAG_ZERO | SemanticFlags::FLAG_CARRY | '
        'SemanticFlags::FLAG_OVERFLOW')

# (regular expression, category, units, properties). First match wins
RULES = [
    (r'ld[1-4]r[a-z]*|ld1ro[a-z]|ld1rq[a-z]|ld[1-4][bhwdq]|ldff1[a-z]*|ldnf1[a-z]*|ldnt1[a-z]*|ldr',
     'LOAD', ['LOAD'], ['READS_MEMORY', 'MAY_FAULT']),
    (r'st[1-4][bhwdq]|stnt1[a-z]*|str', 'STORE', ['STORE'], ['WRITES_MEMORY', 'MAY_FAULT']),
    (r'prf[bhwd]', 'CACHE', ['LOAD'], []),
    (r'rdffr|rdffrs|setffr|wrffr', 'MOVE', ['VECTOR_INTEGER'], []),
    (r'while[a-z]+|ctermeq|ctermne', 'COMPARE', ['INTEGER_ALU'], []),
    (r'ptest|ptrues?|pfalse|pfirst|pnext|brk[a-z]*|punpk[a-z]+', 'LOGIC', ['VECTOR_INTEGER'], []),
    (r'cntp|incp|decp|sqincp|sqdecp|uqincp|uqdecp|cnt[bhwd]|inc[bhwd]|dec[bhwd]|sq(inc|dec)[bhwd]|'
     r'uq(inc|dec)[bhwd]|rdvl|addvl|addpl|rdsvl|addsvl|addspl', 'ARITHMETIC', ['INTEGER_ALU'],
     ['VECTOR_LENGTH']),
    (r'cmp[a-z]+', 'COMPARE', ['VECTOR_INTEGER'], []),
    (r'f(cmp[a-z]*|acge|acgt)', 'COMPARE', ['VECTOR_FLOAT'], []),
    (r'f?cvt[a-z0-9]*|[su]cvtf[a-z]*|bf[12]?cvt[a-z]*|f[12]cvt[a-z]*|fcvt[a-z]*|[su]qcvt[a-z]*|'
     r'fcvtx[a-z]*|frint[a-z0-9]*|flogb', 'CONVERT', ['VECTOR_FLOAT'], []),
    (r'f[a-z0-9]*|bf[a-z0-9]*', 'VECTOR_FLOAT', ['VECTOR_FLOAT'], []),
    (r'aes[a-z]*|sm4[a-z]*|rax1|sha[a-z0-9]*|pmull[bt]?|pmul|pmlal', 'CRYPTO', ['CRYPTO'], []),
    (r'zip[12]|zipq[12]|uzp[12]|uzpq[12]|trn[12]|rev[bhwd]?|tbl[q]?|tbx[q]?|ext[q]?|splice|compact|expand|'
     r'insr|dupq?|sel|[su]unpk[a-z]+|clast[ab]|last[ab]|lastp|firstp|psel|pext|pmov|luti[246]',
     'VECTOR_PERMUTE', ['VECTOR_PERMUTE'], []),
    (r'mov|movs|movprfx|index|dupm|fmov', 'MOVE', ['VECTOR_INTEGER'], []),
    (r'asr[a-z]*|lsl[a-z]*|lsr[a-z]*|sli|sri|[su]?shl[a-z]*|[su]rsra|[su]sra|[su]rshr|sqshl[a-z]*|'
     r'uqshl[a-z]*|sqrshl[a-z]*|uqrshl[a-z]*|srshl[a-z]*|urshl[a-z]*|sshll[bt]|ushll[bt]|.*shrn[bt]|.*shr[a-z]*',
     'SHIFT', ['VECTOR_INTEGER'], []),
    (r'mul|mla|mls|mad|msb|smulh|umulh|[su]mull[bt]|[su]ml[as]l[bt]|sq(r?d)mul[a-z]*|sqrdml[as]h|sqdml[as]l[a-z]*|'
     r'[su]dot|usdot|sudot|cdot|cmla|sqrdcmlah|[su]mmla|usmmla|mlapt|madpt',
     'MULTIPLY', ['VECTOR_INTEGER'], []),
    (r'sdiv|udiv|sdivr|udivr', 'DIVIDE', ['VECTOR_INTEGER'], []),
    (r'and|ands|orr|orrs|eor|eors|bic|bics|orn|orns|nand|nands|nor|nors|not|nots|cnot|bsl[12n]*|nbsl|eor3|bcax|xar|'
     r'eorbt|eortb',
     'LOGIC', ['VECTOR_INTEGER'], []),
    (r'cls|clz|cnt|rbit|bdep|bext|bgrp|histcnt|histseg|match|nmatch', 'BIT_MANIPULATION', ['VECTOR_INTEGER'], []),
    (r'[su]xt[bhw]', 'CONVERT', ['VECTOR_INTEGER'], []),
]
DEFAULT = ('VECTOR_INTEGER', ['VECTOR_INTEGER'], [])

# The mnemonics which write NZCV
FLAGS_WRITTEN = (r'ptest|ptrues|pfirst|pnext|brk[a-z]*s|while[a-z]+|cmp[a-z]+|ctermeq|ctermne|rdffrs|ands|bics|eors|'
                 r'nands|nors|orns|orrs|nots|movs|brkn|brkns|brkpas|brkpbs')
FLAGS_READ = r'ctermeq|ctermne'
PREDICATED = r'.*'


def table_mnemonics():
    text = open(TABLE).read()
    names = []
    for m in re.finditer(r'\bE\("([^"]+)"', text):
        if m.group(1) not in names:
            names.append(m.group(1))
    return names


def lifter_of(name, tables):
    for _, function in LIFTERS:
        if name in tables.get(function, ()):
            return function
    return None


def read_tables():
    tables = {}
    for filename, function in LIFTERS:
        path = os.path.join(ARM, filename)
        if not os.path.exists(path):
            continue
        names = []
        for line in open(path):
            if not re.match(r'^\s*(?:\{|[A-Z_0-9]+\()\s*"', line):
                continue
            for m in re.finditer(r'(?:\{|[A-Z_0-9]+\()\s*"([a-z0-9_]+)"\s*,', line):
                names.append(m.group(1))
        tables[function] = set(names)
    return tables


def classify(name):
    for pattern, category, units, props in RULES:
        if re.fullmatch(pattern, name):
            return category, units, props
    return DEFAULT


def generate():
    tables = read_tables()
    out = []
    w = out.append
    w('''/*
 * Arm64SemanticTableSve.cpp
 *
 * Implementation file
 *
 * The semantic table of the A64 SVE, SVE2 and SVE2.1 instructions (see
 * ArmSemantics.h): the mnemonics of gArm64SveEncodingTable which the lifters
 * (Arm64SemanticLiftSve*.cpp) lift, with their classification. The lifter of
 * a mnemonic decodes the operands of the instruction (the z and p registers,
 * the element sizes, the immediates, the memory operands) and works by the
 * shape of them, so an alias (mov, not, cmple...) has the entry of its name.
 * The vector length is the VL of the Arm64Architecture.
 *
 * GENERATED by tests/arm/tools/gen_a64_sve_semantic_table.py: edit the generator, not this
 * file. The mnemonics which are not here are not lifted (OP_UNDEFINED).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticSve.h"

namespace armdis {

namespace {

// An entry: the mnemonic, the lifter, the category, the units, the properties,
// the flags read and written
#define SVE(name, lifter, category, units, properties, read, written) \\
    { ArmSemanticEntry::ANY_OPERANDS_COUNT, \\
      { name, NULL, 0, OpcodeClassification::CATEGORY_##category, units, \\
        properties | OpcodeClassification::PROPERTY_PREDICATED, read, written, 0, 0 }, \\
      a64::lifter, 0, 0, 0, 0 }
#define U_(x) OpcodeClassification::UNIT_##x
#define P_(x) OpcodeClassification::PROPERTY_##x

} // namespace

const ArmSemanticEntry gArm64SveSemanticTable[] =
{''')
    lifted = 0
    for name in table_mnemonics():
        function = lifter_of(name, tables)
        if function is None:
            continue
        lifted += 1
        category, units, props = classify(name)
        us = ' | '.join('U_(%s)' % u for u in units) if units else '0'
        ps = ' | '.join('P_(%s)' % p for p in props) if props else '0'
        read = NZCV if re.fullmatch(FLAGS_READ, name) else '0'
        written = NZCV if re.fullmatch(FLAGS_WRITTEN, name) else '0'
        w('    SVE("%s", %s, %s, %s, %s, %s, %s),' % (name, function, category, us, ps, read, written))
    w('    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }, NULL, 0, 0, 0, 0 }')
    w('};')
    w('')
    w('} // namespace armdis')
    w('')
    return '\n'.join(out), lifted


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--missing', action='store_true')
    args = parser.parse_args()
    if args.missing:
        tables = read_tables()
        missing = [n for n in table_mnemonics() if lifter_of(n, tables) is None]
        print('%d of %d mnemonics are not lifted:' % (len(missing), len(table_mnemonics())))
        print(' '.join(missing))
        return 0
    text, lifted = generate()
    if args.check:
        if not os.path.exists(OUTPUT) or open(OUTPUT).read() != text:
            sys.stderr.write('%s is out of date\n' % OUTPUT)
            return 1
        return 0
    with open(OUTPUT, 'w') as f:
        f.write(text)
    print('%s: %d mnemonics' % (OUTPUT, lifted))
    return 0


if __name__ == '__main__':
    sys.exit(main())
