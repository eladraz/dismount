#!/usr/bin/env python3
"""The VFP and Advanced SIMD (NEON) table of AArch32, gArmA32SimdEncodingTable
(ArmEncodingTableA32Simd.cpp), generated from the LLVM records of the decoder
namespaces VFP, VFPV8, NEONData, NEONDup, NEONLoadStore, v8NEON and v8Crypto.

The T32 forms are the same entries: the decoder translates the T32 encoding to
the A32 one (ArmStreamDisassembler.cpp). The oracle fixups (the differences
between the LLVM source tree and the LLVM which objdump is) are here.
"""
import os
import sys

import aarch32_classes as classes
import aarch32_classes_simd  # registers the handlers
import gen_aarch32_tables as gen

KEY = 'simd'

NAMESPACES = set(['VFP', 'VFPV8', 'NEONData', 'NEONDup', 'NEONLoadStore', 'v8NEON', 'v8Crypto'])

# The LLVM predicates and their feature: (priority, feature). The entry takes the
# feature of the highest priority (the features imply each other)
SIMD_PREDICATES = {
    'HasV8_3a': (90, 'FEAT_FCMA_AA32'), 'HasV8_1a': (90, 'FEAT_RDM_AA32'),
    'HasFP16FML': (90, 'FEAT_FHM_AA32'), 'HasDotProd': (90, 'FEAT_DOTPROD_AA32'),
    'HasMatMulInt8': (90, 'FEAT_I8MM_AA32'), 'HasBF16': (90, 'FEAT_BF16_AA32'),
    'HasSHA2': (85, 'FEAT_SHA256_AA32'), 'HasAES': (85, 'FEAT_AES_AA32'),
    'HasFullFP16': (80, 'FEAT_FP16_AA32'), 'HasFP16': (75, 'FEAT_FP16_CVT'),
    'HasFPARMv8': (70, 'FEAT_FPARMV8'), 'HasV8': (55, 'FEAT_V8_AA32'),
    'HasVFP4': (60, 'FEAT_VFP4'), 'HasNEON': (50, 'FEAT_NEON'), 'HasVFP3': (40, 'FEAT_VFP3'),
    'HasVFP2': (30, 'FEAT_VFP2'), 'HasFPRegs': (20, 'FEAT_VFP2'),
    'HasFPRegs16': (80, 'FEAT_FP16_AA32'), 'HasFPRegs64': (20, 'FEAT_VFP2'),
}
IGNORED = set(['HasDPVFP', 'UseFusedMAC', 'UseFPVMLx', 'DontUseNEONForFP', 'UseVMOVSR',
               'HasFastVGETLNi32', 'HasFastVDUP32', 'IsARM', 'IsThumb', 'IsThumb2', 'PreV8'])
SKIPPED = set(['Has8MSecExt', 'HasV8_1MMainline', 'HasV8MMainline', 'HasMVEInt', 'HasLOB',
               'HasPACBTI', 'IsMClass', 'HasCDE'])

for name in SIMD_PREDICATES:
    gen.PREDICATE_FEATURE.setdefault(name, None)
gen.IGNORED_PREDICATES.update(IGNORED)
gen.SKIPPED_PREDICATES.update(SKIPPED)
gen.FIXED_LITERALS.update({
    'fpscr': 'ARM_A32_SIMD_FIXED_FPSCR', 'fpexc': 'ARM_A32_SIMD_FIXED_FPEXC',
    'fpsid': 'ARM_A32_SIMD_FIXED_FPSID', 'mvfr0': 'ARM_A32_SIMD_FIXED_MVFR0',
    'mvfr1': 'ARM_A32_SIMD_FIXED_MVFR1', 'mvfr2': 'ARM_A32_SIMD_FIXED_MVFR2',
    'fpinst': 'ARM_A32_SIMD_FIXED_FPINST', 'fpinst2': 'ARM_A32_SIMD_FIXED_FPINST2',
    'APSR_nzcv': 'ARM_A32_SIMD_FIXED_APSR_NZCV',
})


def simd_feature(ins):
    best = None
    for predicate in ins.predicates:
        if predicate in SIMD_PREDICATES:
            candidate = SIMD_PREDICATES[predicate]
            if predicate == 'HasV8_3a' and ins.name == 'VJCVT':
                candidate = (90, 'FEAT_JSCVT_AA32')
            if predicate == 'HasSHA2' and ins.name.startswith('SHA1'):
                candidate = (85, 'FEAT_SHA1_AA32')
            if best is None or candidate[0] > best[0]:
                best = candidate
    return best[1] if best else 'FEAT_VFP2'


FLOWS = {}


def fixups(entries, records):
    # vpush / vpop: vstmdb / vldmia with writeback of sp
    aliases = []
    for entry in entries:
        if entry.comment in ('VSTMSDB_UPD', 'VSTMDDB_UPD', 'VLDMSIA_UPD', 'VLDMDIA_UPD'):
            alias = gen.Entry('vpush' if 'VSTM' in entry.comment else 'vpop',
                              entry.mask | 0x000f0000, entry.value | 0x000d0000,
                              [s for s in entry.specs if s[0] != 'ARM_KIND_GPR'],
                              entry.feature, ['ARM_ENTRY_ALIAS'], entry.flow, entry.condition,
                              entry.comment)
            aliases.append(alias)
    return entries + aliases


def generate(records, args):
    entries = []
    skipped = {}
    for ins in gen.llvm.instructions(records, NAMESPACES):
        try:
            entry = gen.build_entry(ins, records)
        except classes.Unmapped as exc:
            skipped[ins.name] = str(exc)
            continue
        if entry is None:
            continue
        entry.feature = simd_feature(ins)
        entry.comment = ins.name
        entries.append(entry)
    entries = fixups(entries, records)
    entries = gen.sort_entries(entries)
    path = os.path.join(gen.SOURCE, 'ArmEncodingTableA32Simd.cpp')
    gen.emit(path, 'gArmA32SimdEncodingTable',
             'The encoding table of the A32 and T32 VFP and Advanced SIMD instructions', entries)
    print('simd: %d entries -> ArmEncodingTableA32Simd.cpp' % len(entries))
    reasons = {}
    for name, why in skipped.items():
        reasons.setdefault(why, []).append(name)
    for why, names in sorted(reasons.items(), key=lambda kv: -len(kv[1])):
        print('   skipped %d (%s): %s' % (len(names), why, ' '.join(names[:5])))
