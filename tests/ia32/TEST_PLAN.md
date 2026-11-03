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
```

Build: `cmake -DDISMOUNT_BUILD_TESTS=ON ..` adds `dismount_cli` and `test_dismount`
(the existing xStl-harness unit test, with `tests/TestAMD64Disassembler.cpp`: the
directed decode cases of 64-bit mode) to the CMake build.

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

`--strict` checks the DIS column of `corpus64/` only, because the assembler does
not implement 64-bit mode in every build: `--strict-asm64` checks ASM too.

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

## Checks (per line)

`run_compat.py` assembles each corpus file with nasm and takes each line's reference
bytes from the nasm listing (`-l`). Then it runs these checks on every line:

| check | how | statuses |
|---|---|---|
| **ASM** | `dismount_cli asm` assembles the line on its own | `PASS`, `ALT` (different bytes, but ndisasm decodes both to the same instruction, i.e. a valid alternative encoding), `MISMATCH`, `ERROR` (exception / unsupported) |
| **DIS** | `dismount_cli dis` decodes nasm's bytes | `PASS` (decoded length == nasm length, and the normalized text re-assembled by nasm gives identical bytes), `LENGTH`, `REASM_DIFF`, `INVALID` (decoded as an invalid opcode), `ERROR` |
| **TEXT** | normalized source vs. normalized dismount disassembly | `SAME` / `DIFF` (informational only) |

A line counts as **fully compatible** when ASM=PASS and DIS=PASS. A line is
**UNSUPPORTED** when the mnemonic is not in any dismount table at all. That keeps
"not implemented yet" separate from "implemented but wrong".

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
  any line that dismount implements is not ASM=PASS and DIS=PASS.

## Report

`out/compat/report.json` (every line, every check), `out/compat/summary.md` (matrix
of feature × status, plus the top failure clusters by mnemonic/operand form), and
`out/compat/failures.txt` (one failure per line: file:line, source, nasm hex,
dismount hex/text, reason).

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
