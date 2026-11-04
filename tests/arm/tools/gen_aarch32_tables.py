#!/usr/bin/env python3
"""Generate the AArch32 encoding tables from the LLVM description of the ARM
backend (see aarch32_llvm.py for how to get it):

    Source/dismount/proc/arm/ArmEncodingTableA32.cpp      gArmA32EncodingTable
    Source/dismount/proc/arm/ArmEncodingTableT16.cpp      gArmT16EncodingTable
    Source/dismount/proc/arm/ArmEncodingTableT32.cpp      gArmT32EncodingTable
    Source/dismount/proc/arm/ArmEncodingTableA32Simd.cpp  gArmA32SimdEncodingTable

The instruction records give the bits, the operands and the syntax; the
operand classes are mapped to dismount's specs in aarch32_classes.py. The
aliases which the LLVM printer produces in C++ (the shifts of mov, push, pop)
and the other special cases are in this file.

    gen_aarch32_tables.py [--only a32|t16|t32|simd] [--check]
"""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import aarch32_llvm as llvm
import aarch32_classes as classes

REPO = llvm.REPO
SOURCE = os.path.join(REPO, 'Source', 'dismount', 'proc', 'arm')

# The LLVM predicates and the dismount feature which they need. The order is
# the order of the architecture versions: the entry gets the highest feature
FEATURE_ORDER = [
    'FEAT_A32_BASE', 'FEAT_T32_BASE', 'FEAT_V5T', 'FEAT_V5TE', 'FEAT_V6', 'FEAT_V6M', 'FEAT_V6K',
    'FEAT_V6T2', 'FEAT_V7', 'FEAT_MP', 'FEAT_DIV', 'FEAT_DIV_ARM', 'FEAT_V8_AA32',
]
PREDICATE_FEATURE = {
    'HasV4T': None, 'HasV5T': 'FEAT_V5T', 'HasV5TE': 'FEAT_V5TE', 'HasV6': 'FEAT_V6',
    'HasV6M': 'FEAT_V6M', 'HasV6K': 'FEAT_V6K', 'HasV6T2': 'FEAT_V6T2', 'HasV7': 'FEAT_V7',
    'HasDB': 'FEAT_V7', 'HasMP': 'FEAT_MP', 'HasV7Clrex': 'FEAT_V6K',
    'HasAcquireRelease': 'FEAT_V8_AA32', 'HasV8': 'FEAT_V8_AA32',
    'HasDivideInARM': 'FEAT_DIV_ARM', 'HasDivideInThumb': 'FEAT_DIV',
    'HasV8MBaseline': 'FEAT_V6T2', 'HasDSP': 'FEAT_V6T2', 'IsThumb2': 'FEAT_V6T2',
    'HasCRC': 'FEAT_CRC32_AA32', 'HasVirtualization': 'FEAT_VIRT', 'HasTrustZone': 'FEAT_TRUSTZONE',
    'HasSB': 'FEAT_SB_AA32', 'HasRAS': 'FEAT_RAS_AA32', 'HasV8_1a': 'FEAT_PAN_AA32',
    'HasV8_4a': 'FEAT_V8_4A_AA32',
}
# Predicates which don't change the encoding (the tuning of the processor, the
# instruction set mode, the target ABI)
IGNORED_PREDICATES = set([
    'IsARM', 'IsThumb', 'IsThumb1Only', 'IsNotMClass', 'UseMulOps', 'UseFPVMLx', 'DontUseNaClTrap',
    'DontUseNEONForFP', 'UseFusedMAC', 'UseVMOVSR', 'HasFastVGETLNi32', 'HasFastVDUP32',
    'NoV4T', 'IsWindows',
])
# Predicates of the M profile and of the unsupported extensions: the
# instruction is not in the tables
SKIPPED_PREDICATES = set([
    'IsMClass', 'Has8MSecExt', 'HasV8_1MMainline', 'HasV8MMainline', 'HasMVEInt', 'HasLOB',
    'HasPACBTI', 'UseNaClTrap', 'HasCDE', 'HasV8_3a_unused',
])


