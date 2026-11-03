; Flag edge cases for the semantics tests (dismount IR vs. Unicorn), bits 32
; Same format as tests/ia32/corpus. The states (registers, flags) are drawn by run_semantics.py.
; @file-feature: i386
bits 32

; --- add/adc/sub/sbb/cmp/and/or/xor/test/neg/not/inc/dec
add al, bl
add byte [esi], al
add al, [esi+edi*2+0x10]
add al, 0x0
add bl, 0x0
add byte [esi], 0x0
add al, 0x1
add bl, 0x1
add byte [esi], 0x1
add al, 0x7f
add bl, 0x7f
add byte [esi], 0x7f
add al, 0x80
add bl, 0x80
add byte [esi], 0x80
add al, 0xff
add bl, 0xff
add byte [esi], 0xff
add al, -0x1
add bl, -0x1
add byte [esi], -0x1
add al, -0x80
add bl, -0x80
add byte [esi], -0x80
add ax, bx
add word [esi], ax
add ax, [esi+edi*2+0x10]
add ax, 0x0
add bx, 0x0
add word [esi], 0x0
add ax, 0x1
add bx, 0x1
add word [esi], 0x1
add ax, 0x7f
add bx, 0x7f
add word [esi], 0x7f
add ax, 0x80
add bx, 0x80
add word [esi], 0x80
add ax, 0xff
add bx, 0xff
add word [esi], 0xff
add ax, 0x7fff
add bx, 0x7fff
add word [esi], 0x7fff
add ax, 0x8000
add bx, 0x8000
add word [esi], 0x8000
add ax, 0xffff
add bx, 0xffff
add word [esi], 0xffff
add ax, -0x1
add bx, -0x1
add word [esi], -0x1
add ax, -0x8000
add bx, -0x8000
add word [esi], -0x8000
add eax, ebx
add dword [esi], eax
add eax, [esi+edi*2+0x10]
add eax, 0x0
add ebx, 0x0
add dword [esi], 0x0
add eax, 0x1
add ebx, 0x1
add dword [esi], 0x1
add eax, 0x7f
add ebx, 0x7f
add dword [esi], 0x7f
add eax, 0x80
add ebx, 0x80
add dword [esi], 0x80
add eax, 0xff
add ebx, 0xff
add dword [esi], 0xff
add eax, 0x7fff
add ebx, 0x7fff
add dword [esi], 0x7fff
add eax, 0x8000
add ebx, 0x8000
add dword [esi], 0x8000
add eax, 0xffff
add ebx, 0xffff
add dword [esi], 0xffff
add eax, 0x7fffffff
add ebx, 0x7fffffff
add dword [esi], 0x7fffffff
add eax, 0x80000000
add ebx, 0x80000000
add dword [esi], 0x80000000
add eax, 0xffffffff
add ebx, 0xffffffff
add dword [esi], 0xffffffff
add eax, -0x1
add ebx, -0x1
add dword [esi], -0x1
add eax, -0x80000000
add ebx, -0x80000000
add dword [esi], -0x80000000
adc al, bl
adc byte [esi], al
adc al, [esi+edi*2+0x10]
adc al, 0x0
adc bl, 0x0
adc byte [esi], 0x0
adc al, 0x1
adc bl, 0x1
adc byte [esi], 0x1
adc al, 0x7f
adc bl, 0x7f
adc byte [esi], 0x7f
adc al, 0x80
adc bl, 0x80
adc byte [esi], 0x80
adc al, 0xff
adc bl, 0xff
adc byte [esi], 0xff
adc al, -0x1
adc bl, -0x1
adc byte [esi], -0x1
adc al, -0x80
adc bl, -0x80
adc byte [esi], -0x80
adc ax, bx
adc word [esi], ax
adc ax, [esi+edi*2+0x10]
adc ax, 0x0
adc bx, 0x0
adc word [esi], 0x0
adc ax, 0x1
adc bx, 0x1
adc word [esi], 0x1
adc ax, 0x7f
adc bx, 0x7f
adc word [esi], 0x7f
adc ax, 0x80
adc bx, 0x80
adc word [esi], 0x80
adc ax, 0xff
adc bx, 0xff
adc word [esi], 0xff
adc ax, 0x7fff
adc bx, 0x7fff
adc word [esi], 0x7fff
adc ax, 0x8000
adc bx, 0x8000
adc word [esi], 0x8000
adc ax, 0xffff
adc bx, 0xffff
adc word [esi], 0xffff
adc ax, -0x1
adc bx, -0x1
adc word [esi], -0x1
adc ax, -0x8000
adc bx, -0x8000
adc word [esi], -0x8000
adc eax, ebx
adc dword [esi], eax
adc eax, [esi+edi*2+0x10]
adc eax, 0x0
adc ebx, 0x0
adc dword [esi], 0x0
adc eax, 0x1
adc ebx, 0x1
adc dword [esi], 0x1
adc eax, 0x7f
adc ebx, 0x7f
adc dword [esi], 0x7f
adc eax, 0x80
adc ebx, 0x80
adc dword [esi], 0x80
adc eax, 0xff
adc ebx, 0xff
adc dword [esi], 0xff
adc eax, 0x7fff
adc ebx, 0x7fff
adc dword [esi], 0x7fff
adc eax, 0x8000
adc ebx, 0x8000
adc dword [esi], 0x8000
adc eax, 0xffff
adc ebx, 0xffff
adc dword [esi], 0xffff
adc eax, 0x7fffffff
adc ebx, 0x7fffffff
adc dword [esi], 0x7fffffff
adc eax, 0x80000000
adc ebx, 0x80000000
adc dword [esi], 0x80000000
adc eax, 0xffffffff
adc ebx, 0xffffffff
adc dword [esi], 0xffffffff
adc eax, -0x1
adc ebx, -0x1
adc dword [esi], -0x1
adc eax, -0x80000000
adc ebx, -0x80000000
adc dword [esi], -0x80000000
sub al, bl
sub byte [esi], al
sub al, [esi+edi*2+0x10]
sub al, 0x0
sub bl, 0x0
sub byte [esi], 0x0
sub al, 0x1
sub bl, 0x1
sub byte [esi], 0x1
sub al, 0x7f
sub bl, 0x7f
sub byte [esi], 0x7f
sub al, 0x80
sub bl, 0x80
sub byte [esi], 0x80
sub al, 0xff
sub bl, 0xff
sub byte [esi], 0xff
sub al, -0x1
sub bl, -0x1
sub byte [esi], -0x1
sub al, -0x80
sub bl, -0x80
sub byte [esi], -0x80
sub ax, bx
sub word [esi], ax
sub ax, [esi+edi*2+0x10]
sub ax, 0x0
sub bx, 0x0
sub word [esi], 0x0
sub ax, 0x1
sub bx, 0x1
sub word [esi], 0x1
sub ax, 0x7f
sub bx, 0x7f
sub word [esi], 0x7f
sub ax, 0x80
sub bx, 0x80
sub word [esi], 0x80
sub ax, 0xff
sub bx, 0xff
sub word [esi], 0xff
sub ax, 0x7fff
sub bx, 0x7fff
sub word [esi], 0x7fff
sub ax, 0x8000
sub bx, 0x8000
sub word [esi], 0x8000
sub ax, 0xffff
sub bx, 0xffff
sub word [esi], 0xffff
sub ax, -0x1
sub bx, -0x1
sub word [esi], -0x1
sub ax, -0x8000
sub bx, -0x8000
sub word [esi], -0x8000
sub eax, ebx
sub dword [esi], eax
sub eax, [esi+edi*2+0x10]
sub eax, 0x0
sub ebx, 0x0
sub dword [esi], 0x0
sub eax, 0x1
sub ebx, 0x1
sub dword [esi], 0x1
sub eax, 0x7f
sub ebx, 0x7f
sub dword [esi], 0x7f
sub eax, 0x80
sub ebx, 0x80
sub dword [esi], 0x80
sub eax, 0xff
sub ebx, 0xff
sub dword [esi], 0xff
sub eax, 0x7fff
sub ebx, 0x7fff
sub dword [esi], 0x7fff
sub eax, 0x8000
sub ebx, 0x8000
sub dword [esi], 0x8000
sub eax, 0xffff
sub ebx, 0xffff
sub dword [esi], 0xffff
sub eax, 0x7fffffff
sub ebx, 0x7fffffff
sub dword [esi], 0x7fffffff
sub eax, 0x80000000
sub ebx, 0x80000000
sub dword [esi], 0x80000000
sub eax, 0xffffffff
sub ebx, 0xffffffff
sub dword [esi], 0xffffffff
sub eax, -0x1
sub ebx, -0x1
sub dword [esi], -0x1
sub eax, -0x80000000
sub ebx, -0x80000000
sub dword [esi], -0x80000000
sbb al, bl
sbb byte [esi], al
sbb al, [esi+edi*2+0x10]
sbb al, 0x0
sbb bl, 0x0
sbb byte [esi], 0x0
sbb al, 0x1
sbb bl, 0x1
sbb byte [esi], 0x1
sbb al, 0x7f
sbb bl, 0x7f
sbb byte [esi], 0x7f
sbb al, 0x80
sbb bl, 0x80
sbb byte [esi], 0x80
sbb al, 0xff
sbb bl, 0xff
sbb byte [esi], 0xff
sbb al, -0x1
sbb bl, -0x1
sbb byte [esi], -0x1
sbb al, -0x80
sbb bl, -0x80
sbb byte [esi], -0x80
sbb ax, bx
sbb word [esi], ax
sbb ax, [esi+edi*2+0x10]
sbb ax, 0x0
sbb bx, 0x0
sbb word [esi], 0x0
sbb ax, 0x1
sbb bx, 0x1
sbb word [esi], 0x1
sbb ax, 0x7f
sbb bx, 0x7f
sbb word [esi], 0x7f
sbb ax, 0x80
sbb bx, 0x80
sbb word [esi], 0x80
sbb ax, 0xff
sbb bx, 0xff
sbb word [esi], 0xff
sbb ax, 0x7fff
sbb bx, 0x7fff
sbb word [esi], 0x7fff
sbb ax, 0x8000
sbb bx, 0x8000
sbb word [esi], 0x8000
sbb ax, 0xffff
sbb bx, 0xffff
sbb word [esi], 0xffff
sbb ax, -0x1
sbb bx, -0x1
sbb word [esi], -0x1
sbb ax, -0x8000
sbb bx, -0x8000
sbb word [esi], -0x8000
sbb eax, ebx
sbb dword [esi], eax
sbb eax, [esi+edi*2+0x10]
sbb eax, 0x0
sbb ebx, 0x0
sbb dword [esi], 0x0
sbb eax, 0x1
sbb ebx, 0x1
sbb dword [esi], 0x1
sbb eax, 0x7f
sbb ebx, 0x7f
sbb dword [esi], 0x7f
sbb eax, 0x80
sbb ebx, 0x80
sbb dword [esi], 0x80
sbb eax, 0xff
sbb ebx, 0xff
sbb dword [esi], 0xff
sbb eax, 0x7fff
sbb ebx, 0x7fff
sbb dword [esi], 0x7fff
sbb eax, 0x8000
sbb ebx, 0x8000
sbb dword [esi], 0x8000
sbb eax, 0xffff
sbb ebx, 0xffff
sbb dword [esi], 0xffff
sbb eax, 0x7fffffff
sbb ebx, 0x7fffffff
sbb dword [esi], 0x7fffffff
sbb eax, 0x80000000
sbb ebx, 0x80000000
sbb dword [esi], 0x80000000
sbb eax, 0xffffffff
sbb ebx, 0xffffffff
sbb dword [esi], 0xffffffff
sbb eax, -0x1
sbb ebx, -0x1
sbb dword [esi], -0x1
sbb eax, -0x80000000
sbb ebx, -0x80000000
sbb dword [esi], -0x80000000
cmp al, bl
cmp byte [esi], al
cmp al, [esi+edi*2+0x10]
cmp al, 0x0
cmp bl, 0x0
cmp byte [esi], 0x0
cmp al, 0x1
cmp bl, 0x1
cmp byte [esi], 0x1
cmp al, 0x7f
cmp bl, 0x7f
cmp byte [esi], 0x7f
cmp al, 0x80
cmp bl, 0x80
cmp byte [esi], 0x80
cmp al, 0xff
cmp bl, 0xff
cmp byte [esi], 0xff
cmp al, -0x1
cmp bl, -0x1
cmp byte [esi], -0x1
cmp al, -0x80
cmp bl, -0x80
cmp byte [esi], -0x80
cmp ax, bx
cmp word [esi], ax
cmp ax, [esi+edi*2+0x10]
cmp ax, 0x0
cmp bx, 0x0
cmp word [esi], 0x0
cmp ax, 0x1
cmp bx, 0x1
cmp word [esi], 0x1
cmp ax, 0x7f
cmp bx, 0x7f
cmp word [esi], 0x7f
cmp ax, 0x80
cmp bx, 0x80
cmp word [esi], 0x80
cmp ax, 0xff
cmp bx, 0xff
cmp word [esi], 0xff
cmp ax, 0x7fff
cmp bx, 0x7fff
cmp word [esi], 0x7fff
cmp ax, 0x8000
cmp bx, 0x8000
cmp word [esi], 0x8000
cmp ax, 0xffff
cmp bx, 0xffff
cmp word [esi], 0xffff
cmp ax, -0x1
cmp bx, -0x1
cmp word [esi], -0x1
cmp ax, -0x8000
cmp bx, -0x8000
cmp word [esi], -0x8000
cmp eax, ebx
cmp dword [esi], eax
cmp eax, [esi+edi*2+0x10]
cmp eax, 0x0
cmp ebx, 0x0
cmp dword [esi], 0x0
cmp eax, 0x1
cmp ebx, 0x1
cmp dword [esi], 0x1
cmp eax, 0x7f
cmp ebx, 0x7f
cmp dword [esi], 0x7f
cmp eax, 0x80
cmp ebx, 0x80
cmp dword [esi], 0x80
cmp eax, 0xff
cmp ebx, 0xff
cmp dword [esi], 0xff
cmp eax, 0x7fff
cmp ebx, 0x7fff
cmp dword [esi], 0x7fff
cmp eax, 0x8000
cmp ebx, 0x8000
cmp dword [esi], 0x8000
cmp eax, 0xffff
cmp ebx, 0xffff
cmp dword [esi], 0xffff
cmp eax, 0x7fffffff
cmp ebx, 0x7fffffff
cmp dword [esi], 0x7fffffff
cmp eax, 0x80000000
cmp ebx, 0x80000000
cmp dword [esi], 0x80000000
cmp eax, 0xffffffff
cmp ebx, 0xffffffff
cmp dword [esi], 0xffffffff
cmp eax, -0x1
cmp ebx, -0x1
cmp dword [esi], -0x1
cmp eax, -0x80000000
cmp ebx, -0x80000000
cmp dword [esi], -0x80000000
and al, bl
and byte [esi], al
and al, [esi+edi*2+0x10]
and al, 0x0
and bl, 0x0
and byte [esi], 0x0
and al, 0x1
and bl, 0x1
and byte [esi], 0x1
and al, 0x7f
and bl, 0x7f
and byte [esi], 0x7f
and al, 0x80
and bl, 0x80
and byte [esi], 0x80
and al, 0xff
and bl, 0xff
and byte [esi], 0xff
and al, -0x1
and bl, -0x1
and byte [esi], -0x1
and al, -0x80
and bl, -0x80
and byte [esi], -0x80
and ax, bx
and word [esi], ax
and ax, [esi+edi*2+0x10]
and ax, 0x0
and bx, 0x0
and word [esi], 0x0
and ax, 0x1
and bx, 0x1
and word [esi], 0x1
and ax, 0x7f
and bx, 0x7f
and word [esi], 0x7f
and ax, 0x80
and bx, 0x80
and word [esi], 0x80
and ax, 0xff
and bx, 0xff
and word [esi], 0xff
and ax, 0x7fff
and bx, 0x7fff
and word [esi], 0x7fff
and ax, 0x8000
and bx, 0x8000
and word [esi], 0x8000
and ax, 0xffff
and bx, 0xffff
and word [esi], 0xffff
and ax, -0x1
and bx, -0x1
and word [esi], -0x1
and ax, -0x8000
and bx, -0x8000
and word [esi], -0x8000
and eax, ebx
and dword [esi], eax
and eax, [esi+edi*2+0x10]
and eax, 0x0
and ebx, 0x0
and dword [esi], 0x0
and eax, 0x1
and ebx, 0x1
and dword [esi], 0x1
and eax, 0x7f
and ebx, 0x7f
and dword [esi], 0x7f
and eax, 0x80
and ebx, 0x80
and dword [esi], 0x80
and eax, 0xff
and ebx, 0xff
and dword [esi], 0xff
and eax, 0x7fff
and ebx, 0x7fff
and dword [esi], 0x7fff
and eax, 0x8000
and ebx, 0x8000
and dword [esi], 0x8000
and eax, 0xffff
and ebx, 0xffff
and dword [esi], 0xffff
and eax, 0x7fffffff
and ebx, 0x7fffffff
and dword [esi], 0x7fffffff
and eax, 0x80000000
and ebx, 0x80000000
and dword [esi], 0x80000000
and eax, 0xffffffff
and ebx, 0xffffffff
and dword [esi], 0xffffffff
and eax, -0x1
and ebx, -0x1
and dword [esi], -0x1
and eax, -0x80000000
and ebx, -0x80000000
and dword [esi], -0x80000000
or al, bl
or byte [esi], al
or al, [esi+edi*2+0x10]
or al, 0x0
or bl, 0x0
or byte [esi], 0x0
or al, 0x1
or bl, 0x1
or byte [esi], 0x1
or al, 0x7f
or bl, 0x7f
or byte [esi], 0x7f
or al, 0x80
or bl, 0x80
or byte [esi], 0x80
or al, 0xff
or bl, 0xff
or byte [esi], 0xff
or al, -0x1
or bl, -0x1
or byte [esi], -0x1
or al, -0x80
or bl, -0x80
or byte [esi], -0x80
or ax, bx
or word [esi], ax
or ax, [esi+edi*2+0x10]
or ax, 0x0
or bx, 0x0
or word [esi], 0x0
or ax, 0x1
or bx, 0x1
or word [esi], 0x1
or ax, 0x7f
or bx, 0x7f
or word [esi], 0x7f
or ax, 0x80
or bx, 0x80
or word [esi], 0x80
or ax, 0xff
or bx, 0xff
or word [esi], 0xff
or ax, 0x7fff
or bx, 0x7fff
or word [esi], 0x7fff
or ax, 0x8000
or bx, 0x8000
or word [esi], 0x8000
or ax, 0xffff
or bx, 0xffff
or word [esi], 0xffff
or ax, -0x1
or bx, -0x1
or word [esi], -0x1
or ax, -0x8000
or bx, -0x8000
or word [esi], -0x8000
or eax, ebx
or dword [esi], eax
or eax, [esi+edi*2+0x10]
or eax, 0x0
or ebx, 0x0
or dword [esi], 0x0
or eax, 0x1
or ebx, 0x1
or dword [esi], 0x1
or eax, 0x7f
or ebx, 0x7f
or dword [esi], 0x7f
or eax, 0x80
or ebx, 0x80
or dword [esi], 0x80
or eax, 0xff
or ebx, 0xff
or dword [esi], 0xff
or eax, 0x7fff
or ebx, 0x7fff
or dword [esi], 0x7fff
or eax, 0x8000
or ebx, 0x8000
or dword [esi], 0x8000
or eax, 0xffff
or ebx, 0xffff
or dword [esi], 0xffff
or eax, 0x7fffffff
or ebx, 0x7fffffff
or dword [esi], 0x7fffffff
or eax, 0x80000000
or ebx, 0x80000000
or dword [esi], 0x80000000
or eax, 0xffffffff
or ebx, 0xffffffff
or dword [esi], 0xffffffff
or eax, -0x1
or ebx, -0x1
or dword [esi], -0x1
or eax, -0x80000000
or ebx, -0x80000000
or dword [esi], -0x80000000
xor al, bl
xor byte [esi], al
xor al, [esi+edi*2+0x10]
xor al, 0x0
xor bl, 0x0
xor byte [esi], 0x0
xor al, 0x1
xor bl, 0x1
xor byte [esi], 0x1
xor al, 0x7f
xor bl, 0x7f
xor byte [esi], 0x7f
xor al, 0x80
xor bl, 0x80
xor byte [esi], 0x80
xor al, 0xff
xor bl, 0xff
xor byte [esi], 0xff
xor al, -0x1
xor bl, -0x1
xor byte [esi], -0x1
xor al, -0x80
xor bl, -0x80
xor byte [esi], -0x80
xor ax, bx
xor word [esi], ax
xor ax, [esi+edi*2+0x10]
xor ax, 0x0
xor bx, 0x0
xor word [esi], 0x0
xor ax, 0x1
xor bx, 0x1
xor word [esi], 0x1
xor ax, 0x7f
xor bx, 0x7f
xor word [esi], 0x7f
xor ax, 0x80
xor bx, 0x80
xor word [esi], 0x80
xor ax, 0xff
xor bx, 0xff
xor word [esi], 0xff
xor ax, 0x7fff
xor bx, 0x7fff
xor word [esi], 0x7fff
xor ax, 0x8000
xor bx, 0x8000
xor word [esi], 0x8000
xor ax, 0xffff
xor bx, 0xffff
xor word [esi], 0xffff
xor ax, -0x1
xor bx, -0x1
xor word [esi], -0x1
xor ax, -0x8000
xor bx, -0x8000
xor word [esi], -0x8000
xor eax, ebx
xor dword [esi], eax
xor eax, [esi+edi*2+0x10]
xor eax, 0x0
xor ebx, 0x0
xor dword [esi], 0x0
xor eax, 0x1
xor ebx, 0x1
xor dword [esi], 0x1
xor eax, 0x7f
xor ebx, 0x7f
xor dword [esi], 0x7f
xor eax, 0x80
xor ebx, 0x80
xor dword [esi], 0x80
xor eax, 0xff
xor ebx, 0xff
xor dword [esi], 0xff
xor eax, 0x7fff
xor ebx, 0x7fff
xor dword [esi], 0x7fff
xor eax, 0x8000
xor ebx, 0x8000
xor dword [esi], 0x8000
xor eax, 0xffff
xor ebx, 0xffff
xor dword [esi], 0xffff
xor eax, 0x7fffffff
xor ebx, 0x7fffffff
xor dword [esi], 0x7fffffff
xor eax, 0x80000000
xor ebx, 0x80000000
xor dword [esi], 0x80000000
xor eax, 0xffffffff
xor ebx, 0xffffffff
xor dword [esi], 0xffffffff
xor eax, -0x1
xor ebx, -0x1
xor dword [esi], -0x1
xor eax, -0x80000000
xor ebx, -0x80000000
xor dword [esi], -0x80000000
test al, bl
test byte [esi], al
test al, [esi+edi*2+0x10]
test al, 0x0
test bl, 0x0
test byte [esi], 0x0
test al, 0x1
test bl, 0x1
test byte [esi], 0x1
test al, 0x7f
test bl, 0x7f
test byte [esi], 0x7f
test al, 0x80
test bl, 0x80
test byte [esi], 0x80
test al, 0xff
test bl, 0xff
test byte [esi], 0xff
test al, -0x1
test bl, -0x1
test byte [esi], -0x1
test al, -0x80
test bl, -0x80
test byte [esi], -0x80
test ax, bx
test word [esi], ax
test ax, [esi+edi*2+0x10]
test ax, 0x0
test bx, 0x0
test word [esi], 0x0
test ax, 0x1
test bx, 0x1
test word [esi], 0x1
test ax, 0x7f
test bx, 0x7f
test word [esi], 0x7f
test ax, 0x80
test bx, 0x80
test word [esi], 0x80
test ax, 0xff
test bx, 0xff
test word [esi], 0xff
test ax, 0x7fff
test bx, 0x7fff
test word [esi], 0x7fff
test ax, 0x8000
test bx, 0x8000
test word [esi], 0x8000
test ax, 0xffff
test bx, 0xffff
test word [esi], 0xffff
test ax, -0x1
test bx, -0x1
test word [esi], -0x1
test ax, -0x8000
test bx, -0x8000
test word [esi], -0x8000
test eax, ebx
test dword [esi], eax
test eax, [esi+edi*2+0x10]
test eax, 0x0
test ebx, 0x0
test dword [esi], 0x0
test eax, 0x1
test ebx, 0x1
test dword [esi], 0x1
test eax, 0x7f
test ebx, 0x7f
test dword [esi], 0x7f
test eax, 0x80
test ebx, 0x80
test dword [esi], 0x80
test eax, 0xff
test ebx, 0xff
test dword [esi], 0xff
test eax, 0x7fff
test ebx, 0x7fff
test dword [esi], 0x7fff
test eax, 0x8000
test ebx, 0x8000
test dword [esi], 0x8000
test eax, 0xffff
test ebx, 0xffff
test dword [esi], 0xffff
test eax, 0x7fffffff
test ebx, 0x7fffffff
test dword [esi], 0x7fffffff
test eax, 0x80000000
test ebx, 0x80000000
test dword [esi], 0x80000000
test eax, 0xffffffff
test ebx, 0xffffffff
test dword [esi], 0xffffffff
test eax, -0x1
test ebx, -0x1
test dword [esi], -0x1
test eax, -0x80000000
test ebx, -0x80000000
test dword [esi], -0x80000000
neg al
neg bl
neg byte [esi]
neg ax
neg bx
neg word [esi]
neg eax
neg ebx
neg dword [esi]
not al
not bl
not byte [esi]
not ax
not bx
not word [esi]
not eax
not ebx
not dword [esi]
inc al
inc bl
inc byte [esi]
inc ax
inc bx
inc word [esi]
inc eax
inc ebx
inc dword [esi]
dec al
dec bl
dec byte [esi]
dec ax
dec bx
dec word [esi]
dec eax
dec ebx
dec dword [esi]
inc cx
dec cx
inc si
dec di
inc bp

