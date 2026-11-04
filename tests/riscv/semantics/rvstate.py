#!/usr/bin/env python3
"""
The state / result line protocol of the RISC-V semantics tests (see "The run protocol" in
tests/riscv/semantics/TEST_PLAN.md), shared by the oracles (oracle_qemu.py, oracle_unicorn.py,
oracle_spike.py) and the driver. The same shape as the ARM one (tests/arm/semantics/armstate.py).

A state line is what `dismount_cli run --arch rv64|rv32 [--vlen N]` reads:

    <hex bytes of the code> ; token=hex token=hex ...

and a result line is what it prints: `OK exit=<exit> token=hex ...` or `ERR <message>`.
Every value is hex without 0x. The tokens (the registers that are 0 are omitted from a state):

    x1..x31   XLEN bits                     pc          the address of the code
    f0..f31   64 bits raw (the NaN boxing is in the bits)
    fcsr      frm = bits 5-7, fflags = bits 0-4
    vlen      VLEN in bits (hex: 80 = 128)  v0..v31     VLEN bits (VLEN/4 digits)
    vl, vtype (the raw CSR, vill = bit XLEN-1), vstart, vxrm, vxsat
    ssp, elp  Zicfiss / Zicfilp state       win=BASE:SIZE  the data window
    mem=ADDR:HEX,ADDR:HEX   initial bytes (the other bytes of the window are the pattern)

The vector tokens (and the vector part of the result) exist when the state has any of v*, vl,
vtype, vstart, vxrm, vxsat, vlen (the 'vector' flag of the state dictionary).

The dictionaries used here (a state, and a result which adds `exit`):
    xlen, vlen   ints              x   list of 32 ints (x[0] = 0)      pc   int
    f            list of 32 ints   v   list of 32 ints (VLEN bits)
    fcsr, vl, vtype, vstart, vxrm, vxsat, ssp, elp   ints
    vector       bool              mem {address: byte}                 win  (base, size)
    exit         'fall' | 'branch' | 'trap:<kind>'  kinds: ecall ebreak illegal fault swcheck
"""
import re

CODE_PAGE = 0x80001000              # the default pc: the middle page of the 3 pages of the code region
WIN_BASE = 0x80100000
WIN_SIZE = 0x4000                   # (the protocol default is 0x10000; the states carry win= always)
DEFAULT_VLEN = 128
M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
FCSR_MASK = 0xFF


def mask(xlen):
    return M64 if xlen == 64 else M32


def pattern_byte(a):
    """The initial content of the data window: byte(A) = ((uint32)(A * 2654435761) >> 24), the
    same pattern as ARM (with the low 32 bits of the address)."""
    return (((a & M32) * 2654435761) & M32) >> 24


# ---------------------------------------------------------------------------
# Memory runs: 80100000:aabbcc,80100010:11 or '-'
# ---------------------------------------------------------------------------

def format_mem(mem):
    if not mem:
        return '-'
    runs = []
    for a in sorted(mem):
        if runs and runs[-1][0] + len(runs[-1][1]) == a:
            runs[-1][1].append(mem[a])
        else:
            runs.append((a, bytearray([mem[a]])))
    return ','.join('%x:%s' % (a, bytes(b).hex()) for a, b in runs)


