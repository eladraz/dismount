# IA-32 compatibility test plan (dismount vs. nasm)

Goal: for a known assembly source file, prove that
1. **dismount assembles** every line to the same bytes nasm produces, and
2. **dismount disassembles** nasm's bytes back to an instruction that means
   the same thing (same length, same bytes when re-assembled by nasm, same text
   after normalization),

for every IA-32 (32-bit, plus 16-bit operating mode) instruction form, every
addressing mode and every ISA extension ("feature"). Where dismount does not yet
implement something, the report says so explicitly instead of failing silently.

Reference material:
- Intel SDM Vol. 2 (Instruction Set Reference), order number **325383-093US**,
  Sept 2026, from https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html.
  The opcode maps are in Appendix A, and the instruction formats in Appendix B.
- nasm 3.02, and `x86/insns.dat` from the nasm repo, used as a machine-readable cross-check
  of the SDM.

## Layout

```
tests/ia32/
  TEST_PLAN.md            this file
  corpus/*.asm            the test sources (generated, checked in, plain nasm)
  corpus64/*.asm          the same for 64-bit mode (`bits 64`), see "64-bit mode"
  tools/gen_corpus.py     regenerates corpus/ and corpus64/ from insns.dat + the matrices
  tools/gen_future_opcodes.py  regenerates opcodeTableFuture.inc, the opcode inventory
  dismount_cli.cpp        `asm`, `dis`, `run` and `cpuinfo` sub-commands over libdismount
                          (`--bits 16|32|64`)
  cpuprobe.c              stand-alone CPUID probe (used under Rosetta on arm64 macOS)
  opcodeTableFuture.inc   generated, comment-only inventory of the opcodes not implemented
  run_compat.py           the driver: nasm <-> dismount comparison + report
  tools/fuzz_lengths.py   random byte sequences: dismount vs ndisasm lengths and errors
  tools/fuzz_roundtrip.py random byte sequences: dismount decoder -> assembler -> decoder
  tools/fuzz_validity.py  systematic sweep (opcode x ModR/M x prefix, 16/32/64 bit): which
                          encodings are valid, dismount vs ndisasm
  validity_exceptions.txt the encodings where dismount follows the SDM and not ndisasm
  asm_errors.txt          lines the 32-bit assembler must reject
  asm_errors64.txt        lines the 64-bit assembler must reject
```

Build: `cmake -DDISMOUNT_BUILD_TESTS=ON ..` adds `dismount_cli` and `test_dismount`
(the existing xStl-harness unit test, with `tests/TestAMD64Disassembler.cpp`: the
directed decode cases of 64-bit mode, and `tests/TestAMD64Assembler.cpp`: the 64-bit
assembler, see "The assembler in 64-bit mode") to the CMake build.

## Corpus format

Every corpus file is a valid nasm source that you can assemble directly:
`nasm -f bin corpus/sse2.asm -o /dev/null`.

```nasm
; @file-feature: sse2
bits 32
paddq xmm1, xmm2                 ; @feature: sse2
paddq xmm1, [eax+ecx*4+0x10]     ; @feature: sse2
```

- One instruction per line. `bits 16` / `bits 32` select the operating mode. The
  feature tag is optional per line and defaults to `@file-feature`.
- Syntax is a fixed **nasm subset** that dismount must also accept:
  - lowercase;
  - hex as `0x..`;
  - size keywords `byte`/`word`/`dword`/`qword`/`tword`/`oword`/`yword`/`zword`
    without `ptr`, given only where nasm needs them;
  - memory as `[seg:base+index*scale+disp]`;
  - relative branches as `$+N` / `$-N`, measured from the start of the
    instruction (nasm semantics);
  - `;` comments.
- Feature names are the Linux `/proc/cpuinfo` flag names (`fpu`, `mmx`, `sse`,
  `sse2`, `pni` for SSE3, `ssse3`, `sse4_1`, `sse4_2`, `aes`, `pclmulqdq`, `sha_ni`,
  `avx`, `avx2`, `fma`, `f16c`, `bmi1`, `bmi2`, `adx`, `avx512f`, ...). The baseline
  i386 set is `i386`, and `i486`, `pentium`, `p6` (cmov, ...) are separate tags.

