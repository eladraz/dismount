#!/usr/bin/env python3
"""The T32 corpus and its checks (tests/arm/TEST_PLAN.md). The instructions
are 2 or 4 bytes, so a corpus line is a list of units: one instruction, or
several separated by ';' when they need an it block ("it eq ; addeq r0, r1,
r2"). gen_corpus_aarch32.generate() and run_compat.run_file() call this for
t32.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib
import fuzz_decode

CORPUS = os.path.join(armlib.REPO, 'tests', 'arm', 'corpus')
CONDITIONS = ['eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le',
              'cs', 'cc']
_BRANCH = re.compile(r'^(b|bl|blx|cbz|cbnz)(%s)?(\.w)? (r\d+, )?(0x[0-9a-f]+)$' % '|'.join(CONDITIONS))
_PCOUNT = re.compile(r'0x[0-9a-f]+$')


def to_relative(text, address):
    """Replace the absolute target of a branch by '.+offset'."""
    m = _BRANCH.match(text)
    if not m:
        return text
    offset = int(m.group(5), 16) - address
    if offset >= 1 << 31:
        offset -= 1 << 32
    sign = '+' if offset >= 0 else '-'
    return text[:text.rindex(' ') + 1] + '.%s%d' % (sign, abs(offset))


def split_units(data):
    """The units of the bytes of an assembled line."""
    units = []
    i = 0
    while i + 1 < len(data):
        hw1 = data[i] | (data[i + 1] << 8)
        if armlib.t32_size(hw1) == 4 and i + 3 < len(data):
            units.append((4, (hw1 << 16) | data[i + 2] | (data[i + 3] << 8)))
            i += 4
        else:
            units.append((2, hw1))
            i += 2
    return units


def unit_bytes(unit):
    size, value = unit
    if size == 2:
        return bytes([value & 0xff, value >> 8])
    return bytes([(value >> 16) & 0xff, value >> 24, value & 0xff, (value >> 8) & 0xff])


def objdump_lines(unit_lists, mattr=None):
    """objdump the units of the lines (one stream). Return (texts of the lines,
    the address of each line); a text is the texts of the units joined with
    ' ; '. The streams are cut every 40000 units, the addresses restart."""
    texts = []
    addresses = []
    start = 0
    while start < len(unit_lists):
        part = []
        count = 0
        while start + len(part) < len(unit_lists) and count < 40000:
            part.append(unit_lists[start + len(part)])
            count += len(part[-1])
        flat = [u for units in part for u in units]
        unit_texts, unit_addresses, _ = armlib.objdump_units(flat, mattr)
        position = 0
        for units in part:
            texts.append(' ; '.join(unit_texts[position:position + len(units)]))
            addresses.append(unit_addresses[position])
            position += len(units)
        start += len(part)
    return texts, addresses


def dis_lines(unit_lists, addresses, mattr=None):
    """dismount's disassembly of the lines, each one a sequence of units."""
    args = ['dis', '--arch', 't32']
    if mattr:
        args.extend(['--mattr', mattr])
    lines = ['@%x %s' % (a, ' ; '.join(armlib.t32_hex(u) for u in units))
             for units, a in zip(unit_lists, addresses)]
    return armlib._parse_dis(armlib.cli_lines(args, lines))


# ---------------------------------------------------------------------------
# The corpus

def candidate_units(rows, rng, per_entry_random=6):
    units = []
    for mask, value, feature, flags, size, mnemonic in rows:
        if size == 2:
            free = ~mask & 0xffff
            patterns = [value, value | free, value | (free & 0x5555), value | (free & 0xaaaa)]
            patterns += [value | (rng.getrandbits(16) & free) for _ in range(per_entry_random)]
            for v in patterns:
                units.append((2, v & 0xffff))
        else:
            free = ~mask & 0xffffffff
            patterns = [value, value | free, value | (free & 0x55555555), value | (free & 0xaaaaaaaa)]
            patterns += [value | (rng.getrandbits(32) & free) for _ in range(per_entry_random)]
            for v in patterns:
                units.append(armlib.t32_unit(v & 0xffffffff))
    return units


