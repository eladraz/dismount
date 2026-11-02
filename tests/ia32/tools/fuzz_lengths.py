#!/usr/bin/env python3
"""Random byte-sequence fuzzer for the IA-32 length decoder.

Every sequence of --count random 1-15 byte strings is decoded twice: by
`dismount_cli dis` (first instruction only, see dismount_cli.cpp) and by
`ndisasm -b 32` (or -b 16). The result is classified as follows.

(a) Exceptions. An `ERR` line from dismount (CHECK failures, unknown
    exceptions, a crashed CLI) is always a bug. The one exception is
    "truncated instruction": that is the right answer when the bytes stop
    inside an instruction. It is counted as a bug only when ndisasm decodes a
    complete known instruction from the same bytes, because then the bytes
    were enough.

(b) Length disagreements. ndisasm decodes a known instruction whose mnemonic
    is in dismount's tables, and dismount decodes a different length. Skipped:
    ndisasm `db` (undecodable), prefix-only lines (`lock`, `o16`, `cs`, ...),
    mnemonics that dismount does not implement, and VEX/EVEX lines.
    ndisasm folds a leading 9B (fwait) into the next instruction ("9B D9 7D"
    is one ndisasm line). dismount decodes the 9B as `fwait` (1 byte), so for
    those lines the rest of the sequence (bytes[1:]) is decoded and its length
    must be ndisasm's length minus one. Prefixes are part of the instruction
    in both decoders, so their length is compared as is.

Information only (not a failure): dismount says INVALID where ndisasm decodes
a known instruction. That is an unimplemented or undefined encoding, and it is
counted, not failed.

Exit status: 1 when (a) or (b) is non-empty, else 0.

Usage:
    python3 tests/ia32/tools/fuzz_lengths.py --count 50000 --seed 1
    python3 tests/ia32/tools/fuzz_lengths.py --bits 16 --count 5000 --cli out/bin/dismount_cli
"""

import argparse
import concurrent.futures
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TESTS_DIR)
# run_compat supplies the dismount CLI driver, the table mnemonics and the
# prefix-word list, so the two harnesses cannot disagree about them
import run_compat as rc  # noqa: E402

MAX_LEN = 15
ERR_LIMIT = 25  # samples printed per category

NDIS_HEAD = re.compile(r'^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)\s+(.*?)\s*$')
NDIS_CONT = re.compile(r'^\s+-([0-9A-Fa-f]+)\s*$')


def parse_ndisasm(output):
    """Returns (length, mnemonic, text) of the first ndisasm line, or None when
    the output is not a line. The hex column may wrap onto '-HEX' lines."""
    lines = output.splitlines()
    head = None
    hexcol = ''
    for idx, raw in enumerate(lines):
        m = NDIS_HEAD.match(raw)
        if m:
            head = m
            hexcol = m.group(2)
            for cont in lines[idx + 1:]:
                c = NDIS_CONT.match(cont)
                if not c:
                    break
                hexcol += c.group(1)
            break
    if head is None:
        return None
    text = head.group(3)
    words = text.split()
    prefixes = []
    # ndisasm shows a 9B that it folded into the next instruction as 'wait'
    # in front of that instruction ("wait pop ss")
    while len(words) > 1 and words[0].lower().rstrip(':') in (rc.PREFIX_WORDS | {'wait'}):
        prefixes.append(words.pop(0))
    mnemonic = words[0].lower() if words else ''
    if not words:
        mnemonic = ''
    return len(hexcol) // 2, mnemonic, text, prefixes


