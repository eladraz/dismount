#!/usr/bin/env python3
"""
The Unicorn side of the 64 bit mode semantics tests (see "64 bit mode" in
tests/semantics/TEST_PLAN.md): ONE instruction run from a given state on
UC_MODE_64 (QEMU's x86-64 CPU), with the result in the shape of the
`dismount_cli run --bits 64` output. oracle.py dispatches to it by the keys of
the state ('rax' present), see oracle.get_oracle(64).

Behaviour of Unicorn 2.1.4 in 64 bit mode that matters here (verified):
  * The engine starts in long mode with every selector 0 and flat addressing.
    The fs and gs bases are UC_X86_REG_FS_BASE / GS_BASE, the other gs base
    (swapgs) is the MSR 0xC0000102 (msr_read / msr_write).
  * Memory is mapped lazily from UC_HOOK_MEM_UNMAPPED, as in oracle.py. The
    addresses are 52 bits: Unicorn reports 0xffff800000000000 as
    0x000f800000000000, an access at 0xfffffffffffff000 can't be mapped and
    there is no canonical address check (a non canonical address is used as
    it is, modulo 2^52). The states of the harness (states64.py) keep the
    addresses of the memory operands in 0..2^47, where the two engines agree.
  * `syscall` doesn't raise an interrupt: it needs a UC_HOOK_INSN hook, which
    reports it as trap:SYSCALL (0x100, the dismount_cli number of a system
    call).
  * The REX prefixes are decoded by the helpers of oracle.py after
    split_rex() removes them (the register numbers need the REX bits).
"""
import os
import sys

from unicorn import (Uc, UcError, UC_ARCH_X86, UC_MODE_64, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_WRITE, UC_HOOK_INTR, UC_HOOK_INSN, UC_ERR_INSN_INVALID)
from unicorn import x86_const as X

import oracle
from oracle import (OracleError, EFLAGS_MASK, PAGE, PREFIX_BYTES, SEGS, UC_SEG,
                    DEFAULT_FCW, DEFAULT_MXCSR, pattern_byte, pattern_page)

M64 = 0xFFFFFFFFFFFFFFFF
M32 = 0xFFFFFFFF
REGS64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
          'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
UC_REG64 = {n: getattr(X, 'UC_X86_REG_' + n.upper()) for n in REGS64}
KERNEL_GS_BASE_MSR = 0xC0000102
# The trap number of `syscall` in the protocol (not an interrupt vector)
TRAP_SYSCALL = 0x100
XMM_COUNT = 16
BASES = ['es_base', 'cs_base', 'ss_base', 'ds_base', 'fs_base', 'gs_base']


# ---------------------------------------------------------------------------
# Decoding helpers (REX, the ModRM memory operand of 64 bit mode)
# ---------------------------------------------------------------------------

def split_rex(code):
    """(the code without its REX prefix, the REX byte or 0)."""
    i = 0
    while i < len(code) and code[i] in PREFIX_BYTES:
        i += 1
    if i < len(code) and 0x40 <= code[i] <= 0x4F:
        return code[:i] + code[i + 1:], code[i]
    return code, 0


def memory_operand_address(code, rex, state, modrm_at, length):
    """The linear address of the ModRM memory operand at code[modrm_at] (`code`
    has no REX prefix, `length` is the length of the whole instruction, the
    rip relative addresses are relative to its end), or None for a register
    operand."""
    modrm = code[modrm_at]
    mod, rm = modrm >> 6, modrm & 7
    if mod == 3:
        return None
    count = 0
    while code[count] in PREFIX_BYTES:
        count += 1
    prefixes = code[:count]
    mask = M32 if 0x67 in prefixes else M64
    pos = modrm_at + 1
    total = 0
    disp_size = {0: 0, 1: 1, 2: 4}[mod]
    if rm == 4:
        sib = code[pos]
        pos += 1
        scale, index, base = 1 << (sib >> 6), ((sib >> 3) & 7) | ((rex & 2) << 2), sib & 7
        if index != 4:
            total += state[REGS64[index]] * scale
        if base == 5 and mod == 0:
            disp_size = 4
        else:
            total += state[REGS64[base | ((rex & 1) << 3)]]
    elif mod == 0 and rm == 5:
        disp_size = 4
        total += state['rip'] + length
    else:
        total += state[REGS64[rm | ((rex & 1) << 3)]]
    disp = int.from_bytes(code[pos:pos + disp_size], 'little', signed=True) if disp_size else 0
    address = (total + disp) & mask
    for b in prefixes:
        if b == 0x64:
            address += state.get('fs_base', 0)
        elif b == 0x65:
            address += state.get('gs_base', 0)
    return address & M64


