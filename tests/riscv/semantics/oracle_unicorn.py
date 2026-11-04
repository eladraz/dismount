#!/usr/bin/env python3
"""
The Unicorn side of the RISC-V semantics tests (see tests/riscv/semantics/TEST_PLAN.md): the bytes
of one instruction (or a short straight-line sequence) run from a given state on UC_ARCH_RISCV
(Unicorn 2.1.4 = QEMU 5.0.1, the cpu model "any": RV32GC / RV64GC in M-mode; no V, no B, no Zfa,
Zicond, Zcb, Zimop... and the F/D of QEMU 5: see oracle_bugs.txt), with the result in the shape
of the `dismount_cli run --arch rv64|rv32` output (rvstate.py).

  * The code region is the 3 pages around the page of the pc, zero bytes (an illegal
    instruction) but the instruction; the data window is mapped read/write. An access to an
    unmapped address is a UcError (trap:fault), the exceptions of the core come through
    UC_HOOK_INTR (the QEMU exception number: 2 illegal, 3 ebreak, 8 ecall from U / 11 from M).
  * A state with vector state (v*, vl, vtype...) is an OracleError: there is no V.
  * mstatus.FS is set to dirty so the F/D instructions run.
"""
from unicorn import Uc, UcError, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_MODE_RISCV64, UC_HOOK_INTR, UC_HOOK_MEM_WRITE
from unicorn import unicorn_const as UC
from unicorn import riscv_const as R

import rvstate as S

PAGE = 0x1000
MAX_INSNS = 64


class OracleError(Exception):
    """The state or the instruction can't be run on this oracle."""


def xreg(i):
    return R.UC_RISCV_REG_X0 + i if hasattr(R, 'UC_RISCV_REG_X0') else None


