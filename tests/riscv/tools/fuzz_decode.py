#!/usr/bin/env python3
"""Random encodings: dismount vs llvm-objdump (valid / invalid, size, text), for
several feature sets and both formats (default and -M no-aliases).

  python3 tests/riscv/tools/fuzz_decode.py [--count N] [--seed S] [--sets all,rva23u64,...]
                                           [--bits 32|64] [--formats default,no-aliases]

Half of the units are random 16 / 32 bit values, half are made from a random
entry of the encoding tables (its fixed bits, random fields), so the valid
space is covered. The units are placed in one stream, as a linear sweep:
dismount_cli sweep (the fault tolerance on) against llvm-objdump -d.

--class scalar (default) skips the instructions of the vector extension (an
llvm-objdump text which starts with 'v') and draws the units from the scalar
entries; --class vector draws them from the vector entries (random fields,
flipped bits, the reserved fields of the OP-V, vector load and vector store
opcodes) and compares everything; --class all mixes both.

Exit status 1 if any unit differs.
"""
import argparse
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rvlib

SETS = {
    'all': ('all', None),
    'rva23u64': ('rva23u64', None),
    'rva23s64': ('rva23s64', None),
    'rv64gc': ('rv64gc', None),
    'rv32gc': ('rv32gc', None),
    'rv64i': ('rv64i', None),
    'rv32i': ('rv32i', None),
    'rv64gcv': ('rv64gcv', None),
    'rv32gcv': ('rv32gcv', None),
    'zve32x': ('none,+zve32x', None),
    'zve64d': ('none,+zve64d', None),
}

CHUNK = 4000


def cli(args, lines):
    r = rvlib.run([rvlib.CLI] + args, '\n'.join(lines) + '\n')
    return r.stdout.splitlines()


def feature_list(bits, mattr):
    """The LLVM feature list of a dismount feature set (dismount_cli set)."""
    out = cli(['set', '--arch', 'rv%d' % bits, '--mattr', mattr], [''])
    names = out[0].split()
    features = [n for n in names if n not in ('zcf',) or bits == 32]
    return features


def read_entries(bits):
    entries = []
    for line in cli(['entries', '--arch', 'rv%d' % bits], ['']):
        mask, value, feature, flags, size, mnemonic = line.split()
        entries.append((int(mask, 16), int(value, 16), int(feature), int(flags, 16), int(size), mnemonic))
    return entries


def is_vector_entry(entry):
    return entry[5].startswith('v')


def split_entries(entries):
    """(the scalar entries, the vector entries). The vector entries are those
    with a mnemonic of the vector extension (vsetvli is one too)."""
    vector = [e for e in entries if is_vector_entry(e)]
    scalar = [e for e in entries if not is_vector_entry(e)]
    return scalar, vector


def random_vector_unit(rng, vector):
    """A 32 bit unit near the vector encodings."""
    kind = rng.random()
    if kind < 0.3:
        mask, value, feature, flags, size, mnemonic = rng.choice(vector)
        return 4, ((value & mask) | (rng.getrandbits(32) & ~mask)) & 0xffffffff
    if kind < 0.5:
        # flipped bits of the fixed part: the neighbours
        mask, value, feature, flags, size, mnemonic = rng.choice(vector)
        bitsv = (value & mask) | (rng.getrandbits(32) & ~mask)
        positions = [i for i in range(2, 32) if (mask >> i) & 1]
        for _ in range(rng.randint(1, 3)):
            bitsv ^= 1 << rng.choice(positions)
        return 4, bitsv & 0xffffffff
    if kind < 0.75:
        # OP-V: any funct3 and funct6
        return 4, (rng.getrandbits(32) & ~0x7f) | 0x57
    if kind < 0.9:
        # the vector loads and stores, and the scalar floating point ones
        width = rng.choice((0, 5, 6, 7, 5, 6, 7, 1, 2, 3, 4))
        major = rng.choice((0x07, 0x27))
        return 4, (rng.getrandbits(32) & ~0x707f) | major | (width << 12)
    if kind < 0.95:
        # vsetvli / vsetivli / vsetvl
        return 4, (rng.getrandbits(32) & ~0x707f) | 0x57 | (7 << 12)
    return 4, rng.getrandbits(32) | 3


