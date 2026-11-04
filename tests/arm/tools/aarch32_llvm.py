#!/usr/bin/env python3
"""The LLVM description of AArch32, for gen_aarch32_tables.py.

The encoding tables of A32 and T32 are generated from the instruction records
of the LLVM ARM backend (llvm/lib/Target/ARM/*.td), dumped by llvm-tblgen:

    llvm-tblgen --dump-json -I <llvm>/include -I <llvm>/lib/Target/ARM \
                <llvm>/lib/Target/ARM/ARM.td -o arm.json

Set LLVM_ARM_JSON to the dump, or LLVM_SRC to the LLVM source tree (and
LLVM_TBLGEN to the llvm-tblgen binary, default: llvm-tblgen on the PATH or the
Homebrew one). The generated C++ is committed: the sources of LLVM are needed
only to regenerate it.

This module reads the records: the bits of the encoding, the operands with the
fields of each one, the assembly string (the order of the operands and the
parts of the mnemonic), the predicates.
"""
import collections
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
DEFAULT_TBLGEN = ['llvm-tblgen', '/opt/homebrew/opt/llvm/bin/llvm-tblgen']
DEFAULT_JSON = os.path.join(REPO, 'out', 'aarch32', 'arm.json')


def _find_tblgen():
    explicit = os.environ.get('LLVM_TBLGEN')
    candidates = [explicit] if explicit else DEFAULT_TBLGEN
    for path in candidates:
        for directory in [''] + os.environ.get('PATH', '').split(os.pathsep):
            full = os.path.join(directory, path) if directory else path
            if os.path.isfile(full) and os.access(full, os.X_OK):
                return full
    raise SystemExit('llvm-tblgen was not found (set LLVM_TBLGEN)')


def load():
    """Return the records of the ARM target (a dict name -> record)."""
    path = os.environ.get('LLVM_ARM_JSON')
    if path is None:
        path = DEFAULT_JSON
        source = os.environ.get('LLVM_SRC')
        if not os.path.isfile(path) and source:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            arm = os.path.join(source, 'llvm', 'lib', 'Target', 'ARM')
            cmd = [_find_tblgen(), '--dump-json', '-I', os.path.join(source, 'llvm', 'include'),
                   '-I', arm, os.path.join(arm, 'ARM.td'), '-o', path]
            subprocess.check_call(cmd)
    if not os.path.isfile(path):
        raise SystemExit('the LLVM ARM records are missing: set LLVM_ARM_JSON or LLVM_SRC')
    with open(path) as f:
        return json.load(f)


def type_name(value):
    """The name of a def / the text of a value of a record."""
    if isinstance(value, dict):
        return value.get('printable', value.get('def'))
    return str(value)


class Operand(object):
    """An operand of an instruction: the name, the class and the bits."""

    def __init__(self, name, cls, bits):
        self.name = name
        self.cls = cls
        self.copies = {}
        # bits: {bit of the operand: bit of the instruction}
        self.bits = bits

    def position(self, low=0):
        """The instruction bit of the bit 'low' of the operand."""
        return self.bits.get(low)

    def runs(self):
        """The (operand low bit, instruction position, width) of the runs of
        consecutive bits."""
        runs = []
        for bit in sorted(self.bits):
            position = self.bits[bit]
            if runs and runs[-1][0] + runs[-1][2] == bit and \
                    runs[-1][1] + runs[-1][2] == position:
                runs[-1][2] += 1
            else:
                runs.append([bit, position, 1])
        return [tuple(r) for r in runs]

    def layout(self):
        return ' '.join('%d-%d@%d' % (low, low + width - 1, pos) if width > 1 else '%d@%d' % (low, pos)
                        for low, pos, width in self.runs())

    def __repr__(self):
        return '%s:%s[%s]' % (self.name, self.cls, self.layout())


class Instruction(object):
    """A decoded record of an instruction."""

    def __init__(self, name, record):
        self.name = name
        self.record = record
        self.namespace = record['DecoderNamespace']
        self.size = record['Size']
        self.asm = record['AsmString']
        self.decoder = record['DecoderMethod']
        self.predicates = [type_name(p) for p in record['Predicates']]
        self.mask = 0
        self.value = 0
        # The bits of fields marked as "?" and the soft fail bits
        self.free = 0
        self.fields = collections.defaultdict(dict)
        # The bits of a field that is in the encoding twice (rev: Rm at 16 and
        # 0): {field: {bit: [positions]}}
        self.copies = collections.defaultdict(lambda: collections.defaultdict(list))
        bits = record['Inst'][:self.size * 8]
        for position, bit in enumerate(bits):
            if isinstance(bit, dict):
                self.fields[bit['var']][bit.get('index', 0)] = position
                self.copies[bit['var']][bit.get('index', 0)].append(position)
            elif bit in (0, 1):
                self.mask |= 1 << position
                self.value |= bit << position
            else:
                self.free |= 1 << position
        # The bits which the decoder only soft fails on ("should be one / zero"):
        # LLVM decodes the instruction whatever their value is
        self.soft = 0
        for position, bit in enumerate(record['SoftFail'][:self.size * 8]):
            if bit == 1:
                self.soft |= 1 << position
        self.operands = []
        for key in ('OutOperandList', 'InOperandList'):
            for arg in record[key]['args']:
                name_ = arg[1]
                cls = type_name(arg[0])
                bits_ = dict(self.fields[name_]) if name_ in self.fields else {}
                operand = Operand(name_, cls, bits_)
                if name_ in self.copies:
                    operand.copies = dict((k, list(v)) for k, v in self.copies[name_].items())
                self.operands.append(operand)
        self.by_name = dict((o.name, o) for o in self.operands if o.name)

    def operand(self, name):
        return self.by_name.get(name)

    def __repr__(self):
        return '<%s %0*x/%0*x %r>' % (self.name, self.size * 2, self.mask, self.size * 2,
                                      self.value, self.asm)


def instructions(records, namespaces):
    """The instructions of the decoder namespaces, sorted by name."""
    result = []
    for name in sorted(records['!instanceof']['Instruction']):
        record = records[name]
        if record['isPseudo'] or record['isCodeGenOnly'] or not record['Inst']:
            continue
        if record['DecoderNamespace'] not in namespaces:
            continue
        result.append(Instruction(name, record))
    return result


def split_asm(asm):
    """Split an AsmString into the mnemonic template and the operand template."""
    asm = asm.replace('\t', ' ')
    if ' ' in asm:
        mnemonic, rest = asm.split(' ', 1)
    else:
        mnemonic, rest = asm, ''
    return mnemonic, rest.strip()


def class_record(records, cls):
    return records.get(cls)


if __name__ == '__main__':
    records = load()
    for ins in instructions(records, set(sys.argv[1].split(','))):
        print(ins, [repr(o) for o in ins.operands])
