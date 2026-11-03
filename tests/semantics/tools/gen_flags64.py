#!/usr/bin/env python3
"""
Generates tests/semantics/corpus/flags64.asm: the directed 64 bit mode cases of
the semantics tests (flags edge cases of the ALU on 64 bit operands, the
zero extension of the 32 bit writes, the shift counts, rdx:rax, the 8 byte
stack, rip relative operands, the 32 bit address size, the string
instructions, jrcxz / loop, movsxd and the sign conversions, cmpxchg16b,
bswap, bt, movzx / movsx, setcc and the high byte registers).

  python3 tests/semantics/tools/gen_flags64.py

The states (registers, flags) are drawn by run_semantics.py; a line with
`; @state: rax=.. rcx=..` is added to its random states with those registers.
"""
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'corpus', 'flags64.asm')

# The output: section titles and instruction texts in order, with the fixed states of each text
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


GP64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11',
        'r12', 'r13', 'r14', 'r15']
GP32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi', 'r8d', 'r9d', 'r10d', 'r11d',
        'r12d', 'r13d', 'r14d', 'r15d']
GP16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di', 'r8w', 'r9w', 'r10w', 'r11w',
        'r12w', 'r13w', 'r14w', 'r15w']
GP8 = ['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil', 'r8b', 'r9b', 'r10b', 'r11b',
       'r12b', 'r13b', 'r14b', 'r15b']
HIGH8 = ['ah', 'ch', 'dh', 'bh']
# registers that are not the stack pointer or the memory base of the lines
SAFE64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsi', 'rdi', 'r8', 'r9', 'r12', 'r15']
SAFE32 = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi', 'r8d', 'r9d', 'r12d', 'r15d']
SAFE16 = ['ax', 'cx', 'dx', 'bx', 'si', 'di', 'r8w', 'r9w', 'r12w', 'r15w']
SAFE8 = ['al', 'cl', 'dl', 'bl', 'sil', 'dil', 'r8b', 'r9b', 'r12b', 'r15b']

IMM32 = ['0', '1', '0x7f', '0x80', '0xff', '0x7fffffff', '-1', '-0x80', '-0x80000000',
         '0x12345678', '-0x7f']
IMM64 = ['0', '1', '-1', '0x7fffffffffffffff', '0x8000000000000000', '0x123456789abcdef0',
         '0xffffffff', '0x100000000', '0xffffffff80000000', '0x80000000']

ALU = ['add', 'adc', 'sub', 'sbb', 'and', 'or', 'xor', 'cmp', 'test']

section('ALU on 64, 32, 16 and 8 bit operands: register, memory and immediate forms')
for op in ALU:
    for dst, src in (('rax', 'rbx'), ('r8', 'r9'), ('rsi', 'r15'), ('rbx', 'rax'), ('r12', 'rcx')):
        emit('%s %s, %s' % (op, dst, src))
    for r in ('rax', 'r9'):
        emit('%s %s, [rbx]' % (op, r))
        emit('%s [rbx], %s' % (op, r))
        emit('%s %s, [rsi+rdi*8+0x10]' % (op, r))
        emit('%s %s, [r8+r9*4-8]' % (op, r))
    emit('%s qword [r12], rcx' % op)
    for imm in IMM32:
        emit('%s rax, %s' % (op, imm))
        emit('%s r10, %s' % (op, imm))
        emit('%s qword [rbx], %s' % (op, imm))
    for imm in IMM32[:6]:
        emit('%s eax, %s' % (op, imm))
        emit('%s r11d, %s' % (op, imm))
        emit('%s dword [rbx], %s' % (op, imm))
        emit('%s ax, %s' % (op, imm))
        emit('%s word [rbx], %s' % (op, imm))
        emit('%s al, %s' % (op, imm))
        emit('%s sil, %s' % (op, imm))
        emit('%s r9b, %s' % (op, imm))
        emit('%s byte [rbx], %s' % (op, imm))
    for a, b in zip(SAFE32, SAFE32[1:]):
        emit('%s %s, %s' % (op, a, b))
    for a, b in zip(SAFE16, SAFE16[1:]):
        emit('%s %s, %s' % (op, a, b))
    for a, b in zip(SAFE8, SAFE8[1:]):
        emit('%s %s, %s' % (op, a, b))
    emit('%s dword [rbx+rcx], r10d' % op)
    emit('%s r10b, [rbx]' % op)

for op in ('neg', 'not', 'inc', 'dec'):
    for r in GP64[:4] + GP64[8:]:
        emit('%s %s' % (op, r))
    for r in SAFE32:
        emit('%s %s' % (op, r))
    for r in SAFE16[:4]:
        emit('%s %s' % (op, r))
    for r in SAFE8:
        emit('%s %s' % (op, r))
    for size in ('qword', 'dword', 'word', 'byte'):
        emit('%s %s [rbx]' % (op, size))
    for r in ('rax', 'rax'):
        emit('%s %s' % (op, r), 'rax=7fffffffffffffff')
        emit('%s %s' % (op, r), 'rax=8000000000000000')
        emit('%s %s' % (op, r), 'rax=ffffffffffffffff')
        emit('%s %s' % (op, r), 'rax=0')
    emit('%s eax' % op, 'rax=7fffffff')
    emit('%s eax' % op, 'rax=ffffffff80000000')
    emit('%s eax' % op, 'rax=ffffffffffffffff')

