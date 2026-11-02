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
  tools/gen_corpus.py     regenerates corpus/ from insns.dat + addressing matrix
  tools/gen_future_opcodes.py  regenerates the commented-out opcode inventory
  dismount_cli.cpp        `asm`, `dis` and `cpuinfo` sub-commands over libdismount
  cpuprobe.c              stand-alone CPUID probe (used under Rosetta on arm64 macOS)
  opcodeTableFuture.inc   generated, comment-only inventory of 0F38/0F3A/VEX/EVEX opcodes
  run_compat.py           the driver: nasm <-> dismount comparison + report
  tools/fuzz_lengths.py   random byte sequences: dismount vs ndisasm lengths and errors
```

Build: `cmake -DDISMOUNT_BUILD_TESTS=ON ..` adds `dismount_cli` and `test_dismount`
(the existing xStl-harness unit test) to the CMake build.

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

Instructions that are valid only in 64-bit mode (REX, APX, AMX, ...) are excluded,
because this is a 32-bit test plan.

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
**commented-out** entry, grouped by feature, with its SDM encoding:

- legacy one-byte, two-byte (0F) and x87 opcodes go inline in
  `Source/dismount/proc/ia32/opcodeTable.cpp`, in their table position;
- three-byte maps (0F38, 0F3A) and VEX/EVEX-encoded instructions (AVX, AVX2, FMA,
  AVX-512, ...), which the current table format cannot express, go in
  `tests/ia32/opcodeTableFuture.inc` (not compiled; it is reference data for
  future table work, kept next to its generator).

## Library work expected to fall out of the tests (assembler side)

The nasm syntax subset (optional `ptr`, `;` comments, `[seg:...]`, `0x`), SIB and
scaled index, reg+reg memory, the 16-bit addressing table, segment-override and
lock/rep prefixes, nasm `$` semantics, the 16-bit operating mode, and missing table
entries. Order of work is driven by the failure clusters in `summary.md`.

**Behavior change:** in the assembler, `$+N` now means "N bytes from the start
of this instruction" (nasm). Before, it was a raw displacement from the end of the
instruction.