; --- shifts and rotates: counts 0,1,7,8,9,15,16,17,31,32 by immediate and by cl
shl al, 0
shl al, 1
shl al, 7
shl al, 8
shl al, 9
shl al, 15
shl al, 16
shl al, 17
shl al, 31
shl al, 32
shl al, cl
shl bl, cl
shl byte [esi], 1
shl byte [esi], 3
shl byte [esi+edi*2+0x10], cl
shl ax, 0
shl ax, 1
shl ax, 7
shl ax, 8
shl ax, 9
shl ax, 15
shl ax, 16
shl ax, 17
shl ax, 31
shl ax, 32
shl ax, cl
shl bx, cl
shl word [esi], 1
shl word [esi], 3
shl word [esi+edi*2+0x10], cl
shl eax, 0
shl eax, 1
shl eax, 7
shl eax, 8
shl eax, 9
shl eax, 15
shl eax, 16
shl eax, 17
shl eax, 31
shl eax, 32
shl eax, cl
shl ebx, cl
shl dword [esi], 1
shl dword [esi], 3
shl dword [esi+edi*2+0x10], cl
shr al, 0
shr al, 1
shr al, 7
shr al, 8
shr al, 9
shr al, 15
shr al, 16
shr al, 17
shr al, 31
shr al, 32
shr al, cl
shr bl, cl
shr byte [esi], 1
shr byte [esi], 3
shr byte [esi+edi*2+0x10], cl
shr ax, 0
shr ax, 1
shr ax, 7
shr ax, 8
shr ax, 9
shr ax, 15
shr ax, 16
shr ax, 17
shr ax, 31
shr ax, 32
shr ax, cl
shr bx, cl
shr word [esi], 1
shr word [esi], 3
shr word [esi+edi*2+0x10], cl
shr eax, 0
shr eax, 1
shr eax, 7
shr eax, 8
shr eax, 9
shr eax, 15
shr eax, 16
shr eax, 17
shr eax, 31
shr eax, 32
shr eax, cl
shr ebx, cl
shr dword [esi], 1
shr dword [esi], 3
shr dword [esi+edi*2+0x10], cl
sar al, 0
sar al, 1
sar al, 7
sar al, 8
sar al, 9
sar al, 15
sar al, 16
sar al, 17
sar al, 31
sar al, 32
sar al, cl
sar bl, cl
sar byte [esi], 1
sar byte [esi], 3
sar byte [esi+edi*2+0x10], cl
sar ax, 0
sar ax, 1
sar ax, 7
sar ax, 8
sar ax, 9
sar ax, 15
sar ax, 16
sar ax, 17
sar ax, 31
sar ax, 32
sar ax, cl
sar bx, cl
sar word [esi], 1
sar word [esi], 3
sar word [esi+edi*2+0x10], cl
sar eax, 0
sar eax, 1
sar eax, 7
sar eax, 8
sar eax, 9
sar eax, 15
sar eax, 16
sar eax, 17
sar eax, 31
sar eax, 32
sar eax, cl
sar ebx, cl
sar dword [esi], 1
sar dword [esi], 3
sar dword [esi+edi*2+0x10], cl
rol al, 0
rol al, 1
rol al, 7
rol al, 8
rol al, 9
rol al, 15
rol al, 16
rol al, 17
rol al, 31
rol al, 32
rol al, cl
rol bl, cl
rol byte [esi], 1
rol byte [esi], 3
rol byte [esi+edi*2+0x10], cl
rol ax, 0
rol ax, 1
rol ax, 7
rol ax, 8
rol ax, 9
rol ax, 15
rol ax, 16
rol ax, 17
rol ax, 31
rol ax, 32
rol ax, cl
rol bx, cl
rol word [esi], 1
rol word [esi], 3
rol word [esi+edi*2+0x10], cl
rol eax, 0
rol eax, 1
rol eax, 7
rol eax, 8
rol eax, 9
rol eax, 15
rol eax, 16
rol eax, 17
rol eax, 31
rol eax, 32
rol eax, cl
rol ebx, cl
rol dword [esi], 1
rol dword [esi], 3
rol dword [esi+edi*2+0x10], cl
ror al, 0
ror al, 1
ror al, 7
ror al, 8
ror al, 9
ror al, 15
ror al, 16
ror al, 17
ror al, 31
ror al, 32
ror al, cl
ror bl, cl
ror byte [esi], 1
ror byte [esi], 3
ror byte [esi+edi*2+0x10], cl
ror ax, 0
ror ax, 1
ror ax, 7
ror ax, 8
ror ax, 9
ror ax, 15
ror ax, 16
ror ax, 17
ror ax, 31
ror ax, 32
ror ax, cl
ror bx, cl
ror word [esi], 1
ror word [esi], 3
ror word [esi+edi*2+0x10], cl
ror eax, 0
ror eax, 1
ror eax, 7
ror eax, 8
ror eax, 9
ror eax, 15
ror eax, 16
ror eax, 17
ror eax, 31
ror eax, 32
ror eax, cl
ror ebx, cl
ror dword [esi], 1
ror dword [esi], 3
ror dword [esi+edi*2+0x10], cl
rcl al, 0
rcl al, 1
rcl al, 7
rcl al, 8
rcl al, 9
rcl al, 15
rcl al, 16
rcl al, 17
rcl al, 31
rcl al, 32
rcl al, cl
rcl bl, cl
rcl byte [esi], 1
rcl byte [esi], 3
rcl byte [esi+edi*2+0x10], cl
rcl ax, 0
rcl ax, 1
rcl ax, 7
rcl ax, 8
rcl ax, 9
rcl ax, 15
rcl ax, 16
rcl ax, 17
rcl ax, 31
rcl ax, 32
rcl ax, cl
rcl bx, cl
rcl word [esi], 1
rcl word [esi], 3
rcl word [esi+edi*2+0x10], cl
rcl eax, 0
rcl eax, 1
rcl eax, 7
rcl eax, 8
rcl eax, 9
rcl eax, 15
rcl eax, 16
rcl eax, 17
rcl eax, 31
rcl eax, 32
rcl eax, cl
rcl ebx, cl
rcl dword [esi], 1
rcl dword [esi], 3
rcl dword [esi+edi*2+0x10], cl
rcr al, 0
rcr al, 1
rcr al, 7
rcr al, 8
rcr al, 9
rcr al, 15
rcr al, 16
rcr al, 17
rcr al, 31
rcr al, 32
rcr al, cl
rcr bl, cl
rcr byte [esi], 1
rcr byte [esi], 3
rcr byte [esi+edi*2+0x10], cl
rcr ax, 0
rcr ax, 1
rcr ax, 7
rcr ax, 8
rcr ax, 9
rcr ax, 15
rcr ax, 16
rcr ax, 17
rcr ax, 31
rcr ax, 32
rcr ax, cl
rcr bx, cl
rcr word [esi], 1
rcr word [esi], 3
rcr word [esi+edi*2+0x10], cl
rcr eax, 0
rcr eax, 1
rcr eax, 7
rcr eax, 8
rcr eax, 9
rcr eax, 15
rcr eax, 16
rcr eax, 17
rcr eax, 31
rcr eax, 32
rcr eax, cl
rcr ebx, cl
rcr dword [esi], 1
rcr dword [esi], 3
rcr dword [esi+edi*2+0x10], cl
sal al, 0
sal al, 1
sal al, 7
sal al, 8
sal al, 9
sal al, 15
sal al, 16
sal al, 17
sal al, 31
sal al, 32
sal al, cl
sal bl, cl
sal byte [esi], 1
sal byte [esi], 3
sal byte [esi+edi*2+0x10], cl
sal ax, 0
sal ax, 1
sal ax, 7
sal ax, 8
sal ax, 9
sal ax, 15
sal ax, 16
sal ax, 17
sal ax, 31
sal ax, 32
sal ax, cl
sal bx, cl
sal word [esi], 1
sal word [esi], 3
sal word [esi+edi*2+0x10], cl
sal eax, 0
sal eax, 1
sal eax, 7
sal eax, 8
sal eax, 9
sal eax, 15
sal eax, 16
sal eax, 17
sal eax, 31
sal eax, 32
sal eax, cl
sal ebx, cl
sal dword [esi], 1
sal dword [esi], 3
sal dword [esi+edi*2+0x10], cl
shld ax, bx, 0
shld ax, bx, 1
shld ax, bx, 7
shld ax, bx, 8
shld ax, bx, 9
shld ax, bx, 15
shld ax, bx, 16
shld ax, bx, 17
shld ax, bx, 31
shld ax, bx, 32
shld ax, bx, cl
shld word [esi], bx, 5
shld word [esi+edi*2+0x10], bx, cl
shld eax, ebx, 0
shld eax, ebx, 1
shld eax, ebx, 7
shld eax, ebx, 8
shld eax, ebx, 9
shld eax, ebx, 15
shld eax, ebx, 16
shld eax, ebx, 17
shld eax, ebx, 31
shld eax, ebx, 32
shld eax, ebx, cl
shld dword [esi], ebx, 5
shld dword [esi+edi*2+0x10], ebx, cl
shrd ax, bx, 0
shrd ax, bx, 1
shrd ax, bx, 7
shrd ax, bx, 8
shrd ax, bx, 9
shrd ax, bx, 15
shrd ax, bx, 16
shrd ax, bx, 17
shrd ax, bx, 31
shrd ax, bx, 32
shrd ax, bx, cl
shrd word [esi], bx, 5
shrd word [esi+edi*2+0x10], bx, cl
shrd eax, ebx, 0
shrd eax, ebx, 1
shrd eax, ebx, 7
shrd eax, ebx, 8
shrd eax, ebx, 9
shrd eax, ebx, 15
shrd eax, ebx, 16
shrd eax, ebx, 17
shrd eax, ebx, 31
shrd eax, ebx, 32
shrd eax, ebx, cl
shrd dword [esi], ebx, 5
shrd dword [esi+edi*2+0x10], ebx, cl

