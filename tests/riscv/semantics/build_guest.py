#!/usr/bin/env python3
"""
Builds the bare metal guest runner of the QEMU oracle (guest/runner.c, guest/rt.S, guest/link.ld)
for RV64 and RV32 with Homebrew clang + ld.lld (`brew install llvm lld`). The ELF files are written
to out/riscv_guest/guest{64,32}.elf and rebuilt when a source is newer.

  python3 tests/riscv/semantics/build_guest.py [--force]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
GUEST = os.path.join(HERE, 'guest')
OUT = os.path.join(REPO, 'out', 'riscv_guest')
LLVM = '/opt/homebrew/opt/llvm/bin'
LLD = '/opt/homebrew/opt/lld/bin/ld.lld'

CONFIG = {
    64: ('riscv64', 'rv64imac_zicsr_zifencei', 'rv64gcv_zicsr_zifencei', 'lp64'),
    32: ('riscv32', 'rv32imac_zicsr_zifencei', 'rv32gcv_zicsr_zifencei', 'ilp32'),
}


def guest_path(xlen, spike=False):
    return os.path.join(OUT, 'guest%d%s.elf' % (xlen, '_spike' if spike else ''))


def build(xlen, force=False, spike=False):
    target, march_c, march_s, abi = CONFIG[xlen]
    elf = guest_path(xlen, spike)
    sources = [os.path.join(GUEST, n) for n in ('runner.c', 'rt.S', 'link.ld')] + [os.path.abspath(__file__)]
    if not force and os.path.exists(elf) and os.path.getmtime(elf) >= max(os.path.getmtime(s) for s in sources):
        return elf
    os.makedirs(OUT, exist_ok=True)
    common = [os.path.join(LLVM, 'clang'), '--target=' + target, '-mabi=' + abi, '-mcmodel=medany',
              '-mno-relax', '-fno-pic', '-ffreestanding', '-nostdlib']
    objs = []
    for src, march, extra in (('runner.c', march_c, ['-O2']), ('rt.S', march_s, [])):
        obj = os.path.join(OUT, '%s%d%s.o' % (src.split('.')[0], xlen, '_spike' if spike else ''))
        defs = ['-DSPIKE'] if spike else []
        subprocess.run(common + defs + ['-march=' + march] + extra + ['-c', os.path.join(GUEST, src), '-o', obj], check=True)
        objs.append(obj)
    subprocess.run([LLD, '-T', os.path.join(GUEST, 'link.ld'), '-o', elf] + objs, check=True)
    return elf


if __name__ == '__main__':
    force = '--force' in sys.argv
    for x in (64, 32):
        print(build(x, force))
        print(build(x, force, spike=True))