def feature_of(ins):
    """Return (feature, extra flags) of an instruction, None to skip it."""
    ranked = None
    special = None
    flags = []
    for predicate in ins.predicates:
        if predicate in SKIPPED_PREDICATES:
            return None
        if predicate == 'PreV8':
            flags.append('ARM_ENTRY_PRE_V8')
            continue
        if predicate in IGNORED_PREDICATES:
            continue
        if predicate not in PREDICATE_FEATURE:
            raise classes.Unmapped('predicate %s' % predicate)
        feature = PREDICATE_FEATURE[predicate]
        if feature is None:
            continue
        if feature in FEATURE_ORDER:
            if ranked is None or FEATURE_ORDER.index(feature) > FEATURE_ORDER.index(ranked):
                ranked = feature
        elif special is None:
            special = feature
    return (special or ranked), flags


class Entry(object):
    def __init__(self, mnemonic, mask, value, specs, feature, flags=None, flow='0', condition=None,
                 comment=''):
        self.mnemonic = mnemonic
        self.mask = mask
        self.value = value
        self.specs = specs
        self.feature = feature
        self.flags = flags or []
        self.flow = flow
        self.condition = condition
        self.comment = comment


def parse_template(template):
    """Split "adc${s}${p}.w" into the base, the parts and the literal suffix."""
    template = template.replace('{$p}', '$p')
    parts = []
    base = ''
    suffix = ''
    position = 0
    seen_part = False
    while position < len(template):
        m = re.match(r'\$\{(\w+)\}|\$(\w+)', template[position:])
        if m:
            parts.append(m.group(1) or m.group(2))
            position += m.end()
            seen_part = True
        else:
            if seen_part:
                suffix += template[position]
            else:
                base += template[position]
            position += 1
    return base, parts, suffix


def split_pieces(text):
    """The comma separated pieces of the operand text of an AsmString."""
    pieces = []
    depth = 0
    current = ''
    for ch in text:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            pieces.append(current.strip())
            current = ''
        else:
            current += ch
    if current.strip():
        pieces.append(current.strip())
    return pieces


# Fixed operands of the text (registers and names written in the AsmString): the
# other areas add theirs
FIXED_LITERALS = {}


class Context(object):
    def __init__(self, ins, records):
        self.ins = ins
        self.records = records


def part_spec(ins, name):
    """The spec of a part of the mnemonic: ${p}, ${s}..."""
    op = ins.operand(name)
    if name == 'p':
        if op is None:
            raise classes.Unmapped('no p operand')
        if op.bits:
            position, width = classes.first_bit(op)
            if width != 4:
                raise classes.Unmapped('p layout %s' % op.layout())
            return classes.S('ARM_KIND_CONDITION_MNEMONIC', position)
        if ins.namespace not in ('Thumb', 'ThumbSBit', 'Thumb2', 'Thumb2CoProc'):
            # (the Advanced SIMD instructions: unconditional in A32)
            return classes.S('ARM_KIND_CONDITION_MNEMONIC', 0, 'ARM_A32_COND_IT_ONLY')
        return classes.S('ARM_KIND_CONDITION_MNEMONIC', 0, 'ARM_A32_COND_IT')
    if name == 's':
        if op.cls == 'cc_out':
            return classes.S('ARM_KIND_SETS_FLAGS', 20)
        if op.cls == 's_cc_out':
            return classes.S('ARM_KIND_SETS_FLAGS', 0, 'ARM_A32_SETS_FLAGS_OUTSIDE_IT')
    if name == 'imod' and op.layout() == '0@4':
        return classes.S('ARM_KIND_A32_MNEMONIC_PART', 0, '0', 'ARM_A32_PART_CPS_IMOD_T16')
    if name == 'imod' and op.layout() == '0-1@9':
        return classes.S('ARM_KIND_A32_MNEMONIC_PART', 0, '0', 'ARM_A32_PART_CPS_IMOD_T32')
    if name == 'imod':
        return classes.S('ARM_KIND_A32_MNEMONIC_PART', 0, '0', 'ARM_A32_PART_CPS_IMOD')
    if name == 'mask':
        return classes.S('ARM_KIND_T16_IT_MASK', 0, '0', 4)
    raise classes.Unmapped('part ${%s}' % name)