; --- multiply and divide (divide errors come out of the random states)
mul bl
mul byte [esi]
mul bx
mul word [esi]
mul ebx
mul dword [esi]
imul bl
imul byte [esi]
imul bx
imul word [esi]
imul ebx
imul dword [esi]
div bl
div byte [esi]
div bx
div word [esi]
div ebx
div dword [esi]
idiv bl
idiv byte [esi]
idiv bx
idiv word [esi]
idiv ebx
idiv dword [esi]
imul ax, bx
imul ax, [esi]
imul ax, bx, 0x7f
imul ax, [esi], -0x80
imul ax, bx, 0x1234
imul ax, [esi], 0x7fff
imul eax, ebx
imul eax, [esi]
imul eax, ebx, 0x7f
imul eax, [esi], -0x80
imul eax, ebx, 0x1234
imul eax, [esi], 0x7fff
imul eax, ebx, 0x12345678
imul eax, [esi], 0x80000000
aam
aam 0x10
aad
aad 0x10

; --- bit instructions
bt ax, bx
bt word [esi], bx
bt ax, 0
bt word [esi], 0
bt ax, 1
bt word [esi], 1
bt ax, 7
bt word [esi], 7
bt ax, 8
bt word [esi], 8
bt ax, 15
bt word [esi], 15
bt ax, 16
bt word [esi], 16
bt ax, 31
bt word [esi], 31
bt ax, 33
bt word [esi], 33
bt ax, 255
bt word [esi], 255
bt eax, ebx
bt dword [esi], ebx
bt eax, 0
bt dword [esi], 0
bt eax, 1
bt dword [esi], 1
bt eax, 7
bt dword [esi], 7
bt eax, 8
bt dword [esi], 8
bt eax, 15
bt dword [esi], 15
bt eax, 16
bt dword [esi], 16
bt eax, 31
bt dword [esi], 31
bt eax, 33
bt dword [esi], 33
bt eax, 255
bt dword [esi], 255
bts ax, bx
bts word [esi], bx
bts ax, 0
bts word [esi], 0
bts ax, 1
bts word [esi], 1
bts ax, 7
bts word [esi], 7
bts ax, 8
bts word [esi], 8
bts ax, 15
bts word [esi], 15
bts ax, 16
bts word [esi], 16
bts ax, 31
bts word [esi], 31
bts ax, 33
bts word [esi], 33
bts ax, 255
bts word [esi], 255
bts eax, ebx
bts dword [esi], ebx
bts eax, 0
bts dword [esi], 0
bts eax, 1
bts dword [esi], 1
bts eax, 7
bts dword [esi], 7
bts eax, 8
bts dword [esi], 8
bts eax, 15
bts dword [esi], 15
bts eax, 16
bts dword [esi], 16
bts eax, 31
bts dword [esi], 31
bts eax, 33
bts dword [esi], 33
bts eax, 255
bts dword [esi], 255
btr ax, bx
btr word [esi], bx
btr ax, 0
btr word [esi], 0
btr ax, 1
btr word [esi], 1
btr ax, 7
btr word [esi], 7
btr ax, 8
btr word [esi], 8
btr ax, 15
btr word [esi], 15
btr ax, 16
btr word [esi], 16
btr ax, 31
btr word [esi], 31
btr ax, 33
btr word [esi], 33
btr ax, 255
btr word [esi], 255
btr eax, ebx
btr dword [esi], ebx
btr eax, 0
btr dword [esi], 0
btr eax, 1
btr dword [esi], 1
btr eax, 7
btr dword [esi], 7
btr eax, 8
btr dword [esi], 8
btr eax, 15
btr dword [esi], 15
btr eax, 16
btr dword [esi], 16
btr eax, 31
btr dword [esi], 31
btr eax, 33
btr dword [esi], 33
btr eax, 255
btr dword [esi], 255
btc ax, bx
btc word [esi], bx
btc ax, 0
btc word [esi], 0
btc ax, 1
btc word [esi], 1
btc ax, 7
btc word [esi], 7
btc ax, 8
btc word [esi], 8
btc ax, 15
btc word [esi], 15
btc ax, 16
btc word [esi], 16
btc ax, 31
btc word [esi], 31
btc ax, 33
btc word [esi], 33
btc ax, 255
btc word [esi], 255
btc eax, ebx
btc dword [esi], ebx
btc eax, 0
btc dword [esi], 0
btc eax, 1
btc dword [esi], 1
btc eax, 7
btc dword [esi], 7
btc eax, 8
btc dword [esi], 8
btc eax, 15
btc dword [esi], 15
btc eax, 16
btc dword [esi], 16
btc eax, 31
btc dword [esi], 31
btc eax, 33
btc dword [esi], 33
btc eax, 255
btc dword [esi], 255
bsf ax, bx
bsf ax, [esi]
bsf eax, ebx
bsf eax, [esi]
bsr ax, bx
bsr ax, [esi]
bsr eax, ebx
bsr eax, [esi]

