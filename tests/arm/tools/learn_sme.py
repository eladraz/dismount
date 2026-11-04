#!/usr/bin/env python3
"""Learn SME encodings from objdump (tests/arm/TEST_PLAN.md) and write them as
rows for gen_a64_sme_table.py (a64groups_sme_learned.py).

The SME2 instructions are many (every multi vector instruction has a form with
two and with four registers, for each element size, with a single vector, with
a vector group and with an index) and regular. The tool starts from a word that
objdump decodes, flips each of its 32 bits and decodes the result:

  - the flip is invalid or gives another instruction (another mnemonic, other
    element sizes, another shape): the bit is part of the encoding (fixed);
  - the flip gives the same text: LLVM ignores the bit (it is not in the mask);
  - the flip changes the number of a register, a tile, an offset or an index
    (and nothing else): the bit is a field of the operand which prints that
    number; the weights of the changes give the position, the width and the
    alignment of the field.

From the text of the operands (za.s[w8, 0, vgx2], { z0.b, z1.b }, z0.h[1], p0/m,
pn8...) and their fields the tool builds the operand specs of the SME and SVE
kinds, and the features from the message of clang ("instruction requires:").
The rows are what the table has. The words come from the sample of
out/scratch (the groups of survey words), from the files of --words, or from
the words which gen_a64_sme_table.py --check does not cover.

    learn_sme.py --groups GROUPS.pkl --out tests/arm/tools/a64groups_sme_learned.py
    learn_sme.py --words FILE [--add]           (add the rows of these words)
"""
import argparse
import collections
import os
import pickle
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

SIZE_ENUM = {'b': 'ARM_SVE_B', 'h': 'ARM_SVE_H', 's': 'ARM_SVE_S', 'd': 'ARM_SVE_D', 'q': 'ARM_SVE_Q',
             None: 'ARM_SVE_NONE'}
SIZE_CODE = {'b': 0, 'h': 1, 's': 2, 'd': 3, 'q': 4}

# The features which make an encoding, strongest first (the first of them found in the
# message of clang is the feature of the entry, the other one is the second feature)
FEATURE_ORDER = ['sme-mop4', 'sme-tmop', 'sme-f8f16', 'sme-f8f32', 'sme-f16f16', 'sme-b16b16', 'fp8',
                 'faminmax', 'sve-b16b16', 'sve-bfscale', 'sme-f64f64', 'sme-i16i64', 'sme-lutv2',
                 'sme2p3', 'sme2p2', 'sme2p1', 'sme2', 'sme']
FEATURE_NAME = {'sme': 'SME', 'sme2': 'SME2', 'sme2p1': 'SME2P1', 'sme2p2': 'SME2P2', 'sme2p3': 'SME2P3',
                'sme-f64f64': 'F64F64', 'sme-i16i64': 'I16I64', 'sme-f16f16': 'F16F16',
                'sme-b16b16': 'SME_B16B16', 'sme-f8f16': 'F8F16', 'sme-f8f32': 'F8F32',
                'sme-lutv2': 'LUTV2', 'sme-mop4': 'MOP4', 'sme-tmop': 'TMOP', 'fp8': 'FP8',
                'faminmax': 'FAMINMAX', 'sve-b16b16': 'B16B16', 'sve-bfscale': 'BFSCALE'}

TOKEN = re.compile(r'[A-Za-z]+|0x[0-9a-f]+|[0-9]+|[^\sA-Za-z0-9]')


class Unsupported(Exception):
    pass


def expand_ranges(text):
    """{ z0.s - z3.s } as { z0.s, z1.s, z2.s, z3.s }: a flip which wraps a list around z31
    prints it as a list and not as a range, the tokens of the two have to line up"""
    def expand(m):
        first, last = int(m.group(1)), int(m.group(3))
        size = m.group(2) or ''
        if last < first:
            return m.group(0)
        return '{ ' + ', '.join('z%d%s' % (n, size) for n in range(first, last + 1)) + ' }'
    return re.sub(r'\{ z(\d+)(\.\w)? - z(\d+)(\.\w)? \}', expand, text)


def tokenize(text):
    return [(m.group(0), m.start(), m.end()) for m in TOKEN.finditer(text)]