def build_entry(ins, records):
    """Build the Entry of an instruction record."""
    result = feature_of(ins)
    if result is None:
        return None
    feature, flags = result
    if feature is None:
        feature = 'FEAT_T32_BASE' if ins.namespace in ('Thumb', 'ThumbSBit') else 'FEAT_A32_BASE'
    if ins.namespace in ('Thumb2', 'Thumb2CoProc') and feature in ('FEAT_A32_BASE', 'FEAT_T32_BASE'):
        feature = 'FEAT_V6T2'

    mnemonic_template, operand_text = llvm.split_asm(ins.asm)
    base, parts, suffix = parse_template(mnemonic_template)
    specs = []
    for part in parts:
        specs.append(part_spec(ins, part))

    ctx = Context(ins, records)
    pieces = [classes.Piece(p) for p in split_pieces(operand_text)]
    index = 0
    while index < len(pieces):
        piece = pieces[index]
        index += 1
        handled = False
        for piece_handler in classes.PIECE_HANDLERS:
            result = piece_handler(ctx, pieces, index - 1)
            if result is not None:
                new_specs, index = result
                specs.extend(new_specs)
                handled = True
                break
        if handled:
            continue
        if not piece.names:
            # A fixed operand
            literal = piece.literal
            fixed = dict(FIXED_LITERALS)
            fixed.update({'sp': 'ARM_A32_FIXED_SP', 'lr': 'ARM_A32_FIXED_LR', 'apsr': 'ARM_A32_FIXED_APSR',
                     'spsr': 'ARM_A32_FIXED_SPSR', '#0': 'ARM_A32_FIXED_ZERO',
                     'sp!': 'ARM_A32_FIXED_SP_WRITEBACK', 'pc': 'ARM_A32_FIXED_PC'})
            if literal not in fixed:
                raise classes.Unmapped('literal operand %r' % literal)
            specs.append(classes.S('ARM_KIND_A32_FIXED', 0, '0', fixed[literal]))
            continue
        names = piece.names
        op = ins.operand(names[0])
        if op is None:
            raise classes.Unmapped('no operand %s' % names[0])
        if len(names) == 2 and op.cls == 'addr_offset_none':
            nxt = ins.operand(names[1])
            post = classes.THUMB_POST_CLASSES.get(nxt.cls) if nxt is not None else None
            if post is None:
                raise classes.Unmapped('two names %s' % piece.text)
            specs.append(classes.S('ARM_KIND_A32_MEMORY', 16, '0', post))
            continue
        if len(names) == 2:
            # Rm$sh: a register and its shift
            shift = ins.operand(names[1])
            result = classes.handle(ctx, shift, classes.Piece('$' + names[1]), records)
            if result[0][0] != 'SHIFT':
                raise classes.Unmapped('two names %s' % piece.text)
            reg = ins.operand(names[0])
            position, width = classes.first_bit(reg)
            specs.append(classes.S('ARM_KIND_SHIFTED_REGISTER', position, '0', result[0][1]))
            continue
        if len(names) != 1:
            raise classes.Unmapped('piece %s' % piece.text)
        # A memory operand followed by its post index
        if op.cls == 'addr_offset_none' and index < len(pieces) and pieces[index].names:
            nxt = ins.operand(pieces[index].names[0])
            if nxt is not None and nxt.cls in classes.POST_CLASSES:
                scheme, layout = classes.POST_CLASSES[nxt.cls]
                classes.need(nxt, layout)
                specs.append(classes.S('ARM_KIND_A32_MEMORY', 16, '0', scheme))
                index += 1
                continue
        # The second register of a pair
        if op.cls in ('GPR', 'GPRnopc') and not op.bits and specs and specs[-1][0] == 'ARM_KIND_GPR' \
                and op.name == 'Rt2':
            kind, position, flags_, extra = specs[-1]
            specs[-1] = (kind, position, 'ARM_A32_GPR_PAIR_NEXT', extra)
            continue
        for spec in classes.handle(ctx, op, piece, records):
            specs.append(spec)
    # The suffix text goes to the mnemonic after the parts
    mnemonic = base + suffix
    if len(specs) > 8:
        raise classes.Unmapped('%d specs' % len(specs))
    return Entry(mnemonic, ins.mask & ~ins.soft, ins.value, specs, feature, flags)