class RiscvUnicorn(object):
    def __init__(self, xlen):
        self.xlen = xlen
        self.uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV64 if xlen == 64 else UC_MODE_RISCV32)
        self.uc.ctl_set_cpu_model(R.UC_CPU_RISCV64_ANY if xlen == 64 else R.UC_CPU_RISCV32_ANY) \
            if hasattr(self.uc, 'ctl_set_cpu_model') else None
        self.base = None
        self.code_page = None
        self.win = None
        self.trap = None
        self.writes = []
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        self.xregs = [R.UC_RISCV_REG_X0 + i for i in range(32)]
        self.fregs = [R.UC_RISCV_REG_F0 + i for i in range(32)]

    def _on_intr(self, uc, intno, user):
        self.trap = intno
        uc.emu_stop()

    def _on_write(self, uc, access, address, size, value, user):
        self.writes.append((address, size))

    def _map_window(self, win):
        base, size = win
        if self.win != win:
            if self.win is not None:
                self.uc.mem_unmap(*self.win)
            self.uc.mem_map(base, size, UC.UC_PROT_READ | UC.UC_PROT_WRITE)
            self.win = win
            self.pristine = bytes(S.pattern_byte(base + i) for i in range(size))
        self.uc.mem_write(base, self.pristine)

    def _map_code(self, pc):
        page = pc & ~(PAGE - 1)
        if self.code_page != page:
            if self.code_page is not None:
                self.uc.mem_unmap(self.code_page - PAGE, 3 * PAGE)
            self.uc.mem_map(page - PAGE, 3 * PAGE, UC.UC_PROT_ALL)
            self.code_page = page
        self.uc.mem_write(page - PAGE, bytes(3 * PAGE))

    def run(self, code, st):
        if st['vector']:
            raise OracleError('Unicorn has no vector extension')
        uc = self.uc
        xlen = self.xlen
        mask = S.mask(xlen)
        self._map_window(st['win'])
        self._map_code(st['pc'])
        for a, b in st['mem'].items():
            uc.mem_write(a, bytes([b]))
        uc.mem_write(st['pc'], code)
        for i in range(1, 32):
            uc.reg_write(self.xregs[i], st['x'][i] & mask)
        for i in range(32):
            uc.reg_write(self.fregs[i], st['f'][i])
        uc.reg_write(R.UC_RISCV_REG_MSTATUS, uc.reg_read(R.UC_RISCV_REG_MSTATUS) | 0x6000)
        uc.reg_write(R.UC_RISCV_REG_FCSR, st['fcsr'] & 0xFF)
        self.trap = None
        self.writes = []
        # the number of instructions of the line
        n, off = 0, 0
        while off < len(code):
            off += 2 if (code[off] & 3) != 3 else 4
            n += 1
        pc, end = st['pc'], st['pc'] + len(code)
        exit_ = None
        try:
            uc.emu_start(pc, end, 0, n)
        except UcError as e:
            if e.errno in (UC.UC_ERR_FETCH_UNMAPPED, UC.UC_ERR_FETCH_PROT, UC.UC_ERR_FETCH_UNALIGNED):
                exit_ = 'branch'                # a jump outside of the code region: the fetch faults
            elif e.errno in (UC.UC_ERR_READ_UNMAPPED, UC.UC_ERR_WRITE_UNMAPPED, UC.UC_ERR_READ_PROT,
                             UC.UC_ERR_WRITE_PROT, UC.UC_ERR_READ_UNALIGNED, UC.UC_ERR_WRITE_UNALIGNED):
                exit_ = 'trap:fault'
            elif e.errno in (UC.UC_ERR_INSN_INVALID, UC.UC_ERR_EXCEPTION):
                word = int.from_bytes(bytes(uc.mem_read(uc.reg_read(R.UC_RISCV_REG_PC), 4)), 'little')
                exit_ = 'trap:ebreak' if word in (0x00100073,) or (word & 0xFFFF) == 0x9002 else 'trap:illegal'
            else:
                raise OracleError('unicorn: %s' % e)
        res = S.default_state(xlen, st['vlen'])
        res['mem'] = {}
        newpc = uc.reg_read(R.UC_RISCV_REG_PC)
        if self.trap is not None:
            kind = {2: 'trap:illegal', 3: 'trap:ebreak', 8: 'trap:ecall', 11: 'trap:ecall', 9: 'trap:ecall'}
            if self.trap in kind:
                exit_ = kind[self.trap]
            elif self.trap in (0, 1, 4, 5, 6, 7, 12, 13, 15):
                exit_ = 'trap:fault'
            else:
                raise OracleError('unicorn: exception %d' % self.trap)
        if exit_ is None:
            exit_ = 'fall' if newpc == end else 'branch'
        res['exit'] = exit_
        res['x'] = [0] + [uc.reg_read(self.xregs[i]) & mask for i in range(1, 32)]
        res['f'] = [uc.reg_read(self.fregs[i]) for i in range(32)]
        res['fcsr'] = uc.reg_read(R.UC_RISCV_REG_FCSR) & 0xFF
        res['pc'] = st['pc'] if exit_.startswith('trap:') else newpc & mask
        base, size = st['win']
        for a, sz in self.writes:
            if base <= a < base + size:
                for k in range(sz):
                    if base <= a + k < base + size:
                        res['mem'][a + k] = uc.mem_read(a + k, 1)[0]
        return res

    def close(self):
        pass


_oracles = {}


def get_oracle(xlen):
    if xlen not in _oracles:
        _oracles[xlen] = RiscvUnicorn(xlen)
    return _oracles[xlen]


def run_tests(xlen, vlen, tests, jobs=1, batch=0, cpu=None):
    """Same interface as oracle_qemu.run_tests: [(code, state)] -> [('ok', result) | ('err', message)]"""
    o = get_oracle(xlen)
    out = []
    for code, st in tests:
        try:
            out.append(('ok', o.run(code, st)))
        except OracleError as e:
            out.append(('err', str(e)))
        except Exception as e:
            out.append(('err', 'unicorn: %s: %s' % (type(e).__name__, e)))
    return out
