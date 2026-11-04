#!/usr/bin/env python3
"""
The QEMU oracle of the RISC-V semantics tests (see tests/riscv/semantics/TEST_PLAN.md): batches of
tests run on `qemu-system-riscv64|32 -machine virt` (QEMU 11) in a bare metal guest (guest/runner.c)
which runs each instruction in U-mode from the registers of a record, and writes a result record
per test. The results have the shape of the `dismount_cli run --arch rv64|rv32` output (rvstate.py).

One QEMU process runs a batch of thousands of tests (the start-up costs ~0.15 s). The records are
loaded into the guest memory with `-device loader`, the results are written to a host file by the
semihosting call SYS_WRITE at the end of the batch.

The CPU is `max` / `max32` (everything QEMU implements; the RVA23 profile is a subset: rva23u64 and
rva23s64 can be selected with --qemu-cpu) with `vlen=N`; rvv_ta_all_1s and rvv_ma_all_1s are false
(QEMU's default), so the tail and the masked-off elements stay undisturbed.
"""
import os
import struct
import subprocess
import tempfile
import concurrent.futures

import build_guest
import rvstate as S

SS_VA = 0x80200000
INPUT_PA = 0x90000000
OUTPUT_PA = 0xA0000000
MAGIC = 0x31545652
HEADER_SIZE = 256
QEMU = {64: '/opt/homebrew/bin/qemu-system-riscv64', 32: '/opt/homebrew/bin/qemu-system-riscv32'}
DEFAULT_CPU = {64: 'max', 32: 'max'}
MAX_CODE = 40


class OracleError(Exception):
    """The state or the instruction can't be run on this oracle."""


def available():
    return all(os.path.exists(p) for p in QEMU.values())


def pack_record(code, st, vlenb):
    if len(code) > MAX_CODE:
        raise OracleError('code too long')
    x = list(st['x'])
    x[0] = 0
    vcsr = (st['vxrm'] << 1) | st['vxsat']
    vtype = st['vtype'] if st['vector'] else 0
    head = struct.pack('<II48s9Q', 0, len(code), code, st['pc'], vtype, st['vl'] if st['vector'] else 0,
                       st['vstart'] if st['vector'] else 0, st['fcsr'], vcsr if st['vector'] else 0,
                       st['ssp'], 0, st['elp'] if st.get('cfi') else 0)
    body = struct.pack('<32Q32Q', *[v & S.M64 for v in x], *st['f'])
    vbytes = b''.join(v.to_bytes(vlenb, 'little') for v in st['v'])
    base, size = st['win']
    runs = []
    def offset(a):
        # the window, then the shadow stack page (cfi) at the offsets from the size of the window
        if base <= a < base + size:
            return a - base
        if st.get('cfi') and SS_VA <= a < SS_VA + 0x1000:
            return size + a - SS_VA
        raise OracleError('mem outside of the window: %x' % a)
    runs = []
    for a in sorted(st['mem']):
        off = offset(a)
        if runs and runs[-1][0] + len(runs[-1][1]) == off:
            runs[-1][1].append(st['mem'][a])
        else:
            runs.append((off, bytearray([st['mem'][a]])))
    mem = b''
    for off, data in runs:
        pad = (-len(data)) % 8
        mem += struct.pack('<II', off, len(data)) + bytes(data) + b'\0' * pad
    rec = bytearray(head + body + vbytes + mem)
    rec[0:4] = struct.pack('<I', len(rec))
    # (nmem is the last field of the head)
    struct.pack_into('<Q', rec, 8 + 48 + 7 * 8, len(runs))
    return bytes(rec)


def build_input(xlen, vlen, tests, out_path, cfi=False):
    vlenb = vlen // 8
    base, size = tests[0][1]['win']
    path = out_path.encode()
    header = struct.pack('<8I', MAGIC, len(tests), xlen, vlenb, base, size, 1 if cfi else 0, 0) + path + b'\0' * (224 - len(path))
    assert len(header) == HEADER_SIZE
    return header + b''.join(pack_record(c, s, vlenb) for c, s in tests)