Corpus files:
| file | content |
|---|---|
| `00_addressing32.asm` | every ModRM mod/rm, every SIB base/index/scale (incl. no-index, ESP base, EBP/disp32 special cases), disp8/disp32 boundaries (0x7f, 0x80, -0x80, -0x81), direct address, for a representative set of instructions (mov, add, lea, inc, push, test, movzx, imul) |
| `01_addressing16.asm` | 16-bit addressing via the 0x67 prefix in 32-bit mode: [bx+si] ... [bp+di+disp16] |
| `02_prefixes.asm` | 0x66 operand size, segment overrides (es/cs/ss/ds/fs/gs), lock, rep/repe/repne on string instructions |
| `03_immediates.asm` | imm8 vs sign-extended imm8 (0x83 group) vs imm16/imm32, boundaries, negative values |
| `04_branches.asm` | jcc/jmp/call/loop/jecxz short and near, `$+N` both directions, far ptr forms |
| `05_bits16.asm` | the same core coverage in `bits 16` operating mode |
| `i386.asm`, `i486.asm`, `pentium.asm`, `p6.asm` | every general-purpose form from insns.dat that is valid in 32-bit mode |
| `x87.asm`, `mmx.asm`, `sse*.asm`, ..., `avx*.asm`, `avx512*.asm`, ... | one file per feature, every form, generated from insns.dat |

Instructions that are valid only in 64-bit mode (REX, APX, AMX, ...) are not in
`corpus/`: they are in `corpus64/` (APX and AMX are excluded by the plan).

## 64-bit mode (`corpus64/`)

`tests/ia32/corpus64/*.asm` is the same comparison for the 64-bit (long) mode:
`bits 64` sources, run with `dismount_cli dis|asm --bits 64`, and assembled /
disassembled by `nasm` and `ndisasm -b 64`. `run_compat.py` runs `corpus/` and
`corpus64/` by default (`--corpus 32`, `--corpus 64` select one), and reports the
64-bit files as `corpus64/NAME.asm` (the summary lists their features as
`feature [64]`).

```
python3 tests/ia32/run_compat.py --corpus 64                    # only corpus64/
python3 tests/ia32/run_compat.py --file corpus64/sse2.asm       # one file (the basename also works)
python3 tests/ia32/run_compat.py --corpus 64 --grep 'rel \$'    # the rip relative lines
python3 tests/ia32/tools/gen_corpus.py --only 64                # regenerate corpus64/
python3 tests/ia32/tools/fuzz_lengths.py --bits 64 --count 50000
```

`--strict` checks ASM, DIS and RT of `corpus64/` (`--strict-asm64` is an old name,
it has no effect any more).

Files: `00_addressing64.asm` (every base / index / scale / displacement with the
REX registers r8-r15, rsp / r12 and rbp / r13, no base, absolute, rip relative and
the 32-bit addresses of the 0x67 prefix, plus the byte registers spl..dil / ah..bh
and r8-r15 in all register-to-register forms), `02_prefixes64.asm` (0x66, REX.W,
the segment overrides, lock, rep, `a32`), `03_immediates64.asm` (imm64, the sign
extended imm32 and imm8, moffs64 / moffs32), `04_branches64.asm` (jcc, jmp, call,
loop, jrcxz / jecxz, indirect and far branches, ret), and one file per feature
generated from `insns.dat` in long mode: the `NOLONG` entries are dropped, the
`LONG` / `X86_64` ones are kept, with the registers r8-r15, xmm8-xmm15 and
spl..dil.

Syntax additions of the nasm subset in 64-bit mode (dismount must accept and
print them):

- 64-bit registers `rax..r15`, `r8d..r15d`, `r8w..r15w`, `spl bpl sil dil`,
  `r8b..r15b`, `xmm8..xmm15`, `cr8`.
- Rip relative memory is `[rel $+N]` / `[rel $-N]` (`N` counts from the start of
  the instruction, like the branch targets: the displacement is `N - length`).
  nasm has no `rip` register. With the 0x67 prefix it is `[a32 rel $+N]`.
- A 32-bit address without registers is `[a32 0x12345678]`; the 32-bit address
  registers (`[eax+r9d*2]`) select 0x67 by themselves.
