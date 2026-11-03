#!/usr/bin/env python3
"""Systematic validity sweep of the IA-32 / AMD64 decoder against ndisasm.

For every one-byte opcode, every 0F xx, 0F 38 xx and 0F 3A xx opcode, every
ModR/M byte (all 256: mod / reg / rm) and a set of prefixes (none, 66, F2, F3
and, in 64 bit mode, REX / REX.B / REX.R / REX.W combinations), one 16 byte
block "prefix opcode modrm 00 00 ..." is decoded by `dismount_cli dis` and by
ndisasm (one ndisasm run per chunk, the blocks are separated by sync points).
The results are classified as:

  (a) dismount decodes, ndisasm says `db` (or only a prefix): dismount accepts
      an invalid encoding.
  (b) ndisasm decodes an instruction whose mnemonic is in dismount's opcode
      table, dismount says invalid: dismount misses a valid encoding.
  (c) both decode, with a different length ((c) length) or a different
      mnemonic ((c) mnemonic).
  (n) nasm rejects the text which dismount prints for an
      instruction it decodes (the second oracle: where ndisasm is lenient, nasm
      is not).

ndisasm is the reference, except where the Intel SDM says otherwise: those
cases are listed in tests/ia32/validity_exceptions.txt (one rule per line:
BITS CATEGORIES REGEX  # SDM reason; BITS is 16, 32, 64 or all, CATEGORIES a
comma list of a, b, cl, cm, x or *, REGEX a full match of the 16 byte block in
lower case hex) and are not reported. The report groups the findings by prefix, opcode,
mod (memory / register) and reg field.

Exit status: 1 when anything is reported outside the allowlist, else 0.

Usage:
    python3 tests/ia32/tools/fuzz_validity.py                  # 16, 32 and 64 bit
    python3 tests/ia32/tools/fuzz_validity.py --bits 64 --only '^0f0[12]'
    python3 tests/ia32/tools/fuzz_validity.py --bits 32 --category a --show 100
"""

import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TESTS_DIR)
import run_compat as rc  # noqa: E402
import fuzz_lengths as fl  # noqa: E402
from validity_allow import DEFAULT_ALLOW, allowed, load_allow  # noqa: E402

BLOCK = 16
CHUNK = 4096

# Opcode bytes which are prefixes (and the REX prefixes of 64 bit mode, which
# are covered by the prefix sets); 9B is folded by ndisasm into the next
# instruction
PREFIX_BYTES = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf0, 0xf2, 0xf3, 0x9b}

PREFIX_SETS = {
    16: ['', '66', 'f2', 'f3'],
    32: ['', '66', 'f2', 'f3'],
    64: ['', '66', 'f2', 'f3', '40', '41', '44', '48', '6648', 'f248', 'f348', '664c'],
}

# Mnemonics which are the same instruction with another spelling
ALIASES = [
    {'sal', 'shl'}, {'jz', 'je'}, {'jnz', 'jne'}, {'jc', 'jb', 'jnae'},
    {'jnc', 'jae', 'jnb'}, {'jna', 'jbe'}, {'ja', 'jnbe'}, {'jl', 'jnge'},
    {'jge', 'jnl'}, {'jle', 'jng'}, {'jg', 'jnle'}, {'jpe', 'jp'}, {'jpo', 'jnp'},
    {'setz', 'sete'}, {'setnz', 'setne'}, {'setc', 'setb', 'setnae'},
    {'setnc', 'setae', 'setnb'}, {'setna', 'setbe'}, {'seta', 'setnbe'},
    {'setl', 'setnge'}, {'setge', 'setnl'}, {'setle', 'setng'}, {'setg', 'setnle'},
    {'setpe', 'setp'}, {'setpo', 'setnp'},
    {'cmovz', 'cmove'}, {'cmovnz', 'cmovne'}, {'cmovc', 'cmovb', 'cmovnae'},
    {'cmovnc', 'cmovae', 'cmovnb'}, {'cmovna', 'cmovbe'}, {'cmova', 'cmovnbe'},
    {'cmovl', 'cmovnge'}, {'cmovge', 'cmovnl'}, {'cmovle', 'cmovng'},
    {'cmovg', 'cmovnle'}, {'cmovpe', 'cmovp'}, {'cmovpo', 'cmovnp'},
    {'repe', 'repz'}, {'loopnz', 'loopne'}, {'loopz', 'loope'}, {'repne', 'repnz'}, {'wait', 'fwait'},
    {'xchg', 'nop'}, {'movsx', 'movsxd'}, {'swapgs', 'swapgs'},
    {'iretd', 'iret'}, {'pushfd', 'pushf'}, {'popfd', 'popf'},
    {'pushad', 'pusha'}, {'popad', 'popa'}, {'cwde', 'cbw'}, {'cdq', 'cwd'},
    {'cdqe', 'cwde'}, {'cqo', 'cdq'}, {'lodsd', 'lodsw', 'lodsq'},
    {'movsd', 'movsw', 'movsq'}, {'stosd', 'stosw', 'stosq'},
    {'scasd', 'scasw', 'scasq'}, {'cmpsd', 'cmpsw', 'cmpsq'},
    {'insd', 'insw'}, {'outsd', 'outsw'},
]


