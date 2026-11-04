#!/usr/bin/env python3
"""The instruction lists of the vector encoding tables: V (and the Zve*
subsets), Zvfh / Zvfhmin (no instructions), Zvfbfmin, Zvfbfwma, Zvbb, Zvkb,
Zvbc and the vector cryptography. Same format as riscvinsns.py (the order is
the order of the decoder: an alias before the general encoding it prints for);
the feature 'AUTO' means that gen_encoding_tables.py finds the weakest feature
for which llvm-mc assembles the entry, and the constraint flags of the
destination (RISCV_VR_*) are probed from llvm-mc too.

The operand tokens are riscvdsl.add_vector_tokens.
"""

A = 'RISCV_ENTRY_ALIAS'
ASM = 'RISCV_ENTRY_ASSEMBLE_ONLY'
AUTO = 'AUTO'

EEWS = (8, 16, 32, 64)
SEGMENTS = (2, 3, 4, 5, 6, 7, 8)


def define_vector(ins, table):
    def i(mn, ops, flags=(), probe=None):
        # (LLVM: the indexed loads and stores of 64 bit indices are RV64 only)
        ins(mn, ops, AUTO, list(flags), rv=64 if 'ei64' in mn else None, probe=probe)

    # ------------------------------------------------------------------
    table('vector_config')
    i('vsetvli', ['rd', 'rs1', 'vtype11'])
    i('vsetivli', ['rd', 'imm_v_avl', 'vtype10'])
    i('vsetvl', ['rd', 'rs1', 'rs2'])

    # ------------------------------------------------------------------
    table('vector_mem')
    # whole register loads and stores (the eew 8 loads print as vl<n>r.v)
    for n in (1, 2, 4, 8):
        reg = 'vd' if n == 1 else 'vd_m%d' % n
        i('vl%dr.v' % n, [reg, 'mem0'], [A], probe='vl%dre8.v {0}, {1}' % n)
    for n in (1, 2, 4, 8):
        reg = 'vd' if n == 1 else 'vd_m%d' % n
        for eew in EEWS:
            i('vl%dre%d.v' % (n, eew), [reg, 'mem0'])
    for n in (1, 2, 4, 8):
        reg = 'vs3' if n == 1 else 'vs3_m%d' % n
        i('vs%dr.v' % n, [reg, 'mem0'])
    i('vlm.v', ['vd', 'mem0'])
    i('vsm.v', ['vs3', 'mem0'])
    i('vle1.v', ['vd', 'mem0'], [ASM])
    i('vse1.v', ['vs3', 'mem0'], [ASM])
    for eew in EEWS:
        for nf in (1,) + SEGMENTS:
            seg = '' if nf == 1 else 'seg%d' % nf
            dest = 'vd' if nf == 1 else 'vd_n%d' % nf
            src = 'vs3' if nf == 1 else 'vs3_n%d' % nf
            # unit stride, fault only first
            i('vl%se%d.v' % (seg, eew), [dest, 'mem0', 'vm'])
            i('vl%se%dff.v' % (seg, eew), [dest, 'mem0', 'vm'])
            i('vlse%d.v' % eew if nf == 1 else 'vlsseg%de%d.v' % (nf, eew),
              [dest, 'mem0', 'rs2', 'vm'])
            i('vlux%sei%d.v' % (seg, eew), [dest, 'mem0', 'vs2', 'vm'])
            i('vlox%sei%d.v' % (seg, eew), [dest, 'mem0', 'vs2', 'vm'])
            i('vs%se%d.v' % (seg, eew), [src, 'mem0', 'vm'])
            i('vsse%d.v' % eew if nf == 1 else 'vssseg%de%d.v' % (nf, eew),
              [src, 'mem0', 'rs2', 'vm'])
            i('vsux%sei%d.v' % (seg, eew), [src, 'mem0', 'vs2', 'vm'])
            i('vsox%sei%d.v' % (seg, eew), [src, 'mem0', 'vs2', 'vm'])

    # ------------------------------------------------------------------
    table('vector_int')

    def vv(mn, extra=()):
        i(mn + '.vv', ['vd', 'vs2', 'vs1', 'vm'], extra)

    def vx(mn):
        i(mn + '.vx', ['vd', 'vs2', 'rs1', 'vm'])

    def vi(mn, imm='imm_v_simm5'):
        i(mn + '.vi', ['vd', 'vs2', imm, 'vm'])

    # the aliases of the integer instructions
    i('vneg.v', ['vd', 'vs2', 'vm'], [A])
    i('vnot.v', ['vd', 'vs2', 'vm'], [A])
    i('vwcvt.x.x.v', ['vd', 'vs2', 'vm'], [A])
    i('vwcvtu.x.x.v', ['vd', 'vs2', 'vm'], [A])
    i('vncvt.x.x.w', ['vd', 'vs2', 'vm'], [A])
    # the compares which are other compares with the operands swapped
    for mn, real in (('vmsgt', 'vmslt'), ('vmsgtu', 'vmsltu'), ('vmsge', 'vmsle'),
                     ('vmsgeu', 'vmsleu')):
        i(mn + '.vv', ['vd', 'vs1', 'vs2', 'vm'], [ASM])
    for mn in ('vmfgt', 'vmfge'):
        i(mn + '.vv', ['vd', 'vs1', 'vs2', 'vm'], [ASM])

    for mn in ('vadd',):
        vv(mn); vx(mn); vi(mn)
    vv('vsub'); vx('vsub')
    vx('vrsub'); vi('vrsub')
    for mn in ('vminu', 'vmin', 'vmaxu', 'vmax'):
        vv(mn); vx(mn)
    for mn in ('vand', 'vor', 'vxor'):
        vv(mn); vx(mn); vi(mn)
    vv('vrgather'); vx('vrgather'); vi('vrgather', 'imm_v_uimm5')
    vv('vrgatherei16')
    vx('vslideup'); vi('vslideup', 'imm_v_uimm5')
    vx('vslidedown'); vi('vslidedown', 'imm_v_uimm5')
    # carries
    i('vadc.vvm', ['vd', 'vs2', 'vs1', 'v0'])
    i('vadc.vxm', ['vd', 'vs2', 'rs1', 'v0'])
    i('vadc.vim', ['vd', 'vs2', 'imm_v_simm5', 'v0'])
    i('vmadc.vvm', ['vd', 'vs2', 'vs1', 'v0'])
    i('vmadc.vxm', ['vd', 'vs2', 'rs1', 'v0'])
    i('vmadc.vim', ['vd', 'vs2', 'imm_v_simm5', 'v0'])
    i('vmadc.vv', ['vd', 'vs2', 'vs1'])
    i('vmadc.vx', ['vd', 'vs2', 'rs1'])
    i('vmadc.vi', ['vd', 'vs2', 'imm_v_simm5'])
    i('vsbc.vvm', ['vd', 'vs2', 'vs1', 'v0'])
    i('vsbc.vxm', ['vd', 'vs2', 'rs1', 'v0'])
    i('vmsbc.vvm', ['vd', 'vs2', 'vs1', 'v0'])
    i('vmsbc.vxm', ['vd', 'vs2', 'rs1', 'v0'])
    i('vmsbc.vv', ['vd', 'vs2', 'vs1'])
    i('vmsbc.vx', ['vd', 'vs2', 'rs1'])
    # merge and move
    i('vmerge.vvm', ['vd', 'vs2', 'vs1', 'v0'])
    i('vmerge.vxm', ['vd', 'vs2', 'rs1', 'v0'])
    i('vmerge.vim', ['vd', 'vs2', 'imm_v_simm5', 'v0'])
    i('vmv.v.v', ['vd', 'vs1'])
    i('vmv.v.x', ['vd', 'rs1'])
    i('vmv.v.i', ['vd', 'imm_v_simm5'])
    # compares
    for mn in ('vmseq', 'vmsne', 'vmsleu', 'vmsle'):
        vv(mn); vx(mn); vi(mn)
    for mn in ('vmsltu', 'vmslt'):
        vv(mn); vx(mn)
    for mn in ('vmsgtu', 'vmsgt'):
        vx(mn); vi(mn)
    # saturating and fixed point
    for mn in ('vsaddu', 'vsadd'):
        vv(mn); vx(mn); vi(mn)
    for mn in ('vssubu', 'vssub', 'vsmul', 'vaaddu', 'vaadd', 'vasubu', 'vasub'):
        vv(mn); vx(mn)
    for mn in ('vsll', 'vsrl', 'vsra', 'vssrl', 'vssra'):
        vv(mn); vx(mn); vi(mn, 'imm_v_uimm5')
    for mn in ('vnsrl', 'vnsra', 'vnclipu', 'vnclip'):
        i(mn + '.wv', ['vd', 'vs2', 'vs1', 'vm'])
        i(mn + '.wx', ['vd', 'vs2', 'rs1', 'vm'])
        i(mn + '.wi', ['vd', 'vs2', 'imm_v_uimm5', 'vm'])
    # multiply and divide
    for mn in ('vmul', 'vmulh', 'vmulhu', 'vmulhsu', 'vdivu', 'vdiv', 'vremu', 'vrem'):
        vv(mn); vx(mn)
    for mn in ('vwaddu', 'vwadd', 'vwsubu', 'vwsub'):
        vv(mn); vx(mn)
        i(mn + '.wv', ['vd', 'vs2', 'vs1', 'vm'])
        i(mn + '.wx', ['vd', 'vs2', 'rs1', 'vm'])
    for mn in ('vwmul', 'vwmulu', 'vwmulsu'):
        vv(mn); vx(mn)
    for mn in ('vmacc', 'vnmsac', 'vmadd', 'vnmsub', 'vwmaccu', 'vwmacc', 'vwmaccsu'):
        i(mn + '.vv', ['vd', 'vs1', 'vs2', 'vm'])
        i(mn + '.vx', ['vd', 'rs1', 'vs2', 'vm'])
    i('vwmaccus.vx', ['vd', 'rs1', 'vs2', 'vm'])
    # extension
    for mn in ('vzext', 'vsext'):
        for k in (2, 4, 8):
            i('%s.vf%d' % (mn, k), ['vd', 'vs2', 'vm'])
    # reductions
    for mn in ('vredsum', 'vredand', 'vredor', 'vredxor', 'vredminu', 'vredmin', 'vredmaxu',
               'vredmax', 'vwredsumu', 'vwredsum'):
        i(mn + '.vs', ['vd', 'vs2', 'vs1', 'vm'])
    # whole register moves
    for n in (1, 2, 4, 8):
        if n == 1:
            i('vmv1r.v', ['vd', 'vs2'])
        else:
            i('vmv%dr.v' % n, ['vd_m%d' % n, 'vs2_m%d' % n])
    vx('vslide1up'); vx('vslide1down')

    # ------------------------------------------------------------------
    table('vector_mask')
    # (the aliases first: they are other mask instructions with the same
    # registers)
    i('vmmv.m', ['vd', 'vs2', riscvdsl_tie(15, 20)], [A])
    i('vmclr.m', ['vd', riscvdsl_tie(20, 7), riscvdsl_tie(15, 7)], [A])
    i('vmset.m', ['vd', riscvdsl_tie(20, 7), riscvdsl_tie(15, 7)], [A])
    i('vmnot.m', ['vd', 'vs2', riscvdsl_tie(15, 20)], [A])
    for mn in ('vmandn', 'vmand', 'vmor', 'vmxor', 'vmorn', 'vmnand', 'vmnor', 'vmxnor'):
        i(mn + '.mm', ['vd', 'vs2', 'vs1'])
    i('vmandnot.mm', ['vd', 'vs2', 'vs1'], [ASM])
    i('vmornot.mm', ['vd', 'vs2', 'vs1'], [ASM])
    i('vcpop.m', ['rd', 'vs2', 'vm'])
    i('vpopc.m', ['rd', 'vs2', 'vm'], [ASM])
    i('vfirst.m', ['rd', 'vs2', 'vm'])
    for mn in ('vmsbf', 'vmsof', 'vmsif', 'viota'):
        i(mn + '.m', ['vd', 'vs2', 'vm'])
    i('vid.v', ['vd', 'vm'])
    i('vcompress.vm', ['vd', 'vs2', 'vs1'])
    i('vmv.x.s', ['rd', 'vs2'])
    i('vmv.s.x', ['vd', 'rs1'])

    # ------------------------------------------------------------------
    table('vector_fp')
    i('vfneg.v', ['vd', 'vs2', riscvdsl_tie(15, 20), 'vm'], [A])
    i('vfabs.v', ['vd', 'vs2', riscvdsl_tie(15, 20), 'vm'], [A])

    def fvv(mn, extra=()):
        i(mn + '.vv', ['vd', 'vs2', 'vs1', 'vm'], extra)

    def fvf(mn):
        i(mn + '.vf', ['vd', 'vs2', 'frs1', 'vm'])

    for mn in ('vfadd', 'vfsub', 'vfmin', 'vfmax', 'vfsgnj', 'vfsgnjn', 'vfsgnjx', 'vfdiv',
               'vfmul'):
        fvv(mn); fvf(mn)
    fvf('vfrsub'); fvf('vfrdiv')
    for mn in ('vfmacc', 'vfnmacc', 'vfmsac', 'vfnmsac', 'vfmadd', 'vfnmadd', 'vfmsub',
               'vfnmsub', 'vfwmacc', 'vfwnmacc', 'vfwmsac', 'vfwnmsac'):
        i(mn + '.vv', ['vd', 'vs1', 'vs2', 'vm'])
        i(mn + '.vf', ['vd', 'frs1', 'vs2', 'vm'])
    for mn in ('vfwadd', 'vfwsub'):
        fvv(mn); fvf(mn)
        i(mn + '.wv', ['vd', 'vs2', 'vs1', 'vm'])
        i(mn + '.wf', ['vd', 'vs2', 'frs1', 'vm'])
    fvv('vfwmul'); fvf('vfwmul')
    for mn in ('vfsqrt', 'vfrsqrt7', 'vfrec7', 'vfclass'):
        i(mn + '.v', ['vd', 'vs2', 'vm'])
    i('vfmerge.vfm', ['vd', 'vs2', 'frs1', 'v0'])
    i('vfmv.v.f', ['vd', 'frs1'])
    i('vfmv.f.s', ['frd', 'vs2'])
    i('vfmv.s.f', ['vd', 'frs1'])
    for mn in ('vmfeq', 'vmfne', 'vmflt', 'vmfle'):
        fvv(mn); fvf(mn)
    fvf('vmfgt'); fvf('vmfge')
    fvf('vfslide1up'); fvf('vfslide1down')
    for mn in ('vfredosum', 'vfredusum', 'vfredmax', 'vfredmin', 'vfwredosum', 'vfwredusum'):
        i(mn + '.vs', ['vd', 'vs2', 'vs1', 'vm'])
    i('vfredsum.vs', ['vd', 'vs2', 'vs1', 'vm'], [ASM])
    i('vfwredsum.vs', ['vd', 'vs2', 'vs1', 'vm'], [ASM])
    for mn in ('vfcvt.xu.f', 'vfcvt.x.f', 'vfcvt.f.xu', 'vfcvt.f.x', 'vfcvt.rtz.xu.f',
               'vfcvt.rtz.x.f', 'vfwcvt.xu.f', 'vfwcvt.x.f', 'vfwcvt.f.xu', 'vfwcvt.f.x',
               'vfwcvt.f.f', 'vfwcvt.rtz.xu.f', 'vfwcvt.rtz.x.f'):
        i(mn + '.v', ['vd', 'vs2', 'vm'])
    for mn in ('vfncvt.xu.f', 'vfncvt.x.f', 'vfncvt.f.xu', 'vfncvt.f.x', 'vfncvt.f.f',
               'vfncvt.rod.f.f', 'vfncvt.rtz.xu.f', 'vfncvt.rtz.x.f'):
        i(mn + '.w', ['vd', 'vs2', 'vm'])

    # ------------------------------------------------------------------
    table('vector_ext')
    # Zvfbfmin, Zvfbfwma
    i('vfwcvtbf16.f.f.v', ['vd', 'vs2', 'vm'])
    i('vfncvtbf16.f.f.w', ['vd', 'vs2', 'vm'])
    i('vfwmaccbf16.vv', ['vd', 'vs1', 'vs2', 'vm'])
    i('vfwmaccbf16.vf', ['vd', 'frs1', 'vs2', 'vm'])
    # Zvbb, Zvkb
    vv('vandn'); vx('vandn')
    i('vbrev8.v', ['vd', 'vs2', 'vm'])
    i('vrev8.v', ['vd', 'vs2', 'vm'])
    i('vbrev.v', ['vd', 'vs2', 'vm'])
    i('vclz.v', ['vd', 'vs2', 'vm'])
    i('vctz.v', ['vd', 'vs2', 'vm'])
    i('vcpop.v', ['vd', 'vs2', 'vm'])
    vv('vrol'); vx('vrol')
    vv('vror'); vx('vror'); vi('vror', 'imm_v_uimm6')
    vv('vwsll'); vx('vwsll'); vi('vwsll', 'imm_v_uimm5')
    # Zvbc
    vv('vclmul'); vx('vclmul'); vv('vclmulh'); vx('vclmulh')
    # Zvkg
    i('vghsh.vv', ['vd', 'vs2', 'vs1'])
    i('vgmul.vv', ['vd', 'vs2'])
    # Zvkned
    for mn in ('vaesdf', 'vaesdm', 'vaesef', 'vaesem', 'vsm4r'):
        i(mn + '.vv', ['vd', 'vs2'])
        i(mn + '.vs', ['vd', 'vs2'])
    i('vaesz.vs', ['vd', 'vs2'])
    i('vaeskf1.vi', ['vd', 'vs2', 'imm_v_uimm5'])
    i('vaeskf2.vi', ['vd', 'vs2', 'imm_v_uimm5'])
    # Zvknha, Zvknhb
    for mn in ('vsha2ms', 'vsha2ch', 'vsha2cl'):
        i(mn + '.vv', ['vd', 'vs2', 'vs1'])
    # Zvksed, Zvksh
    i('vsm4k.vi', ['vd', 'vs2', 'imm_v_uimm5'])
    i('vsm3c.vi', ['vd', 'vs2', 'imm_v_uimm5'])
    i('vsm3me.vv', ['vd', 'vs2', 'vs1'])


def riscvdsl_tie(pos, other):
    import riscvdsl
    return riscvdsl.tie(pos, other)
