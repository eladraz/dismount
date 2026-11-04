#!/usr/bin/env python3
"""
The state / result line protocol of the ARM semantics tests (see "The run
protocol" in tests/arm/semantics/TEST_PLAN.md), shared by the oracles
(oracle_arm64.py, oracle_arm32.py, oracle_host.py) and the driver.

A state line is what `dismount_cli run --arch a32|t32|a64` reads:

    <hex bytes of the code> ; token=hex token=hex ...

and a result line is what it prints: `OK exit=<exit> token=hex ...` or
`ERR <message>`. Every value is hex without 0x. The dictionaries used here:

  A64 state / result:
      x       list of 31 ints (x0..x30)          sp, pc        ints
      nzcv    int 0..15 (N=8 Z=4 C=2 V=1)        fpcr, fpsr    ints
      v       list of 32 ints (128 bit)
      sve     bool: z / p / ffr are part of the state (and of the result)
      sm      bool: streaming mode (SVCR.SM), the vector length is svl
      za      bool: SVCR.ZA, with zarows (list of ints, svl/8 rows) and zt0
      vl, svl vector lengths in bits (the state of the run, default 128, 512)
      z       list of 32 ints (vl or svl bits)   p  list of 16 ints (bits/8)
      ffr     int
      mem     {address: byte}
  A32 / T32 state / result:
      r       list of 13 ints (r0..r12)          sp, lr, pc    ints
      apsr    int, only N Z C V Q (bits 31..27) and GE (19..16)
      t       the Thumb bit (result: after the instruction; a state has it from the arch)
      fpscr   int                                q  list of 16 ints (128 bit)
      mem     {address: byte}
  result only:
      exit    'fall' | 'branch' | 'trap:<kind>'  kinds: svc brk undef fault
      unknown optional set of the names of the registers the architecture
              leaves UNKNOWN for this instruction (not compared)
"""
import re

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF

# Where the instruction runs and the memory window the oracles map (the same
# addresses in every engine, so the pc relative instructions and the
# addresses in the results agree)
A64_PC = 0x0000010000001000
A64_MEM_BASE = 0x0000010100000000
A32_PC = 0x00010000
A32_MEM_BASE = 0x20000000
MEM_SIZE = 0x10000
DEFAULT_VL = 128
DEFAULT_SVL = 512

A64_X = ['x%d' % i for i in range(31)]
A32_R = ['r%d' % i for i in range(13)]
NZCV_BITS = {'n': 8, 'z': 4, 'c': 2, 'v': 1}
# The APSR bits the protocol carries: N Z C V Q and GE[3:0]
APSR_MASK = 0xF80F0000
# The FPSCR / FPCR / FPSR bits that exist. The rest are reserved or RES0 and
# are not stored
FPCR_MASK = 0x07FF9F00 | 0x7                # the control bits (AH FIZ NEP are 0..2)
FPSR_MASK = 0xF800009F                      # N Z C V QC IDC IXC UFC OFC DZC IOC


def pattern_byte(a):
    """The initial content of the memory window: byte(A) = ((A * 2654435761) mod 2^32) >> 24
    (the same pattern as tests/semantics, with the low 32 bits of the address)"""
    return (((a & M32) * 2654435761) & M32) >> 24


_pattern_windows = {}


def pattern_window(base):
    """The initial bytes of the memory window at `base` (MEM_SIZE bytes), cached."""
    if base not in _pattern_windows:
        _pattern_windows[base] = bytes(pattern_byte(base + i) for i in range(MEM_SIZE))
    return _pattern_windows[base]


def is_a64(arch):
    return arch == 'a64'


def vector_bits(state):
    """The size of z in the state: svl in streaming mode, vl otherwise."""
    return state.get('svl', DEFAULT_SVL) if state.get('sm') else state.get('vl', DEFAULT_VL)


# ---------------------------------------------------------------------------
# Memory runs: 4100000000:aabbcc,4100000010:11 or '-'
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
# Defaults
# ---------------------------------------------------------------------------

