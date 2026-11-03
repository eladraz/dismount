; Segment registers and far branches for the semantics tests (dismount IR vs. Unicorn), bits 32
; Same format as tests/ia32/corpus. The states (selectors, the fixed GDT, valid far pointers
; at the absolute addresses) are drawn by run_semantics.py, see "State v2" in TEST_PLAN.md.
; @file-feature: i386
bits 32

; --- mov sreg, r/m16
mov es, ax
mov ss, ax
mov ds, ax
mov fs, ax
mov gs, ax
mov ds, bx
mov es, cx
mov fs, dx
mov gs, si
mov ds, edi
mov es, ebx
mov ds, word [esi]
mov es, word [edi+4]
mov fs, word [0x3000]
mov gs, word [0x3000]
mov ds, word [0x3000]
mov es, word [0x3000]
mov ss, word [0x3000]
mov ss, word [esp+4]
mov ds, word [fs:0x3000]
mov cs, ax

; --- mov r/m, sreg
mov ax, es
mov ax, cs
mov ax, ss
mov ax, ds
mov ax, fs
mov ax, gs
mov bx, ds
mov eax, es
mov eax, cs
mov eax, ss
mov eax, ds
mov ebx, fs
mov edi, gs
mov word [esi], es
mov word [esi], cs
mov word [esi], ss
mov word [0x3000], ds
mov word [esi+edi*2], fs
mov word [0x3000], gs
mov [esi], ds

; --- push / pop sreg
push es
push cs
push ss
push ds
push fs
push gs
o16 push es
o16 push cs
o16 push ds
o16 push gs
pop es
pop ss
pop ds
pop fs
pop gs
o16 pop es
o16 pop ds
o16 pop ss
o16 pop fs

; --- lds / les / lss / lfs / lgs
lds eax, [0x3000]
les ebx, [0x3000]
lss esp, [0x3000]
lss eax, [0x3000]
lfs ecx, [0x3000]
lgs edx, [0x3000]
lds esi, [esi]
les edi, [edi+8]
lss ebp, [0x3000]
lfs eax, [esi]
lgs eax, [0x3000]
o16 lds ax, [0x3000]
o16 les bx, [0x3000]
o16 lss sp, [0x3000]
o16 lfs cx, [0x3000]
o16 lgs dx, [0x3000]

; --- far jmp / call to ptr16:32 (the selectors of the GDT)
jmp 0x08:0x1234
jmp 0x20:0x1000
jmp 0x0b:0x1000
jmp 0x10:0x1000
jmp 0x00:0x1000
jmp 0x30:0x1000
jmp 0x38:0x1000
jmp 0x40:0x1000
jmp 0x1234:0x5678
jmp 0x28:0x1000
jmp 0x48:0x1000
call 0x08:0x1234
call 0x20:0x1000
call 0x0b:0x1000
call 0x10:0x1000
call 0x00:0x1000
call 0x38:0x1000
call 0x1234:0x5678
call 0x28:0x1000
call 0x48:0x1000
jmp word 0x08:0x1234
jmp word 0x20:0xffff
call word 0x08:0x1234
call word 0x20:0xfff0

; --- far jmp / call through memory (m16:32 and m16:16)
jmp far [0x3000]
jmp far [esi]
jmp far [esi+8]
jmp dword far [0x3000]
jmp word far [0x3000]
call far [0x3000]
call far [esi]
call dword far [0x3000]
call word far [0x3000]
call far [esp+4]

; --- retf / iret
retf
retf 8
retf 0xfff8
o16 retf
o16 retf 4
iret
o16 iret
