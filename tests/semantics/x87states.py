#!/usr/bin/env python3
"""
The `@x87:` annotation of the semantics corpus lines (tests/semantics/corpus/x87_edge.asm):

    fadd st0, st1    ; @x87: fcw=27f stack=2 st0=f:1.5 st1=denormal

The line is run from exactly this state (with random integer registers), not from the random states.
Tokens (numbers are hexadecimal, as in the `dismount_cli run` protocol):

    fcw=  fsw=  mxcsr=      the control, status (C0-C3, the flags; TOP comes from top=) and MXCSR words
    top=N                   TOP of the stack (0-7), default 0
    stack=N                 N registers ST(0)..ST(N-1) are valid (the others are empty), default 0
    tags=HEX                the abridged tag word instead of stack= (bit i: physical register i is valid)
    st0=V .. st7=V          the value of ST(i), a valid register
    r0=V .. r7=V            the value of the physical register i, does not change its tag
    m=SPEC                  the value of the memory operand at its effective address (absolute address):
                              f32:V f64:V  x80:V  i16:N i32:N i64:N  bcd:N  raw:HEXBYTES
    mem=ADDR:HEXBYTES,..    raw bytes
    eax= ... edi= eflags=   integer registers

A value V is a hex number of up to 20 digits (an 80 bit register), `f:<float>` (a Python float, exact),
`i:<integer>`, or a name: zero one two three half ten inf nan snan indefinite denormal
pseudo-denormal unnormal pseudo-inf pseudo-nan max min tiny, with a leading `-` for a negative sign.
For f32 / f64 / i16.. the same names and floats are converted to the format.
"""
import re
import struct
from fractions import Fraction

M32 = 0xFFFFFFFF

X80_NAMES = {
    'zero': 0, 'one': 0x3fff8000000000000000, 'two': 0x40008000000000000000,
    'three': 0x4000c000000000000000, 'half': 0x3ffe8000000000000000, 'ten': 0x4002a000000000000000,
    'inf': 0x7fff8000000000000000, 'nan': 0x7fffc000000000000001, 'snan': 0x7fffa000000000000000,
    'indefinite': 0xffffc000000000000000,
    'denormal': 0x00004000000000000000, 'pseudo-denormal': 0x00008000000000000001,
    'unnormal': 0x3fff4000000000000000, 'pseudo-inf': 0x7fff0000000000000000,
    'pseudo-nan': 0x7fff4000000000000000,
    'max': 0x7ffeffffffffffffffff, 'min': 0x00018000000000000000, 'tiny': 0x00000000000000000001,
}


def float_to_x80(value):
    """The 80 bit register of a Python float (exact)."""
    if value != value:
        return X80_NAMES['nan']
    if value in (float('inf'), float('-inf')):
        return X80_NAMES['inf'] | ((1 << 79) if value < 0 else 0)
    sign = 1 if str(value).startswith('-') else 0
    if value == 0:
        return sign << 79
    return fraction_to_x80(Fraction(abs(value)), sign)


def fraction_to_x80(fraction, sign):
    """The 80 bit register of a Fraction that the format represents exactly."""
    e = fraction.numerator.bit_length() - fraction.denominator.bit_length()
    while Fraction(2) ** e > fraction:
        e -= 1
    while Fraction(2) ** (e + 1) <= fraction:
        e += 1
    significand = fraction / (Fraction(2) ** (e - 63))
    assert significand.denominator == 1 and significand.numerator >> 63 == 1, 'not exact: %s' % fraction
    return (sign << 79) | ((e + 0x3fff) << 64) | significand.numerator


def parse_x80(text):
    sign = 0
    if text.startswith('-') and text[1:] in X80_NAMES:
        sign, text = 1, text[1:]
    elif text.startswith('-'):
        return int(text[1:], 16) | (1 << 79) if not re.match(r'^-[fi]:', text) else parse_x80(text[1:]) ^ (1 << 79)
    if text in X80_NAMES:
        return X80_NAMES[text] | (sign << 79)
    if text.startswith('f:'):
        return float_to_x80(float(text[2:]))
    if text.startswith('i:'):
        n = int(text[2:], 0)
        return fraction_to_x80(Fraction(abs(n)), 1 if n < 0 else 0) if n else 0
    return int(text, 16)