def alias_class(m):
    for a in ALIASES:
        if m in a:
            return frozenset(a)
    return frozenset([m])


def same_mnemonic(dm, nd):
    if dm == nd:
        return True
    if alias_class(dm) & alias_class(nd):
        return True
    # nasm spells the size of ret / leave / iret / pushf ... in 64 bit mode
    if dm in rc.SUFFIXED and nd in (dm + 'q', dm + 'w', dm + 'd'):
        return True
    # The pclmulqdq and cmpps pseudo ops
    if dm == 'pclmulqdq' and re.match('^pclmul[lh]q[lh]qdq$', nd):
        return True
    # SSE compare pseudo ops, nasm width spellings, ...
    for sfx in ('ps', 'pd', 'ss', 'sd'):
        if dm == 'cmp' + sfx and re.match('^cmp[a-z]+' + sfx + '$', nd):
            return True
    return False


def make_blocks(bits, only):
    """Yields (hex string of a block)."""
    opcodes = []
    for b in range(256):
        if b in PREFIX_BYTES or (bits == 64 and 0x40 <= b <= 0x4f):
            continue
        if b == 0x0f:
            for c in range(256):
                if c == 0x38 or c == 0x3a:
                    for d in range(256):
                        opcodes.append('0f%02x%02x' % (c, d))
                else:
                    opcodes.append('0f%02x' % c)
        else:
            opcodes.append('%02x' % b)
    for pfx in PREFIX_SETS[bits]:
        for op in opcodes:
            for modrm in range(256):
                head = pfx + op + '%02x' % modrm
                if only is not None and not only.search(head):
                    continue
                yield (head + '00' * BLOCK)[:BLOCK * 2]


