#!/usr/bin/env python3
"""The AArch32 corpus, tests/arm/corpus/<a32|t32>/<feature>.s (called by
gen_corpus.py --arch a32|t32, see tests/arm/TEST_PLAN.md).

The lines come from the encodings of the dismount tables (their fixed bits
with the operand bits all zero, all one, alternating and random) and from a
seeded sample of the random words which objdump decodes (a few per
mnemonic). The text of a line is the objdump text of the word, with the
branch targets written as offsets from the line (".+8"). The features of a
line are the smallest set of LLVM features with which dismount decodes it
(the lines dismount doesn't decode yet are written to out/compat-arm-<arch>/pending.txt, not in the corpus).

A T32 unit that needs an it block is a line with several instructions
separated by ';' ("it eq ; addeq r0, r1, r2").
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib

CORPUS = os.path.join(armlib.REPO, 'tests', 'arm', 'corpus')
CONDITIONS = ['eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le',
              'cs', 'cc']
_BRANCH = re.compile(r'^(b|bl|blx)(%s)?(\.w)? (0x[0-9a-f]+)$' % '|'.join(CONDITIONS))
_TARGET_LAST = re.compile(r'(?<![#\w-])(0x[0-9a-f]+)$')


def to_relative(text, address):
    """Replace the absolute target of a branch by '.+offset'."""
    m = _BRANCH.match(text)
    if not m:
        return text
    target = int(m.group(4), 16)
    offset = target - address
    sign = '+' if offset >= 0 else '-'
    return text[:text.rindex(' ') + 1] + '.%s%d' % (sign, abs(offset))


def candidate_words(rows, rng, per_entry_random=6):
    words = []
    for mask, value, feature, flags, size, mnemonic in rows:
        if size != 4:
            continue
        free = ~mask & 0xffffffff
        words.append(value)
        words.append(value | free)
        words.append(value | (free & 0x55555555))
        words.append(value | (free & 0xaaaaaaaa))
        for _ in range(per_entry_random):
            words.append(value | (rng.getrandbits(32) & free))
    return words


def base_names(rows):
    names = set()
    for row in rows:
        names.add(row[5].split('.')[0])
    return names


def key_of(text, names):
    """The mnemonic of a text without its condition and s: the table name."""
    head = text.split(' ')[0].split('.')[0]
    for length in range(len(head), 0, -1):
        if head[:length] in names:
            return head[:length]
    return head


def required_features(arch, texts, words, addresses, known_order):
    """For each decoded word the smallest list of features with which dismount
    decodes it to the same text: [] (base), ['v6'], ... None when no single
    feature does."""
    base = armlib.dis_words(arch, words, 'none', addresses)
    result = [[] if (b[0] == 'OK' and b[1] == t) else None for b, t in zip(base, texts)]
    for name in known_order:
        todo = [i for i, r in enumerate(result) if r is None]
        if not todo:
            break
        decoded = armlib.dis_words(arch, [words[i] for i in todo], 'none,+' + name,
                                   [addresses[i] for i in todo])
        for i, d in zip(todo, decoded):
            if d[0] == 'OK' and d[1] == texts[i]:
                result[i] = [name]
    # The lines that need two features (a conversion with the d16 - d31
    # registers: neon and fp16)
    todo = [i for i, r in enumerate(result) if r is None]
    for first in known_order:
        if not todo:
            break
        for second in known_order:
            if second == first:
                continue
            decoded = armlib.dis_words(arch, [words[i] for i in todo],
                                       'none,+%s,+%s' % (first, second), [addresses[i] for i in todo])
            for i, d in zip(todo, decoded):
                if d[0] == 'OK' and d[1] == texts[i]:
                    result[i] = [first, second]
            todo = [i for i in todo if result[i] is None]
            if not todo:
                break
    return result


def generate(arch, args):
    if arch == 't32':
        import compat_t32
        return compat_t32.generate(arch, args)
    rng = armlib.seeded_rng(20240607)
    rows = armlib.entries(arch)
    names = base_names(rows)
    sources = set(candidate_words(rows, rng))
    for _ in range(args.random):
        sources.add(rng.getrandbits(32))
    sources = sorted(sources)
    table_words = set(candidate_words(rows, armlib.seeded_rng(20240607)))

    texts, addresses = armlib.objdump_chunks(sources, arch)
    seen = collections.Counter()
    chosen = []        # (line, word, address, original text)
    seen_text = set()
    for word, address, text in zip(sources, addresses, texts):
        if text == '<unknown>':
            continue
        key = key_of(text, names)
        if word not in table_words:
            if seen[key] >= args.per_mnemonic:
                continue
            seen[key] += 1
        line = to_relative(text, address)
        if line in seen_text:
            continue
        seen_text.add(line)
        chosen.append((line, word, address, text))

    # What clang makes of each line, as the checks of run_compat see it: the
    # text has to survive (clang's bytes disassemble to the same text), and the
    # features of a line are the ones of those bytes
    results = armlib.clang_assemble([c[0] for c in chosen], arch)
    kept = []
    dropped = collections.Counter()
    words2 = []
    for (line, word, address, text), (status, data) in zip(chosen, results):
        if status != 'ok':
            dropped[data.split(' ')[0] if data else 'error'] += 1
            continue
        kept.append(line)
        words2.append(armlib.hex_bytes_to_word(data))
    texts2, addresses2 = armlib.objdump_chunks(words2, arch)
    stable = []
    for line, word, text, address in zip(kept, words2, texts2, addresses2):
        if to_relative(text, address) != line:
            dropped['unstable'] += 1
            continue
        stable.append((line, word, text, address))

    order = []
    for _index, name, _isa, _support, _entries in armlib.features(arch):
        if name != '-' and name not in order:
            order.append(name)
    # The architecture versions first, in their order: the smallest feature
    versions = ['v5t', 'v5te', 'v6', 'v6m', 'v6k', 'v6t2', 'v7', 'mp', 'hwdiv', 'hwdiv-arm', 'v8',
                'vfp2', 'vfp3', 'neon', 'vfp4', 'fp-armv8']
    order = [v for v in versions if v in order] + [n for n in order if n not in versions]
    needed = required_features(arch, [c[2] for c in stable], [c[1] for c in stable],
                               [c[3] for c in stable], order)
    files = collections.defaultdict(list)
    pending = []
    for (line, word, text, address), required in zip(stable, needed):
        if required is None:
            pending.append(line)
            continue
        name = '+'.join(required) if required else 'base'
        files[name].append((line, required))

    out_dir = os.path.join(CORPUS, arch)
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.endswith('.s'):
            os.unlink(os.path.join(out_dir, name))
    total = 0
    for name, lines in sorted(files.items()):
        with open(os.path.join(out_dir, name + '.s'), 'w') as f:
            f.write('// Generated by tests/arm/tools/gen_corpus.py. One instruction per line;\n')
            f.write('// "// @feature: x" is the LLVM feature the line needs (none: base).\n')
            for line, required in lines:
                if required:
                    f.write('%s // @feature: %s\n' % (line, ' '.join(required)))
                else:
                    f.write('%s\n' % line)
        total += len(lines)
        print('%-24s %d' % (name + '.s', len(lines)))
    os.makedirs(os.path.join(armlib.REPO, 'out', 'compat-arm-' + arch), exist_ok=True)
    with open(os.path.join(armlib.REPO, 'out', 'compat-arm-' + arch, 'pending.txt'), 'w') as f:
        f.write('\n'.join(pending) + '\n')
    print('%d lines; %d not decoded by dismount yet; clang refused %s' % (
        total, len(pending), dict(dropped)))
