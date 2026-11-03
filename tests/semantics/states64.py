#!/usr/bin/env python3
"""
The random states of the 64 bit mode semantics tests (see "64 bit mode" in
tests/semantics/TEST_PLAN.md): full 64 bit register values, the addresses of
the memory operands in the range where dismount and Unicorn agree, and the
x87 / SSE state of states_v2.py with xmm0-15.

The memory of the two engines is unlimited (Unicorn maps pages on demand), so
a memory operand can point anywhere in 0..2^47: the base and index registers
of the explicit memory operand of the line, and the registers which the
instruction uses implicitly (the stack pointer of push / pop / call / ret, rsi
and rdi of the string instructions, rbx of xlat) are drawn from an address
pool, and the register of an explicit operand is then adjusted so that the
effective address is a drawn target (`solve_address`). Unicorn's address
space is 52 bits and it doesn't check the canonical form (oracle64.py), so an
address outside of 0..2^47 isn't compared.
"""
import random
import re
import zlib

import oracle
import oracle64
import states_v2
import run_compat as rc

M64 = 0xFFFFFFFFFFFFFFFF
M32 = 0xFFFFFFFF
REGS = oracle64.REGS64
EDGE64 = [0, 0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
          0xffffffff, 0x100000000, 0x7fffffffffffffff, 0x8000000000000000,
          0xffffffffffffffff, 0xffffffff80000000, 0xffffffffffff8000, 0xffffffffffffff80,
          0xfffffffffffffffe, 0x00000000ffffff80, 0x0000ffffffffffff, 0x8000000080000000]
FLAG_BITS = [0x1, 0x4, 0x10, 0x40, 0x80, 0x800, 0x400]     # CF PF AF ZF SF OF DF
REP_WORDS = {'rep', 'repe', 'repne', 'repz', 'repnz'}
# Where the code is: below 4GB, across the 4GB border, high
RIPS = [0x1000, 0x401000, 0x7ffe00001000, 0x7fff12340000, 0xfffffff0, 0x1000000000]
# The 32 bit register names of the 0x67 address size
REG32 = {}
for i, n in enumerate(oracle64.REGS64):
    if i < 8:
        REG32['e' + n[1:]] = n
    else:
        REG32[n + 'd'] = n
ADDRESS_REGS = set(oracle64.REGS64) | set(REG32)
STACK_MNEMONICS = {'push', 'pop', 'call', 'ret', 'retn', 'retf', 'enter', 'leave', 'pushf',
                   'popf', 'pushfq', 'popfq', 'int', 'int3', 'into', 'iret', 'iretq', 'iretd',
                   'iretw', 'push', 'pusha', 'popa'}
STRING_PREFIX = re.compile(r'^(movs|cmps|scas|stos|lods|ins|outs)[bwdq]?$')
NO_ACCESS = {'lea', 'nop', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'prefetchnta',
             'prefetchw', 'prefetch'}


def stable_seed(text):
    return zlib.crc32(('64:%s' % text).encode('utf-8'))


def draw_value(rng):
    r = rng.random()
    if r < 0.40:
        return rng.choice(EDGE64)
    if r < 0.65:
        return rng.getrandbits(64)
    if r < 0.80:
        return rng.getrandbits(32)
    if r < 0.88:
        return rng.getrandbits(16)
    if r < 0.94:
        return (rng.getrandbits(32) | 0xffffffff00000000)      # a sign extended 32 bit value
    return rng.getrandbits(8)


def address_pool(rng):
    """A target address for a memory operand: small, in the middle of the low half, across
    the 2GB and 4GB borders, and near the top of the low half (2^47)."""
    r = rng.random()
    if r < 0.25:
        return rng.randrange(0x1000, 0x100000)
    if r < 0.60:
        return rng.randrange(0x100000, 0x7fff00000000)
    if r < 0.72:
        return (1 << 32) + rng.randrange(-0x80, 0x80)
    if r < 0.82:
        return (1 << 31) + rng.randrange(-0x80, 0x80)
    return 0x7fffffff0000 + rng.randrange(0, 0x8000)