def is_cmpxchg16b(code, rex):
    """True for cmpxchg16b (REX.W 0F C7 /1 with a memory operand): #GP unless the operand is
    16 byte aligned (Unicorn raises it)."""
    d = oracle._decode_simd(code)
    if d is None or d[1] != '0f' or d[2] != 0xC7 or d[3] >= len(code) or not rex & 8:
        return False
    return (code[d[3]] >> 3) & 7 == 1 and code[d[3]] >> 6 != 3


def simd_fault(code, state, v2):
    """True if the SDM raises #GP(0) for the instruction in this state and QEMU 5
    doesn't: a misaligned 128 bit operand, or ldmxcsr with a reserved bit."""
    stripped, rex = split_rex(code)
    d = oracle._decode_simd(stripped)
    if d is None or d[3] >= len(stripped):
        return False
    pre, mapid, op, at = d
    if oracle.needs_alignment(stripped):
        address = memory_operand_address(stripped, rex, state, at, len(code))
        return address is not None and address % 16 != 0
    if mapid == '0f' and op == 0xAE and (stripped[at] >> 3) & 7 == 2:
        address = memory_operand_address(stripped, rex, state, at, len(code))
        if address is None:
            return False
        value = 0
        for i in range(4):
            a = (address + i) & M64
            value |= v2['mem'].get(a, pattern_byte(a)) << (8 * i)
        return value & 0xFFFF0000 != 0
    return False


def align_memory_operand(code, state, rng):
    """In most of the states of an instruction that needs an aligned memory
    operand, move a register of its address so that the operand is aligned
    (the others keep the random address and test the #GP)."""
    stripped, rex = split_rex(code)
    d = oracle._decode_simd(stripped)
    if d is None or d[3] >= len(stripped) or \
            not (oracle.needs_alignment(stripped) or is_cmpxchg16b(stripped, rex)):
        return
    pre, _, _, at = d
    if 0x67 in pre or rng.random() < 0.2:
        return
    address = memory_operand_address(stripped, rex, state, at, len(code))
    if address is None:
        return
    modrm = stripped[at]
    mod, rm = modrm >> 6, modrm & 7
    shift = (-address) & 15
    if mod == 0 and rm == 5:
        # rip relative: the code moves
        state['rip'] = (state['rip'] + shift) & M64
    elif rm == 4:
        sib = stripped[at + 1]
        base = sib & 7
        index = ((sib >> 3) & 7) | ((rex & 2) << 2)
        if base == 5 and mod == 0:
            return
        reg = REGS64[base | ((rex & 1) << 3)]
        if index != 4 and REGS64[index] == reg:
            return
        state[reg] = (state[reg] + shift) & M64
    else:
        reg = REGS64[rm | ((rex & 1) << 3)]
        state[reg] = (state[reg] + shift) & M64


# ---------------------------------------------------------------------------
# The oracle
# ---------------------------------------------------------------------------