# carry and overflow edges with the flags set
for op in ('add', 'adc'):
    emit('%s rax, rbx' % op, 'rax=ffffffffffffffff rbx=1 eflags=203')
    emit('%s rax, rbx' % op, 'rax=7fffffffffffffff rbx=1 eflags=202')
    emit('%s rax, rbx' % op, 'rax=7fffffffffffffff rbx=0 eflags=203')
    emit('%s rax, rbx' % op, 'rax=8000000000000000 rbx=8000000000000000 eflags=203')
    emit('%s eax, ebx' % op, 'rax=ffffffff rbx=1 eflags=203')
    emit('%s eax, ebx' % op, 'rax=7fffffff rbx=0 eflags=203')
    emit('%s eax, ebx' % op, 'rax=ffffffffffffffff rbx=ffffffffffffffff eflags=203')
for op in ('sub', 'sbb', 'cmp'):
    emit('%s rax, rbx' % op, 'rax=0 rbx=1 eflags=203')
    emit('%s rax, rbx' % op, 'rax=8000000000000000 rbx=1 eflags=203')
    emit('%s rax, rbx' % op, 'rax=7fffffffffffffff rbx=ffffffffffffffff eflags=202')
    emit('%s eax, ebx' % op, 'rax=80000000 rbx=1 eflags=203')
    emit('%s eax, ebx' % op, 'rax=ffffffff00000000 rbx=ffffffff00000001 eflags=202')

section('mov, movabs and the zero extension of the 32 bit writes')
for imm in IMM64:
    emit('mov rax, %s' % imm)
    emit('mov r9, %s' % imm)
for imm in IMM32:
    emit('mov rax, qword %s' % imm)
    emit('mov qword [rbx], %s' % imm)
    emit('mov r11, qword %s' % imm)
for imm in ('0', '1', '0x7fffffff', '0x80000000', '0xffffffff', '-1'):
    emit('mov eax, %s' % imm)
    emit('mov r8d, %s' % imm)
    emit('mov dword [rbx], %s' % imm)
for imm in ('0', '0x7f', '0x80', '0xff', '-1'):
    emit('mov al, %s' % imm)
    emit('mov spl, %s' % imm)
    emit('mov r13b, %s' % imm)
    emit('mov ah, %s' % imm)
    emit('mov byte [rbx], %s' % imm)
for imm in ('0', '0x7fff', '0x8000', '0xffff', '-1'):
    emit('mov ax, %s' % imm)
    emit('mov r10w, %s' % imm)
    emit('mov word [rbx], %s' % imm)
for a, b in zip(GP64, GP64[1:]):
    emit('mov %s, %s' % (a, b))
for a, b in zip(GP32, GP32[1:]):
    emit('mov %s, %s' % (a, b))
for a, b in zip(GP16, GP16[1:]):
    emit('mov %s, %s' % (a, b))
for a, b in zip(GP8, GP8[1:]):
    emit('mov %s, %s' % (a, b))
for r in SAFE64:
    emit('mov %s, [rbx+8]' % r)
    emit('mov [rbx+8], %s' % r)
for r in SAFE32:
    emit('mov %s, [rbx+8]' % r)
    emit('mov [rbx+8], %s' % r)
for r in SAFE16:
    emit('mov %s, [rbx+8]' % r)
    emit('mov [rbx+8], %s' % r)
for r in SAFE8:
    emit('mov %s, [rbx+8]' % r)
    emit('mov [rbx+8], %s' % r)
# 32 bit write, then the full register is read back by the next instruction
emit('mov eax, ecx', 'rax=ffffffffffffffff rcx=12345678')
emit('mov eax, ecx', 'rax=ffffffffffffffff rcx=ffffffffffffffff')
emit('add r9d, edx', 'r9=ffffffffffffffff rdx=1')
emit('add r9d, edx', 'r9=ffffffff00000001 rdx=0')
emit('xor r9d, r9d', 'r9=ffffffffffffffff')
emit('sub eax, eax', 'rax=ffffffffffffffff')
emit('cmp eax, eax', 'rax=ffffffffffffffff')
emit('xchg eax, ecx', 'rax=ffffffffffffffff rcx=1111111122222222')
emit('xchg ecx, eax', 'rax=ffffffffffffffff rcx=1111111122222222')
emit('xchg r8d, eax', 'rax=ffffffffffffffff r8=1111111122222222')
emit('xchg rax, rcx', 'rax=ffffffffffffffff rcx=1111111122222222')
emit('xchg rax, r9', 'rax=ffffffffffffffff r9=1111111122222222')
emit('xchg ax, cx', 'rax=ffffffffffffffff rcx=1111111122222222')
emit('xchg r9w, ax', 'rax=ffffffffffffffff r9=1111111122222222')
emit('xchg rbx, [rsi]')
emit('xchg ebx, [rsi]')
emit('xchg bl, [rsi]')
emit('xchg r10b, [rsi]')
# the one byte nop (90) is not xchg eax, eax: it doesn't zero extend rax; 87 c0 does
emit('db 0x90', 'rax=ffffffffffffffff')
emit('db 0x87, 0xc0', 'rax=ffffffffffffffff')
emit('db 0x48, 0x90', 'rax=ffffffffffffffff')
emit('db 0x66, 0x90', 'rax=ffffffffffffffff')
emit('db 0xf3, 0x90', 'rax=ffffffffffffffff')
emit('db 0x41, 0x90', 'rax=ffffffffffffffff r8=1111111122222222')
emit('db 0x49, 0x90', 'rax=ffffffffffffffff r8=1111111122222222')
emit('db 0x66, 0x41, 0x90', 'rax=ffffffffffffffff r8=1111111122222222')
emit('db 0x48, 0x87, 0xc0', 'rax=ffffffffffffffff')
emit('db 0x66, 0x87, 0xc0', 'rax=ffffffffffffffff')
for r in SAFE64:
    emit('xchg rax, %s' % r)