def default_state(arch):
    """A state with the default values: everything 0, the stack at the middle of the
    memory window, the pc at the code address."""
    if is_a64(arch):
        return {'x': [0] * 31, 'sp': A64_MEM_BASE + MEM_SIZE // 2, 'pc': A64_PC, 'nzcv': 0,
                'fpcr': 0, 'fpsr': 0, 'v': [0] * 32, 'sve': False, 'sm': False, 'za': False,
                'vl': DEFAULT_VL, 'svl': DEFAULT_SVL, 'mem': {}}
    return {'r': [0] * 13, 'sp': A32_MEM_BASE + MEM_SIZE // 2, 'lr': 0, 'pc': A32_PC, 'apsr': 0,
            'fpscr': 0, 'q': [0] * 16, 't': 1 if arch == 't32' else 0, 'mem': {}}


def complete_vectors(state):
    """Fill z / p / ffr / za with their defaults (z = v zero extended, p = 0) after the tokens
    of a state were parsed (a64 only)."""
    if not (state['sve'] or state['sm']):
        return
    bits = vector_bits(state)
    if 'z' not in state:
        state['z'] = list(state['v'])
    else:
        # v wins over z in the low 128 bits (the tokens are applied z first, then v)
        state['z'] = [(z & ~((1 << 128) - 1)) | v for z, v in zip(state['z'], state['v'])]
    state['z'] = [z & ((1 << bits) - 1) for z in state['z']]
    state.setdefault('p', [0] * 16)
    state.setdefault('ffr', 0)
    if state['za']:
        rows = state['svl'] // 8
        state.setdefault('zarows', [0] * rows)
        state.setdefault('zt0', 0)


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