def base_names(rows):
    return set(row[5].split('.')[0] for row in rows)


def key_of(text, names):
    head = text.split(' ')[0].split('.')[0]
    for length in range(len(head), 0, -1):
        if head[:length] in names:
            return head[:length]
    return head


def generate(arch, args):
    rng = armlib.seeded_rng(20240607)
    rows = armlib.entries('t32')
    names = base_names(rows)
    sources = set(candidate_units(rows, rng))
    table_units = set(candidate_units(rows, armlib.seeded_rng(20240607)))
    for u in fuzz_decode.generate_t32(args.random, 20240607, rows):
        sources.add(u)
    sources = sorted(u for u in sources if not armlib.t32_is_it(u))

    texts, addresses = armlib.objdump_unit_chunks(sources)
    seen = collections.Counter()
    chosen = []        # (line text, units, address)
    seen_text = set()
    for unit, address, text in zip(sources, addresses, texts):
        if text == '<unknown>':
            continue
        key = key_of(text, names)
        if unit not in table_units:
            if seen[key] >= args.per_mnemonic:
                continue
            seen[key] += 1
        line = to_relative(text, address)
        if line in seen_text:
            continue
        seen_text.add(line)
        chosen.append((line, [unit], address))

    # The it blocks: the instructions which are conditional in a block
    pool = [u for u, t in zip(sources, texts) if t != '<unknown>' and not t.startswith('<')]
    block_lines = []
    for _ in range(min(args.random // 10, 12000)):
        firstcond = rng.randrange(14)
        mask = rng.randrange(1, 16)
        count = 4 - ((mask & -mask).bit_length() - 1)
        seq = [(2, 0xbf00 | (firstcond << 4) | mask)]
        seq += [rng.choice(pool) for _ in range(count)]
        block_lines.append(seq)
    block_texts, block_addresses = objdump_lines(block_lines)
    block_seen = collections.Counter()
    for seq, text, address in zip(block_lines, block_texts, block_addresses):
        parts = text.split(' ; ')
        # One of each shape: the mnemonic of the second instruction, the number
        # of instructions
        key = (key_of(parts[1].replace(' ', ''), names) if len(parts) > 1 else '', len(parts))
        if block_seen[key] >= 2 or any('<' in p for p in parts):
            continue
        block_seen[key] += 1
        offset = address
        relative = []
        for p, u in zip(parts, seq):
            relative.append(to_relative(p, offset))
            offset += u[0]
        line = ' ; '.join(relative)
        if line in seen_text:
            continue
        seen_text.add(line)
        chosen.append((line, seq, address))

    # What clang makes of each line: the text has to survive
    results = armlib.clang_assemble([c[0] for c in chosen], 't32')
    kept = []
    dropped = collections.Counter()
    for (line, units, address), (status, data) in zip(chosen, results):
        if status != 'ok':
            dropped[data.split(' ')[0] if data else 'error'] += 1
            continue
        kept.append((line, split_units(data)))
    texts2, addresses2 = objdump_lines([k[1] for k in kept])
    stable = []
    for (line, units), text, address in zip(kept, texts2, addresses2):
        rebuilt = ' ; '.join(to_relative(p, a) for p, a in zip(text.split(' ; '), unit_addresses(units, address)))
        if rebuilt != line:
            dropped['unstable'] += 1
            continue
        stable.append((line, units, text, address))

    # The features: the smallest set with which dismount decodes the line
    order = []
    for _index, name, _isa, _support, _entries in armlib.features('t32'):
        if name != '-' and name not in order:
            order.append(name)
    versions = ['v5t', 'v5te', 'v6', 'v6m', 'v6k', 'v6t2', 'v7', 'mp', 'hwdiv', 'hwdiv-arm', 'v8',
                'vfp2', 'vfp3', 'neon', 'vfp4', 'fp-armv8']
    order = [v for v in versions if v in order] + [n for n in order if n not in versions]
    needed = required_features([s[1] for s in stable], [s[2] for s in stable],
                               [s[3] for s in stable], order)
    files = collections.defaultdict(list)
    pending = []
    for (line, units, text, address), required in zip(stable, needed):
        if required is None:
            pending.append(line)
            continue
        files['+'.join(required) if required else 'base'].append((line, required))

    out_dir = os.path.join(CORPUS, 't32')
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.endswith('.s'):
            os.unlink(os.path.join(out_dir, name))
    total = 0
    for name, lines in sorted(files.items()):
        with open(os.path.join(out_dir, name + '.s'), 'w') as f:
            f.write('// Generated by tests/arm/tools/gen_corpus.py. One instruction per line;\n')
            f.write('// "// @feature: x" is the LLVM feature the line needs (none: base).\n')
            f.write('// Several instructions separated by ";" are an it block.\n')
            for line, required in lines:
                f.write('%s // @feature: %s\n' % (line, ' '.join(required)) if required else '%s\n' % line)
        total += len(lines)
        print('%-24s %d' % (name + '.s', len(lines)))
    os.makedirs(os.path.join(armlib.REPO, 'out', 'compat-arm'), exist_ok=True)
    with open(os.path.join(armlib.REPO, 'out', 'compat-arm', 'pending-t32.txt'), 'w') as f:
        f.write('\n'.join(pending) + '\n')
    print('%d lines; %d not decoded by dismount yet; clang refused %s' % (
        total, len(pending), dict(dropped)))


def unit_addresses(units, address):
    result = []
    for u in units:
        result.append(address)
        address += u[0]
    return result


def required_features(unit_lists, texts, addresses, known_order):
    base = dis_lines(unit_lists, addresses, 'none')
    result = [[] if (b[0] == 'OK' and b[1] == t) else None for b, t in zip(base, texts)]
    for name in known_order:
        todo = [i for i, r in enumerate(result) if r is None]
        if not todo:
            break
        decoded = dis_lines([unit_lists[i] for i in todo], [addresses[i] for i in todo],
                            'none,+' + name)
        for i, d in zip(todo, decoded):
            if d[0] == 'OK' and d[1] == texts[i]:
                result[i] = [name]
    # The units made of several instructions need the features of each of them:
    # the smallest prefix of the ordered list of the features
    todo = [i for i, r in enumerate(result) if r is None]
    for count in range(2, len(known_order) + 1):
        if not todo:
            break
        decoded = dis_lines([unit_lists[i] for i in todo], [addresses[i] for i in todo],
                            'none,' + ','.join('+' + n for n in known_order[:count]))
        for i, d in zip(todo, decoded):
            if d[0] == 'OK' and d[1] == texts[i]:
                result[i] = list(known_order[:count])
        todo = [i for i in todo if result[i] is None]
    return result


# ---------------------------------------------------------------------------
# The checks of run_compat.py

def result(status, error=None):
    r = {'status': status}
    if error:
        r['error'] = error
    return r


def mattr_for(required):
    return 'none,' + ','.join('+' + f for f in required) if required else 'none'


def hex_to_bytes(text):
    return bytes.fromhex(text) if isinstance(text, str) else text


def run_file(name, lines, known_features, wanted_features):
    texts = [t for t, _ in lines]
    reference = armlib.clang_assemble(texts, 't32')
    ok = [i for i, r in enumerate(reference) if r[0] == 'ok']
    ref_units = {}
    for i in ok:
        ref_units[i] = split_units(reference[i][1])
    # objdump over the stream of the lines which clang assembled
    ref_dis = {}
    addresses = {}
    if ok:
        listing, listing_addresses = objdump_lines([ref_units[i] for i in ok])
        for i, t, a in zip(ok, listing, listing_addresses):
            ref_dis[i] = t
            addresses[i] = a
    order_addresses = [addresses.get(i, armlib.BASE) for i in range(len(lines))]
    ours = armlib.asm_lines('t32', texts, None, order_addresses)
    our_dis_list = dis_lines([ref_units[i] for i in ok], [addresses[i] for i in ok]) if ok else []
    our_dis = dict(zip(ok, our_dis_list))

    # The round trip: the text of dismount assembled by dismount
    rt_index = [i for i in ok if our_dis[i][0] == 'OK']
    rt_asm = armlib.asm_lines('t32', [our_dis[i][1] for i in rt_index], None,
                              [addresses[i] for i in rt_index])
    rt_result = dict(zip(rt_index, rt_asm))
    # The ALT checks: the text of the bytes dismount assembled
    alt_index = [i for i in ok if ours[i][0] == 'OK' and ours[i][1] != reference[i][1]]
    alt_texts = {}
    if alt_index:
        alt_dis = dis_lines([split_units(ours[i][1]) for i in alt_index], [addresses[i] for i in alt_index])
        alt_texts = dict(zip(alt_index, alt_dis))
    # The features
    feat_none = dict(zip(ok, dis_lines([ref_units[i] for i in ok], [addresses[i] for i in ok], 'none'))) if ok else {}
    feat_needed = {}
    by_required = collections.defaultdict(list)
    for i in ok:
        required = lines[i][1]
        if required and all(f in known_features for f in required):
            by_required[tuple(required)].append(i)
    for required, indexes in by_required.items():
        decoded = dis_lines([ref_units[i] for i in indexes], [addresses[i] for i in indexes],
                            mattr_for(list(required)))
        for i, d in zip(indexes, decoded):
            feat_needed[i] = d

    records = []
    for i, (text, required) in enumerate(lines):
        record = {'file': name + '.s', 'line': i + 1, 'source': text, 'features': required}
        ref_status, ref_data = reference[i]
        if ref_status != 'ok':
            for check in ('asm', 'dis', 'rt', 'feat'):
                record[check] = result('SKIP', 'clang: ' + ref_data)
            records.append(record)
            continue
        status, data = ours[i]
        if status == 'OK' and data == ref_data:
            record['asm'] = result('PASS')
        elif status == 'OK':
            alt = alt_texts.get(i)
            if alt and alt[0] == 'OK' and alt[1] == ref_dis[i]:
                record['asm'] = result('ALT', 'bytes %s' % data.hex())
            else:
                record['asm'] = result('FAIL', 'got %s want %s' % (data.hex(), ref_data.hex()))
        else:
            record['asm'] = result('FAIL', data)
        status, dtext = our_dis[i]
        if status == 'OK' and dtext == ref_dis[i]:
            record['dis'] = result('PASS')
        elif status == 'OK':
            record['dis'] = result('FAIL', 'got "%s" want "%s"' % (dtext, ref_dis[i]))
        else:
            record['dis'] = result('FAIL', '%s want "%s"' % (status, ref_dis[i]))
        if i in rt_result:
            rstatus, rdata = rt_result[i]
            if rstatus == 'OK' and rdata == ref_data:
                record['rt'] = result('PASS')
            elif rstatus == 'OK':
                other = dis_lines([split_units(rdata)], [addresses[i]])[0]
                if other[0] == 'OK' and other[1] == our_dis[i][1]:
                    record['rt'] = result('ALT')
                else:
                    record['rt'] = result('FAIL', 'text "%s" gives %s' % (our_dis[i][1], rdata.hex()))
            else:
                record['rt'] = result('FAIL', 'text "%s": %s' % (our_dis[i][1], rdata))
        else:
            record['rt'] = result('SKIP', 'not decoded')
        if required and not all(f in known_features for f in required):
            record['feat'] = result('SKIP', 'no feature for ' + ' '.join(required))
        else:
            none = feat_none[i]
            none_status = none[0]
            if none_status == 'OK' and none[1] != ref_dis[i]:
                none_status = 'OTHER'
            if required:
                needed = feat_needed.get(i)
                if none_status == 'OK':
                    record['feat'] = result('FAIL', 'decoded without %s' % ' '.join(required))
                elif needed is None or needed[0] != 'OK' or needed[1] != ref_dis[i]:
                    record['feat'] = result('FAIL', 'not decoded with %s' % ' '.join(required))
                else:
                    record['feat'] = result('PASS')
            else:
                if none_status == 'OK' and none[1] == ref_dis[i]:
                    record['feat'] = result('PASS')
                else:
                    record['feat'] = result('FAIL', 'base line not decoded without features')
        records.append(record)
    return records
