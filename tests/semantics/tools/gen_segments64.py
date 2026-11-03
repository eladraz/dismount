#!/usr/bin/env python3
"""
Generates tests/semantics/corpus/segments64.asm: the segment registers, far branches and far returns of
the 64 bit mode (see "64 bit mode" in tests/semantics/TEST_PLAN.md). The fixed GDT is oracle64.GDT64.

  out/venv/bin/python tests/semantics/tools/gen_segments64.py

Every line has `; @state:` comments: one for each selector kind (null with every RPL, each descriptor
of the GDT, past the limit, the LDT, every RPL), with the selector in the register, in the memory
operand or on the stack, see run_semantics.py. The random states of the line are drawn as well
(states64.segment_operands).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import oracle64                                             # noqa: E402

OUT = os.path.join(HERE, '..', 'corpus', 'segments64.asm')

lines = []
states = {}


def emit(text, state=None):
    if text not in states:
        states[text] = []
        lines.append(text)
    if state and state not in states[text]:
        states[text].append(state)


def section(title):
    lines.append('; --- ' + title)


# The selectors: every descriptor with RPL 0, the RPLs of a few, null with every RPL, past the limit, the LDT
DATA_SELECTORS = [0x00, 0x01, 0x02, 0x03] + list(range(0x08, 0x98, 8)) + [
    0x0b, 0x12, 0x13, 0x1a, 0x41, 0x42, 0x43, 0x79, 0x7b, 0x81, 0x83, 0x98, 0xa0, 0xf8, 0x1000, 0x04, 0x0c, 0x14]
CODE_BRANCH = [s for s in DATA_SELECTORS if not oracle64.reaches_not_lifted(s, False)]
CODE_RETURN = [s for s in DATA_SELECTORS if not oracle64.reaches_not_lifted(s, True)]
# The ones the lifter doesn't handle (OP_UNDEFINED): a gate, a 16 bit code segment, an outer privilege level
UNLIFTED_BRANCH = [0x28, 0x48, 0x50]
# A code segment with RPL 0 and a frame stack that is fine
GOOD_CODE = [0x08, 0x20, 0x60, 0x90]

DATA_ADDRESS = 0x100000
STACK_ADDRESS = 0x200000


def le(value, size):
    return ''.join('%02x' % ((value >> (8 * i)) & 0xff) for i in range(size))


def mem(address, data_hex):
    return 'mem=%x:%s' % (address, data_hex)


section('mov sreg, r/m16: every selector kind in the register and in memory')
for sreg in ('ds', 'es', 'fs', 'gs', 'ss'):
    for source in ('ax', 'eax', 'rax', 'r9w', 'r9d'):
        reg = 'rax' if source[0] in 'ear' and source in ('ax', 'eax', 'rax') else 'r9'
        for sel in DATA_SELECTORS:
            emit('mov %s, %s' % (sreg, source), '%s=%x' % (reg, sel))
    for sel in DATA_SELECTORS:
        emit('mov %s, [rbx]' % sreg, 'rbx=%x %s' % (DATA_ADDRESS, mem(DATA_ADDRESS, le(sel, 2))))
        emit('mov %s, word [r9+rcx*2+0x10]' % sreg,
             'r9=%x rcx=4 %s' % (DATA_ADDRESS, mem(DATA_ADDRESS + 0x18, le(sel, 2))))
emit('mov cs, ax')
for sreg in ('ds', 'es', 'fs', 'gs', 'ss', 'cs'):
    emit('mov ax, %s' % sreg)
    emit('mov eax, %s' % sreg)
    emit('mov rax, %s' % sreg)
    emit('mov [rbx], %s' % sreg)
    emit('mov r9d, %s' % sreg)
    emit('mov word [rel $+0x2000], %s' % sreg)

section('push / pop of fs and gs (64 bit operands, 16 bit with the 66 prefix)')
for sreg in ('fs', 'gs'):
    emit('push %s' % sreg)
    emit('o16 push %s' % sreg)
    for sel in DATA_SELECTORS:
        emit('pop %s' % sreg, 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, le(sel, 8))))
        emit('o16 pop %s' % sreg, 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, le(sel, 2))))

section('lfs, lgs, lss: m16:16, m16:32 and m16:64 (REX.W)')
for text, size in (('lfs', 0), ('lgs', 0), ('lss', 0)):
    for reg, regsize in (('ax', 2), ('eax', 4), ('rax', 8), ('r9w', 2), ('r9d', 4), ('r9', 8)):
        for sel in DATA_SELECTORS:
            offset = 0x1122334455667788 & ((1 << (8 * regsize)) - 1)
            emit('%s %s, [rbx]' % (text, reg), 'rbx=%x %s' % (
                DATA_ADDRESS, mem(DATA_ADDRESS, le(offset, regsize) + le(sel, 2))))
    emit('%s rcx, [r9+rdi*4-8]' % text)
    emit('%s ecx, [rel $+0x2000]' % text)
    emit('%s ax, [rbx]' % text)
    emit('a32 %s eax, [ebx]' % text)

section('jmp far / call far m16:16, m16:32, m16:64: the code segments, a fault for the others')
for op in ('jmp', 'call'):
    for form, size in (('far [rbx]', 4), ('far dword [rbx]', 4), ('far qword [rbx]', 8)):
        for sel in CODE_BRANCH:
            offset = 0x1000 + sel
            emit('%s %s' % (op, form), 'ss=10 rbx=%x rsp=%x %s' % (
                DATA_ADDRESS, STACK_ADDRESS, mem(DATA_ADDRESS, le(offset, size) + le(sel, 2))))
    emit('%s far [r9+rcx*2+0x10]' % op)
    emit('%s far qword [rel $+0x2000]' % op)
    emit('%s far dword [rsp]' % op)
    emit('a32 %s far [ebx]' % op)
    # The selectors which are not lifted (a gate, a 16 bit code segment): OP_UNDEFINED
    for sel in UNLIFTED_BRANCH:
        emit('%s far dword [r15]' % op, 'ss=10 r15=%x rsp=%x %s' % (
            DATA_ADDRESS, STACK_ADDRESS, mem(DATA_ADDRESS, le(0x1000, 4) + le(sel, 2))))
    # A code segment with L and D: #GP (QEMU doesn't fault, oracle_bugs.txt)
    emit('%s far dword [r13]' % op, 'ss=10 r13=%x rsp=%x %s' % (
        DATA_ADDRESS, STACK_ADDRESS, mem(DATA_ADDRESS, le(0x1000, 4) + le(0x68, 2))))
    # The offset past the limit of a compatibility mode segment (L = 0), and a 64 bit offset
    emit('%s far qword [rbx]' % op, 'ss=10 rbx=%x rsp=%x %s' % (
        DATA_ADDRESS, STACK_ADDRESS, mem(DATA_ADDRESS, le(0xffffffff12345678, 8) + le(0x08, 2))))
    emit('%s far qword [r14]' % op, 'ss=10 r14=%x rsp=%x %s' % (
        DATA_ADDRESS, STACK_ADDRESS, mem(DATA_ADDRESS, le(0xffffffff12345678, 8) + le(0x20, 2))))
    emit('%s far dword [rbx]' % op, 'ss=10 rbx=%x rsp=%x %s' % (
        DATA_ADDRESS, STACK_ADDRESS, mem(DATA_ADDRESS, le(0xffffffff, 4) + le(0x20, 2))))
    emit('%s far dword [rbx]' % op, 'ss=10 rbx=%x rsp=%x %s' % (
        DATA_ADDRESS, STACK_ADDRESS, mem(DATA_ADDRESS, le(0xffffffff, 4) + le(0x08, 2))))

section('retf, retfq and iretq / iretd: the frame on the stack')
for text, size, with_flags in (('retf', 4, False), ('retfq', 8, False), ('retf 8', 4, False),
                               ('retfq 0x10', 8, False), ('iretq', 8, True), ('iretd', 4, True),
                               ('iret', 4, True)):
    for sel in CODE_RETURN:
        offset = 0x2000 + sel
        data = le(offset, size) + le(sel, size)
        if with_flags:
            for ss in (0x00, 0x10, 0x18, 0x38, 0x30, 0x40, 0x78, 0x98, 0x08):
                emit(text, 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(
                    STACK_ADDRESS, data + le(0x202 | (sel & 0x400), size) + le(0x300000, size) + le(ss | (sel & 3), size))))
        else:
            emit(text, 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, data)))
    # a null ss, an RPL of 3 for ss (iretq with a code RPL 0)
    if with_flags:
        for ss in (0x00, 0x03, 0x01, 0x10, 0x13):
            emit(text, 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(
                STACK_ADDRESS, le(0x2000, size) + le(0x08, size) + le(0x202, size) +
                le(0x300000, size) + le(ss, size))))
# not lifted: a 16 bit code segment, a return to an outer privilege level (their own texts)
emit('retf 0x18', 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, le(0x2000, 4) + le(0x28, 4))))
emit('retf 0x28', 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, le(0x2000, 4) + le(0x68, 4))))
emit('retfq 0x28', 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, le(0x2000, 8) + le(0x68, 8))))
emit('retf 0x20', 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, le(0x2000, 4) + le(0x0b, 4))))
emit('retfq 0x20', 'ss=10 rsp=%x %s' % (STACK_ADDRESS, mem(STACK_ADDRESS, le(0x2000, 8) + le(0x5b, 8))))

section('the base of fs and gs is used by the next access: the instruction that loads and reads')
for sreg, other in (('fs', 'gs'), ('gs', 'fs')):
    for sel in (0x10, 0x18, 0x40, 0x80, 0x00):
        emit('mov %s, [%s:rbx]' % (sreg, other), 'rbx=%x %s' % (DATA_ADDRESS, mem(DATA_ADDRESS, le(sel, 2))))
        emit('l%s rax, [%s:rbx]' % (sreg, sreg))

with open(OUT, 'w') as f:
    f.write('; The segment registers, far branches and far returns of the 64 bit mode for the semantics tests\n')
    f.write('; GENERATED by tests/semantics/tools/gen_segments64.py. The GDT is oracle64.GDT64\n')
    f.write('; @file-feature: i386\n')
    f.write('bits 64\n')
    for text in lines:
        if text.startswith('; ---'):
            f.write('\n' + text + '\n')
        elif states[text]:
            f.write('%s  ; %s\n' % (text, ' '.join('@state: ' + st for st in states[text])))
        else:
            f.write(text + '\n')
print('%s: %d lines' % (os.path.relpath(OUT), len([l for l in lines if not l.startswith(';')])))