def is_number(token):
    return re.fullmatch(r'0x[0-9a-f]+|[0-9]+', token) is not None


def value_of(token):
    return int(token, 0)


def split_operands(text):
    """The character spans of the operands of an instruction text"""
    space = text.find(' ')
    if space < 0:
        return []
    spans = []
    depth = 0
    start = space + 1
    for i in range(space + 1, len(text) + 1):
        c = text[i] if i < len(text) else ','
        if c in '{[':
            depth += 1
        elif c in '}]':
            depth -= 1
        elif c == ',' and depth == 0:
            spans.append((start, i))
            start = i + 1
            while start < len(text) and text[start] == ' ':
                start += 1
    return spans


class Flips(object):
    """The effect of the 32 single bit flips of a word"""

    def __init__(self, word, base, flipped):
        self.word = word
        base = expand_ranges(base)
        flipped = [expand_ranges(t) for t in flipped]
        self.base = base
        self.tokens = tokenize(base)
        self.operands_start = base.find(' ') + 1 if ' ' in base else len(base)
        self.tied = {}
        self.invalid = set()
        self.ignored = set()
        self.fixed = set()
        # bit -> list of (token index, delta) of the numeric flips; bit -> token index of a h / v flip
        self.numeric = {}
        self.direction = {}
        for bit, text in enumerate(flipped):
            if text == '<unknown>':
                self.invalid.add(bit)
                continue
            if text == base:
                self.ignored.add(bit)
                continue
            other = tokenize(text)
            if len(other) != len(self.tokens):
                self.fixed.add(bit)
                continue
            diffs = [(i, a[0], b[0]) for i, (a, b) in enumerate(zip(self.tokens, other)) if a[0] != b[0]]
            if any(self.tokens[i][1] < self.operands_start for i, _, _ in diffs):
                # (the mnemonic changes: another instruction)
                self.fixed.add(bit)
                continue
            if all(is_number(a) and is_number(b) for _, a, b in diffs):
                changes = [(i, value_of(b) - value_of(a)) for i, a, b in diffs]
                if not self.is_uniform(changes):
                    # (the registers of a list moved by different amounts: another stride)
                    self.fixed.add(bit)
                    continue
                self.numeric[bit] = changes
            elif len(diffs) == 1 and {diffs[0][1], diffs[0][2]} == {'h', 'v'}:
                self.direction[bit] = diffs[0][0]
            else:
                self.fixed.add(bit)

    def is_uniform(self, changes):
        """True if the registers of each list moved by the same amount"""
        opens = [i for i, t in enumerate(self.tokens) if t[0] == '{']
        for start in opens:
            end = start
            while end < len(self.tokens) and self.tokens[end][0] != '}':
                end += 1
            registers = [i for i in range(start + 1, end) if self.tokens[i - 1][0] == 'z' and is_number(self.tokens[i][0])]
            moved = dict((i, d % 32) for i, d in changes if i in registers)
            if moved and (len(moved) != len(registers) or len(set(moved.values())) > 1):
                return False
        return True

    def bits_of(self, indexes):
        """The numeric field bits which change only tokens of 'indexes' (and at least one);
        the tokens of an operand tied to another one (the same text: add { z0.b, z1.b }, { z0.b,
        z1.b }, z0.b) change together"""
        indexes = set(indexes)
        for i in list(indexes):
            indexes |= self.tied.get(i, set())
        bits = {}
        for bit, changes in self.numeric.items():
            touched = set(i for i, _ in changes)
            if touched & indexes:
                if not touched <= indexes:
                    raise Unsupported('bit %d is shared by operands' % bit)
                bits[bit] = changes
        return bits


def run_of(bits):
    """(position, width) of a set of contiguous bits"""
    low, high = min(bits), max(bits)
    if sorted(bits) != list(range(low, high + 1)):
        raise Unsupported('the field %s is not contiguous' % sorted(bits))
    return low, high - low + 1


def weight(bits, changes_of, token):
    """The change of the token by the lowest bit of the field"""
    low = min(bits)
    for i, d in changes_of[low]:
        if i == token:
            return abs(d)
    raise Unsupported('no weight')