for r in SAFE32:
    emit('xchg eax, %s' % r)
for r in SAFE16:
    emit('xchg ax, %s' % r)
for a in ('rcx', 'r10'):
    emit('cmovz %s, rbx' % a)
    emit('cmovnz %s, rbx' % a)
    emit('cmovc %s, [rbx]' % a)
for a, b in (('ecx', 'ebx'), ('r10d', 'r11d'), ('cx', 'bx'), ('r10w', 'r11w')):
    for cc in ('z', 'nz', 'c', 'nc', 'o', 's', 'p', 'l', 'ge', 'le', 'g', 'be', 'a'):
        emit('cmov%s %s, %s' % (cc, a, b))
for cc in ('o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g'):
    emit('cmov%s rcx, rbx' % cc)

section('setcc, with the REX byte registers and the high byte registers')
for cc in ('o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g'):
    for r in ('al', 'ah', 'spl', 'bpl', 'sil', 'dil', 'r8b', 'r15b'):
        emit('set%s %s' % (cc, r))
    emit('set%s byte [rbx]' % cc)
    emit('set%s byte [r9+r10*2]' % cc)

section('the high byte registers')
for a in HIGH8:
    for b in HIGH8:
        emit('mov %s, %s' % (a, b))
    emit('mov %s, bl' % a)
    emit('mov %s, [rbx]' % a)
    emit('mov [rbx], %s' % a)
    emit('add %s, 0x80' % a)
    emit('add %s, dl' % a)
    emit('xchg %s, al' % a)
    emit('xchg %s, [rsi]' % a)
    emit('cmp %s, 0x7f' % a)
    emit('test %s, %s' % (a, a))
    emit('neg %s' % a)
    emit('not %s' % a)
    emit('inc %s' % a)
    emit('movzx eax, %s' % a)
    emit('movsx eax, %s' % a)
    emit('movzx ecx, %s' % a)
    emit('shl %s, 1' % a)
    emit('shr %s, cl' % a)
    emit('rol %s, 3' % a)
    emit('mul %s' % a)
    emit('imul %s' % a)
    emit('div %s' % a)
    emit('idiv %s' % a)
emit('mov ah, 0x12', 'rax=ffffffffffffffff')
emit('mov al, ah', 'rax=ffffffffffffffff')
emit('xchg ah, al', 'rax=1122334455667788')

section('8 and 16 bit writes keep the rest of the register')
for r8, r16, r32 in (('al', 'ax', 'eax'), ('r8b', 'r8w', 'r8d'), ('sil', 'si', 'esi'), ('r15b', 'r15w', 'r15d')):
    emit('mov %s, 0x55' % r8, '%s=ffffffffffffffff' % ('r' + r32[1:] if r32[0] == 'e' else r32[:-1]))
    emit('add %s, 1' % r8)
    emit('add %s, 1' % r16)
    emit('add %s, 1' % r32)
    emit('not %s' % r8)
    emit('not %s' % r16)
    emit('not %s' % r32)
    emit('neg %s' % r16)
    emit('inc %s' % r16)
    emit('dec %s' % r8)
    emit('bswap %s' % r32)
    emit('movzx %s, %s' % (r32, r8))
    emit('movzx %s, %s' % (r16, r8))
    emit('movsx %s, %s' % (r16, r8))

section('movzx / movsx / movsxd to 64 bit registers')
for dst in ('rax', 'r9', 'rsi', 'r15'):
    for src in ('bl', 'sil', 'r10b', 'cx', 'r11w'):
        emit('movzx %s, %s' % (dst, src))
        emit('movsx %s, %s' % (dst, src))
    emit('movzx %s, byte [rbx]' % dst)
    emit('movsx %s, byte [rbx]' % dst)
    emit('movzx %s, word [rbx]' % dst)
    emit('movsx %s, word [rbx]' % dst)
    emit('movsxd %s, ecx' % dst)
    emit('movsxd %s, r11d' % dst)
    emit('movsxd %s, dword [rbx]' % dst)
for dst in ('eax', 'r9d', 'esi'):
    for src in ('bl', 'sil', 'r10b', 'cx', 'r11w'):
        emit('movzx %s, %s' % (dst, src))
        emit('movsx %s, %s' % (dst, src))
    emit('movzx %s, byte [rbx]' % dst)
    emit('movsx %s, word [rbx]' % dst)
for dst in ('ax', 'r9w'):
    emit('movzx %s, bl' % dst)
    emit('movsx %s, bl' % dst)
    emit('movzx %s, byte [rbx]' % dst)
    emit('movsx %s, byte [rbx]' % dst)
