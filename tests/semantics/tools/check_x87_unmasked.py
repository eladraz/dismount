#!/usr/bin/env python3
"""
Directed checks of the x87 exceptions that Unicorn and Rosetta 2 don't model: the unmasked exceptions
(ES and B, the faults that leave the registers alone, the traps that store the result) and the #MF of a
pending exception. The expected values are those of the Intel SDM (Vol. 1 8.4, 8.5, Vol. 2 of each
instruction); the states and the results go through `dismount_cli run`.

  out/venv/bin/python tests/semantics/tools/check_x87_unmasked.py [--cli PATH]

Exit status 1 if a check fails.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'ia32'))

import oracle                                              # noqa: E402
import x87states                                           # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
ADDRESS = 0x2000

IE, DE, ZE, OE, UE, PE, SF, ES, C1, B = 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x200, 0x8000
FCW_ALL_MASKED = 0x37f


def value(name):
    return x87states.parse_x80(name)


CODES = {
    'fadd st0, st1': 'd8c1', 'faddp st1, st0': 'dec1', 'fdiv st0, st1': 'd8f1', 'fdivp st1, st0': 'def9',
    'fmul st0, st1': 'd8c9', 'fld dword [0x2000]': 'd90500200000', 'fstp dword [0x2000]': 'd91d00200000',
    'fistp word [0x2000]': 'df1d00200000', 'fcomi st0, st1': 'dbf1', 'fucomip st0, st1': 'dfe9',
    'fwait': '9b', 'fnop': 'd9d0', 'fnstsw ax': 'dfe0', 'fld1': 'd9e8', 'fsqrt': 'd9fa', 'fxtract': 'd9f4',
    'fscale': 'd9fd', 'fldcw [0x2000]': 'd92d00200000', 'fchs': 'd9e0', 'fcom st1': 'd8d1',
    'fbstp [0x2000]': 'df3500200000', 'fnclex': 'dbe2', 'fxch st1': 'd9c9', 'frndint': 'd9fc',
}


def state(fcw=FCW_ALL_MASKED, fsw=0, top=0, stack=0, regs=None, memory=None, **integers):
    st = {n: 0 for n in oracle.REGS}
    st.update(esp=0x8000, eip=0x1000, eflags=0x202)
    st.update(integers)
    st['fcw'] = fcw
    st['fsw'] = (fsw & ~0x3800) | (top << 11)
    st['ftw'] = sum(1 << ((top + i) & 7) for i in range(stack))
    st['fpr'] = [0] * 8
    for i, v in (regs or {}).items():
        st['fpr'][(top + i) & 7] = value(v) if isinstance(v, str) else v
    st['mem'] = {}
    for offset, data in (memory or {}).items():
        for i, b in enumerate(data):
            st['mem'][ADDRESS + offset + i] = b
    return st


def run(cli, name, st):
    code = bytes.fromhex(CODES[name])
    line = oracle.format_state(code, st)
    out = subprocess.run([cli, 'run', '--bits', '32'], input=line + '\n', capture_output=True, text=True).stdout
    res = oracle.parse_result(out.strip().split('\n')[0])
    if 'err' in res:
        raise RuntimeError('%s: %s' % (name, res['err']))
    return res


failures = []
count = 0


def expect(label, condition, detail=''):
    global count
    count += 1
    if not condition:
        failures.append('%s %s' % (label, detail))


def top_register(res, st, index=0):
    return res['fpr'][(((st['fsw'] >> 11) & 7) + index) & 7]


def check(cli):
    # --- faults: IE, DE, ZE unmasked leave the stack and the registers alone, set the flag, ES and B
    cases = [
        ('invalid', 'fadd st0, st1', 0x37e, IE, dict(regs={0: 'snan', 1: 'one'}, stack=2)),
        ('denormal', 'faddp st1, st0', 0x37d, DE, dict(regs={0: 'denormal', 1: 'one'}, stack=2)),
        ('zero divide', 'fdiv st0, st1', 0x37b, ZE, dict(regs={0: 'one', 1: 'zero'}, stack=2)),
        ('sqrt of a negative', 'fsqrt', 0x37e, IE, dict(regs={0: '-one'}, stack=1)),
        ('fscale snan', 'fscale', 0x37e, IE, dict(regs={0: 'one', 1: 'snan'}, stack=2)),
        ('fxtract zero', 'fxtract', 0x37b, ZE, dict(regs={0: 'zero'}, stack=1, top=3)),
        ('frndint snan', 'frndint', 0x37e, IE, dict(regs={0: 'snan'}, stack=1)),
        ('fcom nan', 'fcom st1', 0x37e, IE, dict(regs={0: 'nan', 1: 'one'}, stack=2, fsw=0x4700)),
    ]
    for label, name, fcw, flag, kw in cases:
        st = state(fcw=fcw, **kw)
        res = run(cli, name, st)
        expect(label + ': flag, ES, B', res['fsw'] & (flag | ES | B) == flag | ES | B, '%04x' % res['fsw'])
        expect(label + ': tags and registers unchanged', res['ftw'] == st['ftw'] and
               all(res['fpr'][i] == st['fpr'][i] for i in range(8)), '')
        expect(label + ': TOP unchanged', (res['fsw'] >> 11) & 7 == (st['fsw'] >> 11) & 7)
        if name == 'fcom st1':
            expect(label + ': C0 C2 C3 unchanged', res['fsw'] & 0x4500 == 0x4500, '%04x' % res['fsw'])
    # --- stores: no memory write, no pop
    for label, name, fcw, flag, regs, mem_size in (
            ('fistp out of range', 'fistp word [0x2000]', 0x37e, IE, {0: 'f:100000.0'}, 2),
            ('fstp snan', 'fstp dword [0x2000]', 0x37e, IE, {0: 'snan'}, 4),
            ('fbstp out of range', 'fbstp [0x2000]', 0x37e, IE, {0: 'f:1e19'}, 10)):
        st = state(fcw=fcw, stack=1, regs=regs)
        res = run(cli, name, st)
        expect(label + ': nothing stored', not res['mem'], str(res['mem']))
        expect(label + ': not popped', res['ftw'] == 1 and (res['fsw'] >> 11) & 7 == 0)
        expect(label + ': IE, ES, B', res['fsw'] & (flag | ES | B) == flag | ES | B)
    # --- the push does not happen
    st = state(fcw=0x37e, stack=0, memory={0: bytes.fromhex('0000a07f')})
    res = run(cli, 'fld dword [0x2000]', st)           # 0x7fa00000, a signaling NaN
    expect('fld snan: no push', res['ftw'] == 0 and (res['fsw'] >> 11) & 7 == 0, '%04x' % res['fsw'])
    expect('fld snan: IE, ES, B', res['fsw'] & (IE | ES | B) == IE | ES | B)
    # --- stack fault, unmasked: SF, IE, C1 = 0 (underflow) or 1 (overflow), nothing else changes
    st = state(fcw=0x37e, stack=1, regs={0: 'one'}, fsw=C1)
    res = run(cli, 'faddp st1, st0', st)
    expect('underflow: IE SF ES B, C1 = 0', res['fsw'] & (IE | SF | ES | B | C1) == IE | SF | ES | B, '%04x' % res['fsw'])
    expect('underflow: no pop, ST(0) kept', res['ftw'] == 1 and res['fpr'][0] == value('one'))
    st = state(fcw=0x37e, stack=8, regs={0: 'one'})
    res = run(cli, 'fld1', st)
    expect('overflow: IE SF ES B C1', res['fsw'] & (IE | SF | ES | B | C1) == IE | SF | ES | B | C1, '%04x' % res['fsw'])
    expect('overflow: no push', res['ftw'] == 0xff and (res['fsw'] >> 11) & 7 == 0)
    # --- fcomi: OF, SF, AF are cleared even if the exception is unmasked, ZF, PF, CF stay
    for name in ('fcomi st0, st1', 'fucomip st0, st1'):
        st = state(fcw=0x37e, stack=2, regs={0: 'snan' if 'fucomi' in name else 'nan', 1: 'one'}, eflags=0x8d7)
        res = run(cli, name, st)
        expect(name + ': OF SF AF cleared, ZF PF CF kept', res['eflags'] & 0x8d5 == 0x45, '%08x' % res['eflags'])
        expect(name + ': IE, ES, B, not popped', res['fsw'] & (IE | ES | B) == IE | ES | B and res['ftw'] == 3)
    # --- traps: OE, UE, PE: the result is stored (here the masked response), the instruction completes
    st = state(fcw=0x35f, stack=2, regs={0: 'one', 1: 'three'})
    res = run(cli, 'fdiv st0, st1', st)
    expect('precision trap: result stored', res['fpr'][0] == 0x3ffdaaaaaaaaaaaaaaab, '%020x' % res['fpr'][0])
    expect('precision trap: PE, ES, B, C1 (rounded up)', res['fsw'] & (PE | ES | B | C1) == PE | ES | B | C1, '%04x' % res['fsw'])
    st = state(fcw=0x35f, stack=1, regs={0: 'f:0.1'})
    res = run(cli, 'fstp dword [0x2000]', st)
    expect('precision trap fstp: stored and popped', res['mem'] and res['ftw'] == 0 and (res['fsw'] >> 11) & 7 == 1, str(res['mem']))
    expect('precision trap fstp: PE, ES, B', res['fsw'] & (PE | ES | B) == PE | ES | B)
    st = state(fcw=0x377, stack=2, regs={0: 'max', 1: 'two'})
    res = run(cli, 'fmul st0, st1', st)
    expect('overflow trap: OE, PE, ES, B', res['fsw'] & (OE | PE | ES | B) == OE | PE | ES | B, '%04x' % res['fsw'])
    expect('overflow trap: the result is stored', res['fpr'][0] != st['fpr'][0])
    st = state(fcw=0x36f, stack=2, regs={0: 'min', 1: 'half'})
    res = run(cli, 'fmul st0, st1', st)
    # (the result 2^-16383 is exact: UE because the exception is unmasked, no PE)
    expect('underflow trap: UE, ES, B', res['fsw'] & (UE | PE | ES | B) == UE | ES | B, '%04x' % res['fsw'])
    # --- a pending exception: the waiting instructions raise #MF (16), fnstsw does not
    for name, regs in (('fadd st0, st1', {0: 'one', 1: 'one'}), ('fnop', {}), ('fwait', {}), ('fld1', {}),
                       ('fchs', {0: 'one'}), ('fldcw [0x2000]', {}), ('fxch st1', {0: 'one', 1: 'one'}),
                       ('fcomi st0, st1', {0: 'one', 1: 'one'})):
        st = state(fcw=0x37e, fsw=IE | ES | B, stack=2, regs=regs, memory={0: b'\x7f\x03'})
        res = run(cli, name, st)
        expect(name + ': #MF', res['exit'] == 'trap:16', res['exit'])
        expect(name + ': nothing changed', res['ftw'] == st['ftw'] and res['fsw'] == st['fsw'])
    st = state(fcw=0x37e, fsw=IE | ES | B, stack=2, regs={0: 'one', 1: 'one'})
    res = run(cli, 'fnstsw ax', st)
    expect('fnstsw: no #MF, ax = fsw', res['exit'] == 'fall' and res['regs']['eax'] & 0xffff == st['fsw'], res['exit'])
    res = run(cli, 'fnclex', st)
    expect('fnclex: no #MF, clears the flags, ES and B', res['exit'] == 'fall' and res['fsw'] & 0x80ff == 0, '%04x' % res['fsw'])
    # --- fldcw unmasking a flagged exception sets ES and B, masking it clears them
    st = state(fcw=0x37f, fsw=IE, stack=1, regs={0: 'one'}, memory={0: bytes([0x7e, 0x03])})
    res = run(cli, 'fldcw [0x2000]', st)
    expect('fldcw unmask: ES, B', res['fsw'] & (ES | B) == ES | B and res['fcw'] == 0x37e, '%04x %04x' % (res['fsw'], res['fcw']))
    st = state(fcw=0x37f, fsw=IE | PE, stack=1, regs={0: 'one'}, memory={0: bytes([0x5f, 0x03])})
    res = run(cli, 'fldcw [0x2000]', st)
    expect('fldcw unmask PE only: ES, B', res['fsw'] & (ES | B) == ES | B)
    st = state(fcw=0x37f, fsw=IE | DE, stack=1, regs={0: 'one'}, memory={0: bytes([0x7f, 0x03])})
    res = run(cli, 'fldcw [0x2000]', st)
    expect('fldcw all masked: no ES, B', res['fsw'] & (ES | B) == 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', default=os.path.join(REPO, 'out', 'bin', 'dismount_cli'))
    args = parser.parse_args()
    check(os.path.abspath(args.cli))
    for f in failures:
        print('FAIL', f)
    print('%d checks, %d failed' % (count, len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
