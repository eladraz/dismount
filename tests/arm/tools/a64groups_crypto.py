"""A64 cryptographic extensions: AES, SHA1, SHA256, SHA512, SHA3, SM3 and SM4
(and the carry-less pmull, which is in a64groups_simd.py)."""
import a64dsl
from a64dsl import *
from a64simd_dsl import *


def crypto():
    # AES: 0100 1110 size 10100 opcode 10 Rn Rd
    for opcode, name in [('00100', 'aese'), ('00101', 'aesd'), ('00110', 'aesmc'),
                         ('00111', 'aesimc')]:
        E(name, '0100 1110 00 10100 %s 10 Rn Rd' % opcode, [vecf('Rd', '16B'), vecf('Rn', '16B')],
          feat='FEAT_AES')
    # SHA1 and SHA256, three registers: 0101 1110 size 0 Rm 0 opcode 00 Rn Rd
    for opcode, name in [('000', 'sha1c'), ('001', 'sha1p'), ('010', 'sha1m')]:
        E(name, '0101 1110 00 0 Rm 0 %s 00 Rn Rd' % opcode,
          [sfp('Rd', 'q'), sfp('Rn', 's'), vecf('Rm', '4S')], feat='FEAT_SHA1')
    E('sha1su0', '0101 1110 00 0 Rm 0 011 00 Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '4S'), vecf('Rm', '4S')], feat='FEAT_SHA1')
    for opcode, name in [('100', 'sha256h'), ('101', 'sha256h2')]:
        E(name, '0101 1110 00 0 Rm 0 %s 00 Rn Rd' % opcode,
          [sfp('Rd', 'q'), sfp('Rn', 'q'), vecf('Rm', '4S')], feat='FEAT_SHA256')
    E('sha256su1', '0101 1110 00 0 Rm 0 110 00 Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '4S'), vecf('Rm', '4S')], feat='FEAT_SHA256')
    # Two registers: 0101 1110 size 10100 opcode 10 Rn Rd
    E('sha1h', '0101 1110 00 10100 00000 10 Rn Rd', [sfp('Rd', 's'), sfp('Rn', 's')],
      feat='FEAT_SHA1')
    E('sha1su1', '0101 1110 00 10100 00001 10 Rn Rd', [vecf('Rd', '4S'), vecf('Rn', '4S')],
      feat='FEAT_SHA1')
    E('sha256su0', '0101 1110 00 10100 00010 10 Rn Rd', [vecf('Rd', '4S'), vecf('Rn', '4S')],
      feat='FEAT_SHA256')
    # SHA512, SHA3 and SM3 / SM4, three registers: 1100 1110 011 Rm 1 O 00 opcode Rn Rd
    E('sha512h', '1100 1110 011 Rm 1 0 00 00 Rn Rd',
      [sfp('Rd', 'q'), sfp('Rn', 'q'), vecf('Rm', '2D')], feat='FEAT_SHA512')
    E('sha512h2', '1100 1110 011 Rm 1 0 00 01 Rn Rd',
      [sfp('Rd', 'q'), sfp('Rn', 'q'), vecf('Rm', '2D')], feat='FEAT_SHA512')
    E('sha512su1', '1100 1110 011 Rm 1 0 00 10 Rn Rd',
      [vecf('Rd', '2D'), vecf('Rn', '2D'), vecf('Rm', '2D')], feat='FEAT_SHA512')
    E('rax1', '1100 1110 011 Rm 1 0 00 11 Rn Rd',
      [vecf('Rd', '2D'), vecf('Rn', '2D'), vecf('Rm', '2D')], feat='FEAT_SHA3')
    E('sm3partw1', '1100 1110 011 Rm 1 1 00 00 Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '4S'), vecf('Rm', '4S')], feat='FEAT_SM3')
    E('sm3partw2', '1100 1110 011 Rm 1 1 00 01 Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '4S'), vecf('Rm', '4S')], feat='FEAT_SM3')
    E('sm4ekey', '1100 1110 011 Rm 1 1 00 10 Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '4S'), vecf('Rm', '4S')], feat='FEAT_SM4')
    # Two registers: 1100 1110 1100 0000 1000 opcode Rn Rd
    E('sha512su0', '1100 1110 1100 0000 1000 00 Rn Rd', [vecf('Rd', '2D'), vecf('Rn', '2D')],
      feat='FEAT_SHA512')
    E('sm4e', '1100 1110 1100 0000 1000 01 Rn Rd', [vecf('Rd', '4S'), vecf('Rn', '4S')],
      feat='FEAT_SM4')
    # Four registers: 1100 1110 op3 Rm 0 Ra Rn Rd
    E('eor3', '1100 1110 000 Rm 0 Ra Rn Rd',
      [vecf('Rd', '16B'), vecf('Rn', '16B'), vecf('Rm', '16B'), vecf('Ra', '16B')], feat='FEAT_SHA3')
    E('bcax', '1100 1110 001 Rm 0 Ra Rn Rd',
      [vecf('Rd', '16B'), vecf('Rn', '16B'), vecf('Rm', '16B'), vecf('Ra', '16B')], feat='FEAT_SHA3')
    E('sm3ss1', '1100 1110 010 Rm 0 Ra Rn Rd',
      [vecf('Rd', '4S'), vecf('Rn', '4S'), vecf('Rm', '4S'), vecf('Ra', '4S')], feat='FEAT_SM3')
    # xar: 1100 1110 100 Rm imm6 Rn Rd
    E('xar', '1100 1110 100 Rm imm6 Rn Rd',
      [vecf('Rd', '2D'), vecf('Rn', '2D'), vecf('Rm', '2D'), shiftimm(hex_=True, xar='imm6')],
      feat='FEAT_SHA3')
    # sm3tt1a...: 1100 1110 010 Rm 10 imm2 opcode Rn Rd
    for opcode, name in [('00', 'sm3tt1a'), ('01', 'sm3tt1b'), ('10', 'sm3tt2a'), ('11', 'sm3tt2b')]:
        E(name, '1100 1110 010 Rm 10 imm2 %s Rn Rd' % opcode,
          [vecf('Rd', '4S'), vecf('Rn', '4S'), elem('Rm', 'imm2_s')], feat='FEAT_SM3')