; --- setcc / cmovcc / jcc, all 16 conditions
seto al
seto byte [esi]
cmovo ax, bx
cmovo eax, ebx
cmovo eax, [esi]
jo $+0x10
jo $-0x10
jo $+2
jo near $+0x100
jo near $-0x100
setno al
setno byte [esi]
cmovno ax, bx
cmovno eax, ebx
cmovno eax, [esi]
jno $+0x10
jno $-0x10
jno $+2
jno near $+0x100
jno near $-0x100
setb al
setb byte [esi]
cmovb ax, bx
cmovb eax, ebx
cmovb eax, [esi]
jb $+0x10
jb $-0x10
jb $+2
jb near $+0x100
jb near $-0x100
setae al
setae byte [esi]
cmovae ax, bx
cmovae eax, ebx
cmovae eax, [esi]
jae $+0x10
jae $-0x10
jae $+2
jae near $+0x100
jae near $-0x100
sete al
sete byte [esi]
cmove ax, bx
cmove eax, ebx
cmove eax, [esi]
je $+0x10
je $-0x10
je $+2
je near $+0x100
je near $-0x100
setne al
setne byte [esi]
cmovne ax, bx
cmovne eax, ebx
cmovne eax, [esi]
jne $+0x10
jne $-0x10
jne $+2
jne near $+0x100
jne near $-0x100
setbe al
setbe byte [esi]
cmovbe ax, bx
cmovbe eax, ebx
cmovbe eax, [esi]
jbe $+0x10
jbe $-0x10
jbe $+2
jbe near $+0x100
jbe near $-0x100
seta al
seta byte [esi]
cmova ax, bx
cmova eax, ebx
cmova eax, [esi]
ja $+0x10
ja $-0x10
ja $+2
ja near $+0x100
ja near $-0x100
sets al
sets byte [esi]
cmovs ax, bx
cmovs eax, ebx
cmovs eax, [esi]
js $+0x10
js $-0x10
js $+2
js near $+0x100
js near $-0x100
setns al
setns byte [esi]
cmovns ax, bx
cmovns eax, ebx
cmovns eax, [esi]
jns $+0x10
jns $-0x10
jns $+2
jns near $+0x100
jns near $-0x100
setp al
setp byte [esi]
cmovp ax, bx
cmovp eax, ebx
cmovp eax, [esi]
jp $+0x10
jp $-0x10
jp $+2
jp near $+0x100
jp near $-0x100
setnp al
setnp byte [esi]
cmovnp ax, bx
cmovnp eax, ebx
cmovnp eax, [esi]
jnp $+0x10
jnp $-0x10
jnp $+2
jnp near $+0x100
jnp near $-0x100
setl al
setl byte [esi]
cmovl ax, bx
cmovl eax, ebx
cmovl eax, [esi]
jl $+0x10
jl $-0x10
jl $+2
jl near $+0x100
jl near $-0x100
setge al
setge byte [esi]
cmovge ax, bx
cmovge eax, ebx
cmovge eax, [esi]
jge $+0x10
jge $-0x10
jge $+2
jge near $+0x100
jge near $-0x100
setle al
setle byte [esi]
cmovle ax, bx
cmovle eax, ebx
cmovle eax, [esi]
jle $+0x10
jle $-0x10
jle $+2
jle near $+0x100
jle near $-0x100
setg al
setg byte [esi]
cmovg ax, bx
cmovg eax, ebx
cmovg eax, [esi]
jg $+0x10
jg $-0x10
jg $+2
jg near $+0x100
jg near $-0x100
setz al
setnz al
setc al
setnc al
cmovz ax, bx
cmovnz eax, ebx
jz $+5
jnz $+5
jc $+5
jnc $+5
jpe $+5
jpo $+5
jna $+5
jnae $+5
jnb $+5
jnbe $+5
jng $+5
jnge $+5
jnl $+5
jnle $+5