def ndisasm_chunk(ndis, bits, blocks, tmpdir, index):
    path = os.path.join(tmpdir, 'chunk%06d.bin' % index)
    with open(path, 'wb') as f:
        for b in blocks:
            f.write(bytes.fromhex(b))
    cmd = [ndis, '-b', str(bits), '-o', '0']
    for i in range(1, len(blocks)):
        cmd += ['-s', str(i * BLOCK)]
    cmd.append(path)
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    os.remove(path)
    blocks_lines = collections.defaultdict(list)
    lines = res.stdout.splitlines()
    for idx, raw in enumerate(lines):
        m = fl.NDIS_HEAD.match(raw)
        if not m:
            continue
        off = int(m.group(1), 16)
        blk = off // BLOCK
        hexcol = m.group(2)
        nxt = idx + 1
        while nxt < len(lines):
            c = fl.NDIS_CONT.match(lines[nxt])
            if not c:
                break
            hexcol += c.group(1)
            nxt += 1
        blocks_lines[blk].append((off % BLOCK, len(hexcol) // 2, m.group(3)))
    return blocks_lines


# Prefix words of ndisasm in front of the mnemonic, besides rc.PREFIX_WORDS:
# osp / o64 (the ignored 66 / REX.W), the MPX and HLE spellings of F2 / F3
ND_PREFIXES = rc.PREFIX_WORDS | {'osp', 'o64', 'bnd', 'xacquire', 'xrelease', 'wait', 'notrack'}

# The mnemonics of ndisasm that no Intel / AMD manual has: Cyrix, ... Their
# opcodes are undefined for dismount (the unprefixed instruction with an
# ignored F2 / F3 prefix decodes instead)
NON_INTEL = {'paveb', 'pmagw', 'pdistib', 'psubsiw', 'pmvlzb', 'pmvzb', 'pmvnzb',
             'pmvgezb', 'paddsiw', 'pmulhrw', 'pmulhriw', 'pmachriw', 'rdshr',
             'wrshr', 'smint', 'smintold', 'rdm', 'dmint', 'svdc', 'rsdc', 'svldt',
             'rsldt', 'svts', 'rsts', 'umov', 'ibts', 'xbts', 'loadall',
             'loadall286', 'ud1', 'ud0', 'ud2b', 'icebp', 'int01', 'int1', 'int03',
             'syscall_', 'altinst'}


def nd_info(entries):
    """(valid, length, mnemonic, text) of the first instruction of a block: the
    ndisasm lines from the start of the block. A lone prefix (an unused prefix,
    or a REX prefix which ndisasm prints by itself) joins the instruction after
    it, the length is the sum."""
    if not entries:
        return (False, 0, '', '')
    pos = 0
    prefix_text = ''
    for off, length, text in entries:
        if off != pos:
            break
        valid, length, mnemonic, text = nd_line(length, text)
        if valid is None:
            pos += length
            prefix_text += text + ' '
            continue
        return (valid, pos + length, mnemonic, prefix_text + text)
    # Only prefixes before the end of the block
    return (None, pos, '', prefix_text)


def nd_line(length, text):
    words = text.split()
    while len(words) > 1 and words[0].lower().rstrip(':') in ND_PREFIXES:
        words.pop(0)
    mnemonic = words[0].lower() if words else ''
    if mnemonic in ('db', 'dw', 'dd', ''):
        return (False, length, mnemonic, text)
    if mnemonic.rstrip(':') in ND_PREFIXES or re.match(r'^rex(\.[wrxb]+)?$', mnemonic):
        return (None, length, mnemonic, text)
    return (True, length, mnemonic, text)


def process(args, rules, table, chunk, index, tmpdir):
    nd = ndisasm_chunk(args.ndisasm, args.bits, chunk, tmpdir, index)
    dis = rc.run_cli(args.cli, 'dis', args.bits, chunk)
    found = []
    for i, hexs in enumerate(chunk):
        dm = fl.parse_dis(dis[i])
        valid, nlen, nmnem, ntext = nd_info(nd.get(i))
        if valid and nmnem in NON_INTEL:
            continue
        if valid is None:
            continue
        cat = None
        if dm[0] == 'ERR':
            cat = 'x'
            detail = dm[1]
        elif dm[0] == 'OK':
            if not valid:
                cat = 'a'
            else:
                _, dmnem, _ = rc.split_prefixes(dm[2])
                if dm[1] != nlen:
                    cat = 'cl'
                elif not same_mnemonic(dmnem, nmnem):
                    cat = 'cm'
            detail = dm[2]
        else:
            if valid and nmnem in table:
                cat = 'b'
            detail = dm[2]
        if cat is None or allowed(rules, args.bits, cat, hexs):
            continue
        found.append((cat, hexs, detail, ntext, dm[1] if dm[0] != 'ERR' else 0, nlen))
    if args.nasm_check:
        found.extend(nasm_check(args, rules, chunk, dis, tmpdir, index))
    return found


def nasm_check(args, rules, chunk, dis, tmpdir, index):
    """(n) The text which dismount prints for a valid instruction must be
    accepted by nasm (the second oracle: ndisasm is lenient where nasm is not).
    Returns the findings."""
    texts = {}
    for i, hexs in enumerate(chunk):
        dm = fl.parse_dis(dis[i])
        if dm[0] != 'OK':
            continue
        texts.setdefault(rc.norm_text(dm[2]), []).append((hexs, dm[1], dm[2]))
    if not texts:
        return []
    sub = os.path.join(tmpdir, 'nasm%06d' % index)
    os.makedirs(sub)
    keys = sorted(texts)
    result = rc.nasm_assemble(args.nasm, keys, args.bits, sub)
    shutil.rmtree(sub, ignore_errors=True)
    found = []
    for key, (hexb, err) in zip(keys, result):
        if err is None:
            continue
        for hexs, dlen, detail in texts[key]:
            if not allowed(rules, args.bits, 'n', hexs):
                found.append(('n', hexs, detail, err, dlen, 0))
    return found


def group_key(cat, hexs, bits):
    """prefix, opcode, mod class, reg: the instruction group of a finding."""
    h = hexs
    pfx = ''
    while True:
        b = int(h[:2], 16)
        if b in (0x66, 0xf2, 0xf3) or (bits == 64 and 0x40 <= b <= 0x4f):
            pfx += h[:2]
            h = h[2:]
        else:
            break
    if h.startswith('0f38') or h.startswith('0f3a'):
        op, h = h[:6], h[6:]
    elif h.startswith('0f'):
        op, h = h[:4], h[4:]
    else:
        op, h = h[:2], h[2:]
    modrm = int(h[:2], 16)
    return (cat, pfx, op, 'reg' if modrm >> 6 == 3 else 'mem', (modrm >> 3) & 7)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bits', type=int, choices=(16, 32, 64), action='append',
                    help='operating mode (default: 16, 32 and 64)')
    ap.add_argument('--cli', default=rc.DEFAULT_CLI)
    ap.add_argument('--ndisasm', default='ndisasm')
    ap.add_argument('--nasm', default='nasm')
    ap.add_argument('--no-nasm-check', dest='nasm_check', action='store_false',
                    help='do not assemble the text of dismount with nasm (class n)')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--only', help='regex on the "prefix opcode modrm" hex of a block')
    ap.add_argument('--category', help='comma list of a,b,cl,cm,x,n to report')
    ap.add_argument('--allow', default=DEFAULT_ALLOW, help='the allowlist file')
    ap.add_argument('--show', type=int, default=60, help='groups to print per mode')
    ap.add_argument('--out', default=os.path.join(rc.REPO, 'out', 'validity'))
    args = ap.parse_args()

    if not os.path.exists(args.cli):
        print('dismount_cli not found: %s' % args.cli)
        return 2
    if shutil.which(args.ndisasm) is None:
        print('ndisasm not found on PATH')
        return 2
    only = re.compile(args.only) if args.only else None
    cats = set(args.category.split(',')) if args.category else None
    rules = load_allow(args.allow)
    os.makedirs(args.out, exist_ok=True)

    failed = False
    for bits in (args.bits or [16, 32, 64]):
        args.bits_now = bits
        table = rc.table_mnemonics(bits == 64)
        blocks = list(make_blocks(bits, only))
        chunks = [blocks[i:i + CHUNK] for i in range(0, len(blocks), CHUNK)]
        tmpdir = tempfile.mkdtemp(prefix='fuzz_validity_')
        sub = argparse.Namespace(**vars(args))
        sub.bits = bits
        found = []
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                futures = [pool.submit(process, sub, rules, table, c, i, tmpdir)
                           for i, c in enumerate(chunks)]
                for fut in futures:
                    found.extend(fut.result())
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)
        if cats is not None:
            found = [f for f in found if f[0] in cats]
        groups = collections.OrderedDict()
        for f in found:
            groups.setdefault(group_key(f[0], f[1], bits), []).append(f)
        path = os.path.join(args.out, 'validity%d.txt' % bits)
        with open(path, 'w') as out:
            for key, items in groups.items():
                for cat, hexs, detail, ntext, dlen, nlen in items:
                    out.write('%s %s  dismount[%d] "%s"  %s[%d] "%s"\n'
                              % (cat, hexs[:12], dlen, detail,
                                 'nasm' if cat == 'n' else 'ndisasm', nlen, ntext))
        counts = collections.Counter(f[0] for f in found)
        print('fuzz_validity %d-bit: %d blocks, a=%d b=%d length=%d mnemonic=%d err=%d nasm=%d, '
              '%d groups (details: %s)'
              % (bits, len(blocks), counts['a'], counts['b'], counts['cl'], counts['cm'],
                 counts['x'], counts['n'], len(groups), path))
        for n, (key, items) in enumerate(groups.items()):
            if n >= args.show:
                print('  ... %d more groups' % (len(groups) - n))
                break
            cat, hexs, detail, ntext, dlen, nlen = items[0]
            print('  (%s) %s %s %s /%d: %d x  e.g. %s  dismount "%s" [%d]  %s "%s" [%d]'
                  % (key[0], key[1] or '-', key[2], key[3], key[4], len(items),
                     hexs[:12], detail, dlen, 'nasm' if cat == 'n' else 'ndisasm',
                     ntext, nlen))
        if found:
            failed = True
    print('RESULT: %s' % ('FAIL' if failed else 'OK'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
