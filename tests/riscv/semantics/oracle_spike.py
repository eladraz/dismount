#!/usr/bin/env python3
"""
The Spike (riscv-isa-sim) oracle of the RISC-V semantics tests (see tests/riscv/semantics/TEST_PLAN.md):
the golden model of the RISC-V International, independent of QEMU. It runs the same bare metal guest
as oracle_qemu.py (guest/runner.c built with -DSPIKE: the input is the flat image loaded by
`--kernel` at 0x80200000 (RV64) / 0x80400000 (RV32), the results are written to a host file through
the HTIF syscall proxy, the machine stops by tohost) with the same records and the same decoding.

Spike is built from source into out/tools (tests/riscv/semantics/TEST_PLAN.md, "Building Spike"):
    git clone https://github.com/riscv-software-src/riscv-isa-sim out/tools/riscv-isa-sim
    git -C out/tools/riscv-isa-sim submodule update --init --depth 1
    mkdir out/tools/riscv-isa-sim/build && cd out/tools/riscv-isa-sim/build
    ../configure --prefix=$PWD/../../spike-install && make -j10
The path of the binary is $SPIKE or out/tools/riscv-isa-sim/build/spike.
"""
import concurrent.futures
import os
import subprocess
import tempfile

import build_guest
import oracle_qemu as Q
import rvstate as S

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
SPIKE = os.environ.get('SPIKE') or os.path.join(REPO, 'out', 'tools', 'riscv-isa-sim', 'build', 'spike')
EXTENSIONS = ('zba_zbb_zbc_zbs_zbkb_zbkc_zbkx_zknd_zkne_zknh_zksed_zksh_zfa_zfh_zfhmin_zfbfmin_zvfbfmin_zvfbfwma_'
              'zvfh_zicond_zcb_zcmop_zimop_zawrs_zacas_zabha_zicbom_zicboz_zicbop_zihintpause_zihintntl_zvbb_'
              'zvbc_zvkb_zvkg_zvkned_zvknha_zvknhb_zvksed_zvksh_zvkt_zama16b_zicclsm_zicsr_zifencei')
CFI = '_zicfilp_zicfiss'
OracleError = Q.OracleError


def available():
    return os.path.exists(SPIKE)


def isa_string(xlen, vlen=128):
    base = 'rv%dimafdcv' % xlen
    # (VLEN is the largest zvlNb of the ISA string)
    return base + '_' + EXTENSIONS + ('_zcf' if xlen == 32 else '') + '_zcd' + CFI + ('_zvl%db' % vlen if vlen > 128 else '')


def run_batch(xlen, vlen, tests, cpu=None, extra=(), timeout=None, keep=False):
    cfi = bool(tests and tests[0][1].get('cfi'))
    if not tests:
        return []
    if not available():
        return [('err', 'spike: not built (%s)' % SPIKE)] * len(tests)
    guest = build_guest.build(xlen, spike=True)
    with tempfile.TemporaryDirectory(prefix='rvspike') as tmp:
        out_path = os.path.join(tmp, 'out.bin')
        in_path = os.path.join(tmp, 'in.bin')
        with open(in_path, 'wb') as f:
            f.write(Q.build_input(xlen, vlen, tests, out_path, cfi))
        cmd = [SPIKE, '--isa=' + (cpu or isa_string(xlen, vlen)),
               '-m0x80000000:0x40000000', '--kernel=' + in_path] + list(extra) + [guest]
        if timeout is None:
            timeout = 60 + len(tests) * 0.3
        try:
            p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            return [('err', 'spike: the batch timed out')] * len(tests)
        if not os.path.exists(out_path):
            tail = (p.stderr.decode(errors='replace').strip().splitlines() or [''])[-1][:200]
            return [('err', 'spike: no output (rc=%d) %s' % (p.returncode, tail))] * len(tests)
        blob = open(out_path, 'rb').read()
        if keep:
            open(keep, 'wb').write(blob)
    results = Q.decode(xlen, vlen, tests, blob, 'spike')
    # Spike raises an illegal instruction for a nonzero vstart in the vector arithmetic instructions, which the
    # specification permits ("implementations may raise illegal-instruction when vstart is nonzero in
    # instructions that can't be interrupted"; QEMU runs them): not a difference to compare
    for i, ((code, st), (k, res)) in enumerate(zip(tests, results)):
        if (k == 'ok' and res['exit'] == 'trap:illegal' and st['vector'] and st['vstart'] and (code[0] & 0x7f) not in (0x07, 0x27)):
            results[i] = ('err', 'spike: illegal instruction for vstart != 0 (permitted by the specification)')
    return results


def run_tests(xlen, vlen, tests, jobs=4, batch=2000, cpu=None):
    return Q.run_tests(xlen, vlen, tests, jobs, batch, cpu, runner=run_batch)