; --- xadd / cmpxchg / cmpxchg8b / xchg, with and without lock
xadd al, bl
xadd byte [esi], bl
lock xadd byte [esi], bl
cmpxchg bl, dl
cmpxchg byte [esi], bl
lock cmpxchg byte [esi], bl
xchg al, bl
xchg byte [esi], bl
lock xchg byte [esi], bl
lock add byte [esi], bl
lock inc byte [esi]
lock dec byte [esi]
lock neg byte [esi]
lock not byte [esi]
nop
xadd ax, bx
xadd word [esi], bx
lock xadd word [esi], bx
cmpxchg bx, dx
cmpxchg word [esi], bx
lock cmpxchg word [esi], bx
xchg ax, bx
xchg word [esi], bx
lock xchg word [esi], bx
lock add word [esi], bx
lock inc word [esi]
lock dec word [esi]
lock neg word [esi]
lock not word [esi]
lock bts word [esi], 3
xadd eax, ebx
xadd dword [esi], ebx
lock xadd dword [esi], ebx
cmpxchg ebx, edx
cmpxchg dword [esi], ebx
lock cmpxchg dword [esi], ebx
xchg eax, ebx
xchg dword [esi], ebx
lock xchg dword [esi], ebx
lock add dword [esi], ebx
lock inc dword [esi]
lock dec dword [esi]
lock neg dword [esi]
lock not dword [esi]
lock bts dword [esi], 3
cmpxchg8b qword [esi]
lock cmpxchg8b qword [esi]
cmpxchg8b qword [esi+edi*2+0x10]
xchg eax, ecx
xchg bx, si