class Operand(object):
    def __init__(self, text, start, end, flips):
        self.text = text
        self.start = start
        self.end = end
        self.token_indexes = set(i for i, (t, s, e) in enumerate(flips.tokens) if s >= start and e <= end)
        self.bits = set()


def pow2_log(value):
    if value not in (1, 2, 4, 8):
        raise Unsupported('weight %d' % value)
    return {1: 0, 2: 1, 4: 2, 8: 3}[value]


def number_token_indexes(flips, operand, pattern):
    """The indexes of the numeric tokens of the operand text which a regex group
    matches: pattern is a regex with named groups over the operand text"""
    m = re.fullmatch(pattern, operand.text)
    if not m:
        return None
    result = {}
    for name, value in m.groupdict().items():
        if value is None:
            continue
        s = operand.start + m.start(name)
        for i in operand.token_indexes:
            if flips.tokens[i][1] == s and is_number(flips.tokens[i][0]):
                result[name] = i
    return m, result


def field_spec_register(flips, token, text_number, kind):
    """(position, width, base, multiple_log2) of the register number at a token"""
    bits = flips.bits_of({token})
    if not bits:
        raise Unsupported('register without a field')
    pos, width = run_of(bits.keys())
    multiple = pow2_log(weight(bits.keys(), bits, token))
    value = (flips.word >> pos) & ((1 << width) - 1)
    base = text_number - (value << multiple)
    return pos, width, base, multiple, bits


def register_weights(bits, token):
    """(log2 of the multiple, split bits, split step) of a register field from the
    weights its bits give to the register: w, 2w, 4w... or the low bits linear and the
    bits above them counting more (the 20 + {0-3}, 28 + {0-3} of the zk)"""
    ordered = sorted(bits)
    weights = []
    for bit in ordered:
        for i, d in bits[bit]:
            if i == token:
                weights.append(abs(d))
    if len(weights) != len(ordered):
        raise Unsupported('register weights')
    multiple = pow2_log(weights[0])
    linear = 1
    while linear < len(weights) and weights[linear] == weights[0] << linear:
        linear += 1
    if linear == len(weights):
        return multiple, 0, 0
    # the bits above the linear part: the first one counts 'step' times the weight of the low part
    step = weights[linear] // weights[0]
    if weights[linear] % weights[0] != 0 or any(
            weights[linear + k] != weights[linear] << k for k in range(len(weights) - linear)):
        raise Unsupported('register weights %s' % weights)
    return multiple, linear, step


def split_value(value, split_bits, split_step):
    if not split_bits:
        return value
    return (value & ((1 << split_bits) - 1)) + (value >> split_bits) * split_step


def spec_tuple(kind, pos, flags, extra):
    return (kind, pos, flags, extra)