# ---------------------------------------------------------------------------
# The memory operand of a corpus line (nasm syntax)
# ---------------------------------------------------------------------------

def parse_memory_operand(text):
    """The first memory operand of a line as {'terms': [(sign, register, scale)], 'disp': int,
    'seg': 'fs' / 'gs' / None, 'rip': bool, 'addr32': bool}, or None. [rel $+N] is rip + N
    (nasm's $ is the start of the instruction); the other symbols aren't supported."""
    m = re.search(r'(?:\b(fs|gs|es|cs|ss|ds)\s*:\s*)?\[\s*(?:(fs|gs|es|cs|ss|ds)\s*:\s*)?([^\]]*)\]', text)
    if not m:
        return None
    seg = m.group(1) or m.group(2)
    inner = m.group(3).replace(' ', '').lower()
    inner = re.sub(r'^(rel|abs|nosplit|byte|word|dword|qword)+', '', inner)
    rip = False
    if inner.startswith('$'):
        rip = True
        inner = inner[1:]
        if not inner or inner[0] not in '+-':
            inner = '+0' + inner
    terms = []
    disp = 0
    addr32 = False
    for sign, term in re.findall(r'([+-]?)([^+-]+)', inner):
        sgn = -1 if sign == '-' else 1
        t = re.match(r'^([a-z0-9]+)(?:\*(\d))?$', term)
        t2 = re.match(r'^(\d)\*([a-z0-9]+)$', term)
        if t and t.group(1) in ADDRESS_REGS:
            reg, scale = t.group(1), int(t.group(2) or 1)
        elif t2 and t2.group(2) in ADDRESS_REGS:
            reg, scale = t2.group(2), int(t2.group(1))
        else:
            try:
                disp += sgn * int(term, 0)
            except ValueError:
                return None
            continue
        if reg in REG32:
            addr32 = True
        terms.append((sgn, reg, scale))
    return {'terms': terms, 'disp': disp, 'seg': seg if seg in ('fs', 'gs') else None,
            'rip': rip, 'addr32': addr32}


def effective_address(info, state):
    """The linear address of a memory operand, or None."""
    if info is None:
        return None
    total = info['disp']
    for sign, reg, scale in info['terms']:
        value = state[REG32.get(reg, reg)]
        if reg in REG32:
            value &= M32
        total += sign * value * scale
    if info['rip']:
        total += state['rip']
    total &= M32 if info['addr32'] else M64
    if info['seg']:
        total += state.get(info['seg'] + '_base', 0)
    return total & M64


def solve_address(info, state, target):
    """Set a register of the memory operand so that its address is `target` (an address
    before the segment base). Returns True if it worked."""
    if info is None or info['rip']:
        return False
    base_value = state.get(info['seg'] + '_base', 0) if info['seg'] else 0
    for sign, reg, scale in info['terms']:
        full = REG32.get(reg, reg)
        # the address without this term
        mine = sign * (state[full] & (M32 if reg in REG32 else M64)) * scale
        rest = effective_address(info, state) - base_value - mine
        # sign * value * scale == target - rest (mod 2^64 / 2^32)
        mask = M32 if info['addr32'] else M64
        need = (target - rest) & mask
        if sign < 0:
            need = (-need) & mask
        if scale != 1:
            if need % scale:
                continue
            need //= scale
        if sum(1 for t in info['terms'] if REG32.get(t[1], t[1]) == full) > 1:
            continue
        state[full] = need if reg not in REG32 else (need & M32)
        return True
    return False


# ---------------------------------------------------------------------------
# The states
# ---------------------------------------------------------------------------