; --- string instructions with rep / repe / repne
movsb
rep movsb
repe movsb
repne movsb
movsw
rep movsw
repe movsw
repne movsw
movsd
rep movsd
repe movsd
repne movsd
cmpsb
rep cmpsb
repe cmpsb
repne cmpsb
cmpsw
rep cmpsw
repe cmpsw
repne cmpsw
cmpsd
rep cmpsd
repe cmpsd
repne cmpsd
scasb
rep scasb
repe scasb
repne scasb
scasw
rep scasw
repe scasw
repne scasw
scasd
rep scasd
repe scasd
repne scasd
stosb
rep stosb
repe stosb
repne stosb
stosw
rep stosw
repe stosw
repne stosw
stosd
rep stosd
repe stosd
repne stosd
lodsb
rep lodsb
repe lodsb
repne lodsb
lodsw
rep lodsw
repe lodsw
repne lodsw
lodsd
rep lodsd
repe lodsd
repne lodsd
a16 rep movsb
a16 repe cmpsb
xlatb
xlat
a16 xlatb

; --- stack and flags transfer
push ax
pop ax
push bx
pop bx
push cx
pop cx
push dx
pop dx
push si
pop si
push di
pop di
push bp
pop bp
push sp
pop sp
push eax
pop eax
push ebx
pop ebx
push esi
pop esi
push ebp
pop ebp
push esp
pop esp
push dword 0x12345678
push word 0x1234
push byte -1
push byte 0x7f
push dword -1
push dword [esi]
push word [esi]
pop dword [esi]
pop word [esi]
pop dword [esi+edi*2+0x10]
push es
push cs
push ss
push ds
push fs
push gs
pushf
popf
pushfd
popfd
pushfw
popfw
lahf
sahf
pusha
popa
pushad
popad
pushaw
popaw
clc
stc
cmc
cld
std
enter 0x10, 0
enter 8, 1
enter 0, 0
enter 4, 2
enter 0x20, 3
enter 0, 31
enterw 8, 0
enterd 8, 0
leave
leavew
leaved