def parse_operand(flips, operand, mnemonic, context):
    """Return (spec tuple or None for a mnemonic part, set of bits)"""
    text = operand.text
    tokens = flips.tokens

    def num(i):
        return value_of(tokens[i][0])

    # za array: za.s[w8, 0, vgx2], za.s[w9, 0x2:0x3, vgx4], za[w12, 5]
    pattern = (r'za(?:\.(?P<size>\w))?\[w(?P<rv>\d+), (?P<first>0x[0-9a-f]+|\d+)(?::(?P<last>0x[0-9a-f]+|\d+))?'
               r'(?:, vgx(?P<vg>\d))?\]')
    found = number_token_indexes(flips, operand, pattern)
    if found:
        m, idx = found
        bits_rv = flips.bits_of({idx['rv']})
        if sorted(bits_rv) != [13, 14]:
            raise Unsupported('the vector select register is not at 14-13')
        base = int(m.group('rv')) - ((flips.word >> 13) & 3)
        if base not in (8, 12):
            raise Unsupported('vector select base %d' % base)
        first = int(m.group('first'), 0)
        last = int(m.group('last'), 0) if m.group('last') else first
        length = last - first + 1
        offset_tokens = {idx['first']}
        if 'last' in idx:
            offset_tokens.add(idx['last'])
        bits = flips.bits_of(offset_tokens)
        if bits:
            pos, width = run_of(bits.keys())
            if weight(bits.keys(), bits, idx['first']) != length:
                raise Unsupported('range and weight')
        else:
            pos, width = 0, 0
            if first != 0:
                raise Unsupported('offset without a field')
        group = int(m.group('vg')) if m.group('vg') else 0
        flags = {0: 0, 2: 1, 4: 2}[group] | (pow2_log(length) << 2) | (width << 4) | (0x80 if base == 12 else 0)
        size = m.group('size')
        return (spec_tuple('ARM_KIND_SME_ARRAY', pos, flags, SIZE_ENUM[size]),
                set(bits) | set(bits_rv))

    # za slice: za0h.s[w12, 1], {za0h.b[w12, 0]}, za1v.h[w13, 0x2:0x3]
    pattern = (r'(?P<open>\{)?za(?P<tile>\d+)(?P<dir>[hv])\.(?P<size>\w)\[w(?P<rv>\d+), '
               r'(?P<first>0x[0-9a-f]+|\d+)(?::(?P<last>0x[0-9a-f]+|\d+))?\](?P<close>\})?')
    found = number_token_indexes(flips, operand, pattern)
    if found:
        m, idx = found
        base = int(m.group('rv')) - ((flips.word >> 13) & 3)
        if base != 12:
            raise Unsupported('slice base %d' % base)
        bits_rv = flips.bits_of({idx['rv']})
        if sorted(bits_rv) != [13, 14]:
            raise Unsupported('the slice register is not at 14-13')
        first = int(m.group('first'), 0)
        last = int(m.group('last'), 0) if m.group('last') else first
        length = last - first + 1
        tokens_set = {idx['tile'], idx['first']}
        if 'last' in idx:
            tokens_set.add(idx['last'])
        bits = flips.bits_of(tokens_set)
        pos, width = run_of(bits.keys())
        dir_token = None
        for i in operand.token_indexes:
            if tokens[i][0] in ('h', 'v') and tokens[i - 1][0].isdigit():
                dir_token = i
        direction_bits = [b for b, i in flips.direction.items() if i == dir_token]
        if direction_bits != [15]:
            raise Unsupported('direction bits %s' % direction_bits)
        flags = (SIZE_CODE[m.group('size')]) | (pow2_log(length) << 3) | (0x20 if m.group('open') else 0)
        return (spec_tuple('ARM_KIND_SME_TILE_SLICE', pos, flags, 0),
                set(bits) | set(bits_rv) | {15})

    # za tile: za0.s
    found = number_token_indexes(flips, operand, r'za(?P<tile>\d+)\.(?P<size>\w)')
    if found:
        m, idx = found
        bits = flips.bits_of({idx['tile']})
        if bits:
            pos, width = run_of(bits.keys())
        else:
            pos, width = 0, 0
        return (spec_tuple('ARM_KIND_SME_TILE', pos, width, SIZE_ENUM[m.group('size')]), set(bits))

    # zt0
    if text == 'zt0':
        return (spec_tuple('ARM_KIND_SME_ZT0', 0, 0, 'ARM_SME_ZT0_PLAIN'), set())

    # a list of z registers, with an optional index: { z0.s, z1.s }, { z0.s - z3.s }, { z0, z1 }[1]
    m = re.fullmatch(r'\{ (?P<items>[^}]*) \}(?:\[(?P<index>\d+)\])?', text)
    if m and text.startswith('{ z'):
        items = m.group('items')
        regs = []
        size = None
        zs = re.findall(r'z(\d+)(?:\.(\w))?', items)
        for n, s in zs:
            regs.append(int(n))
            size = s or None
        is_range = ' - ' in items
        if is_range:
            first, last = regs[0], regs[-1]
            count = last - first + 1
            stride = 1
        else:
            count = len(regs)
            stride = (regs[1] - regs[0]) % 32 if count > 1 else 1
        # the token of the first register
        first_token = None
        for i in sorted(operand.token_indexes):
            if tokens[i][0] == 'z' and is_number(tokens[i + 1][0]):
                first_token = i + 1
                break
        if first_token is None:
            raise Unsupported('list without a register')
        list_tokens = set(i + 1 for i in operand.token_indexes
                          if tokens[i][0] == 'z' and i + 1 < len(tokens) and is_number(tokens[i + 1][0])
                          and not (m.group('index') is not None and False))
        reg_bits = flips.bits_of(list_tokens)
        if not reg_bits:
            raise Unsupported('list without a field')
        if stride != 1:
            pos = min(reg_bits)
            lows = sorted(b for b in reg_bits if b < pos + 4)
            if sorted(reg_bits) != lows + [pos + 4] or lows != list(range(pos, pos + len(lows))):
                raise Unsupported('strided field %s' % sorted(reg_bits))
            if stride != 16 // count:
                raise Unsupported('stride %d' % stride)
            flags = count | 8
            bits = set(reg_bits)
            width = None
        else:
            pos, width = run_of(reg_bits.keys())
            multiple = pow2_log(weight(reg_bits.keys(), reg_bits, first_token))
            value = (flips.word >> pos) & ((1 << width) - 1)
            first_base = regs[0] - (value << multiple)
            if first_base != 0 or width != 5 - multiple:
                if m.group('index') is not None:
                    pass
                elif first_base in (0, 16) and width + multiple <= 5:
                    layout = ('LAYOUT', (), count, multiple, first_base, 0, 0)
                    flags = width | ((SIZE_CODE[size] if size else 7) << 4)
                    return (spec_tuple('ARM_KIND_SME_Z_INDEXED', pos, flags, layout), set(reg_bits))
                raise Unsupported('list field of %d bits, multiple %d, base %d' % (width, 1 << multiple, first_base))
            flags = count | (multiple << 4)
            bits = set(reg_bits)
        if m.group('index') is not None:
            # { z0, z1 }[1]: an indexed list
            index_token = None
            for i in sorted(operand.token_indexes):
                if tokens[i][0] == '[' and is_number(tokens[i + 1][0]) and i > first_token:
                    index_token = i + 1
            index_bits = flips.bits_of({index_token})
            runs = index_runs(index_bits, index_token)
            if stride != 1:
                raise Unsupported('indexed strided list')
            multiple = (flags >> 4) & 3
            layout = ('LAYOUT', tuple(runs), count, multiple, 0, 0, 0)
            return (spec_tuple('ARM_KIND_SME_Z_INDEXED', pos, (5 - multiple) | (7 << 4), layout),
                    bits | set(index_bits))
        return (spec_tuple('ARM_KIND_SVE_Z_LIST', pos, flags, SIZE_ENUM[size]), bits)

    # a z register, with an element index: z0.s, z0.h[1], z0[1]
    found = number_token_indexes(flips, operand, r'z(?P<reg>\d+)(?:\.(?P<size>\w))?(?:\[(?P<index>\d+)\])?')
    if found:
        m, idx = found
        reg_bits = flips.bits_of({idx['reg']})
        if not reg_bits:
            raise Unsupported('register without a field')
        pos, width = run_of(reg_bits.keys())
        value = (flips.word >> pos) & ((1 << width) - 1)
        base = int(m.group('reg')) - value
        multiple, split_bits, split_step = register_weights(reg_bits, idx['reg'])
        base = int(m.group('reg')) - (split_value(value, split_bits, split_step) << multiple)
        if base not in (0, 16, 20):
            raise Unsupported('register base %d' % base)
        size = m.group('size')
        if 'index' not in idx:
            if width == 5 and base == 0 and multiple == 0 and not split_bits:
                return (spec_tuple('ARM_KIND_SVE_Z', pos, 0, SIZE_ENUM[size]), set(reg_bits))
            # a narrow register
            layout = ('LAYOUT', (), 0, multiple, base, split_bits, split_step)
            flags = width | ((SIZE_CODE[size] if size else 7) << 4)
            return (spec_tuple('ARM_KIND_SME_Z_INDEXED', pos, flags, layout), set(reg_bits))
        index_bits = flips.bits_of({idx['index']})
        runs = index_runs(index_bits, idx['index'])
        layout = ('LAYOUT', tuple(runs), 0, multiple, base, split_bits, split_step)
        flags = width | ((SIZE_CODE[size] if size else 7) << 4)
        return (spec_tuple('ARM_KIND_SME_Z_INDEXED', pos, flags, layout), set(reg_bits) | set(index_bits))

    # predicates: p0/m, p0/z, pn8
    found = number_token_indexes(flips, operand, r'(?P<kind>pn|p)(?P<reg>\d+)(?:/(?P<q>[mz]))?')
    if found:
        m, idx = found
        reg_bits = flips.bits_of({idx['reg']})
        pos, width = run_of(reg_bits.keys())
        value = (flips.word >> pos) & ((1 << width) - 1)
        base = int(m.group('reg')) - value
        qual = {None: 0, 'm': 1, 'z': 2}[m.group('q')]
        if m.group('kind') == 'p':
            if base != 0 or width not in (3, 4):
                raise Unsupported('predicate field')
            return (spec_tuple('ARM_KIND_SVE_PREDICATE', pos, (1 if width == 3 else 0) | (qual << 1), 'ARM_SVE_NONE'),
                    set(reg_bits))
        if (base == 8 and width == 3):
            return (spec_tuple('ARM_KIND_SVE_PREDICATE_COUNTER', pos, 1 | (qual << 1), 'ARM_SVE_NONE'),
                    set(reg_bits))
        if base == 0 and width == 4:
            return (spec_tuple('ARM_KIND_SVE_PREDICATE_COUNTER', pos, qual << 1, 'ARM_SVE_NONE'), set(reg_bits))
        raise Unsupported('counter field')

    # the shift of the narrowing instructions: #0x10
    found = number_token_indexes(flips, operand, r'#(?P<value>0x[0-9a-f]+|\d+)')
    if found:
        m, idx = found
        bits = flips.bits_of({idx['value']})
        if bits and min(bits) == 16 and max(bits) == 22 and 21 not in bits:
            # the 64 bit sources: tsz and imm in the bits 22-16
            return (spec_tuple('ARM_KIND_SVE_IMMEDIATE', 0, 0, 'ARM_SVE_IMM_SHIFT_MULTI64'),
                    set(bits) | {21})
        pos, width = run_of(bits.keys())
        if pos != 16:
            raise Unsupported('immediate field')
        if width == 4:
            return (spec_tuple('ARM_KIND_SVE_IMMEDIATE', 0, 0, 'ARM_SVE_IMM_SHIFT_MULTI'), set(bits))
        if width == 5:
            return (spec_tuple('ARM_KIND_SVE_IMMEDIATE', 0, 0, 'ARM_SVE_IMM_SHIFT_MULTI32'), set(bits))
        raise Unsupported('immediate of %d bits' % width)
    raise Unsupported('operand %s' % text)