def narrow(text, bits):
    """f32 / f64 patterns of the names and floats."""
    fmt = '<f' if bits == 32 else '<d'
    names = {'zero': 0.0, 'one': 1.0, 'two': 2.0, 'three': 3.0, 'half': 0.5, 'ten': 10.0,
             'inf': float('inf'), 'nan': float('nan')}
    sign = 0
    if text.startswith('-') and not re.match(r'^-\d', text):
        sign, text = 1, text[1:]
    if text == 'snan':
        v = 0x7fa00000 if bits == 32 else 0x7ff4000000000000
    elif text == 'denormal':
        v = 0x00000001 if bits == 32 else 0x0000000000000001
    elif text == 'max':
        v = 0x7f7fffff if bits == 32 else 0x7fefffffffffffff
    elif text == 'min':
        v = 0x00800000 if bits == 32 else 0x0010000000000000
    elif text in names:
        v = int.from_bytes(struct.pack(fmt, names[text]), 'little')
        if text == 'nan':
            v = 0x7fc00000 if bits == 32 else 0x7ff8000000000000
    elif text.startswith('f:') or re.match(r'^-?\d+\.\d*$', text):
        v = int.from_bytes(struct.pack(fmt, float(text[2:] if text.startswith('f:') else text)), 'little')
    else:
        v = int(text, 16)
    return v | (sign << (bits - 1))


def parse_memory_spec(spec):
    """(size, value) of an `m=` value."""
    kind, _, text = spec.partition(':')
    if kind == 'f32':
        return 4, narrow(text, 32)
    if kind == 'f64':
        return 8, narrow(text, 64)
    if kind == 'x80':
        return 10, parse_x80(text)
    if kind in ('i16', 'i32', 'i64'):
        bits = int(kind[1:])
        return bits // 8, int(text, 0) & ((1 << bits) - 1)
    if kind == 'bcd':
        n = int(text, 0)
        digits = '%018d' % abs(n)
        value = int(digits, 16) | ((1 << 79) if n < 0 else 0)
        return 10, value
    if kind == 'raw':
        data = bytes.fromhex(text)
        return len(data), int.from_bytes(data, 'little')
    raise ValueError('bad m= spec %r' % spec)


def effective_address(text):
    """The absolute address [0x...] of a line."""
    m = re.search(r'\[\s*(0x[0-9a-fA-F]+)\s*\]', text)
    if not m:
        raise ValueError('the m= value needs an absolute memory operand: ' + text)
    return int(m.group(1), 16) & M32


def apply(state, annotation, text):
    """Set the x87 state (and the integer registers) of `state` from the annotation."""
    top = 0
    stack = 0
    tags = None
    regs = [0] * 8
    stack_values = {}
    fsw = 0
    state['mem'] = dict(state.get('mem', {}))
    for token in annotation.split():
        key, _, value = token.partition('=')
        if key == 'fcw':
            state['fcw'] = int(value, 16)
        elif key == 'fsw':
            fsw = int(value, 16)
        elif key == 'mxcsr':
            state['mxcsr'] = int(value, 16)
        elif key == 'tag':
            pass                    # the name of the group of the case, for the regexes of oracle_bugs.txt
        elif key == 'top':
            top = int(value)
        elif key == 'stack':
            stack = int(value)
        elif key == 'tags':
            tags = int(value, 16)
        elif re.match(r'^st[0-7]$', key):
            stack_values[int(key[2])] = parse_x80(value)
        elif re.match(r'^r[0-7]$', key):
            regs[int(key[1])] = parse_x80(value)
        elif re.match(r'^xmm[0-7]$', key):
            state.setdefault('xmm', [0] * 8)[int(key[3])] = int(value, 16)
        elif key == 'm' and '[' not in text:
            pass                    # the value of a memory operand the line doesn't have
        elif key == 'm':
            size, number = parse_memory_spec(value)
            address = effective_address(text)
            for i in range(size):
                state['mem'][(address + i) & M32] = (number >> (8 * i)) & 0xff
        elif key == 'mem':
            for run in value.split(','):
                a, _, data = run.partition(':')
                for i in range(len(data) // 2):
                    state['mem'][(int(a, 16) + i) & M32] = int(data[2 * i:2 * i + 2], 16)
        elif key in ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi', 'eflags'):
            state[key] = int(value, 16)
        else:
            raise ValueError('unknown @x87 token %r' % token)
    state['fsw'] = (fsw & ~0x3800) | (top << 11)
    state.setdefault('fcw', 0x37f)
    if tags is None:
        tags = sum(1 << ((top + i) & 7) for i in range(stack))
    state['ftw'] = tags
    for i, v in stack_values.items():
        regs[(top + i) & 7] = v
        state['ftw'] |= 1 << ((top + i) & 7)
    state['fpr'] = regs