class Oracle64(object):
    def __init__(self):
        self.uc = None
        self.pages = []
        self.writes = []
        self.trap = None

    def _new_engine(self):
        # A fresh engine for every instruction, see oracle.Oracle
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_64)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        uc.hook_add(UC_HOOK_INTR, self._intr)
        uc.hook_add(UC_HOOK_INSN, self._syscall, None, 1, 0, X.UC_X86_INS_SYSCALL)
        return uc

    def _map(self, base):
        self.uc.mem_map(base, PAGE)
        self.uc.mem_write(base, pattern_page(base))
        self.pages.append(base)

    def _unmapped(self, uc, access, addr, size, value, user):
        try:
            self._map(addr & ~(PAGE - 1))
        except UcError:
            return False
        return True

    def _write(self, uc, access, addr, size, value, user):
        self.writes.append((addr, size))

    def _intr(self, uc, intno, user):
        self.trap = intno
        uc.emu_stop()

    def _syscall(self, uc, user):
        self.trap = TRAP_SYSCALL
        uc.emu_stop()

    def run(self, code, state):
        """code: bytes. state: dict with rax..r15, rip, eflags, fs_base, gs_base,
        kernel_gs_base and the v2 keys. Returns {'exit', 'regs', 'rip', 'eflags',
        'mem', ...}. Raises OracleError when the state can't be compared."""
        uc = self._new_engine()
        rip = state['rip'] & M64
        self.pages = []
        self.writes = []
        self.trap = None
        v2 = oracle.state_defaults(state)
        v2['xmm'] = list(state.get('xmm', [0] * XMM_COUNT))
        if simd_fault(code, state, v2):
            # #GP(0) (QEMU 5 doesn't raise it): the instruction doesn't execute
            res = self.run(b'\x90', state)
            res['exit'] = 'trap:13'
            return res
        try:
            for base in sorted({((rip + i) & M64) & ~(PAGE - 1) for i in range(len(code) + 1)}):
                self._map(base)
            for a in v2['mem']:
                if (a & ~(PAGE - 1)) not in self.pages:
                    self._map(a & ~(PAGE - 1))
            for a, b in v2['mem'].items():
                uc.mem_write(a, bytes([b]))
            for i, b in enumerate(code):
                uc.mem_write((rip + i) & M64, bytes([b]))
            for n in REGS64:
                uc.reg_write(UC_REG64[n], state[n] & M64)
            uc.reg_write(X.UC_X86_REG_EFLAGS, state['eflags'] & M32)
            uc.reg_write(X.UC_X86_REG_RIP, rip)
            uc.reg_write(X.UC_X86_REG_FS_BASE, state.get('fs_base', 0))
            uc.reg_write(X.UC_X86_REG_GS_BASE, state.get('gs_base', 0))
            uc.msr_write(KERNEL_GS_BASE_MSR, state.get('kernel_gs_base', 0))
            self.writes = []
            uc.reg_write(X.UC_X86_REG_FPCW, v2['fcw'])
            uc.reg_write(X.UC_X86_REG_FPSW, v2['fsw'])
            full_tag = 0
            for i in range(8):
                if not (v2['ftw'] >> i) & 1:
                    full_tag |= 3 << (2 * i)
            uc.reg_write(X.UC_X86_REG_FPTAG, full_tag)
            for i in range(8):
                uc.reg_write(X.UC_X86_REG_FP0 + i, (v2['fpr'][i] & (2 ** 64 - 1), v2['fpr'][i] >> 64))
            uc.reg_write(X.UC_X86_REG_MXCSR, v2['mxcsr'])
            for i in range(XMM_COUNT):
                uc.reg_write(X.UC_X86_REG_XMM0 + i, v2['xmm'][i])

            rep = False
            for b in code[:14]:
                if b not in PREFIX_BYTES:
                    break
                rep = rep or b in (0xF2, 0xF3)
            invalid = False
            steps = 0
            while True:
                steps += 1
                if steps > oracle.MAX_STEPS:
                    raise OracleError('rep iteration limit')
                try:
                    uc.emu_start(rip, 0, count=1)
                except UcError as e:
                    if e.errno == UC_ERR_INSN_INVALID:
                        invalid = True
                        break
                    raise OracleError('unicorn: %s' % e)
                if self.trap is not None:
                    break
                if uc.reg_read(X.UC_X86_REG_RIP) != rip:
                    break
                code_page = rip & ~(PAGE - 1)
                touched = any(((a + i) & M64) & ~(PAGE - 1) == code_page
                              for a, s in self.writes for i in range(s))
                if not (rep or touched):
                    break
                if touched and not rep and steps > 16:
                    # QEMU re-translates the code page after a store to it and restarts
                    # the instruction, which stores again: no progress
                    raise OracleError('store to the page of the code: Unicorn does not complete it')

            code_range = {(rip + i) & M64 for i in range(len(code))}
            addrs = set()
            for a, s in self.writes:
                for i in range(s):
                    addrs.add((a + i) & M64)
            if addrs & code_range:
                raise OracleError('self-modifying instruction')
            mem = {}
            for a in addrs:
                mem[a] = uc.mem_read(a, 1)[0]

            if invalid:
                exit_ = 'trap:6'
            elif self.trap is not None:
                exit_ = 'trap:%d' % self.trap
            else:
                new_rip = uc.reg_read(X.UC_X86_REG_RIP)
                exit_ = 'fall' if new_rip == (rip + len(code)) & M64 else 'branch'
            res = {
                'exit': exit_,
                'regs': {n: uc.reg_read(UC_REG64[n]) for n in REGS64},
                'rip': uc.reg_read(X.UC_X86_REG_RIP),
                'eflags': (uc.reg_read(X.UC_X86_REG_EFLAGS) & EFLAGS_MASK) | 2,
                'mem': mem,
            }
            res['seg'] = {n: uc.reg_read(UC_SEG[n]) for n in SEGS}
            res['fs_base'] = uc.reg_read(X.UC_X86_REG_FS_BASE)
            res['gs_base'] = uc.reg_read(X.UC_X86_REG_GS_BASE)
            res['kernel_gs_base'] = uc.msr_read(KERNEL_GS_BASE_MSR)
            res['fcw'] = uc.reg_read(X.UC_X86_REG_FPCW)
            res['fsw'] = uc.reg_read(X.UC_X86_REG_FPSW)
            tag = uc.reg_read(X.UC_X86_REG_FPTAG)
            res['ftw'] = sum(1 << i for i in range(8) if (tag >> (2 * i)) & 3 != 3)
            fpr = []
            for i in range(8):
                mant, exp = uc.reg_read(X.UC_X86_REG_FP0 + i)
                fpr.append((exp << 64) | mant)
            stripped, rex = split_rex(code)
            mm = oracle.mmx_written_register(stripped) if exit_ in ('fall', 'branch') else None
            if mm is not None:
                fpr[mm] = (fpr[mm] & (2 ** 64 - 1)) | (0xFFFF << 64)
            res['fpr'] = fpr
            res['mxcsr'] = uc.reg_read(X.UC_X86_REG_MXCSR)
            res['xmm'] = [uc.reg_read(X.UC_X86_REG_XMM0 + i) for i in range(XMM_COUNT)]
            if exit_ == 'fall':
                flushed = oracle.flush_to_zero_lanes(stripped, v2['mxcsr'], res['xmm'], rex)
                if flushed is not None:
                    res['xmm'][flushed[0]] = flushed[1]
            return res
        finally:
            self.uc = None