# ---------------------------------------------------------------------------
# The emitter

PREAMBLE = r'''#include "dismount/dismount.h"
/*
 * %(file)s
 *
 * %(what)s (see ArmEncodingTable.h).
 *
 * GENERATED by tests/arm/tools/gen_aarch32_tables.py from the LLVM ARM
 * backend: edit the generator, not this file. The order of the entries is the
 * order of the decoder: an alias comes before the general encoding it prints
 * for, a more specific encoding before the general one.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/Opcode.h"
#include "dismount/proc/arm/ArmEncodingTable.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecInternal.h"
#include "dismount/proc/arm/ArmOperandCodecA32.h"

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
#undef BITS
} // anonymous namespace

const ArmEncodingEntry %(table)s[] = {
'''

POSTAMBLE = r'''    // The end of the table
    { NULL, 0, 0, {{ ARM_KIND_NONE, 0, 0, 0 }}, 0, 0, 0, NULL }
};

%(extra)s} // namespace armdis
'''


def emit(path, table, what, entries, helpers='', extra=''):
    out = [PREAMBLE % {'file': os.path.basename(path), 'what': what, 'table': table,
                       'helpers': helpers}]
    lines = []
    for entry in entries:
        ops = ', '.join('S(%s, %s, %s, %s)' % tuple(s) for s in entry.specs)
        flags = ' | '.join(entry.flags) if entry.flags else '0'
        if entry.comment:
            lines.append('    // %s' % entry.comment)
        lines.append('    E("%s", 0x%08x, 0x%08x, %s, %s, %s, %s%s),' % (
            entry.mnemonic, entry.mask, entry.value, entry.feature, flags, entry.flow,
            entry.condition if entry.condition else 'NULL', (', ' + ops) if ops else ''))
    out.append('\n'.join(lines) + '\n')
    out.append(POSTAMBLE % {'extra': extra})
    with open(path, 'w') as f:
        f.write(''.join(out))


def popcount(value):
    return bin(value).count('1')


def sort_entries(entries):
    """More specific encodings first (stable)."""
    return [e for _, e in sorted(enumerate(entries), key=lambda ie: (-popcount(ie[1].mask), ie[0]))]


A32_SKIP = set([
    # printed by other entries
    'ADR', 'BX', 'BLX', 'BX_RET', 'TRAPNaCl', 'MOVPCLR',
])

FLOWS = {
    'Bcc': 'FLOW_JUMP', 'BL': 'FLOW_CALL', 'BL_pred': 'FLOW_CALL', 'BLXi': 'FLOW_CALL',
    'BX_pred': 'FLOW_JUMP_REGISTER', 'BLX_pred': 'FLOW_CALL_REGISTER', 'BXJ': 'FLOW_JUMP_REGISTER',
    'SVC': 'FLOW_TRAP', 'BKPT': 'FLOW_TRAP', 'UDF': 'FLOW_TRAP', 'HLT': 'FLOW_TRAP',
    'HVC': 'FLOW_TRAP', 'SMC': 'FLOW_TRAP',
    'tBL': 'FLOW_CALL', 'tBLXi': 'FLOW_CALL', 'tBLXr': 'FLOW_CALL_REGISTER', 'tBX': 'FLOW_JUMP_REGISTER',
    'tB': 'FLOW_JUMP', 'tBcc': 'FLOW_COND', 'tCBZ': 'FLOW_COND', 'tCBNZ': 'FLOW_COND',
    'tBKPT': 'FLOW_TRAP', 'tSVC': 'FLOW_TRAP', 'tUDF': 'FLOW_TRAP',
}