- An absolute address is `[0x12345678]` (a SIB byte with no base and no index, the
  disp32 is sign extended; `[0xffffffff80000000]` or `[-0x80000000]`). The
  `moffs64` form of `mov al, [moffs]` is `[qword 0x1122334455667788]`.
- `mov rax, imm64` is `mov rax, 0x1122334455667788`. The sign extended imm32 of
  `mov rax, -1`, `add rax, -0x80000000`, `push -1` is written as a negative number.
- `retq`, `leaveq`, `pushfq` are the default 64-bit forms (the same bytes as
  `ret`, `leave`, `pushf`); `retw`, `pushfw`, `leavew` have the 0x66 prefix; REX.W
  forms that nasm spells with a suffix: `iretq`, `retfq`, `movsq`, `stosq`, `cdqe`,
  `cqo`, `cmpxchg16b`, `fxsave64`, `movq xmm0, rax`.
- `xchg rax, rax` is `48 90` and `nop` is `90`; `xchg r8d, eax` is `41 90`.

### Decisions where nasm / ndisasm and the SDM differ

- The operand size follows REX.W, then 0x66, then the default (32 bits). The
  instructions with a default of 64 bits (push / pop, pushf / popf, leave, enter,
  call / jmp r/m, ret, push / pop fs / gs) take 0x66 as 16 bits; REX.W is 64 bits
  and wins over 0x66, as everywhere (`MODE64_DEFAULT_64`). Intel ignores 0x66 for `ret`, `call` and `jmp` r/m; nasm and
  ndisasm honor it for `ret` (`retw`), so dismount does as well. `call/jmp ax`
  (`66 ff d0`) is not accepted by nasm: such a line is not in the corpus.
- The near relative branches (`call`, `jmp`, `jcc`, `loop`, `jrcxz`) always have a
  64-bit operand size: 0x66 and REX.W are ignored (`MODE64_FORCE_64`), as in ndisasm.
- `in`, `out`, `ins`, `outs` and the memory operands of the FPU ignore REX.W
  (`MODE64_NO_REXW`).
- Several REX prefixes: the first one is used, like ndisasm (the processors ignore
  all but the one before the opcode). A REX prefix followed by a legacy prefix is
  not an instruction (it decodes as an invalid byte, 1 byte): ndisasm prints it
  as `rex.w` / `rex` by itself.
- `40 90` is `nop`, `48 90` is `xchg rax, rax` (ndisasm shows `o64 nop`, nasm
  assembles `xchg rax, rax` to `48 90`), `66 90` is `xchg ax, ax`.
- `lgdt` / `lidt` / `sgdt` / `sidt` take a 10 byte memory operand: the text has no
  size keyword, the 0x66 form is `lgdt word [...]`.
- `mov cr0, rax` and `mov dr0, rax` always use the 64-bit register; the `mod` field is
  ignored by the processors but only `mod = 11` is decoded.
- 0x67 on a string instruction is written with `a32` (`a32 movsb`), on `loop` with
  the count register (`loop $+N, ecx`), on `jrcxz` as `jecxz`.

### Expected failures

Every line of the instruction sets which dismount supports in 32-bit mode (i386,
i486, pentium, p6, x87, mmx, sse, sse2, the hand matrices and the instructions
only valid in long mode that it implements) is DIS PASS: the decoded length is
nasm's and nasm assembles the printed text to the same bytes. These lines
are DIS INVALID because the mnemonic exists but the encoding is not in the opcode
table:

- `pextrw word [mem], xmm, imm` (SSE4.1, `66 0f 3a 15`): 8 lines, as in the 32-bit
  corpus.
- `rdmsr r64, imm32` (`msr_imm`, VEX encoded): 2 lines.

Everything else that is UNSUPPORTED is a mnemonic that is in no table (SSE3 and later,
AVX, ...): the 32-bit tables do not have it either.

What `corpus64/` cannot express, because nasm cannot assemble it: a redundant or
misplaced REX prefix (`40 89 c0`, `48 66 89 c0`, `48 48 ...`), 0x66 on a near branch
or `call/jmp r/m`, REX.W on an instruction that ignores it, `mov cr0, eax`. They are
covered by `tests/TestAMD64Disassembler.cpp` (directed decode cases of the text,
the length and the structured operands) and by `fuzz_lengths.py --bits 64`, which
compares the instruction lengths with `ndisasm -b 64` for random bytes (REX
prefixes alone, `d5` REX2 and `rex.*` lines of ndisasm are skipped).