def index_runs(index_bits, token):
    """The fields of an index, from the most significant: runs of adjacent bits
    whose weights are consecutive powers of two"""
    items = sorted(((abs(ch[0][1]), bit) for bit, ch in index_bits.items()), reverse=True)
    if not items:
        raise Unsupported('index without bits')
    runs = []
    for weight_, bit in items:
        if runs and runs[-1]['low'] == bit + 1 and runs[-1]['weight'] == weight_ * 2:
            runs[-1]['low'] = bit
            runs[-1]['weight'] = weight_
            runs[-1]['width'] += 1
        else:
            runs.append({'low': bit, 'weight': weight_, 'width': 1})
    # the weights are 2^(n-1) ... 1 over the bits taken from the most significant
    if [w for w, _ in items] != [1 << (len(items) - 1 - i) for i in range(len(items))]:
        raise Unsupported('index weights')
    if len(runs) > 3:
        raise Unsupported('index of %d fields' % len(runs))
    return [(run['low'], run['width']) for run in runs]


def learn_word(word, base, flipped, mnemonic_text=None):
    """Return (mnemonic, mask, value, specs, text) or raise Unsupported"""
    flips = Flips(word, base, flipped)
    original = base
    base = flips.base
    mnemonic = base.split(' ')[0]
    spans = split_operands(base)
    operands = [Operand(base[s:e], s, e, flips) for s, e in spans]
    specs = []
    field_bits = set()
    # The operands which print the same text and change together are one field (add { z0.b, z1.b },
    # { z0.b, z1.b }, z0.b)
    def touching(operand):
        return set(bit for bit, changes in flips.numeric.items()
                   if any(i in operand.token_indexes for i, _ in changes))
    for a in range(len(operands)):
        for b in range(a + 1, len(operands)):
            if operands[a].text == operands[b].text and touching(operands[a]) and \
                    touching(operands[a]) == touching(operands[b]):
                ta = sorted(operands[a].token_indexes)
                tb = sorted(operands[b].token_indexes)
                for x, y in zip(ta, tb):
                    flips.tied.setdefault(x, set()).add(y)
                    flips.tied.setdefault(y, set()).add(x)
    seen = {}
    for operand in operands:
        spec, bits = parse_operand(flips, operand, mnemonic, None)
        specs.append(spec)
        if operand.text in seen and seen[operand.text] == (spec, bits):
            continue
        seen[operand.text] = (spec, bits)
        if field_bits & bits:
            raise Unsupported('operands share bits %s' % sorted(field_bits & bits))
        field_bits |= bits
    # every numeric flip must belong to a field
    for bit in flips.numeric:
        if bit not in field_bits:
            raise Unsupported('bit %d changes a number which no operand claims' % bit)
    for bit in flips.direction:
        if bit not in field_bits:
            raise Unsupported('bit %d changes a direction which no operand claims' % bit)
    # The vector group can be left out when the instruction has a list of registers
    # (add za.s[w8, 0], { z0.s, z1.s }): the registers say it
    has_list = any(kind == 'ARM_KIND_SVE_Z_LIST' or (kind == 'ARM_KIND_SME_Z_INDEXED' and isinstance(extra, tuple)
                                                     and extra[2] != 0)
                   for kind, _, _, extra in specs)
    list_counts = set(flags & 7 if kind == 'ARM_KIND_SVE_Z_LIST' else extra[2]
                      for kind, _, flags, extra in specs
                      if kind == 'ARM_KIND_SVE_Z_LIST' or
                      (kind == 'ARM_KIND_SME_Z_INDEXED' and isinstance(extra, tuple) and extra[2] != 0))
    if has_list:
        # (fvdotb za.s[w8, 0, vgx4], { z0.b, z1.b }, z0.b[0] needs the group: two registers say vgx2)
        specs = [(kind, pos, flags, extra + ' | ARM_SME_ARRAY_OPTIONAL_GROUP')
                 if kind == 'ARM_KIND_SME_ARRAY' and ((flags & 3) != 0) and
                 list_counts == {1 << (flags & 3)} else (kind, pos, flags, extra)
                 for kind, pos, flags, extra in specs]
    mask = 0xffffffff
    for bit in field_bits | flips.ignored:
        mask &= ~(1 << bit)
    value = word & mask
    return mnemonic, mask, value, specs, original, flips


