# IA-32 semantics test plan (dismount IR vs. the processor)

Goal: prove that the semantic IR of every lifted IA-32 instruction does exactly
what the processor does. Each instruction is executed twice from the same
random state:

1. **dismount**: decode the bytes, lift them with `IA32SemanticLifter`, run the
   IR with `SemanticInterpreter`.
2. **oracle**: run the same bytes on the Unicorn engine (QEMU's x86 CPU), in
   16-bit real mode or 32-bit protected mode.

The final registers, flags, written memory and the exit (fall through, branch,
trap) must be equal. The flags that the instruction leaves undefined
(`OpcodeClassification::getFlagsUndefined()`) are not compared.

Two C++ unit tests check the IR on its own (`tests/TestSemantics.cpp`, in
`test_dismount`):
- `SemanticInterpreter::evaluate()` and `evaluateFlags()` on hand-computed
  vectors, for every operation and recipe the interpreter implements.
- `run()` on hand-written blocks: labels and branches, sub-register aliases
  (al/ah/ax/eax), sub-register writes, traps, memory faults, helpers.

## Layout

```
tests/semantics/
  TEST_PLAN.md          this file
  requirements.txt      the Python packages of the oracle (unicorn)
  run_semantics.py      the driver: nasm -> dismount_cli run / Unicorn -> report
  oracle.py             the Unicorn side
  skip.txt              regexes of the corpus lines that aren't compared, with
                        the reason (segment loads, privileged instructions...)
  oracle_bugs.txt       regexes of the lines where Unicorn is wrong, with the
                        evidence
  corpus/*.asm          extra hand-written lines (flag edge cases)
tests/TestSemantics.cpp  the C++ unit tests
```

The default corpus is the IA-32 compatibility corpus (`tests/ia32/corpus/`
`00_addressing32`, `01_addressing16`, `02_prefixes`, `03_immediates`,
`04_branches`, `05_bits16`, `i386`, `i486`, `pentium`, `p6`), plus
`tests/semantics/corpus/*.asm`. The same format is used, see
`tests/ia32/TEST_PLAN.md`.

Setup:
```
python3 -m venv out/venv && out/venv/bin/pip install -r tests/semantics/requirements.txt
cd build && cmake .. -DDISMOUNT_BUILD_TESTS=ON && make -j8
out/venv/bin/python tests/semantics/run_semantics.py          # -> out/semantics/
out/venv/bin/python tests/semantics/run_semantics.py --grep '^adc ' --states 64 -v
```

## `dismount_cli run`

```
dismount_cli run [--bits 16|32]
```

Each input line is `<hex bytes of one instruction> ; <state>`. The state has
these tokens, as hex numbers without `0x`:
`eax= ecx= edx= ebx= esp= ebp= esi= edi= eip= eflags=`.

The segment registers and their bases are 0. The memory is unlimited, and
every byte that hasn't been written reads as the **memory pattern**:

```
byte(A) = ((A * 2654435761) mod 2^32) >> 24      (A is the 32 bit linear address)
```

The instruction bytes are at linear address `eip` (they replace the pattern
there).

The output is one line for each input line:

```
OK exit=<exit> undef=<eflags bits> eax=XXXXXXXX ecx=... edi=XXXXXXXX eip=XXXXXXXX eflags=XXXXXXXX mem=<writes>
ERR <message>
```

- `exit`:
  - `fall` (no branch);
  - `branch` (any taken branch, call or return);
  - `trap:N` (an interrupt or exception with vector N, such as `trap:0` for a divide error or `trap:3` for int3);
  - `fault:ADDR` (a memory fault, which can't happen with unlimited memory);
  - `undefined` (the instruction isn't lifted);
  - `helper:N` (a helper, which isn't executed).
- `undef`: the EFLAGS bits of the flags the instruction leaves undefined.
- `eflags` is built from the flag registers:
  - CF bit 0, PF 2, AF 4, ZF 6, SF 7, DF 10, OF 11, IF 9, AC 18, ID 21;
  - bit 1 is always set.
- `mem`: the bytes written by the instruction, with their final values. They are sorted by address and merged into runs: `mem=00001000:aabbcc,00002000:11`, or `mem=-` when nothing was written.

The registers are always printed as 32 bits, in 16-bit mode too.

## The oracle

Unicorn runs in `UC_MODE_16` (real mode) or `UC_MODE_32`, from the same state:

- **Memory:** pages are mapped on demand and filled with the memory pattern. The code is written at `eip`, and the writes are recorded through a memory hook.
- **Repeated instructions:** a `rep` instruction is stepped until `eip` leaves it, because Unicorn may stop after one iteration.
- **Traps:** interrupts and exceptions stop the run through the interrupt hook, and the result is reported as `trap:N`.
- **Comparing a trap:** when the exit is a trap, `eip` is not compared, because the two engines report it at different points.

## States

For each line, `--states N` random states (16 by default) are drawn, seeded
from the line text so that each run is reproducible.

- **Register values** are drawn from the edge values (0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff) and from random values.
- **ecx** is small (0 to 17) in half of the states, and in every state of a `rep`/`loop` line.
- **eflags** is 0x202 plus random CF, PF, AF, ZF, SF, OF and DF.
- **eip** is 0x1000.

## Report

`out/semantics/report.json`, `summary.md` and `failures.txt`. Each line gets one status:

| status | meaning |
|---|---|
| PASS | every state matched |
| FAIL | a state differs (the first differences are in failures.txt, with the state, so they can be replayed with `--grep` and `-v`) |
| UNLIFTED | dismount exits `undefined` / `helper` |
| SKIP | matched by skip.txt |
| ORACLE_BUG | failed, and matched by oracle_bugs.txt: a documented Unicorn/QEMU bug where dismount follows the SDM (a line that starts to pass becomes PASS) |
| ASM_ERR / DIS_ERR / ORACLE_ERR | nasm, dismount_cli or Unicorn failed on the line |

The driver exits 0 even when lines fail. With `--strict` it exits 1 on any FAIL
or DIS_ERR. `--baseline OLD.json` prints the lines that passed in OLD and don't
pass now, and exits 1 if there are any.

## Phases

1. Integer core:
   - data movement, arithmetic, logic, shifts and rotates, multiply and divide, bits;
   - flags, setcc/cmovcc/jcc, the stack, call/ret/jmp, loop/jcxz;
   - string instructions and rep, xchg/xadd/cmpxchg/cmpxchg8b with and without lock;
   - cbw/cwd, bswap, enter/leave, pushf/popf/lahf/sahf, int/int3/into.
2. x87 and MMX.
3. SSE / SSE2 and later.