def decode(xlen, vlen, tests, blob, name='qemu'):
    # (QEMU RV32 ignores a write of mstatush.MPELP, so the guest can't start with ELP set: those states are
    # errors, see oracle_bugs.txt)
    """The result records of a batch -> a list of ('ok', result) / ('err', message)."""
    vlenb = vlen // 8
    results = []
    pos = 0
    for code, st in tests:
        if pos + 8 > len(blob):
            results.append(('err', name + ': the batch ended before this test (the guest died?)'))
            continue
        total, nmem = struct.unpack_from('<II', blob, pos)
        mcause, mepc, mtval, vstart, vl, vtype, fcsr, vcsr, ssp, mstatus = struct.unpack_from('<10Q', blob, pos + 8)
        x = struct.unpack_from('<32Q', blob, pos + 88)
        f = struct.unpack_from('<32Q', blob, pos + 88 + 256)
        voff = pos + 88 + 512
        v = [int.from_bytes(blob[voff + i * vlenb:voff + (i + 1) * vlenb], 'little') for i in range(32)]
        moff = voff + 32 * vlenb + 8
        mem = {}
        base = st['win'][0]
        for k in range(nmem):
            e, = struct.unpack_from('<I', blob, moff + 4 * k)
            off = e & 0xFFFFFF
            mem[base + off if off < st['win'][1] else SS_VA + off - st['win'][1]] = e >> 24
        pos += total
        mask = S.mask(xlen)
        irq = 1 << (xlen - 1)
        res = S.default_state(xlen, vlen)
        res.update({'x': [a & mask for a in x], 'f': list(f), 'v': v, 'fcsr': fcsr & 0xFF,
                    'vl': vl & mask, 'vtype': vtype & mask, 'vstart': vstart & mask,
                    'vxrm': (vcsr >> 1) & 3, 'vxsat': vcsr & 1, 'ssp': ssp & mask,
                    'vector': st['vector'], 'mem': mem, 'pc': st['pc'],
                    'elp': (mstatus >> 41) & 1 if st.get('cfi') else 0})
        res['x'][0] = 0
        res['info'] = 'mcause=%x mepc=%x mtval=%x' % (mcause, mepc, mtval)
        pc, n = st['pc'], len(code)
        cause = mcause & mask
        if cause & irq:
            results.append(('err', name + ': interrupt %d (timeout of an endless loop?) at %x' % (cause & ~irq, mepc)))
            continue
        if cause == 8:
            if mepc == pc + n:
                res['exit'], res['pc'] = 'fall', mepc
            elif pc <= mepc < pc + n:
                res['exit'] = 'trap:ecall'
            else:
                results.append(('err', name + ': ecall at %x' % mepc))
                continue
        elif cause == 3:
            res['exit'] = 'trap:ebreak'
        elif cause == 2:
            if pc <= mepc < pc + n:
                res['exit'] = 'trap:illegal'
            else:
                res['exit'], res['pc'] = 'branch', mepc
        elif cause in (0, 1, 12):
            if cause == 0 and pc <= mepc < pc + n:
                res['exit'] = 'trap:fault'
            else:
                res['exit'], res['pc'] = 'branch', mepc
        elif cause in (4, 5, 6, 7, 13, 15):
            res['exit'] = 'trap:fault'
        elif cause == 18:
            res['exit'] = 'trap:swcheck'
        else:
            results.append(('err', name + ': unexpected exception %d at %x tval %x' % (cause, mepc, mtval)))
            continue
        if res['exit'] == 'fall' or res['exit'] == 'branch':
            res['x'] = [a & mask for a in x]
        results.append(('ok', res))
    return results


def run_batch(xlen, vlen, tests, cpu=None, extra=(), timeout=None, keep=False):
    cfi = bool(tests and tests[0][1].get('cfi'))
    """Run `tests` (a list of (code, state)) in one QEMU process. Returns decode()'s list."""
    if not tests:
        return []
    cpu = cpu or DEFAULT_CPU[xlen]
    guest = build_guest.build(xlen)
    with tempfile.TemporaryDirectory(prefix='rvqemu') as tmp:
        out_path = os.path.join(tmp, 'out.bin')
        in_path = os.path.join(tmp, 'in.bin')
        with open(in_path, 'wb') as f:
            f.write(build_input(xlen, vlen, tests, out_path, cfi))
        cmd = [QEMU[xlen], '-machine', 'virt', '-cpu', '%s,vlen=%d,rvv_vsetvl_x0_vill=true' % (cpu, vlen) if 'vlen' not in cpu else cpu,
               '-m', '1G', '-smp', '1', '-nographic', '-monitor', 'none', '-serial', 'none', '-bios', 'none',
               '-kernel', guest, '-device', 'loader,file=%s,addr=0x%x,force-raw=on' % (in_path, INPUT_PA),
               '-semihosting-config', 'enable=on,target=native'] + list(extra)
        if timeout is None:
            timeout = 60 + len(tests) * 0.3
        try:
            p = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=timeout)
        except subprocess.TimeoutExpired:
            return [('err', 'qemu: the batch timed out')] * len(tests)
        if not os.path.exists(out_path):
            tail = (p.stderr.decode(errors='replace').strip().splitlines() or [''])[-1][:200]
            return [('err', 'qemu: no output (rc=%d) %s' % (p.returncode, tail))] * len(tests)
        blob = open(out_path, 'rb').read()
        if keep:
            open(keep, 'wb').write(blob)
    return decode(xlen, vlen, tests, blob)


def run_isolating(runner, xlen, vlen, tests, cpu=None):
    """Run a batch; when the emulator dies (no output: an assertion of QEMU, a crash) split the batch
    in two and run the halves, down to the single test that kills it (that one is an ('err', ...))."""
    results = runner(xlen, vlen, tests, cpu)
    if len(tests) < 2 or not all(r[0] == 'err' and ': no output' in r[1] for r in results):
        return results
    mid = len(tests) // 2
    return run_isolating(runner, xlen, vlen, tests[:mid], cpu) + run_isolating(runner, xlen, vlen, tests[mid:], cpu)


def run_tests(xlen, vlen, tests, jobs=4, batch=2000, cpu=None, runner=None):
    """Run many tests: batches of `batch` tests on `jobs` QEMU processes. Order preserved. The tests of
    the states with 'cfi' set (Zicfilp / Zicfiss enabled in the guest) run in their own batches."""
    is_qemu = runner is None
    runner = runner or run_batch
    groups = [[i for i, t in enumerate(tests) if not t[1].get('cfi')], [i for i, t in enumerate(tests) if t[1].get('cfi')]]
    chunks = []
    for g in groups:
        for k in range(0, len(g), batch):
            chunks.append(g[k:k + batch])
    if jobs <= 1 or len(chunks) < 2:
        parts = [run_isolating(runner, xlen, vlen, [tests[i] for i in c], cpu) for c in chunks]
    else:
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            parts = list(pool.map(lambda c: run_isolating(runner, xlen, vlen, [tests[i] for i in c], cpu), chunks))
    results = [None] * len(tests)
    for c, part in zip(chunks, parts):
        for i, r in zip(c, part):
            results[i] = r
    if xlen == 32 and is_qemu:
        for i, (code, st) in enumerate(tests):
            if st.get('cfi') and st['elp']:
                results[i] = ('err', 'qemu rv32: a write of mstatush.MPELP is ignored, ELP can not start set')
    return results