### The assembler in 64-bit mode

`dismount_cli asm --bits 64`, `AssemblingFactory::generateAssembler(binary,
DISASSEMBLER_AMD_64)` and `IA32AssemblerStream` with `IA32eInstructionSet::AMD_64`
assemble the nasm subset above in 64-bit mode. Every line of `corpus64/` which
dismount decodes (DIS=PASS) is also ASM=PASS (the same bytes as nasm 3.02, no
ALT is needed) and RT=PASS: the text dismount prints for nasm's bytes is assembled
to nasm's bytes again. The lines that are ASM=ERROR are the DIS=INVALID ones
(`pextrw word [mem], xmm, imm` and `rdmsr r64, imm32`: the encodings are not in the
opcode table).

How the encoder follows the table (`Mode64` of `opcodeTable.h`):

- The table rows are `MODE64_INVALID` in 64-bit mode and `MODE64_ONLY` in the other
  modes: the rows are skipped. The AMD64 tables are searched too; an AMD64 row replaces
  the IA-32 row which has the same opcode, mandatory prefix and a common modrm
  (`movsxd`/`arpl`, `mov r64, imm64`, `mov cr0, rax`, `cdqe`, ...), the copies of the
  IA-32 rows (`MODE64_LEGACY`) do not.
- The operand size is REX.W (64), 0x66 (16) or the default (32). `MODE64_DEFAULT_64`
  rows (push, pop, call/jmp r/m, ret, leave, ...) are 64 bits without REX.W and 16 bits
  with 0x66 (a 32-bit operand is rejected); `MODE64_FORCE_64` rows (the relative
  branches) have no operand size; `MODE64_NO_REXW` rows (in, out, ins, outs, syscall,
  the FPU memory operands) reject a 64-bit operand.
- The prefix order is the one of nasm: segment, 0x67, lock, 0x66, rep, the
  mandatory prefix (66, F2, F3) and the REX prefix right before the opcode.
- REX.R / X / B come from the registers 8-15 in the reg field, the SIB index and the
  r/m, SIB base or opcode register. spl..dil force a REX prefix and ah..bh in the same
  instruction (or with r8-r15) is rejected.
- Memory: rsp / r12 as base need a SIB, rbp / r13 as base without displacement
  need disp8 0, `[rax*2]` is `[rax+rax]`, `[rax+rsp]` is `[rsp+rax]` (esp / rsp can't
  be an index; r12 can), `[rax*4]` has no base and a disp32. `[rip + N]` is mod 00,
  rm 101 with `N` the displacement from the end of the instruction, nasm's
  `[rel $+N]` (and `[rel N]`) is `N - length`; 32-bit address registers write
  0x67 and `a32`. 16-bit address registers are rejected.
- The address `[0x1234]` is a SIB byte with no base and no index (7 bytes with the
  disp32, which is sign extended); the `A0-A3` offset forms are used only for
  `[qword 0x1122334455667788]` (and the `[a32 0x12345678]` of the 32-bit offset), as
  in nasm. A displacement or an address which does not fit in 32 bits is truncated
  (nasm warns about it).
- `rel8` or `rel32` for the branches (`rel32` is the only near form), `short` and
  `near` pick one. `jrcxz` has the 64-bit count register, `jecxz` is 0x67, `jcxz` is
  rejected; `loop target, ecx` / `a32 loop target` select 0x67.

Decisions where nasm's choice is one of several valid encodings (the assembler
writes what nasm writes):

- `mov r64, imm`: a value of 0..0xffffffff is `mov r32, imm32` (`mov rax, 1` is `b8
  01 00 00 00`, the register is zero extended), -0x80000000..-1 is the sign extended
  `REX.W C7 /0 imm32` (7 bytes), anything else is `REX.W B8+r imm64`.