def db_mnemonic(text):
    """The mnemonic of a `db 0x48, 0xa5` line (the lines with raw bytes): string and xlat
    opcodes give their name, the others 'db'."""
    values = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', text)]
    for v in values:
        if v in (0x66, 0x67, 0xf2, 0xf3, 0xf0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65) or 0x40 <= v <= 0x4f:
            continue
        names = {0xa4: 'movsb', 0xa5: 'movsd', 0xa6: 'cmpsb', 0xa7: 'cmpsd', 0xaa: 'stosb', 0xab: 'stosd',
                 0xac: 'lodsb', 0xad: 'lodsd', 0xae: 'scasb', 0xaf: 'scasd', 0xd7: 'xlatb',
                 0x6c: 'insb', 0x6d: 'insd', 0x6e: 'outsb', 0x6f: 'outsd'}
        return names.get(v, 'db')
    return 'db'


def implicit_registers(mnemonic, prefixes):
    """The registers that an instruction uses as the address of a memory operand
    without naming it."""
    regs = []
    if mnemonic in STACK_MNEMONICS:
        regs.append('rsp')
    if mnemonic in ('leave', 'enter'):
        regs.append('rbp')
    if STRING_PREFIX.match(mnemonic):
        regs += ['rsi', 'rdi']
    if mnemonic == 'xlat' or mnemonic == 'xlatb':
        regs.append('rbx')
    if mnemonic in ('maskmovdqu', 'maskmovq', 'monitor', 'monitorx'):
        regs += ['rdi', 'rax']
    if mnemonic in ('cmpxchg16b', 'cmpxchg8b'):
        pass
    return regs


def data_address(rng, rip):
    """address_pool() away from the page of the code: Unicorn doesn't complete an instruction
    which stores to the page of the code it executes (oracle64.py)."""
    for _ in range(20):
        address = address_pool(rng)
        if abs(address - rip) > 0x3000:
            return address
    return address


def separate_rip_relative(rng, info, state, length):
    """Move rip, inside of its 4K page, so that the rip relative operand (about 16 bytes) is on
    another page than the instruction; it can't be if the displacement is too small."""
    disp = info['disp']
    base = state['rip'] & ~0xfff
    offsets = list(range(0, 0x1000 - length))
    rng.shuffle(offsets)
    for off in offsets:
        rip = base | off
        address = (rip + disp) & M64
        if (address >> 12) != (rip >> 12) and ((address + 15) >> 12) != (rip >> 12) and \
                (address >> 12) != ((rip + length - 1) >> 12):
            state['rip'] = rip
            return True
    return False


def make_states(text, code, count, overrides):
    """The random states of a line (the integer part)."""
    rng = random.Random(stable_seed(text))
    prefixes, mnemonic, _ = rc.split_prefixes(text)
    rep_like = any(p.lower() in REP_WORDS for p in prefixes) or \
        mnemonic.startswith('loop') or mnemonic in ('jrcxz', 'jecxz') or \
        (mnemonic == 'db' and re.search(r'0x[fF][23]\b', text) is not None)
    info = None if mnemonic in NO_ACCESS else parse_memory_operand(text)
    implicit = implicit_registers(db_mnemonic(text) if mnemonic == 'db' else mnemonic, prefixes)
    uses_segment_base = re.search(r'\b(fs|gs)\s*:', text) is not None or \
        mnemonic in ('swapgs', 'rdfsbase', 'rdgsbase', 'wrfsbase', 'wrgsbase')
    states = []
    for _ in range(count):
        st = {n: draw_value(rng) for n in REGS}
        if rep_like or rng.random() < 0.5:
            st['rcx'] = rng.randint(0, 17)
            if not rep_like and rng.random() < 0.15:
                st['rcx'] = rng.choice([31, 32, 33, 63, 64, 65, 127, 128])      # shift / rotate counts
        fl = 0x202
        for b in FLAG_BITS:
            if rng.random() < 0.5:
                fl |= b
        st['eflags'] = fl
        st['rip'] = rng.choice(RIPS) if rng.random() < 0.5 else 0x1000
        if uses_segment_base:
            st['fs_base'] = address_pool(rng) if rng.random() < 0.8 else 0
            st['gs_base'] = address_pool(rng) if rng.random() < 0.8 else 0
            st['kernel_gs_base'] = address_pool(rng) if rng.random() < 0.8 else 0
        for reg in implicit:
            st[reg] = data_address(rng, st['rip'])
        st.update(overrides)
        if info is not None and info['terms']:
            solve_address(info, st, data_address(rng, st['rip']))
        elif info is not None and info['rip']:
            # rip relative: move the code if the address is out of the range, then
            # to another page than the operand
            address = effective_address(info, st)
            if not 0x1000 <= address < 0x7fff00000000:
                rip = (address_pool(rng) - info['disp']) & M64
                if 0x1000 <= rip < 0x7fff00000000:
                    st['rip'] = rip
            separate_rip_relative(rng, info, st, len(code))
        states.append(st)
    return states