emit('movsxd rax, ecx', 'rcx=ffffffff80000000')
emit('movsxd rax, ecx', 'rcx=000000007fffffff')
emit('movsxd rax, ecx', 'rcx=ffffffffffffffff')
emit('movsxd eax, ecx', 'rcx=ffffffff80000000')
emit('movsxd rax, dword [rbx]')
for text in ('cbw', 'cwde', 'cdqe', 'cwd', 'cdq', 'cqo'):
    emit(text)
    emit(text, 'rax=ffffffff80000000 rdx=ffffffffffffffff')
    emit(text, 'rax=000000007fffffff rdx=0')
    emit(text, 'rax=8000000000000000')
    emit(text, 'rax=7fffffffffffffff')
    emit(text, 'rax=80')
    emit(text, 'rax=8000')
    emit(text, 'rax=80000000')
emit('db 0x48, 0x98')
emit('db 0x48, 0x99')
emit('db 0x66, 0x48, 0x98')

section('shifts and rotates: the count masks (6 bits for 64 bit operands, 5 for the others)')
for op in ('shl', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
    for r in ('rax', 'r9', 'rsi'):
        emit('%s %s, 1' % (op, r))
        emit('%s %s, cl' % (op, r))
        for c in (2, 7, 31, 32, 33, 63):
            emit('%s %s, %d' % (op, r, c))
    for r in ('eax', 'r9d'):
        emit('%s %s, 1' % (op, r))
        emit('%s %s, cl' % (op, r))
        for c in (2, 31, 32, 33, 63):
            emit('%s %s, %d' % (op, r, c))
    for r in ('ax', 'r9w', 'al', 'r9b', 'sil'):
        emit('%s %s, 1' % (op, r))
        emit('%s %s, cl' % (op, r))
        for c in (2, 15, 16, 17, 31, 32):
            emit('%s %s, %d' % (op, r, c))
    for size in ('qword', 'dword', 'word', 'byte'):
        emit('%s %s [rbx], 1' % (op, size))
        emit('%s %s [rbx], cl' % (op, size))
        emit('%s %s [rbx], 5' % (op, size))
    for c in (0, 1, 31, 32, 33, 63, 64, 65, 0x7f, 0x80, 0xff, 0x101):
        emit('%s rax, cl' % op, 'rcx=%x' % c)
        emit('%s eax, cl' % op, 'rcx=%x' % c)
        emit('%s ax, cl' % op, 'rcx=%x' % c)
        emit('%s al, cl' % op, 'rcx=%x' % c)
    emit('%s rax, cl' % op, 'rax=8000000000000001 rcx=3f eflags=203')
    emit('%s rax, cl' % op, 'rax=8000000000000000 rcx=40 eflags=203')
    emit('%s rax, 1' % op, 'rax=8000000000000000 eflags=203')
    emit('%s rax, 1' % op, 'rax=4000000000000000 eflags=202')
    emit('%s eax, cl' % op, 'rax=ffffffffffffffff rcx=20 eflags=203')
    emit('%s eax, cl' % op, 'rax=ffffffffffffffff rcx=1f eflags=202')
for op in ('shld', 'shrd'):
    for c in ('1', 'cl', '5', '31', '32', '33', '63'):
        emit('%s rax, rbx, %s' % (op, c))
        emit('%s r9, r10, %s' % (op, c))
        emit('%s eax, ebx, %s' % (op, c))
        emit('%s r9d, r10d, %s' % (op, c))
        emit('%s ax, bx, %s' % (op, c))
        emit('%s qword [rsi], rbx, %s' % (op, c))
        emit('%s dword [rsi], ebx, %s' % (op, c))
    for c in (0, 1, 31, 32, 33, 63, 64, 65):
        emit('%s rax, rbx, cl' % op, 'rcx=%x' % c)
        emit('%s eax, ebx, cl' % op, 'rcx=%x' % c)

section('mul, imul, div, idiv: rdx:rax, #DE')
for op in ('mul', 'imul', 'div', 'idiv'):
    for r in ('rbx', 'r9', 'rcx'):
        emit('%s %s' % (op, r))
    for r in ('ebx', 'r9d'):
        emit('%s %s' % (op, r))
    for r in ('bx', 'r9w'):
        emit('%s %s' % (op, r))
    for r in ('bl', 'r9b', 'sil'):
        emit('%s %s' % (op, r))
    for size in ('qword', 'dword', 'word', 'byte'):
        emit('%s %s [rbx]' % (op, size))
for op in ('div', 'idiv'):
    emit('%s rbx' % op, 'rax=0 rdx=0 rbx=0')
    emit('%s rbx' % op, 'rax=ffffffffffffffff rdx=ffffffffffffffff rbx=1')
    emit('%s rbx' % op, 'rax=8000000000000000 rdx=ffffffffffffffff rbx=ffffffffffffffff')
    emit('%s rbx' % op, 'rax=8000000000000000 rdx=ffffffffffffffff rbx=1')
    emit('%s rbx' % op, 'rax=0 rdx=8000000000000000 rbx=ffffffffffffffff')
    emit('%s rbx' % op, 'rax=ffffffffffffffff rdx=0 rbx=2')
    emit('%s rbx' % op, 'rax=ffffffffffffffff rdx=7fffffffffffffff rbx=ffffffffffffffff')
    emit('%s rbx' % op, 'rax=5 rdx=0 rbx=2')
    emit('%s rbx' % op, 'rax=5 rdx=1 rbx=2')
    emit('%s rbx' % op, 'rax=1234567890abcdef rdx=0 rbx=10000')
    emit('%s ebx' % op, 'rax=ffffffffffffffff rdx=ffffffffffffffff rbx=1')
    emit('%s ebx' % op, 'rax=80000000 rdx=ffffffff rbx=ffffffff')
    emit('%s ebx' % op, 'rax=80000000 rdx=ffffffff rbx=ffffffffffffffff')
    emit('%s ebx' % op, 'rax=ffffffff rdx=ffffffff rbx=2')
    emit('%s ebx' % op, 'rax=ffffffff rdx=0 rbx=2')
    emit('%s ebx' % op, 'rax=ffffffff rdx=0 rbx=0')
    emit('%s ebx' % op, 'rax=5 rdx=ffffffff00000000 rbx=2')
    emit('%s bx' % op, 'rax=ffff rdx=0 rbx=1')
    emit('%s bl' % op, 'rax=ffffffffffffffff rbx=1')
    emit('%s bl' % op, 'rax=ffff rbx=ff')
    emit('%s sil' % op, 'rax=ffff rsi=2')
    emit('%s ah' % op, 'rax=ffff')
for op in ('mul', 'imul'):
    emit('%s rbx' % op, 'rax=ffffffffffffffff rbx=ffffffffffffffff')
    emit('%s rbx' % op, 'rax=8000000000000000 rbx=ffffffffffffffff')
    emit('%s rbx' % op, 'rax=7fffffffffffffff rbx=2')
    emit('%s rbx' % op, 'rax=100000000 rbx=100000000')
    emit('%s ebx' % op, 'rax=ffffffff rbx=ffffffff')
    emit('%s ebx' % op, 'rax=ffffffffffffffff rbx=ffffffffffffffff rdx=ffffffffffffffff')
    emit('%s ebx' % op, 'rax=80000000 rbx=ffffffff')
    emit('%s bl' % op, 'rax=ffffffffffffffff rbx=ffffffffffffffff')
    emit('%s bx' % op, 'rax=ffffffffffffffff rbx=ffffffffffffffff rdx=ffffffffffffffff')
for dst, src in (('rax', 'rbx'), ('r9', 'r10'), ('rsi', 'rdi'), ('eax', 'ebx'), ('r9d', 'r10d'),
                 ('ax', 'bx'), ('r9w', 'r10w')):
    emit('imul %s, %s' % (dst, src))
    emit('imul %s, [rbx]' % dst)
for dst, src in (('rax', 'rbx'), ('r9', 'r10'), ('eax', 'ebx'), ('r9d', 'r10d'), ('ax', 'bx')):
    for imm in ('0', '1', '-1', '0x7f', '-0x80', '0x7fff', '0x12345678', '-0x80000000'):
        if dst.endswith('w') or dst in ('ax', 'bx'):
            if int(imm, 0) > 0x7fff or int(imm, 0) < -0x8000:
                continue
        emit('imul %s, %s, %s' % (dst, src, imm))
        emit('imul %s, [rbx], %s' % (dst, imm))
emit('imul rax, rbx', 'rax=4000000000000000 rbx=2')
emit('imul rax, rbx', 'rax=4000000000000000 rbx=4')
emit('imul rax, rbx', 'rax=ffffffffffffffff rbx=8000000000000000')
emit('imul eax, ebx', 'rax=40000000 rbx=2')
emit('imul eax, ebx', 'rax=ffffffff00000001 rbx=ffffffff')

section('push, pop, call, ret, leave, enter: the 8 byte stack, the 66 prefix is 2 bytes')
for r in ('rax', 'rbx', 'rbp', 'rsp', 'r8', 'r9', 'r12', 'r13', 'r15'):
    emit('push %s' % r)
    emit('pop %s' % r)
for r in ('ax', 'bx', 'bp', 'sp', 'r8w', 'r15w'):
    emit('push %s' % r)
    emit('pop %s' % r)
for imm in ('0', '1', '0x7f', '-1', '0x12345678', '-0x80000000', '0x7fffffff'):
    emit('push qword %s' % imm)
for imm in ('0', '1', '0x7f', '-1', '0x1234', '-0x8000', '0x7fff'):
    emit('push word %s' % imm)
emit('push qword [rbx]')
emit('push qword [rsp]')
emit('push qword [rsp+8]')
emit('push qword [r12+r13*8]')
emit('push word [rbx]')
emit('pop qword [rbx]')
emit('pop qword [rsp]')
emit('pop qword [rsp+8]')
emit('pop qword [r12+r13*8]')
emit('pop word [rbx]')
emit('pushfq')
emit('popfq')
emit('o16 pushfq')
emit('o16 popfq')
emit('pushfq', 'eflags=240ed7')
emit('popfq')
emit('leave')
emit('o16 leave')
emit('leave', 'rbp=ffffffff')
for size, nesting in (('0x10', 0), ('0x0', 0), ('0x20', 1), ('0x8', 2), ('0xfff8', 3), ('0x10', 31)):
    emit('enter %s, %d' % (size, nesting))
emit('call $+0x10')
emit('call $-0x10')
emit('call $+0x1000')
emit('call $+0x80000000')
emit('call $-0x80000000')
emit('call qword [rbx]')
emit('call qword [rsp]')
emit('call qword [rel $+0x40]')
emit('ret')
emit('ret 8')
emit('ret 0')
emit('ret 0xfff8')
emit('jmp $+0x10')
emit('jmp $-0x10')
emit('jmp $+0x80000000')
emit('jmp qword [rbx]')
emit('jmp qword [rel $+0x40]')
emit('jmp short $+0x7f')

section('rip relative operands (nasm [rel $+N]: N from the start of the instruction)')
# Unicorn doesn't complete a store to the page of the code, or to the next one: the stores
# are far from the instruction (oracle64.py). The addresses stay in 0..2^47
for n in ('0x10', '0x40', '0x1000', '-0x40', '-0x1000', '0x10000000', '-0x10000000'):
    for r in ('rax', 'r9'):
        emit('mov %s, [rel $+%s]' % (r, n))
        emit('lea %s, [rel $+%s]' % (r, n))
        emit('add %s, [rel $+%s]' % (r, n))
    emit('mov eax, [rel $+%s]' % n)
    emit('mov al, [rel $+%s]' % n)
    emit('cmp dword [rel $+%s], 5' % n)
    emit('cmp qword [rel $+%s], -1' % n)
    emit('movzx eax, byte [rel $+%s]' % n)
    emit('movsxd rax, dword [rel $+%s]' % n)
    emit('push qword [rel $+%s]' % n)
    emit('movss xmm0, [rel $+%s]' % n)
    emit('movaps xmm9, [rel $+%s]' % n)
    emit('movdqu xmm15, [rel $+%s]' % n)
    emit('fld qword [rel $+%s]' % n)
for n in ('0x2000', '-0x2000', '0x10000000', '-0x10000000'):
    for r in ('rax', 'r9'):
        emit('mov [rel $+%s], %s' % (n, r))
        emit('add [rel $+%s], %s' % (n, r))
    emit('mov [rel $+%s], ecx' % n)
    emit('mov word [rel $+%s], 5' % n)
    emit('mov qword [rel $+%s], 0x12345678' % n)
    emit('mov dword [rel $+%s], 0x12345678' % n)
    emit('inc qword [rel $+%s]' % n)
    emit('pop qword [rel $+%s]' % n)
    emit('xchg [rel $+%s], rax' % n)
    emit('cmpxchg [rel $+%s], rcx' % n)
    emit('fstp tword [rel $+%s]' % n)
emit('lea rax, [rel $+0]')
emit('lea rax, [rel $]')
emit('lea eax, [rel $+0x10]')
emit('lea r9d, [rel $-0x10]')
emit('lea ax, [rel $+0x10]')

section('lea, and the 32 bit address size (67 prefix: the registers are truncated to 32 bits)')
for text in ('lea rax, [rbx]', 'lea rax, [rbx+rcx]', 'lea rax, [rbx+rcx*2]', 'lea rax, [rbx+rcx*4+0x10]',
             'lea rax, [rbx+rcx*8-0x10]', 'lea rax, [rcx*8+0x12345678]', 'lea rax, [rcx*2]',
             'lea rax, [rcx+rcx*4]', 'lea rax, [0x12345678]', 'lea rax, [r8+r9]', 'lea rax, [r12]',
             'lea rax, [r13]', 'lea rax, [r12+r13*8+0x7fffffff]', 'lea rax, [rsp]', 'lea rax, [rsp+0x10]',
             'lea rax, [rbp-8]', 'lea r9, [rax+rax*2]', 'lea eax, [rbx+rcx*4+0x10]',
             'lea r9d, [r10+r11*8-0x100]', 'lea ax, [rbx+rcx*4+0x10]', 'lea r9w, [r10+r11]',
             'lea rax, [eax]', 'lea rax, [ebx+ecx]', 'lea rax, [ebx+ecx*2+0x10]', 'lea rax, [ebx+ecx*8-0x10]',
             'lea rax, [ecx*8+0x12345678]', 'lea eax, [ebx+ecx]', 'lea eax, [ebx+ecx*2+0x7fffffff]',
             'lea eax, [ebx-1]', 'lea ax, [ebx+ecx]', 'lea r8d, [r9d+r10d*4-8]', 'lea r8, [r9d+r10d*4-8]',
             'lea rax, [esp+8]', 'lea rax, [ebp-4]', 'lea rax, [r12d]', 'lea rax, [r13d]',
             'lea rax, [r8d+r9d]', 'lea rax, [eax+eax*8]', 'lea rax, [0x7fffffff]', 'lea eax, [0x12345678]',
             'lea rax, [-1]', 'lea rax, [-0x80000000]', 'lea eax, [ecx+edx-0x80000000]'):
    emit(text)
for text in ('mov rax, [ebx]', 'mov rax, [ebx+ecx*2]', 'mov eax, [ebx+8]', 'mov [ebx+ecx], rax',
             'mov [esi], ecx', 'add rax, [ebx]', 'mov al, [r9d]', 'mov rax, [r9d+r10d*8]',
             'movzx eax, byte [ebx]', 'push qword [ebx]', 'pop qword [ebx]', 'inc dword [ebx]',
             'mov rax, [esp]', 'push rax', 'cmpxchg [ebx], ecx'):
    emit(text)
emit('mov rax, [ebx+8]', 'rbx=ffffffff')
emit('mov rax, [ebx+8]', 'rbx=fffffffffffffffc')
emit('mov rax, [ebx+ecx]', 'rbx=ffffffff rcx=2')
emit('lea rax, [ebx+ecx]', 'rbx=ffffffff rcx=2')
emit('lea rax, [ebx+ecx]', 'rbx=ffffffffffffffff rcx=ffffffffffffffff')
emit('lea eax, [ebx+ecx]', 'rbx=ffffffff rcx=2')

section('string instructions: rsi, rdi, rcx, the direction flag, and the 32 bit address size (67)')
for op in ('movsb', 'movsw', 'movsd', 'movsq', 'cmpsb', 'cmpsw', 'cmpsd', 'cmpsq', 'scasb', 'scasw', 'scasd',
           'scasq', 'stosb', 'stosw', 'stosd', 'stosq', 'lodsb', 'lodsw', 'lodsd', 'lodsq'):
    emit(op)
    emit('a32 ' + op)
    for prefix in ('rep', 'repe', 'repne'):
        if prefix == 'rep' and op.startswith(('cmps', 'scas')):
            continue
        if prefix != 'rep' and not op.startswith(('cmps', 'scas')):
            continue
        emit('%s %s' % (prefix, op))
        emit('a32 %s %s' % (prefix, op))
for op in ('movsq', 'stosq', 'lodsq', 'scasq', 'cmpsq', 'movsb', 'stosb'):
    emit('rep ' + op, 'rcx=0')
    emit('rep ' + op, 'rcx=1')
    emit('a32 rep ' + op, 'rcx=100000002')
    emit('a32 rep ' + op, 'rcx=0')
    emit('a32 rep ' + op, 'rcx=ffffffff00000003')
emit('rep stosq', 'rcx=5 eflags=602 rax=1122334455667788')
emit('rep stosd', 'rcx=5 eflags=202 rax=1122334455667788')
emit('rep stosw', 'rcx=5 eflags=202 rax=1122334455667788')
emit('rep stosb', 'rcx=5 eflags=602 rax=1122334455667788')
emit('rep movsq', 'rcx=4 eflags=602')
emit('rep movsd', 'rcx=4 eflags=602')
emit('repe cmpsb', 'rcx=5')
emit('repne scasb', 'rcx=5 rax=0')
emit('repne scasq', 'rcx=5 rax=0')
emit('db 0x48, 0xa5')
emit('db 0x66, 0x48, 0xa5')
emit('db 0x66, 0xa5')
emit('db 0x67, 0xa5')
emit('db 0x67, 0x48, 0xa5')
emit('db 0xf3, 0x67, 0x48, 0xab', 'rcx=100000003')
emit('xlatb')
emit('a32 xlatb')
emit('db 0x48, 0xd7')
emit('xlatb', 'rbx=7ffffffff000 rax=ff')
emit('a32 xlatb', 'rbx=ffffffff rax=ff')

section('jrcxz, jecxz, loop')
for text in ('jrcxz $+0x10', 'jrcxz $-0x10', 'jecxz $+0x10', 'jecxz $-0x10', 'loop $+0x10', 'loop $-0x10',
             'loope $+0x10', 'loopne $+0x10', 'a32 loop $+0x10', 'a32 loope $+0x10', 'a32 loopne $-0x10'):
    emit(text)
    emit(text, 'rcx=0')
    emit(text, 'rcx=1')
    emit(text, 'rcx=100000000')
    emit(text, 'rcx=100000001')
    emit(text, 'rcx=ffffffff00000000')
    emit(text, 'rcx=ffffffffffffffff')
    emit(text, 'rcx=1 eflags=242')
    emit(text, 'rcx=2 eflags=202')
    emit(text, 'rcx=2 eflags=242')

section('cmpxchg, cmpxchg8b, cmpxchg16b (#GP when not 16 byte aligned), xadd')
emit('cmpxchg16b [rbx]')
emit('cmpxchg16b [r12+r13*8]')
emit('cmpxchg16b [rsp]')
emit('cmpxchg16b [rel $+0x40]')
emit('cmpxchg16b [ebx]')
emit('cmpxchg8b [rbx]')
emit('cmpxchg8b [r9]')
emit('lock cmpxchg8b [rbx]')
emit('lock cmpxchg16b [rbx]')
for dst, src in (('rbx', 'rcx'), ('r9', 'r10'), ('ebx', 'ecx'), ('r9d', 'r10d'), ('bx', 'cx'), ('bl', 'cl'),
                 ('sil', 'dil'), ('r9b', 'r10b')):
    emit('cmpxchg %s, %s' % (dst, src))
    emit('xadd %s, %s' % (dst, src))
for src in ('rcx', 'ecx', 'cx', 'cl', 'r9', 'r9d', 'r9w', 'r9b', 'sil'):
    size = {'r': 'qword', 'e': 'dword'}.get(src[0], 'word' if src.endswith('x') or src.endswith('w') else 'byte')
    emit('cmpxchg [rbx], %s' % src)
    emit('xadd [rbx], %s' % src)
    emit('lock cmpxchg [rbx], %s' % src)
    emit('lock xadd [rbx], %s' % src)
emit('cmpxchg rbx, rcx', 'rax=1122334455667788 rbx=1122334455667788 rcx=5')
emit('cmpxchg ebx, ecx', 'rax=1122334455667788 rbx=ffffffff55667788 rcx=5')
emit('cmpxchg [rsi], ecx', 'rax=0')
emit('lock add [rbx], rax')
emit('lock inc qword [rbx]')
emit('lock dec dword [rbx]')
emit('lock xchg [rbx], rax')
emit('lock btc [rbx], rcx')

section('bswap, bt / bts / btr / btc with 64 bit bit offsets')
for r in GP64[:4] + GP64[8:]:
    emit('bswap %s' % r)
for r in ('eax', 'ecx', 'edx', 'ebx', 'esi', 'edi', 'r8d', 'r9d', 'r15d'):
    emit('bswap %s' % r)
emit('bswap rax', 'rax=0102030405060708')
emit('bswap eax', 'rax=0102030405060708')
emit('bswap eax', 'rax=ffffffffffffffff')
for op in ('bt', 'bts', 'btr', 'btc'):
    for dst, src in (('rax', 'rcx'), ('r9', 'r10'), ('eax', 'ecx'), ('r9d', 'r10d'), ('ax', 'cx'), ('r9w', 'r10w')):
        emit('%s %s, %s' % (op, dst, src))
    for r in ('rax', 'r9', 'eax', 'r9d', 'ax', 'r9w'):
        for imm in (0, 1, 7, 15, 31, 32, 33, 63, 64, 65, 127, 255):
            if r in ('ax', 'r9w') and imm >= 256:
                continue
            emit('%s %s, %d' % (op, r, imm))
    for size, reg in (('qword', 'rcx'), ('dword', 'ecx'), ('word', 'cx')):
        emit('%s %s [rbx], %s' % (op, size, reg))
        emit('%s [rbx], %s' % (op, reg))
        emit('%s [r12+r13*8], %s' % (op, reg))
        for imm in (0, 1, 31, 32, 63, 64, 100, 255):
            emit('%s %s [rbx], %d' % (op, size, imm))
    for bit in (0, 1, 63, 64, 65, 100, 0xffffffffffffffff, 0xffffffffffffffc0, 0xffffffff,
                0x80000000, 0xffffffff80000000):
        emit('%s [rbx], rcx' % op, 'rcx=%x' % bit)
        emit('%s [rbx], ecx' % op, 'rcx=%x' % bit)
        emit('%s [rbx], cx' % op, 'rcx=%x' % bit)
        emit('%s rax, rcx' % op, 'rcx=%x' % bit)
    if op != 'bt':
        emit('lock %s [rbx], rcx' % op)
for op in ('bsf', 'bsr', 'popcnt', 'lzcnt', 'tzcnt'):
    for dst, src in (('rax', 'rbx'), ('r9', 'r10'), ('eax', 'ebx'), ('r9d', 'r10d'), ('ax', 'bx')):
        emit('%s %s, %s' % (op, dst, src))
        emit('%s %s, [rbx]' % (op, dst))
        emit('%s %s, %s' % (op, dst, src), 'rbx=0 r10=0 rax=1122334455667788')
        emit('%s %s, %s' % (op, dst, src), 'rbx=8000000000000000 r10=1 rax=1122334455667788')
        emit('%s %s, %s' % (op, dst, src), 'rbx=ffffffff00000000 r10=100000000 rax=1122334455667788')

section('movbe, crc32, adc/sbb chains, sign and zero extended immediates')
for dst in ('rax', 'r9', 'eax', 'r9d', 'ax', 'r9w'):
    emit('movbe %s, [rbx]' % dst)
    emit('movbe [rbx], %s' % dst)
for dst in ('eax', 'r9d'):
    emit('crc32 %s, byte [rbx]' % dst)
    emit('crc32 %s, word [rbx]' % dst)
    emit('crc32 %s, dword [rbx]' % dst)
    emit('crc32 %s, cl' % dst)
    emit('crc32 %s, sil' % dst)
    emit('crc32 %s, r10b' % dst)
    emit('crc32 %s, cx' % dst)
    emit('crc32 %s, ecx' % dst)
    emit('crc32 %s, r10d' % dst)
for dst in ('rax', 'r9'):
    emit('crc32 %s, byte [rbx]' % dst)
    emit('crc32 %s, qword [rbx]' % dst)
    emit('crc32 %s, cl' % dst)
    emit('crc32 %s, sil' % dst)
    emit('crc32 %s, rcx' % dst)
    emit('crc32 %s, r10' % dst)

section('flag instructions and the like')
for text in ('cld', 'std', 'clc', 'stc', 'cmc', 'sahf', 'lahf', 'nop', 'pause', 'int3', 'wait', 'fwait',
             'push 0x10', 'ud2', 'int 0x80', 'rdtsc', 'cpuid'):
    emit(text)

with open(OUT, 'w') as f:
    f.write('; Directed cases of the 64 bit mode for the semantics tests (dismount IR vs. Unicorn)\n')
    f.write('; GENERATED by tests/semantics/tools/gen_flags64.py\n')
    f.write('; Same format as tests/ia32/corpus. The states (registers, flags) are drawn by run_semantics.py;\n')
    f.write('; "; @state: rax=.. rcx=.." adds a state with those registers.\n')
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