def parse_mem(text):
    mem = {}
    if text in ('-', ''):
        return mem
    for run in text.split(','):
        a, _, data = run.partition(':')
        base = int(a, 16)
        for i in range(len(data) // 2):
            mem[base + i] = int(data[2 * i:2 * i + 2], 16)
    return mem


# ---------------------------------------------------------------------------
# States
# ---------------------------------------------------------------------------

def default_state(xlen, vlen=DEFAULT_VLEN):
    """A state with the default values: everything 0, the stack at the middle of the window."""
    return {'xlen': xlen, 'vlen': vlen, 'x': [0] * 32, 'f': [0] * 32, 'v': [0] * 32,
            'pc': CODE_PAGE, 'fcsr': 0, 'vl': 0, 'vtype': 0, 'vstart': 0, 'vxrm': 0, 'vxsat': 0,
            'ssp': 0, 'elp': 0, 'cfi': False, 'vector': False, 'mem': {}, 'win': (WIN_BASE, WIN_SIZE)}


def copy_state(st):
    c = dict(st)
    c['x'] = list(st['x'])
    c['f'] = list(st['f'])
    c['v'] = list(st['v'])
    c['mem'] = dict(st['mem'])
    return c


def initial_byte(state, addr):
    if addr in state['mem']:
        return state['mem'][addr]
    return pattern_byte(addr)


def format_state(code, state):
    """One input line of `dismount_cli run`."""
    xlen = state['xlen']
    out = []
    for i in range(1, 32):
        if state['x'][i]:
            out.append('x%d=%x' % (i, state['x'][i]))
    out.append('pc=%x' % state['pc'])
    for i in range(32):
        if state['f'][i]:
            out.append('f%d=%x' % (i, state['f'][i]))
    if state['fcsr']:
        out.append('fcsr=%x' % state['fcsr'])
    if state['vector']:
        out.append('vlen=%x' % state['vlen'])
        for i in range(32):
            if state['v'][i]:
                out.append('v%d=%0*x' % (i, state['vlen'] // 4, state['v'][i]))
        for k in ('vl', 'vtype', 'vstart', 'vxrm', 'vxsat'):
            if state[k]:
                out.append('%s=%x' % (k, state[k]))
    if state['ssp']:
        out.append('ssp=%x' % state['ssp'])
    if state['elp']:
        out.append('elp=1')
    out.append('win=%x:%x' % state['win'])
    if state['mem']:
        out.append('mem=' + format_mem(state['mem']))
    return '%s ; %s' % (code.hex(), ' '.join(out))


def parse_tokens(xlen, tokens, state):
    """Apply the `name=value` tokens (a list of strings) to `state`."""
    for tok in tokens:
        if tok.startswith('#'):
            continue
        k, _, v = tok.partition('=')
        if k == 'mem':
            state['mem'].update(parse_mem(v))
        elif k == 'win':
            a, _, b = v.partition(':')
            state['win'] = (int(a, 16), int(b, 16))
        elif k == 'exit':
            state['exit'] = v
        elif k == 'reserved':
            state['reserved'] = v
        elif k == 'vlen':
            state['vlen'] = int(v, 16)
            state['vector'] = True
        elif re.fullmatch(r'x\d+', k) and 0 < int(k[1:]) < 32:
            state['x'][int(k[1:])] = int(v, 16)
        elif re.fullmatch(r'f\d+', k) and int(k[1:]) < 32:
            state['f'][int(k[1:])] = int(v, 16)
        elif re.fullmatch(r'v\d+', k) and int(k[1:]) < 32:
            state['v'][int(k[1:])] = int(v, 16)
            state['vector'] = True
        elif k in ('vl', 'vtype', 'vstart', 'vxrm', 'vxsat'):
            state[k] = int(v, 16)
            state['vector'] = True
        elif k in ('pc', 'fcsr', 'ssp', 'elp'):
            state[k] = int(v, 16)
        else:
            raise ValueError('unknown token %r' % tok)
    return state


def parse_state(xlen, line):
    """Parse an input line: returns (code bytes, state dict)."""
    hexs, _, rest = line.partition(';')
    st = default_state(xlen)
    parse_tokens(xlen, rest.split(), st)
    return bytes.fromhex(hexs.strip()), st


def format_result(res):
    """An `OK ...` output line of a result dict."""
    xlen = res['xlen']
    d = xlen // 4
    out = ['exit=%s' % res['exit']]
    for i in range(1, 32):
        out.append('x%d=%0*x' % (i, d, res['x'][i]))
    out.append('pc=%0*x' % (d, res['pc']))
    for i in range(32):
        out.append('f%d=%016x' % (i, res['f'][i]))
    out.append('fcsr=%08x' % res['fcsr'])
    out.append('ssp=%0*x' % (d, res['ssp']))
    out.append('elp=%x' % res['elp'])
    if res['vector']:
        out.append('vlen=%x' % res['vlen'])
        for k in ('vl', 'vtype', 'vstart', 'vxrm', 'vxsat'):
            out.append('%s=%0*x' % (k, d, res[k]) if k in ('vl', 'vtype', 'vstart') else '%s=%x' % (k, res[k]))
        for i in range(32):
            out.append('v%d=%0*x' % (i, res['vlen'] // 4, res['v'][i]))
    out.append('mem=' + format_mem(res.get('mem', {})))
    if res.get('info'):
        out.append('#' + res['info'].replace(' ', ','))
    return 'OK ' + ' '.join(out)


def parse_result(xlen, line):
    """Parse an `OK ...` / `ERR ...` line. Returns a result dict, or {'err': message}."""
    if not line.startswith('OK '):
        return {'err': line[4:].strip() if line.startswith('ERR ') else line.strip()}
    res = default_state(xlen)
    res['mem'] = {}
    try:
        parse_tokens(xlen, line[3:].split(), res)
    except (ValueError, IndexError) as e:
        return {'err': 'malformed output (%s): %s' % (e, line[:120])}
    if 'exit' not in res:
        return {'err': 'malformed output (no exit): %s' % line[:120]}
    return res


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def exit_class(ex):
    return 'flow' if ex in ('fall', 'branch') else ex


def compare_results(code, state, a, b, names=('a', 'b'), ignore=(), trap_state=True, f_low32=False):
    """Differences between two results (dicts of parse_result / the oracles). `ignore` is a set of
    register names that are not compared. Returns a list of strings. After a trap (illegal, fault)
    the registers are compared too, except pc (the state at the trap is the one of the instruction:
    the exceptions are precise); `trap_state=False` compares the exit only. `f_low32` compares the
    low 32 bits of the f registers only (Unicorn does not NaN box)."""
    diffs = []
    na, nb = names
    ea, eb = a['exit'], b['exit']
    if exit_class(ea) != exit_class(eb):
        return ['exit: %s=%s %s=%s' % (na, ea, nb, eb)]
    skip = set(ignore)
    xlen = state['xlen']
    is_trap = ea.startswith('trap:')
    if is_trap and not trap_state:
        return diffs

    def cmp(name, x, y, width):
        if name in skip or x == y:
            return
        diffs.append('%s: %s=%0*x %s=%0*x' % (name, na, width, x, nb, width, y))

    d = xlen // 4
    for i in range(1, 32):
        cmp('x%d' % i, a['x'][i], b['x'][i], d)
    if not is_trap:
        cmp('pc', a['pc'], b['pc'], d)
    for i in range(32):
        if f_low32:
            cmp('f%d' % i, a['f'][i] & M32, b['f'][i] & M32, 8)
        else:
            cmp('f%d' % i, a['f'][i], b['f'][i], 16)
    cmp('fcsr', a['fcsr'] & FCSR_MASK, b['fcsr'] & FCSR_MASK, 2)
    if state.get('cfi'):
        cmp('ssp', a['ssp'], b['ssp'], d)
        cmp('elp', a['elp'], b['elp'], 1)
    if state['vector']:
        for k in ('vl', 'vtype', 'vstart', 'vxrm', 'vxsat'):
            cmp(k, a[k], b[k], d)
        for i in range(32):
            cmp('v%d' % i, a['v'][i], b['v'][i], state['vlen'] // 4)

    for addr in sorted(set(a['mem']) | set(b['mem'])):
        va = a['mem'].get(addr, initial_byte(state, addr))
        vb = b['mem'].get(addr, initial_byte(state, addr))
        if va != vb and 'mem' not in skip:
            diffs.append('mem[%x]: %s=%02x %s=%02x' % (addr, na, va, nb, vb))
            if len(diffs) > 24:
                diffs.append('...')
                break
    return diffs