# ---------------------------------------------------------------------------
# The dismount_cli run --bits 64 line format
# ---------------------------------------------------------------------------

def format_mem(mem):
    """Sorted bytes merged into runs: 0000000000001000:aabbcc,... or '-'."""
    if not mem:
        return '-'
    runs = []
    for a in sorted(mem):
        if runs and runs[-1][0] + len(runs[-1][1]) == a:
            runs[-1][1].append(mem[a])
        else:
            runs.append((a, bytearray([mem[a]])))
    return ','.join('%016x:%s' % (a, bytes(b).hex()) for a, b in runs)


def parse_mem(text):
    mem = {}
    if text in ('-', ''):
        return mem
    for run in text.split(','):
        a, _, data = run.partition(':')
        base = int(a, 16)
        for i in range(len(data) // 2):
            mem[(base + i) & M64] = int(data[2 * i:2 * i + 2], 16)
    return mem


def format_state(code, state):
    """One input line of `dismount_cli run --bits 64`. The v2 tokens are written
    only when they differ from their defaults."""
    out = '%s ; %s rip=%x eflags=%x' % (
        code.hex(), ' '.join('%s=%x' % (n, state[n] & M64) for n in REGS64),
        state['rip'] & M64, state['eflags'] & M32)
    for n in ('fs_base', 'gs_base', 'kernel_gs_base'):
        if state.get(n, 0):
            out += ' %s=%x' % (n, state[n])
    for n in SEGS:
        if state.get('seg', {}).get(n, 0):
            out += ' %s=%x' % (n, state['seg'][n])
    if state.get('fcw', DEFAULT_FCW) != DEFAULT_FCW:
        out += ' fcw=%x' % state['fcw']
    if state.get('fsw', 0):
        out += ' fsw=%x' % state['fsw']
    if state.get('ftw', 0):
        out += ' ftw=%x' % state['ftw']
    for i, v in enumerate(state.get('fpr', [])):
        if v:
            out += ' r%d=%020x' % (i, v)
    if state.get('mxcsr', DEFAULT_MXCSR) != DEFAULT_MXCSR:
        out += ' mxcsr=%x' % state['mxcsr']
    for i, v in enumerate(state.get('xmm', [])):
        if v:
            out += ' xmm%d=%032x' % (i, v)
    if state.get('mem'):
        out += ' mem=' + format_mem(state['mem'])
    return out


def parse_state(line):
    hexs, _, rest = line.partition(';')
    st = {}
    for tok in rest.split():
        k, _, v = tok.partition('=')
        if k in SEGS:
            st.setdefault('seg', {})[k] = int(v, 16)
        elif k.startswith('xmm'):
            st.setdefault('xmm', [0] * XMM_COUNT)[int(k[3:])] = int(v, 16)
        elif k in ('r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7'):
            st.setdefault('fpr', [0] * 8)[int(k[1])] = int(v, 16)
        elif k == 'mem':
            st['mem'] = parse_mem(v)
        else:
            st[k] = int(v, 16)
    return bytes.fromhex(hexs.strip()), st


def format_result(res, undef=0, fundef=0):
    s = 'OK exit=%s undef=%08x fundef=%04x %s rip=%016x eflags=%08x' % (
        res['exit'], undef, fundef, ' '.join('%s=%016x' % (n, res['regs'][n]) for n in REGS64),
        res['rip'], res['eflags'])
    s += ' ' + ' '.join('%s=%04x' % (n, res['seg'][n]) for n in SEGS)
    # The bases of the other segments are 0 in 64 bit mode
    s += ' es_base=%016x cs_base=%016x ss_base=%016x ds_base=%016x' % (0, 0, 0, 0)
    s += ' fs_base=%016x gs_base=%016x kernel_gs_base=%016x' % (
        res['fs_base'], res['gs_base'], res['kernel_gs_base'])
    s += ' fcw=%04x fsw=%04x ftw=%02x' % (res['fcw'], res['fsw'], res['ftw'])
    s += ' ' + ' '.join('r%d=%020x' % (i, v) for i, v in enumerate(res['fpr']))
    s += ' mxcsr=%08x' % res['mxcsr']
    s += ' ' + ' '.join('xmm%d=%032x' % (i, v) for i, v in enumerate(res['xmm']))
    return s + ' mem=%s' % format_mem(res['mem'])


def parse_result(line):
    """Parses an `OK ...` / `ERR ...` line of `dismount_cli run --bits 64`."""
    if not line.startswith('OK '):
        return {'err': line[4:].strip() if line.startswith('ERR ') else line.strip()}
    kv = {}
    for tok in line[3:].split():
        k, _, v = tok.partition('=')
        kv[k] = v
    try:
        undef = kv.get('undef', '0')
        undef = 0 if undef in ('-', '') else int(undef, 16)
        return {
            'exit': kv['exit'],
            'undef': undef,
            'fundef': int(kv.get('fundef', '0'), 16),
            'regs': {n: int(kv[n], 16) for n in REGS64},
            'rip': int(kv['rip'], 16),
            'eflags': int(kv['eflags'], 16),
            'seg': {n: int(kv[n], 16) for n in SEGS},
            'base': {n: int(kv[n + '_base'], 16) for n in SEGS},
            'fs_base': int(kv['fs_base'], 16),
            'gs_base': int(kv['gs_base'], 16),
            'kernel_gs_base': int(kv['kernel_gs_base'], 16),
            'fcw': int(kv['fcw'], 16),
            'fsw': int(kv['fsw'], 16),
            'ftw': int(kv['ftw'], 16),
            'fpr': [int(kv['r%d' % i], 16) for i in range(8)],
            'mxcsr': int(kv['mxcsr'], 16),
            'xmm': [int(kv['xmm%d' % i], 16) for i in range(XMM_COUNT)],
            'mem': parse_mem(kv.get('mem', '-')),
        }
    except (KeyError, ValueError) as e:
        return {'err': 'malformed output (%s): %s' % (e, line[:120])}


if __name__ == '__main__':
    # Stand-alone: oracle64.py < input-lines > output-lines (the protocol of
    # `dismount_cli run --bits 64`)
    ora = Oracle64()
    for ln in sys.stdin:
        if ln.strip():
            try:
                code, st = parse_state(ln)
                sys.stdout.write(format_result(ora.run(code, st)) + '\n')
            except OracleError as e:
                sys.stdout.write('ERR oracle: %s\n' % e)
    sys.stdout.flush()