def requirement(text):
    """The features of clang for a text: a list of alternatives, each a list of features"""
    results = armlib.clang_assemble([text], 'a64', [])
    status, data = results[0]
    if status == 'ok':
        return []
    m = re.match(r'instruction requires: (.*)$', data)
    if not m:
        raise Unsupported('clang: ' + data)
    alternatives = []
    for part in m.group(1).split(' or '):
        alternatives.append(part.split())
    return alternatives


def feature_pair(alternative):
    """(feature, second feature) names of a conjunction of LLVM features"""
    ordered = [f for f in FEATURE_ORDER if f in alternative]
    if len(ordered) != len(alternative):
        raise Unsupported('features %s' % alternative)
    if len(ordered) == 1:
        return FEATURE_NAME[ordered[0]], None
    if len(ordered) == 2:
        return FEATURE_NAME[ordered[0]], FEATURE_NAME[ordered[1]]
    raise Unsupported('features %s' % alternative)


def format_extra(extra):
    if isinstance(extra, tuple) and extra[0] == 'LAYOUT':
        return repr(extra)
    return repr(extra) if isinstance(extra, str) and not extra.startswith('ARM_') else extra


def row_source(mnemonic, mask, value, specs, feats, text):
    parts = []
    for kind, pos, flags, extra in specs:
        if isinstance(extra, tuple):
            extra_text = repr(extra)
        elif isinstance(extra, str):
            extra_text = repr(extra)
        else:
            extra_text = str(extra)
        parts.append('(%r, %d, %d, %s)' % (kind, pos, flags, extra_text))
    return '    (%r, 0x%08x, 0x%08x, %r, [%s]),  # %s' % (mnemonic, mask, value, feats, ', '.join(parts), text)