- The immediate of the other 64-bit instructions is the sign extended imm32, or the
  sign extended imm8 when it fits (`add rax, 0x80` is `48 05 80 00 00 00`); a bigger
  value is truncated to the low dword (`test rax, 0x1122334455667788`), as nasm does
  (with a warning). Where an instruction has an imm8 and an imm32 form and neither
  fits the signed range the imm32 is tried before the low byte of the imm8.
- `xchg rax, rax` is `48 90`, `xchg ax, ax` is `66 90`, `xchg eax, eax` is `87 C0`
  (`90` is the nop), `xchg r8, rax` and `xchg rax, r8` are both `49 90`.
- `movsx rax, eax` and `movsxd rax, eax` are `cdqe` (`48 98`), the other `movsx r64,
  r/m32` are `movsxd` (`48 63 /r`).
- `movq xmm1, [mem]` and `movq xmm1, xmm2` are `F3 0F 7E` (not `66 REX.W 0F 6E`).
- `cvttss2si rax, xmm0`, `movmskps rax, xmm0`, `movq` and `cvtsi2sd xmm, r64` have
  REX.W; `pextrw rax, xmm0, 1` has none (nasm treats rax as eax).
- `jmp far [mem]` and `jmp far qword [mem]` are `REX.W FF /5` (m16:64), `jmp far dword
  [mem]` is `FF /5` and `jmp far word [mem]` has 0x66.
- `mov eax, ds` and `mov rax, ds` are `8C D8`, `mov ax, ds` is `66 8C D8`, `mov ds, ax`,
  `mov ds, eax` and `mov ds, rax` are `8E D8`.
- `lgdt [mem]` / `lgdt word [mem]` (the 0x66 form) and the other descriptor table
  instructions take no REX.W.
- `push qword fs` is `0F A0` (the size keyword of a segment register sets the operand
  size: `push word fs` is `66 0F A0`, nasm writes `0F A0` for it).

Decisions of the assembler (nasm accepts more): a repeated prefix is rejected
(`lock lock add`, `rep rep movsb`, see `asm_errors.txt`; nasm writes it once),
the size suffixes `w d q` are accepted only for `ret retf iret leave enter pushf popf
pusha popa` (`retw`, `iretq`), not for the other mnemonics (`pslldq` is not `pslld`
with a `q`), and the register names of 64-bit mode (and rip relative addressing)
are rejected in 16 and 32-bit mode. The `strict` keyword of nasm is not supported.

Checks of the 64-bit assembler:

```
python3 tests/ia32/run_compat.py --corpus 64                      # ASM and RT columns
python3 tests/ia32/run_compat.py --selftest                       # asm_errors*.txt are rejected
python3 tests/ia32/tools/fuzz_roundtrip.py --bits 64 --count 60000 --seed 1
out/bin/test_dismount                                             # tests/TestAMD64Assembler.cpp
```

`fuzz_roundtrip.py` decodes random bytes with the dismount decoder, assembles the
text which the decoder printed and decodes the new bytes: the text must be the same.
Random bytes can decode to texts which are not instructions (the decoder is still
more lenient than the processors for the opcodes it has no entry for); nasm
judges such lines: a text which nasm rejects is not counted, and a text which was
assembled to other bytes than nasm's is a bug. `tests/TestAMD64Assembler.cpp` is the
unit test: a table of text, bytes (from nasm) and the disassembler's text, the text
which must be rejected in 64-bit mode and the 64-bit instructions which must be
rejected in 32-bit mode.

## Validity of the encodings (`fuzz_validity.py`)

The decoder must not accept byte sequences which are not instructions (`8D D9` is not
`lea ebx, ecx`: lea needs a memory operand, `FF EA` is not `jmp far edx`, `8F F0` is not
`pop eax`) and must not decode an instruction as another one. `tools/fuzz_validity.py`
decodes, in 16, 32 and 64 bit mode, every one-byte opcode, every `0F xx`, `0F 38 xx`
and `0F 3A xx`, with every ModR/M byte (all 256) and the prefix sets none, 66, F2, F3
(and in 64 bit mode REX, REX.B, REX.R, REX.W and their combinations with 66 / F2 / F3):
about 5 million 16 byte blocks, in chunks, with `dismount_cli dis` and ndisasm (one
ndisasm run per chunk, `-s` sync points between the blocks). The report groups what
differs by prefix, opcode, mod (memory / register) and reg field:

| class | meaning |
|---|---|
| (a) | dismount decodes, ndisasm says `db`: an invalid encoding is accepted |
| (b) | ndisasm decodes an instruction whose mnemonic is in the opcode table, dismount says invalid |
| (c) length / mnemonic | both decode, with another length or another mnemonic |
| (n) | nasm rejects the text which dismount printed for a valid instruction (the second oracle: where ndisasm is lenient nasm is not, e.g. `fadd word ptr [..]`, `movd mm0, ax`); `--no-nasm-check` skips it |

A lone prefix that ndisasm prints by itself (an unused 66 / F2 / F3, `osp`, `rex.w`) is
joined to the instruction after it. The Cyrix mnemonics of ndisasm are ignored.

```
python3 tests/ia32/tools/fuzz_validity.py                       # 16, 32, 64 bit: exit 1 on a finding
python3 tests/ia32/tools/fuzz_validity.py --bits 64 --only '^(66)?0f0[12]' --show 100
```

ndisasm is the reference, except where the Intel SDM says otherwise. Those cases are in
`tests/ia32/validity_exceptions.txt` (a regular expression per line, the mode, the class
and the SDM reason; the same list is used by `fuzz_lengths.py`, which reports a
sequence that dismount decodes and ndisasm calls `db` as a bug): `mov cr / dr` ignore
the mod field (decoded as the register form), `setcc` ignores the reg field, `82 /r`
in 16 and 32 bit mode, the reserved control registers and segment registers and the
reserved NOPs of `0F 18..1E` (invalid in dismount), `bswap` with the 66 prefix
(invalid), APX `D5` and the instructions of a newer extension which a prefix selects
(`F3 0F BC` tzcnt shows as `rep bsf`, ...).

Rules for the table (`opcodeTable.cpp`): an entry whose SDM form needs a memory operand
(lea, lds / les / lss / lfs / lgs, bound, movnti, sgdt / sidt / lgdt / lidt, invlpg, the
far call and jmp (FF /3, /5), the movnt*, movlps / movhps stores, fxsave, ...) has
`MODRM_MEMORY_ONLY` (or `MODRM_MOD_MEMORY | digit`), an entry that needs a register
(movmskps, pmovmskb, maskmov*, movhlps, the shift groups) `MODRM_REGISTERS_ONLY`, and a
group opcode names the /digit it has (8F, C6, C7, FE, FF, 0F 01, 0F 18, 0F BA, 0F C7,
0F AE); the 0F 01 encodings with mod 11 which are not in the table (vmcall, xgetbv, ...)
are invalid. mov to / from CRn and DRn read the mod field as 11
(`filterModRM` in `IA32StreamDisassembler.cpp`), `MODE64_NO_16BIT` marks the opcodes
without a 16 bit form (bswap) and `MODE64_OPERAND_32_64` the general purpose operand
which is 32 bits (64 with REX.W) whatever the 66 prefix or the 16 bit mode say (movd,
cvtsi2ss / cvtsi2sd, movnti). The memory operand of the FPU is always 32 / 64 / 80
bits of its own: the 66 prefix does not change it.

## Checks (per line)

`run_compat.py` assembles each corpus file with nasm and takes each line's reference
bytes from the nasm listing (`-l`). Then it runs these checks on every line:

| check | how | statuses |
|---|---|---|
| **ASM** | `dismount_cli asm` assembles the line on its own | `PASS`, `ALT` (different bytes, but ndisasm decodes both to the same instruction, i.e. a valid alternative encoding), `MISMATCH`, `ERROR` (exception / unsupported) |
| **DIS** | `dismount_cli dis` decodes nasm's bytes | `PASS` (decoded length == nasm length, and the normalized text re-assembled by nasm gives identical bytes), `LENGTH`, `REASM_DIFF`, `INVALID` (decoded as an invalid opcode), `ERROR` |
| **RT** | round trip: `dismount_cli asm` assembles the text which dismount printed for nasm's bytes (the REASM text), and must write nasm's bytes again. Only for the lines with DIS=PASS | `PASS`, `MISMATCH`, `ERROR` (listed in `rt_failures.txt`) |
| **TEXT** | normalized source vs. normalized dismount disassembly | `SAME` / `DIFF` (informational only) |