; --- sign extension, byte swap, BCD
cbw
cwde
cwd
cdq
bswap eax
bswap ebx
bswap ecx
bswap edx
bswap esi
bswap edi
bswap ebp
bswap esp
daa
das
aaa
aas
salc
xchg eax, eax
mov ax, bx
mov eax, ebx
mov al, bl
mov ah, bl
mov bh, al
movzx ax, bl
movzx eax, bl
movzx eax, bx
movzx eax, byte [esi]
movzx eax, word [esi]
movsx ax, bl
movsx eax, bl
movsx eax, bx
movsx eax, byte [esi]
movsx eax, word [esi]
lea ax, [esi+edi*2+0x10]
lea eax, [esi+edi*2+0x10]
lea eax, [eax+eax*4]

; --- loop / jecxz / jmp / call / ret
loop $+0x10
loop $-0x10
loop $+2
loope $+0x10
loope $-0x10
loope $+2
loopne $+0x10
loopne $-0x10
loopne $+2
loopz $+0x10
loopz $-0x10
loopz $+2
loopnz $+0x10
loopnz $-0x10
loopnz $+2
jecxz $+0x10
jecxz $-0x10
jecxz $+2
jcxz $+0x10
jcxz $-0x10
jcxz $+2
a16 loop $+0x10
a16 loopne $-0x10
jmp $+0x10
jmp $-0x10
jmp short $+0x7f
jmp near $+0x1000
jmp near $-0x1000
jmp $+2
jmp eax
jmp [esi]
jmp dword [esi]
jmp word [esi]
jmp ax
call $+0x100
call $-0x100
call $+5
call eax
call ebx
call [esi]
call dword [esi]
call word [esi]
call ax
ret
retn
ret 0
ret 8
ret 0x10
ret 0xfffc
retw
retw 4
retd
retd 8

; --- interrupts and traps
int3
int 0x21
int 0
int 3
into
int1
ud2
bound ax, [esi]
bound eax, [esi]