def apply_fixed(state, spec):
    """A copy of `state` with the tokens of a corpus `@state:` comment applied (hex numbers,
    the names of the `dismount_cli run --bits 64` protocol)."""
    st = dict(state)
    for key in ('xmm', 'fpr', 'seg', 'mem'):
        if key in st:
            st[key] = type(st[key])(st[key])
    for tok in spec.split():
        k, _, v = tok.partition('=')
        if k in oracle.SEGS:
            st.setdefault('seg', {})[k] = int(v, 16)
        elif k.startswith('xmm'):
            st.setdefault('xmm', [0] * oracle64.XMM_COUNT)[int(k[3:])] = int(v, 16)
        elif k in ('r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7'):
            st.setdefault('fpr', [0] * 8)[int(k[1])] = int(v, 16)
        elif k == 'mem':
            st.setdefault('mem', {}).update(oracle64.parse_mem(v))
        else:
            st[k] = int(v, 16)
    return st


NAN_SELECTING = re.compile(r'^(add|sub|mul|div)(ps|pd|ss|sd) xmm(\d+), xmm(\d+)$')


def avoid_two_nans(text, state):
    """see states_v2.avoid_two_nans, for xmm0-15"""
    m = NAN_SELECTING.match(text.strip().lower())
    if not m or m.group(3) == m.group(4):
        return
    size = 32 if m.group(2) in ('ps', 'ss') else 64
    lanes = 1 if m.group(2) in ('ss', 'sd') else 128 // size
    first, second = int(m.group(3)), int(m.group(4))
    mask = (1 << size) - 1
    one = 0x3f800000 if size == 32 else 0x3ff0000000000000
    for lane in range(lanes):
        x = (state['xmm'][first] >> (size * lane)) & mask
        y = (state['xmm'][second] >> (size * lane)) & mask
        if states_v2.is_nan(x, size) and states_v2.is_nan(y, size):
            state['xmm'][second] = (state['xmm'][second] & ~(mask << (size * lane))) | (one << (size * lane))


def add_fp(text, code, states):
    """Add the x87 / SSE keys to the states of a line that uses them."""
    rng = random.Random(zlib.crc32(('v2:64:%s' % text).encode('utf-8')))
    mnemonic = text.split()[0] if text.split() else ''
    uses_fp = re.match(r'^(lock |rep[a-z]* )?f[a-z0-9]+$', mnemonic) or \
        re.search(r'\b(xmm\d+|mm[0-7]|mxcsr)\b', text) or mnemonic in ('emms', 'fxsave', 'fxrstor')
    if not uses_fp:
        return
    info = parse_memory_operand(text)

    def ea(_, st):
        return effective_address(info, st)

    def align(st, address):
        if info is None or info['rip'] or not info['terms']:
            return
        solve_address(info, st, (address + ((-address) & 0xf)) & M64)

    for st in states:
        fp = states_v2.fp_state(rng)
        fp['xmm'] = fp['xmm'] + [states_v2.sse_register(rng) for _ in range(8)]
        st.update(fp)
        if re.match(r'^f[a-z0-9]+$', mnemonic):
            states_v2.x87_adjust(rng, text, st, False, ea, M64, align)
        avoid_two_nans(text, st)