A line counts as **fully compatible** when ASM=PASS and DIS=PASS. A line is
**UNSUPPORTED** when the mnemonic is not in any dismount table at all (or the line is
of a feature in `VEX_ONLY_FEATURES`: `msr_imm`, `rdmsr rbx, imm32` is a VEX
encoded form, not the plain `rdmsr`). That keeps "not implemented yet" separate from
"implemented but wrong".

Text normalization is done only in the driver (`norm_text` in `run_compat.py`),
not in the library. It is mechanical and nothing else:

- `;` comments are dropped, the text is lowercased, and runs of whitespace collapse
  (a space after each comma).
- The size keyword `ptr` is dropped: `dword ptr [` → `dword [`.
- A segment override in front of a bracket moves inside: `fs:[x]` → `[fs:x]`.
- A hex number `XXh` (two or more hex digits, the formatter's form: `FFh`, `0Ah`,
  `00000010h`) → `0xXX`. A single digit before the `h` is the register `ah`, `bh`,
  `ch` or `dh` and is left alone.
- Memory operands lose their blanks (`[esi + 7Fh]` → `[esi+0x7f]`). The TEXT check also
  sorts the register terms; the reassembly keeps the printed order.
- An absolute branch target (no `$`) → `$+N` / `$-N`.
- `short` / `near` are kept as printed. dismount prints `near` only when the near
  encoding is used although the target fits in rel8 (`jo near $+0x10` is 6 bytes
  and must stay `near`), so no size keyword is added by the driver. The TEXT check
  ignores both keywords.

The reassembly check (DIS) gives nasm exactly this normalized text. Nothing else is
added (the same rule is in `reasm_text`). `run_compat.py --selftest` asserts these rules.

## Features and the host CPU

`dismount_cli cpuinfo` (or `cpuprobe` built as x86_64 and run under Rosetta on arm64
macOS, or `/proc/cpuinfo` on Linux) prints the feature set the **current process**
can execute. The report marks every line with `host: yes/no`.

- `--host-only` restricts the run to features the host supports.
- `--features sse2,avx` and `--file`, `--grep REGEX` pick a subset. Use
  `--grep '^paddq xmm1, xmm2$'` to run a single test.
- Encoding checks do not need the CPU. The host feature set is reported so that
  execution-based checks can be added later.
- The driver exits 0 when lines fail. With `--strict` (used by CI) it exits 1 if
  any line that dismount implements is not ASM=PASS and DIS=PASS (and RT=PASS in
  `corpus64/`).

## Report

`out/compat/report.json` (every line, every check), `out/compat/summary.md` (matrix
of feature × status, plus the top failure clusters by mnemonic/operand form), and
`out/compat/failures.txt` (one failure per line: file:line, source, nasm hex,
dismount hex/text, reason) and `out/compat/rt_failures.txt` (the round trip failures).

## Future-expansion opcode inventory

Every 32-bit-valid opcode that dismount does not implement is written as a
**commented-out** entry, grouped by feature, with its SDM encoding, to
`tests/ia32/opcodeTableFuture.inc` (not compiled; it is reference data for future
table work, kept next to its generator). Nothing is written into
`Source/dismount/proc/ia32/opcodeTable.cpp`. The file has a separate section for:

- each table of `opcodeTable.cpp` (legacy one-byte, two-byte (0F) and x87
  opcodes). Each entry is followed by the ready-to-uncomment table row when its
  operands map to the existing operand types, or by the reason it can't be added
  yet;
- the three-byte maps (0F38, 0F3A) and the VEX/EVEX-encoded instructions (AVX,
  AVX2, FMA, AVX-512, ...), which the current table format cannot express.

## Library work expected to fall out of the tests (assembler side)

The nasm syntax subset (optional `ptr`, `;` comments, `[seg:...]`, `0x`), SIB and
scaled index, reg+reg memory, the 16-bit addressing table, segment-override and
lock/rep prefixes, nasm `$` semantics, the 16-bit operating mode, and missing table
entries. Order of work is driven by the failure clusters in `summary.md`.

**Behavior change:** in the assembler, `$+N` now means "N bytes from the start
of this instruction" (nasm). Before, it was a raw displacement from the end of the
instruction.