def flips_of(words):
    """objdump of the 32 single bit flips of each word"""
    flipped_words = []
    for word in words:
        flipped_words.extend(word ^ (1 << b) for b in range(32))
    flipped, _ = armlib.objdump_chunks(flipped_words)
    return [flipped[i * 32:(i + 1) * 32] for i in range(len(words))]


def learn_all(items, verbose=True):
    """items: list of (word, text). Return (rows, failures, examples)

    The fields of a first pass (the bits which change a number) are cleared and
    the word is analysed again: a list of registers whose first register has bits
    set is printed as another shape when a flip wraps it around z31, a zero field
    has no such flips"""
    state = list(items)
    flipped = flips_of([w for w, _ in state])
    for _ in range(4):
        changed = []
        candidates = []
        for index, ((word, text), flips_text) in enumerate(zip(state, flipped)):
            flips = Flips(word, text, flips_text)
            bits = set(flips.numeric) | set(flips.direction)
            cleared = word & ~sum(1 << b for b in bits)
            if cleared != word:
                changed.append(index)
                candidates.append(cleared)
        if not changed:
            break
        texts, _ = armlib.objdump_chunks(candidates)
        again = []
        for index, other, other_text in zip(changed, candidates, texts):
            if other_text != '<unknown>' and other_text.split(' ')[0] == state[index][1].split(' ')[0]:
                state[index] = (other, other_text)
                again.append(index)
        if not again:
            break
        new_flips = flips_of([state[i][0] for i in again])
        for index, value in zip(again, new_flips):
            flipped[index] = value
    second_items = state
    second = flipped
    rows = {}
    failures = collections.Counter()
    failure_examples = {}
    for (word, text), flipped in zip(second_items, second):
        try:
            mnemonic, mask, value, specs, base, flips = learn_word(word, text, flipped)
            alternatives = requirement(text)
            feats = []
            if not alternatives:
                feats = [('SME', None)]
            else:
                for alternative in alternatives:
                    feats.append(feature_pair(alternative))
            key = (mnemonic, mask, value, repr(specs))
            if key not in rows:
                rows[key] = (mnemonic, mask, value, specs, feats, text)
        except Unsupported as e:
            failures[str(e)] += 1
            failure_examples.setdefault(str(e), (word, text))
    return list(rows.values()), failures, failure_examples


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--groups', help='pickle of the survey groups (out/scratch/sme_groups3.pkl)')
    parser.add_argument('--words', help='file of hex words')
    parser.add_argument('--skip', default='', help='regex of the mnemonics which the groups of the tool skip')
    parser.add_argument('--out')
    args = parser.parse_args()
    items = []
    if args.groups:
        groups = pickle.load(open(args.groups, 'rb'))
        skip = re.compile(args.skip) if args.skip else None
        for (mnemonic, shape), (a, o, c, example) in sorted(groups.items()):
            if skip and skip.fullmatch(mnemonic):
                continue
            items.append(example)
    if args.words:
        words = [int(w, 16) for line in open(args.words) for w in line.split('#')[0].split()]
        texts, _ = armlib.objdump_chunks(words)
        items.extend((w, t) for w, t in zip(words, texts) if t != '<unknown>')
    rows, failures, examples = learn_all(items)
    print('%d rows learned from %d words' % (len(rows), len(items)))
    for message, count in failures.most_common():
        print('  %-50s %d  e.g. %08x %s' % (message, count, examples[message][0], examples[message][1]))
    if args.out:
        with open(args.out, 'w') as f:
            f.write('"""The SME encodings learned from objdump by learn_sme.py (GENERATED: do not edit;\n')
            f.write('the rows are (mnemonic, mask, value, [(feature, second feature)], operand specs))."""\n\n')
            f.write('ROWS = [\n')
            for mnemonic, mask, value, specs, feats, text in sorted(rows, key=lambda r: (r[0], r[1], r[2])):
                f.write(row_source(mnemonic, mask, value, specs, feats, text) + '\n')
            f.write(']\n')


if __name__ == '__main__':
    main()