def ndisasm_one(ndis, bits, data, tmpdir, index):
    path = os.path.join(tmpdir, 'seq%06d.bin' % index)
    with open(path, 'wb') as f:
        f.write(data)
    res = subprocess.run([ndis, '-b', str(bits), '-o', '0', path],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    os.remove(path)
    return parse_ndisasm(res.stdout)


def dismount_lengths(cli, bits, hexes):
    """Returns the CLI output line per hex input (run_compat.run_cli)."""
    return rc.run_cli(cli, 'dis', bits, hexes)


def parse_dis(line):
    """('OK'|'INVALID', length, text) or ('ERR', message)."""
    m = re.match(r'^(OK|INVALID) (\d+) ?(.*)$', line)
    if m:
        return (m.group(1), int(m.group(2)), m.group(3))
    return ('ERR', line)


def is_truncated(message):
    return message.startswith('ERR truncated')


def implemented(nd, table):
    """True when ndisasm decoded a real instruction whose mnemonic dismount
    implements (ndisasm's 'wait' is dismount's 'fwait')."""
    if nd is None or is_prefix_only(nd):
        return False
    mnemonic = nd[1]
    if mnemonic == 'wait':
        mnemonic = 'fwait'
    return mnemonic in table


def is_prefix_only(nd):
    """ndisasm printed only a prefix (lock, o16, cs, ...): not an instruction."""
    return nd is not None and nd[1].rstrip(':') in rc.PREFIX_WORDS


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--count', type=int, default=5000, help='number of random sequences')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--bits', type=int, choices=(16, 32), default=32)
    ap.add_argument('--cli', default=rc.DEFAULT_CLI, help='dismount_cli binary')
    ap.add_argument('--ndisasm', default='ndisasm')
    ap.add_argument('--jobs', type=int, default=8, help='parallel ndisasm runs')
    ap.add_argument('--show', type=int, default=ERR_LIMIT, help='samples to print per category')
    args = ap.parse_args()

    cli = os.path.abspath(args.cli)
    if not os.path.exists(cli):
        print('dismount_cli not found: %s (build with DISMOUNT_BUILD_TESTS=ON)' % cli)
        return 2
    if shutil.which(args.ndisasm) is None:
        print('ndisasm not found on PATH')
        return 2

    rng = random.Random(args.seed)
    seqs = []
    for _ in range(args.count):
        n = rng.randint(1, MAX_LEN)
        seqs.append(bytes(rng.randrange(256) for _ in range(n)))
    hexes = [s.hex() for s in seqs]
    table = rc.table_mnemonics()

    dis_out = dismount_lengths(cli, args.bits, hexes)
    tmpdir = tempfile.mkdtemp(prefix='fuzz_lengths_')
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            nd_results = list(pool.map(
                lambda i: ndisasm_one(args.ndisasm, args.bits, seqs[i], tmpdir, i),
                range(len(seqs))))

        # Second pass: a leading 9B that ndisasm folded into the next instruction
        rest_index = []
        rest_hex = []
        for i, data in enumerate(seqs):
            nd = nd_results[i]
            if data[0:1] == b'\x9b' and len(data) > 1 and implemented(nd, table) \
                    and 'wait' in nd[3]:
                rest_index.append(i)
                rest_hex.append(data[1:].hex())
        rest_out = dismount_lengths(cli, args.bits, rest_hex) if rest_hex else []
        rest_by_index = dict(zip(rest_index, rest_out))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    counts = {'compared': 0, 'agree': 0, 'skip_db': 0, 'skip_prefix_only': 0,
              'skip_not_impl': 0, 'skip_vex': 0, 'trunc_ok': 0,
              'invalid_known': 0, 'fwait_merged': 0,
              'bug_exception': 0, 'bug_truncated_known': 0, 'bug_length': 0}
    samples = {'bug_exception': [], 'bug_truncated_known': [], 'bug_length': [],
               'invalid_known': []}

    def sample(kind, text):
        if len(samples[kind]) < args.show:
            samples[kind].append(text)

    for i, data in enumerate(seqs):
        hexs = hexes[i]
        dm = parse_dis(dis_out[i])
        nd = nd_results[i]
        nd_text = nd[2] if nd else '(no ndisasm line)'
        if dm[0] == 'ERR':
            if not is_truncated(dm[1]):
                counts['bug_exception'] += 1
                sample('bug_exception', '%s  %s' % (hexs, dm[1]))
            elif implemented(nd, table):
                counts['bug_truncated_known'] += 1
                sample('bug_truncated_known', '%s  ndisasm "%s" len %d; dismount: %s'
                       % (hexs, nd_text, nd[0], dm[1]))
            else:
                counts['trunc_ok'] += 1
            continue
        if nd is None or is_prefix_only(nd):
            counts['skip_prefix_only' if is_prefix_only(nd) else 'skip_db'] += 1
            continue
        if nd[1] in ('db', '', 'dd', 'dw'):
            counts['skip_db'] += 1
            continue
        if not implemented(nd, table):
            if re.match(r'^(v|k)[a-z]', nd[1]):
                counts['skip_vex'] += 1
            else:
                counts['skip_not_impl'] += 1
            continue
        length_nd = nd[0]
        if dm[0] == 'INVALID':
            counts['invalid_known'] += 1
            sample('invalid_known', '%s  ndisasm "%s"' % (hexs, nd_text))
            continue

        if i in rest_by_index:
            # ndisasm merged 9B into the next instruction: dismount reads the
            # 9B as fwait, so the remaining bytes must have ndisasm's length - 1
            counts['fwait_merged'] += 1
            rest = parse_dis(rest_by_index[i])
            if rest[0] == 'ERR':
                if is_truncated(rest[1]):
                    counts['bug_truncated_known'] += 1
                    sample('bug_truncated_known', '%s (after 9B) ndisasm "%s"; dismount: %s'
                           % (hexs, nd_text, rest[1]))
                else:
                    counts['bug_exception'] += 1
                    sample('bug_exception', '%s (after 9B)  %s' % (hexs, rest[1]))
                continue
            if rest[0] == 'INVALID':
                counts['invalid_known'] += 1
                sample('invalid_known', '%s (after 9B)  ndisasm "%s"' % (hexs, nd_text))
                continue
            counts['compared'] += 1
            if rest[1] == length_nd - 1:
                counts['agree'] += 1
            else:
                counts['bug_length'] += 1
                sample('bug_length', '%s  ndisasm "%s" len %d; dismount after 9B: len %d "%s"'
                       % (hexs, nd_text, length_nd, rest[1], rest[2]))
            continue

        counts['compared'] += 1
        if dm[1] == length_nd:
            counts['agree'] += 1
        else:
            counts['bug_length'] += 1
            sample('bug_length', '%s  ndisasm "%s" len %d; dismount len %d "%s"'
                   % (hexs, nd_text, length_nd, dm[1], dm[2]))

    print('fuzz_lengths: %d sequences, seed %d, %d-bit, cli %s'
          % (args.count, args.seed, args.bits, cli))
    print('  compared (ndisasm known, dismount implements): %d, lengths agree: %d'
          % (counts['compared'], counts['agree']))
    print('  skipped: ndisasm db %d, prefix-only %d, mnemonic not implemented %d, '
          'VEX/EVEX %d' % (counts['skip_db'], counts['skip_prefix_only'],
                          counts['skip_not_impl'], counts['skip_vex']))
    print('  9B folded by ndisasm (second pass): %d; truncated at end of input, '
          'ndisasm not a complete instruction (ok): %d'
          % (counts['fwait_merged'], counts['trunc_ok']))
    print('  info: dismount INVALID where ndisasm decodes a known instruction: %d'
          % counts['invalid_known'])
    print('(a) dismount exceptions / ERR: %d' % counts['bug_exception'])
    print('(a) truncated where ndisasm decodes a complete instruction: %d'
          % counts['bug_truncated_known'])
    print('(b) length disagreements: %d' % counts['bug_length'])
    for kind, label in (('bug_exception', '(a)'), ('bug_truncated_known', '(a)'),
                        ('bug_length', '(b)'), ('invalid_known', 'info')):
        for line in samples[kind]:
            print('  %s %s' % (label, line))

    failed = counts['bug_exception'] + counts['bug_truncated_known'] + counts['bug_length']
    print('RESULT: %s' % ('FAIL' if failed else 'OK'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