def generate_namespace(records, namespaces, skip, size=None):
    entries = []
    skipped = collections.OrderedDict()
    for ins in llvm.instructions(records, namespaces):
        if ins.name in skip or (size is not None and ins.size != size):
            continue
        try:
            entry = build_entry(ins, records)
        except classes.Unmapped as exc:
            skipped[ins.name] = str(exc)
            continue
        if entry is None:
            continue
        entry.flow = FLOWS.get(ins.name, '0')
        entry.comment = ins.name
        entries.append(entry)
    return entries, skipped


def banked_registers(records):
    """The banked registers (mrs / msr): (encoding R:m1:m, name)."""
    result = []
    for name, record in records.items():
        if isinstance(record, dict) and 'BankedReg' in record.get('!superclasses', []):
            bits = record['Encoding']
            result.append((sum(b << i for i, b in enumerate(bits[:6])), record['Name']))
    return sorted(result)


def banked_code(records):
    rows = ''.join('    { %d, "%s" },\n' % (e, n) for e, n in banked_registers(records))
    return r'''namespace {

// The banked registers of mrs / msr: R:m1:m (6 bits) and the name as LLVM
// prints it (the SPSR ones are renamed spsr_...)
struct BankedRegister {
    uint m_encoding;
    const char* m_name;
};

const BankedRegister gBankedRegisters[] = {
%s};

} // anonymous namespace

const char* getA32BankedName(uint encoding)
{
    for (uint i = 0; i < sizeof(gBankedRegisters) / sizeof(gBankedRegisters[0]); i++)
        if (gBankedRegisters[i].m_encoding == encoding)
            return gBankedRegisters[i].m_name;
    return NULL;
}

bool findA32BankedRegister(const cString& name, uint& encoding)
{
    for (uint i = 0; i < sizeof(gBankedRegisters) / sizeof(gBankedRegisters[0]); i++)
        if (name == gBankedRegisters[i].m_name)
        {
            encoding = gBankedRegisters[i].m_encoding;
            return true;
        }
    return false;
}

''' % rows


# The generators of the other tables: modules with generate(records, args), which
# write their files (the T16 and T32 tables: aarch32_thumb.py, the floating point
# and Advanced SIMD one: aarch32_simd.py)
OTHER_GENERATORS = ['aarch32_thumb', 'aarch32_simd']


def generate_others(records, args):
    import importlib
    for name in OTHER_GENERATORS:
        if not os.path.isfile(os.path.join(HERE, name + '.py')):
            continue
        module = importlib.import_module(name)
        if args.only and args.only != getattr(module, 'KEY', name):
            continue
        module.generate(records, args)


def generate(args):
    records = llvm.load()
    tables = [('a32', 'ArmEncodingTableA32.cpp', 'gArmA32EncodingTable',
               'The encoding table of the A32 integer, branch, load / store and system instructions',
               set(['ARM', 'CoProc']), A32_SKIP)]
    for key, filename, table, what, namespaces, skip in tables:
        if args.only and args.only != key:
            continue
        entries, skipped = generate_namespace(records, namespaces, skip)
        entries = sort_entries(entries)
        extra = ''
        helpers = ''
        if key == 'a32':
            import aarch32_a32
            entries = aarch32_a32.fixups(entries, records)
            entries = sort_entries(entries)
            extra = banked_code(records)
            helpers = ''.join('bool %s(uint32 e)\n{\n    %s\n}\n\n' % (n, b)
                              for n, b in aarch32_a32.HELPERS.items())
        emit(os.path.join(SOURCE, filename), table, what, entries, helpers=helpers, extra=extra)
        print('%s: %d entries -> %s' % (key, len(entries), filename))
        for name, why in skipped.items():
            print('   skipped %s: %s' % (name, why))
    generate_others(records, args)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--only')
    args = parser.parse_args()
    generate(args)


if __name__ == '__main__':
    main()