def random_unit(rng, entries, bits, cls='scalar'):
    if cls != 'scalar':
        scalar, vector = split_entries(entries)
        if cls == 'vector' or rng.random() < 0.5:
            if rng.random() < 0.97:
                return random_vector_unit(rng, vector)
        entries = scalar
    else:
        entries = split_entries(entries)[0]
    kind = rng.random()
    if kind < 0.2:
        # an entry with 1-3 flipped bits of its fixed part: the neighbours
        # (the reserved encodings, the overlaps of the entries)
        mask, value, feature, flags, size, mnemonic = rng.choice(entries)
        bitsv = (value & mask) | (rng.getrandbits(32) & ~mask)
        positions = [i for i in range(2, 16 if size == 2 else 32) if (mask >> i) & 1]
        for _ in range(rng.randint(1, 3)):
            if positions:
                bitsv ^= 1 << rng.choice(positions)
        if size == 2:
            return 2, bitsv & 0xffff
        return 4, bitsv & 0xffffffff
    if kind < 0.5:
        mask, value, feature, flags, size, mnemonic = rng.choice(entries)
        # RV32 only / RV64 only entries are fine: both sides decide
        fill = rng.getrandbits(32)
        bitsv = (value & mask) | (fill & ~mask)
        if size == 2:
            return 2, bitsv & 0xffff
        return 4, bitsv & 0xffffffff
    if kind < 0.8:
        value = rng.getrandbits(32) | 3
        if (value >> 2) & 7 == 7:
            value ^= 0x4
        return 4, value
    if kind < 0.97:
        value = rng.getrandbits(16)
        if value & 3 == 3:
            value ^= 1
        return 2, value
    # a longer format
    first = rng.getrandbits(16) | 0x1f if rng.random() < 0.5 else rng.getrandbits(16) | 0x3f
    first &= ~0x20 if rng.random() < 0.5 else 0xffff
    return 0, first


def unit_bytes(rng, unit):
    size, value = unit
    if size == 2:
        return struct.pack('<H', value)
    if size == 4:
        return struct.pack('<I', value)
    # a longer format: the parcel and enough bytes
    first = value
    if first & 0x20 == 0:
        length = 6
    elif first & 0x40 == 0:
        length = 8
    elif (first >> 12) & 7 != 7:
        length = 10 + 2 * ((first >> 12) & 7)
    else:
        length = 2
    return struct.pack('<H', first) + bytes(rng.getrandbits(8) for _ in range(length - 2))


def objdump_records(data, bits, features, flags):
    records = {}
    for address, size, text in rvlib.objdump_bytes(data, bits, features, flags):
        if text == '<unknown>':
            for i in range(size):
                records[address + i] = (1, text)
        else:
            records[address] = (size, text)
    return records


def dismount_records(data, bits, mattr, fmt):
    args = ['sweep', '--arch', 'rv%d' % bits, '--mattr', mattr]
    if fmt:
        args += ['--format', fmt]
    out = cli(args, ['@%x %s' % (rvlib.BASE, data.hex())])
    if len(out) != 1 or not out[0].startswith('OK '):
        raise RuntimeError('dismount_cli: ' + (out[0] if out else 'no output'))
    records = {}
    address = rvlib.BASE
    for item in out[0][3:].split(' ; '):
        size, text = item.split(':', 1)
        if size == '?':
            break
        size = int(size)
        records[address] = (size, text)
        address += size
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--count', type=int, default=200000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--sets', default='all,rva23u64,rva23s64,rv64gc,rv32gc,rv64i')
    parser.add_argument('--formats', default='default,no-aliases')
    parser.add_argument('--class', dest='cls', default='scalar', choices=['scalar', 'vector', 'all'])
    parser.add_argument('--show', type=int, default=15, help='mismatches to print per set')
    args = parser.parse_args()

    failures = 0
    for name in args.sets.split(','):
        bits = 32 if name.startswith('rv32') else 64
        mattr, _ = SETS[name]
        features = feature_list(bits, mattr)
        entries = read_entries(bits)
        rng = random.Random(args.seed * 1000003 + sum(map(ord, name)))
        for fmt in args.formats.split(','):
            objdump_flags = ['-M', 'no-aliases'] if fmt == 'no-aliases' else []
            cli_format = 'no-aliases' if fmt == 'no-aliases' else None
            total = valid = skipped = mismatched = 0
            shown = 0
            remaining = args.count
            while remaining > 0:
                n = min(CHUNK, remaining)
                remaining -= n
                units = [random_unit(rng, entries, bits, args.cls) for _ in range(n)]
                data = b''.join(unit_bytes(rng, u) for u in units)
                expected = objdump_records(data, bits, features, objdump_flags)
                got = dismount_records(data, bits, mattr, cli_format)
                # (the scalar class doesn't compare the vector instructions)
                ignored = set()
                for address, record in expected.items():
                    if args.cls == 'scalar' and record[1] != '<unknown>' and \
                            record[1].split(' ')[0].startswith('v'):
                        ignored.update(range(address, address + record[0]))
                for address in sorted(set(expected) | set(got)):
                    e = expected.get(address)
                    g = got.get(address)
                    if e is None and g is None:
                        continue
                    if address in ignored:
                        if e is not None:
                            skipped += 1
                        continue
                    total += 1
                    if e is not None and e[1] != '<unknown>':
                        valid += 1
                    if e == g:
                        continue
                    mismatched += 1
                    if shown < args.show:
                        shown += 1
                        offset = address - rvlib.BASE
                        print('%s/%s MISMATCH at +%#x bytes %s: llvm %s | dismount %s' % (
                            name, fmt, offset, data[offset:offset + 4].hex(), e, g))
            print('%-9s %-10s %7d records, %6d valid, %5d vector skipped, %d mismatches' % (
                name, fmt, total, valid, skipped, mismatched))
            failures += mismatched
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
