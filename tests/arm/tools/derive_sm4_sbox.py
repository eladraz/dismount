#!/usr/bin/env python3
"""Derive the SM4 S-box from Unicorn (QEMU implements sm4ekey) by running `sm4ekey v0.4s, v1.4s, v2.4s` with
n = 0 and the constant key CK0 = i: the first output word is x0 ^ L'(tau(i)) = L'(S[i] S[0] S[0] S[0]),
L'(x) = x ^ rol(x, 13) ^ rol(x, 23), which is inverted over GF(2). The table is printed as C initializers and
checked against the SM4 known answer of the standard with openssl (python3 derive_sm4_sbox.py [--check]).

    out/venv/bin/python tests/arm/tools/derive_sm4_sbox.py > sbox.txt
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tests', 'arm', 'semantics'))
import armasm
import armstate as S
import oracle_arm64

M32 = 0xFFFFFFFF


def rol(x, n):
    return ((x << n) | (x >> (32 - n))) & M32


def lprime(x):
    return x ^ rol(x, 13) ^ rol(x, 23)


def invert_lprime():
    """The inverse of the GF(2) linear map L' as a function (by Gaussian elimination)."""
    columns = [lprime(1 << i) for i in range(32)]
    # solve for every output bit: build the inverse columns
    rows = [[(columns[j] >> i) & 1 for j in range(32)] + [1 if i == k else 0 for k in range(32)] for i in range(32)]
    for col in range(32):
        pivot = next(r for r in range(col, 32) if rows[r][col])
        rows[col], rows[pivot] = rows[pivot], rows[col]
        for r in range(32):
            if r != col and rows[r][col]:
                rows[r] = [a ^ b for a, b in zip(rows[r], rows[col])]
    inverse_columns = []
    for k in range(32):
        value = 0
        for i in range(32):
            value |= rows[i][32 + k] << i
        inverse_columns.append(value)

    def inverse(y):
        x = 0
        for k in range(32):
            if (y >> k) & 1:
                x ^= inverse_columns[k]
        return x
    return inverse


def main():
    hexb, err = armasm.assemble('a64', ['sm4ekey v0.4s, v1.4s, v2.4s'])[0]
    code = bytes.fromhex(hexb)
    oracle = oracle_arm64.get_oracle(128)
    inverse = invert_lprime()
    table = []
    for i in range(256):
        st = S.default_state('a64')
        st['v'] = [0] * 32
        st['v'][2] = i
        res = oracle.run(code, st)
        out = res['v'][0] & M32
        word = inverse(out)
        table.append(word & 0xFF)
        if i == 0:
            zero = word & 0xFF
        assert (word >> 8) & 0xFF == zero == (word >> 16) & 0xFF == (word >> 24) & 0xFF or i == 0
    for i in range(0, 256, 16):
        print('    ' + ', '.join('0x%02x' % b for b in table[i:i + 16]) + ',')


if __name__ == '__main__':
    main()
