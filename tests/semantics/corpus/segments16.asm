; Segment registers and far branches for the semantics tests (dismount IR vs. Unicorn), bits 16
; Real mode: base = selector * 16, nothing is checked. See segments.asm.
; @file-feature: i386
bits 16

; --- mov sreg, r/m16
mov es, ax
mov ss, ax
mov ds, ax
mov fs, ax
mov gs, ax
mov ds, bx
mov es, cx
mov ds, word [si]
mov es, word [bx+di+4]
mov fs, word [0x3000]
mov gs, word [0x3000]
mov ds, word [0x3000]
mov ss, word [0x3000]
mov ss, word [bp+4]
mov ds, word [es:0x3000]
mov ds, eax
mov cs, ax

; --- mov r/m, sreg
mov ax, es
mov ax, cs
mov ax, ss
mov ax, ds
mov ax, fs
mov ax, gs
mov eax, es
mov eax, ds
mov ebx, gs
mov word [si], es
mov word [si], cs
mov word [0x3000], ds
mov word [bx+si], fs

; --- push / pop sreg
push es
push cs
push ss
push ds
push fs
push gs
o32 push es
o32 push ds
pop es
pop ss
pop ds
pop fs
pop gs
o32 pop es
o32 pop ds
o32 pop ss

; --- lds / les / lss / lfs / lgs
lds ax, [0x3000]
les bx, [0x3000]
lss sp, [0x3000]
lss ax, [0x3000]
lfs cx, [0x3000]
lgs dx, [0x3000]
lds si, [si]
les di, [bx+di+8]
o32 lds eax, [0x3000]
o32 les ebx, [0x3000]
o32 lss esp, [0x3000]
o32 lfs ecx, [0x3000]
o32 lgs edx, [0x3000]

; --- far jmp / call to ptr16:16 and ptr16:32
jmp 0x1234:0x5678
jmp 0x0000:0x2000
jmp 0xf000:0xfff0
jmp 0x0100:0x0010
jmp dword 0x1234:0x5678
jmp dword 0x0100:0x00012345
call 0x1234:0x5678
call 0x0000:0x2000
call 0xf000:0xfff0
call dword 0x1234:0x5678
call dword 0x0100:0x00012345

; --- far jmp / call through memory
jmp far [0x3000]
jmp far [si]
jmp far [bx+4]
jmp word far [0x3000]
jmp dword far [0x3000]
call far [0x3000]
call far [si]
call word far [0x3000]
call dword far [0x3000]
call far [bp+4]

; --- retf / iret
retf
retf 8
retf 0xfff8
o32 retf
o32 retf 4
iret
o32 iret