def _vector_tokens(arch, s, out, keep_defaults=False):
    """The tokens of the vector part of a state or a result."""
    if is_a64(arch):
        for i, v in enumerate(s['v']):
            if v or keep_defaults:
                out.append('v%d=%032x' % (i, v))
        if s.get('sve') or s.get('sm'):
            digits = vector_bits(s) // 4
            for i, z in enumerate(s.get('z', [])):
                if z or keep_defaults:
                    out.append('z%d=%0*x' % (i, digits, z))
            for i, p in enumerate(s.get('p', [])):
                if p or keep_defaults:
                    out.append('p%d=%0*x' % (i, vector_bits(s) // 32 or 1, p))
            if not s.get('sm') and ('ffr' in s) and (s['ffr'] or keep_defaults):
                out.append('ffr=%0*x' % (vector_bits(s) // 32 or 1, s['ffr']))
        if s.get('za'):
            for i, row in enumerate(s.get('zarows', [])):
                if row or keep_defaults:
                    out.append('za%d=%0*x' % (i, s['svl'] // 4, row))
            if s.get('zt0') or keep_defaults:
                out.append('zt0=%0128x' % s.get('zt0', 0))
    else:
        for i, q in enumerate(s['q']):
            if q or keep_defaults:
                out.append('q%d=%032x' % (i, q))


def _mode_tokens(s, out):
    if s.get('sve'):
        out.append('sve=1')
    if s.get('sm'):
        out.append('sm=1')
    if s.get('za'):
        out.append('za=1')
    if s.get('sve') or s.get('sm'):
        out.append('vl=%x' % s.get('vl', DEFAULT_VL))
        out.append('svl=%x' % s.get('svl', DEFAULT_SVL))


def format_state(arch, code, state):
    """One input line of `dismount_cli run`. The registers that are 0 are omitted, except pc,
    sp and the nzcv / apsr."""
    d = default_state(arch)
    out = []
    if is_a64(arch):
        _mode_tokens(state, out)
        for i, x in enumerate(state['x']):
            if x:
                out.append('x%d=%x' % (i, x))
        out.append('sp=%x' % state['sp'])
        out.append('pc=%x' % state['pc'])
        out.append('nzcv=%x' % state['nzcv'])
        if state['fpcr']:
            out.append('fpcr=%x' % state['fpcr'])
        if state['fpsr']:
            out.append('fpsr=%x' % state['fpsr'])
    else:
        for i, r in enumerate(state['r']):
            if r:
                out.append('r%d=%x' % (i, r))
        out.append('sp=%x' % state['sp'])
        if state['lr']:
            out.append('lr=%x' % state['lr'])
        out.append('pc=%x' % state['pc'])
        out.append('apsr=%x' % state['apsr'])
        if state['fpscr']:
            out.append('fpscr=%x' % state['fpscr'])
    _vector_tokens(arch, state, out)
    if state.get('mem'):
        out.append('mem=' + format_mem(state['mem']))
    return '%s ; %s' % (code.hex(), ' '.join(out))


def parse_tokens(arch, tokens, state):
    """Apply the `name=value` tokens (a list of strings) to `state`."""
    zs = {}
    for tok in tokens:
        k, _, v = tok.partition('=')
        if k == 'mem':
            state['mem'].update(parse_mem(v))
        elif k in ('sve', 'sm', 'za') and is_a64(arch):
            state[k] = bool(int(v, 16))
        elif k in ('vl', 'svl'):
            state[k] = int(v, 16)
        elif is_a64(arch) and re.fullmatch(r'x\d+', k):
            state['x'][int(k[1:])] = int(v, 16)
        elif is_a64(arch) and re.fullmatch(r'v\d+', k):
            state['v'][int(k[1:])] = int(v, 16)
        elif is_a64(arch) and re.fullmatch(r'z\d+', k):
            zs[int(k[1:])] = int(v, 16)
        elif is_a64(arch) and re.fullmatch(r'p\d+', k):
            state.setdefault('p', [0] * 16)[int(k[1:])] = int(v, 16)
        elif is_a64(arch) and k == 'ffr':
            state['ffr'] = int(v, 16)
        elif is_a64(arch) and re.fullmatch(r'za\d+', k):
            rows = state.setdefault('zarows', [0] * (state['svl'] // 8))
            rows[int(k[2:])] = int(v, 16)
        elif is_a64(arch) and k == 'zt0':
            state['zt0'] = int(v, 16)
        elif not is_a64(arch) and re.fullmatch(r'r\d+', k) and int(k[1:]) < 13:
            state['r'][int(k[1:])] = int(v, 16)
        elif not is_a64(arch) and re.fullmatch(r'q\d+', k):
            state['q'][int(k[1:])] = int(v, 16)
        elif k == 'unknown':
            state['unknown'] = set(filter(None, v.split(',')))
        elif k == 'exit':
            state['exit'] = v
        elif k in state or k in ('nzcv', 'apsr', 'fpcr', 'fpsr', 'fpscr', 'sp', 'pc', 'lr'):
            state[k] = int(v, 16)
        else:
            raise ValueError('unknown token %r' % tok)
    if zs:
        z = state.setdefault('z', [0] * 32)
        for i, val in zs.items():
            z[i] = val
        state['sve'] = state.get('sve') or True
    return state


def parse_state(arch, line):
    """Parse an input line: returns (code bytes, state dict)."""
    hexs, _, rest = line.partition(';')
    st = default_state(arch)
    parse_tokens(arch, rest.split(), st)
    complete_vectors(st)
    return bytes.fromhex(hexs.strip()), st


def format_result(arch, res):
    """An `OK ...` output line of a result dict."""
    out = ['exit=%s' % res['exit']]
    if res.get('unknown'):
        out.append('unknown=' + ','.join(sorted(res['unknown'])))
    if is_a64(arch):
        _mode_tokens(res, out)
        for i, x in enumerate(res['x']):
            out.append('x%d=%016x' % (i, x))
        out.append('sp=%016x' % res['sp'])
        out.append('pc=%016x' % res['pc'])
        out.append('nzcv=%x' % res['nzcv'])
        out.append('fpcr=%08x' % res['fpcr'])
        out.append('fpsr=%08x' % res['fpsr'])
    else:
        for i, r in enumerate(res['r']):
            out.append('r%d=%08x' % (i, r))
        out.append('sp=%08x' % res['sp'])
        out.append('lr=%08x' % res['lr'])
        out.append('pc=%08x' % res['pc'])
        out.append('apsr=%08x' % res['apsr'])
        out.append('t=%x' % res['t'])
        out.append('fpscr=%08x' % res['fpscr'])
    _vector_tokens(arch, res, out, keep_defaults=True)
    out.append('mem=' + format_mem(res.get('mem', {})))
    return 'OK ' + ' '.join(out)


def parse_result(arch, line):
    """Parse an `OK ...` / `ERR ...` line. Returns a result dict, or {'err': message}."""
    if not line.startswith('OK '):
        return {'err': line[4:].strip() if line.startswith('ERR ') else line.strip()}
    res = default_state(arch)
    res['mem'] = {}
    toks = line[3:].split()
    try:
        parse_tokens(arch, toks, res)
    except (ValueError, IndexError) as e:
        return {'err': 'malformed output (%s): %s' % (e, line[:120])}
    if 'exit' not in res:
        return {'err': 'malformed output (no exit): %s' % line[:120]}
    if is_a64(arch):
        complete_vectors(res)
    return res


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def exit_class(ex):
    return 'flow' if ex in ('fall', 'branch') else ex


def compare_results(arch, code, state, a, b, names=('a', 'b'), ignore=()):
    """Differences between two results (dicts of parse_result / the oracles). `ignore` is a set of
    register names that are not compared (x18 on the host, ...), the `unknown` sets of both
    results are added. Returns a list of strings."""
    diffs = []
    na, nb = names
    ea, eb = a['exit'], b['exit']
    if exit_class(ea) != exit_class(eb):
        return ['exit: %s=%s %s=%s' % (na, ea, nb, eb)]
    skip = set(ignore) | set(a.get('unknown', ())) | set(b.get('unknown', ()))
    both_trap = ea.startswith('trap:')

    def cmp(name, x, y, width):
        if name in skip or x == y:
            return
        diffs.append('%s: %s=%0*x %s=%0*x' % (name, na, width, x, nb, width, y))

    if both_trap:
        # a trap: nothing else is compared, the engines stop at different points
        return diffs
    if is_a64(arch):
        for i in range(31):
            cmp('x%d' % i, a['x'][i], b['x'][i], 16)
        cmp('sp', a['sp'], b['sp'], 16)
        cmp('pc', a['pc'], b['pc'], 16)
        cmp('nzcv', a['nzcv'], b['nzcv'], 1)
        cmp('fpcr', a['fpcr'] & FPCR_MASK, b['fpcr'] & FPCR_MASK, 8)
        cmp('fpsr', a['fpsr'] & FPSR_MASK, b['fpsr'] & FPSR_MASK, 8)
        for i in range(32):
            cmp('v%d' % i, a['v'][i], b['v'][i], 32)
        if ea == 'fall' and eb == 'fall' and (a.get('sve') or a.get('sm')):
            digits = vector_bits(a) // 4
            for i in range(32):
                cmp('z%d' % i, a['z'][i], b['z'][i], digits)
            for i in range(16):
                cmp('p%d' % i, a['p'][i], b['p'][i], max(1, digits // 8))
            if not a.get('sm'):
                cmp('ffr', a['ffr'], b['ffr'], max(1, digits // 8))
            if a.get('za'):
                for i, (ra, rb) in enumerate(zip(a['zarows'], b['zarows'])):
                    cmp('za%d' % i, ra, rb, a['svl'] // 4)
                cmp('zt0', a['zt0'], b['zt0'], 128)
    else:
        for i in range(13):
            cmp('r%d' % i, a['r'][i], b['r'][i], 8)
        for n in ('sp', 'lr'):
            cmp(n, a[n], b[n], 8)
        # A branch to the A32 address xxx10 (bx, blx, a load of pc): BXWritePC is CONSTRAINED
        # UNPREDICTABLE (Unpredictable_A32FORCEALIGNPC): dismount forces the alignment, QEMU
        # keeps bit 1. The two choices are both the architecture
        if ea == 'branch' and eb == 'branch' and a['t'] == 0 and b['t'] == 0:
            cmp('pc', a['pc'] & ~2, b['pc'] & ~2, 8)
        else:
            cmp('pc', a['pc'], b['pc'], 8)
        cmp('apsr', a['apsr'] & APSR_MASK, b['apsr'] & APSR_MASK, 8)
        cmp('t', a['t'], b['t'], 1)
        cmp('fpscr', a['fpscr'], b['fpscr'], 8)
        for i in range(16):
            cmp('q%d' % i, a['q'][i], b['q'][i], 32)

    base = A64_MEM_BASE if is_a64(arch) else A32_MEM_BASE
    for addr in sorted(set(a['mem']) | set(b['mem'])):
        va = mem_value(arch, a['mem'], addr, code, state)
        vb = mem_value(arch, b['mem'], addr, code, state)
        if va != vb and 'mem' not in skip:
            diffs.append('mem[%x]: %s=%02x %s=%02x' % (addr, na, va, nb, vb))
            if len(diffs) > 24:
                diffs.append('...')
                break
    return diffs


def mem_value(arch, side, addr, code, state):
    """The byte at `addr` after a run: what the run wrote, otherwise the initial byte (the code
    at pc, the state's mem tokens, the pattern)."""
    if addr in side:
        return side[addr]
    off = addr - state['pc']
    if 0 <= off < len(code):
        return code[off]
    if addr in state.get('mem', {}):
        return state['mem'][addr]
    return pattern_byte(addr)
