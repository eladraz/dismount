# ARM semantics test plan

Checks the semantics of the ARM instructions (A32, T32, A64 with Advanced SIMD, SVE and SME)
as dismount lifts them (`dismount_cli run --arch a32|t32|a64`, running the IR with
`SemanticInterpreter`) against reference processors: the Unicorn emulator and the Apple M4 the
tests run on. The same design as `tests/semantics/TEST_PLAN.md` (IA-32): every corpus line runs
from random and directed states in dismount and in an oracle; the final registers, flags, vector
state and the written memory must be equal. The overview of all the ARM tests is
`tests/arm/TEST_PLAN.md`.

This directory holds the oracles, the state generator, the driver and the corpus. The lifter is
`ArmSemantics.cpp` (the context, the AArch32 rules: the condition, pc, interworking) and
`Arm64SemanticLift*.cpp` (the A64 base instructions); `dismount_cli run --arch` implements the
protocol below. The A32 / T32 tables and the A64 SIMD, SVE and SME ones are not there yet: their
lines are `UNLIFTED`.

## Files

| File | What |
|---|---|
| `run_semantics.py` | The driver (see "Running") |
| `armstate.py` | The state / result line protocol (parse, format, compare) |
| `armasm.py` | Assembles the corpus lines with clang (reads the ELF itself: no objcopy) |
| `states_arm.py` | Random and directed states |
| `oracle_arm64.py` | The Unicorn A64 oracle (Advanced SIMD, SVE) |
| `oracle_arm32.py` | The Unicorn A32 / T32 oracle |
| `host_arm64.c`, `oracle_host.py` | The native oracle of the host (built on demand to `out/host_arm64`) |
| `corpus/*.s` | `a64_base`, `a64_base_all` (generated, see below), `a64_neon`, `a64_sve` (streaming compatible), `a64_sme`, `a64_ext`, `a32_base`, `t32_base` |
| `skip.txt`, `oracle_bugs.txt` | Lines that are not compared / oracle mistakes, with the evidence |

Setup: `python3 -m venv out/venv && out/venv/bin/pip install unicorn==2.1.4` (the venv of
`tests/semantics` works). clang is needed for the corpus (Apple clang 21 knows SVE2, SME2 and
the AArch32 extensions) and for the host runner.

## Running

```
out/venv/bin/python tests/arm/semantics/run_semantics.py --arch a64 [--file a64_base] [--grep REGEX] [--states 16]
        [--oracle unicorn|host] [--oracle-only] [--compare-oracles] [--vl 128] [--set x1=5,nzcv=4]
        [--jobs N] [-v] [--strict] [--baseline OLD.json] [--cli PATH] [--out DIR]
```

* default: dismount_cli against the oracle (`--oracle unicorn`, or `host` for A64 on the M4).
  Writes `OUT/report.json`, `summary.md`, `failures.txt` (`out/semantics_arm`). Exit status 0
  unless the harness breaks; `--strict` exits 1 on any FAIL or DIS_ERR, `--baseline` exits 1 when
  a PASS line regressed.
* `--oracle-only`: print what the oracle does (the registers it changed) for every state; `-v`
  also prints the input state.
* `--compare-oracles` (A64): the same states on Unicorn and on the host, and the disagreements
  (the way to find oracle bugs). The lines of mode `sve` run on Unicorn with VL 512 and on the
  host in the streaming mode (SVL 512). A disagreement that matches `oracle_bugs.txt` is
  `ORACLE_BUG`, any other is `FAIL`.
* `--arch a32|t32` use the files `a32_*.s`, `t32_*.s`; only Unicorn runs them (the host has no
  AArch32).

Statuses: PASS, FAIL (a difference), UNLIFTED (dismount ran `undefined` / a helper), SKIP
(`skip.txt`), ORACLE_BUG (a FAIL that `oracle_bugs.txt` explains), ASM_ERR (clang rejects the line),
DIS_ERR (dismount_cli failed), ORACLE_ERR (the oracle can't run the line, e.g. SME on Unicorn).

### Corpus lines

```
<instruction>      // @feature: sve   // @mode: sve   // @state: x1=ffffffff nzcv=2
```
Blank lines and lines that start with `//` are comments. `;` joins instructions that run as one
unit (a T32 IT block: `ite ne ; movne r0, #1 ; moveq r0, #2`). `@mode:` is `plain` (default), `sve`
(z, p, ffr are part of the state; Unicorn runs SVE at `--vl`, the host in the streaming mode),
`sm` (streaming mode, host only) or `za` (streaming mode with ZA and zt0, host only). Every
`@state:` adds a directed state: the tokens of the state line (below) applied over a random one.

The registers x18 and x28 are reserved by the host runner (the platform register and the pointer
of its state block): corpus lines must not use them, and the driver does not compare them against
the host. The pc relative instructions are fine: the code runs at the pc of the state.

## The `run` protocol

`dismount_cli run --arch a32|t32|a64` reads one state line per run from stdin and prints one
result line, in order, flushing after each (the IA-32 protocol, with the ARM registers). The
oracles read and print the same lines (`armstate.py` is the reference implementation, the
`host_arm64` binary reads the A64 lines directly).

### State line

```
<hex bytes of the code> ; token=value token=value ...
```
All values are hex digits, no `0x`, no sign; a vector value is the hex of the whole number (the
lane 0 is the lowest digits). Tokens that are missing have the default. The code is the bytes in
memory order (what clang emits): A64 and A32 a multiple of 4 bytes, T32 of 2. It is a straight
sequence run as one unit: one instruction, or for T32 an `it` plus its block; the decoder tracks
ITSTATE inside the sequence (the state starts with ITSTATE = 0).

A64 tokens:

| Token | Meaning | Default |
|---|---|---|
| `x0`..`x30` | the registers (x28 reserved on the host) | 0 |
| `sp` | the stack pointer | `0x10100008000` (the middle of the memory window) |
| `pc` | the address of the code | `0x10000001000` |
| `nzcv` | 4 bits N=8 Z=4 C=2 V=1 | 0 |
| `fpcr`, `fpsr` | the floating point control and status | 0 |
| `v0`..`v31` | the 128 bit registers (the low part of z) | 0 |
| `sve=1` | z0-z31, p0-p15, ffr are part of the state and the result | 0 |
| `sm=1` | streaming mode (SVCR.SM): the vector length is `svl`, no ffr (implies `sve=1`) | 0 |
| `za=1` | SVCR.ZA: `za0`..`za<svl/8-1>` (rows of `svl` bits) and `zt0` (512 bits) are part of the state | 0 |
| `vl`, `svl` | the vector lengths in bits (hex: `80` = 128, `200` = 512) | 128, 512 |
| `z0`..`z31` | the vectors (vl, or svl in the streaming mode, bits). Applied first: `v<n>` then overwrites the low 128 bits. Without a `z<n>` the vector is `v<n>` zero extended | |
| `p0`..`p15`, `ffr` | the predicates (vl/8 bits, svl/8 in the streaming mode) | 0 |
| `mem` | `addr:hexbytes,addr:hexbytes` (initial memory), see below | `-` |

A32 / T32 tokens: `r0`..`r12`, `sp`, `lr`, `pc` (default `0x10000`, the address of the
instruction; T32 without bit 0), `apsr` (only N Z C V Q bits 31-27 and GE bits 19-16), `fpscr`,
`q0`..`q15` (the d and s registers are views of them), `mem`. The arch (`a32` / `t32`) gives
the instruction set; the T bit of the result is the T bit after the sequence.

The `exit` kinds and the result tokens are in the next section. A64 states with `sve=1`,
`sm=1` or `za=1` are only run on the oracle that has the feature; `vl` is the vector length
of the processor the description is built with: `Arm64Architecture(vl, svl)`.

### Memory

* The window `[MEM_BASE, MEM_BASE + 0x10000)`: A64 `MEM_BASE = 0x10100000000`, A32/T32 `0x20000000`.
  The initial byte at address A is `((A mod 2^32) * 2654435761 mod 2^32) >> 24` (the pattern of
  tests/semantics) unless a `mem` token sets it. An access outside the window, and outside the
  code region, is a fault (`trap:fault`).
* The code region: A64 `[P - 0x4000, P + 0x8000)` with `P = pc & ~0x3fff` (the 16 KB pages of the
  host runner); it reads as `brk #0x1234` words (`0xd4224680`), the code bytes at `pc`, and
  `brk #0x1235` (`0xd42246a0`) right after the code. A pc relative load therefore reads the same
  value on every engine. A32/T32: `[P - 0x1000, P + 0x2000)`, `P = pc & ~0xfff`, zero except the
  code. Writes into the code region are allowed and not reported.
* The written bytes are reported in the result as `mem=addr:hex,...` (sorted, merged into runs;
  `-` for none): the final value of every byte the run wrote. The comparison reads a byte that
  one side didn't write from the initial image, so writing the old value is the same as not
  writing.

### Result line

```
OK exit=<exit> [unknown=<names>] [sve=1] [sm=1] [za=1] [vl=<hex> svl=<hex>] <registers> mem=<runs>
ERR <message>
```
`exit` is `fall` (the pc is `pc + len`), `branch` (the pc is the target: the code left the
sequence, a taken branch; the pc has bit 0 clear and `t` says the new instruction set),
`trap:svc`, `trap:brk` (brk, bkpt), `trap:undef` (the architecture says UNDEFINED / the
instruction doesn't exist for the processor), `trap:fault` (a memory access outside the window or
a data abort). After a trap only the exit is compared (the engines stop at different points).
dismount additionally prints `exit=undefined` when the lifter ended in `OP_UNDEFINED` (an
instruction it doesn't lift: status UNLIFTED) and `exit=helper:<n>` for an `OP_HELPER` the CLI
doesn't implement.

The registers are all printed, fixed width, in this order:

* A64: `x0`..`x30` (16 digits), `sp`, `pc`, `nzcv` (1 digit), `fpcr`, `fpsr` (8), `v0`..`v31` (32);
  with `sve` / `sm`: `z0`..`z31` (vl/4 digits), `p0`..`p15` (vl/32 digits), `ffr` (not in the
  streaming mode); with `za`: `za0`.., `zt0`. After a trap or a branch the z / p / za registers
  are not reported (an oracle can't read them).
* A32 / T32: `r0`..`r12`, `sp`, `lr`, `pc`, `apsr`, `t` (1 digit), `fpscr`, `q0`..`q15`.

`unknown=` (optional, comma separated register names, e.g. `unknown=x3,nzcv`) lists registers
whose value the architecture leaves UNKNOWN / UNPREDICTABLE for this instruction in this state;
the driver doesn't compare them.

The comparison masks `fpcr` with `0x07ff9f07` (the defined control bits), `fpsr` with
`0xf800009f`, and `apsr` with `0xf80f0000`. The A32 `fpscr` is compared whole.

### What dismount_cli has to do (for the lifter)

1. Parse the line (`armstate.py:parse_state` is the model). Build the description:
   `ArchitectureDescriptionFactory::getArchitecture(DISASSEMBLER_ARM_64_LE)`, or
   `new Arm64Architecture(vl, svl)` for the A64 states (the tokens `vl`/`svl`), `..._ARM_32_LE` /
   `..._THUMB_LE` for a32 / t32. Registers not named are 0 (sp, pc: the defaults above).
2. Disassemble and lift the code at `pc`, one instruction after the other; the lifter gets the
   T bit / ITSTATE from the sequence. Execute with the `SemanticInterpreter`, memory as above.
   Reading `pc` in an instruction gives the architectural value (A32 +8, T32 +4, A64 the
   instruction address), the pc register of the description holds the address of the current
   instruction. Stop when the pc leaves `[pc, pc + len)` (`branch`) or reaches `pc + len`
   (`fall`).
3. Print the result line. The streaming mode (`sm=1`): z and p are SVL wide, `ffr` is not
   printed; `smstart` / `smstop` and `svcr` change the mode inside the sequence only as a
   result of the instruction.
4. A memory access that fails (`SemanticMemory` returns false) is `trap:fault`; `svc` is
   `trap:svc`, `brk` / `bkpt` `trap:brk`, `udf` and the encodings that are UNDEFINED
   `trap:undef`.

## The oracles

### Unicorn 2.1.4 (QEMU 5.0.1)

`oracle_arm64.py`:

* `UC_CPU_ARM64_MAX` (`uc.ctl_set_cpu_model`, before anything else): Advanced SIMD, FP16 and FHM,
  dot product, RDM, FCMA, JSCVT, SHA1/256/512/3, SM3/SM4, AES, PMULL, CRC32, LSE atomics, RCPC,
  FlagM, PAuth(1), **SVE** (`ID_AA64PFR0.SVE = 1`). Not: SVE2 (`ID_AA64ZFR0 = 0`), SME, BF16,
  I8MM, FEAT_LSE2, FRINTTS. The default model (A57) has no SVE.
* The engine starts at EL1h. FP is on, but **SVE traps** (an UNDEF) until
  `CPACR_EL1.ZEN = 3` **and `CPTR_EL3.EZ = 1`** (the max cpu has EL3; EZ = 0 sends the SVE
  instruction to EL3). Those two registers are not in the register list of Unicorn: they are
  written with `uc.reg_write(UC_ARM64_REG_CP_REG, (crn, crm, op0, op1, op2, value))`.
* **Vector length**: `ZCR_EL1`, `ZCR_EL2` and `ZCR_EL3` `.LEN` (CP_REG (1, 2, 3, op1, 0), op1 = 0, 4,
  6) all set to `vl/128 - 1`. Unconfigured the engine runs 128 bits (`rdvl x0, #1` = 16); the max
  cpu accepts every multiple of 128 up to 2048 (the harness uses 128..512, `--vl`). Verified
  with `rdvl`.
* **z / p / ffr**: Unicorn 2.1.4 has no register ids for them. When the state has `sve=1` the
  harness runs a prologue before the instruction (`ldr z0..z31, [x0, #n, mul vl]`,
  `ldr p0..p15, [x1, #n, mul vl]`, ffr through `ldr p0` and `wrffr p0.b`, from a transfer buffer
  that the harness writes) and after it an epilogue that stores them (`str z`, `str p`, `rdffr`,
  `str p0`); x0 and x1 (the base registers) are read before the epilogue and the x / v / flags
  of the instruction are read before it as well, so the sandwich does not change the
  result. The instruction under test is run alone between the two (`emu_start` until the end of
  the code, so the epilogue is not part of it). After a branch or a trap z / p are not read.
  The encodings of the sandwich are verified against clang (16 instructions).
* No SME: `smstart` and every ZA instruction is an UNDEF; a state with `sm=1` / `za=1` is an
  ORACLE_ERR.
* The v registers are written through `UC_ARM64_REG_Q0 + n` (consecutive ids). `X0 + 29` is NOT x29
  (the ids of x29 and x30 are `UC_ARM64_REG_X29` = 1 and `X30` = 2: `X0 + 29` is v0!).
* Exceptions: `UC_HOOK_INTR` with the QEMU index (1 UDEF, 2 SWI, 7 BKPT, 3 / 4 aborts) → trap kind.
  After the exception the banked registers are those of the exception level: sp / lr of a trap
  are not meaningful (traps are not compared). An unmapped access is a `UcError` (fault); an
  unmapped fetch is a taken branch (the pc of the engine is the target).
* The code hook stops the run at the first instruction that is outside the code: a taken branch.
* A fresh engine for every run (the cost is small: 0.2 ms).

`oracle_arm32.py`:

* `UC_CPU_ARM_MAX` is an ARMv8-A AArch32 processor: VFPv4 + Advanced SIMD (32 d registers), CRC32, the
  crypto extensions, `vrint*`, `vmaxnm`/`vminnm`, `vsel`, `vcvta..`, SDIV/UDIV, dot product (`vsdot`
  runs). No FP16 arithmetic (`vadd.f16` is UNDEF) and no BF16. `UC_CPU_ARM_CORTEX_A15` is ARMv7
  (VFPv4, NEON, no ARMv8 instructions).
* The engine starts in SVC mode: the harness sets `CPSR = flags | 0x10 (user) [| 0x20 Thumb]`.
* The FPU is off until `CPACR` (`UC_ARM_REG_C1_C0_2`) = `0xF << 20` (cp10, cp11) and
  `FPEXC.EN` (`UC_ARM_REG_FPEXC` = `1 << 30`).
* `UC_MODE_THUMB` and the start address `pc | 1`; an IT block is part of the code. The T bit of the
  result is `CPSR.T`: `bx r5` with an odd r5 gives `t = 1`.
* Registers: `UC_ARM_REG_R0 + n`, `Q0 + n` (128 bit values), `FPSCR`, `CPSR`.

Limits of both: no MMU (the memory is flat, unaligned accesses are allowed except the exclusive
and atomic ones), no MTE, no pointer authentication keys, `mrs` of the ID registers returns the
model's values, the timers and `rndr` are not deterministic or not present, system registers are
at EL1, the memory ordering is not modeled.

### The host: Apple M4 (`host_arm64.c`)

```
clang -O1 -o out/host_arm64 tests/arm/semantics/host_arm64.c      # done on demand by oracle_host.py
```
Reads the same state lines on stdin and prints the result lines. Per run:

* A code region of three 16 KB pages is mapped with `MAP_JIT` at the page of the state's pc
  (the hint address; `MAP_FIXED` + `MAP_JIT` fails), so the pc relative instructions and the pc in
  the result are the same as in the other engines. macOS arm64 user addresses in
  `[0x1000000000, 0x7fffffffff]` are not mappable: the harness addresses are above 1 TB.
* The first page holds the prologue, the instruction is at `pc`, followed by the epilogue (only in
  the streaming mode) and `brk #0x1235`; every other word is `brk #0x1234`. A taken branch into the
  region ends at a `brk` whose address is the target.
* The prologue loads the state from a block addressed by **x28**: `smstart sm` / `smstart za` when
  the state has them, z / p (`ldr z`, `ldr p`) or v (`ldp q`), the rows of ZA (`ldr za[w12, 0]`
  in a loop) and zt0, then fpcr, fpsr, nzcv, sp and x0-x27, x29, x30 (the registers it uses as
  temporaries are loaded last). **x28 and x18** are reserved (the pointer of the block; the
  platform register): they are not state, not compared, and a corpus line must not use them.
  The stack pointer is the `sp` of the state (inside the memory window: a private stack).
* The run ends in a signal, handled on an alternate stack by a handler that copies x0-x30, sp, pc,
  nzcv, fpcr, fpsr and v0-v31 from the `ucontext` and returns to the main loop with
  `siglongjmp`: SIGTRAP at the end marker = `fall`; SIGTRAP at another `brk` of the region = `branch`
  (the pc is the brk); a SIGSEGV / SIGBUS whose pc is outside the instruction = a fetch of a branch
  target = `branch`; inside it = `trap:fault`; SIGILL = `trap:undef`; `brk` of the instruction =
  `trap:brk`. A 2 s timer ends a loop (ERR timeout). The handler first executes `smstop`: a
  signal arrives in the streaming mode of the interrupted code, where the Advanced SIMD
  instructions (memcpy in libc) are illegal.
* `svc` is not run (the host would call the kernel): ERR.
* `brk #0xbeef` kills the process on macOS (SIGKILL, no signal): the end marker is `brk #0x1235`.
* Streaming mode (`sm=1`): after `smstart sm`, z0-z31 and p0-p15 are loaded, the instruction runs,
  and the epilogue stores z, p (and ZA, zt0) and fpcr / fpsr (`mrs`); v0-v31 are the low 128 bits
  of z. A SIGILL in the streaming mode is an instruction that is not legal in it (NEON, ffr, fadda,
  compact, ... : the same lines are run non-streaming on Unicorn).
* The `sve` lines are run in the streaming mode on the host (`adapt_for_host` in the driver) at
  SVL 512: the same state (z, p) without ffr; only instructions legal in the streaming mode
  (SME without FA64) belong to the corpus `a64_sve.s`.
* The wrapper restarts the runner when a line kills it.

The M4 (`sysctl hw.optional.arm`): `AdvSIMD`, `FEAT_FP16`, `FEAT_FHM`, `FEAT_DotProd`, `FEAT_RDM`,
`FEAT_LSE`, `FEAT_LSE2`, `FEAT_RCPC`/`RCPC2`, `FEAT_FCMA`, `FEAT_JSCVT`, `FEAT_FRINTTS`, `FEAT_FlagM`/`FlagM2`,
`FEAT_BF16` (`EBF16` 0), `FEAT_I8MM`, `FEAT_AES`/`PMULL`/`SHA1`/`SHA256`/`SHA512`/`SHA3`, `FEAT_CRC32`,
`FEAT_AFP`, `FEAT_DPB`/`DPB2`, `FEAT_PAuth`/`PAuth2`/`FPAC`, `FEAT_BTI`, `FEAT_DIT`, `FEAT_SB`, `FEAT_ECV`,
`FEAT_WFxT`, `FEAT_RPRES`, **`FEAT_SME`, `FEAT_SME2`**, `SME_F32F32`, `SME_BI32I32`, `SME_B16F32`,
`SME_F16F32`, `SME_I8I32`, `SME_I16I32`, `FEAT_SME_F64F64`, `FEAT_SME_I16I64`, `sme_max_svl_b` = 64
(SVL 512 bits). Not present: **no SVE (outside the streaming mode)**, SVE2, SM3/SM4, MTE,
`FEAT_CSSC`, `FEAT_HBC`, `FEAT_SME2p1`, `FEAT_SME_F16F16`, `FEAT_SME_B16B16`, `FEAT_SPECRES`, `FEAT_SSBS`,
`FEAT_EBF16`. No AArch32. pointer authentication keys are per process (the corpus skips pac/aut).

## The oracles against each other: findings

`--compare-oracles --arch a64` over the seed corpora (3000-4000 states per file):

| File | states compared | agree | differ | explanation |
|---|---|---|---|---|
| `a64_base` | 3783 | 3780 | 3 | unaligned exclusive / atomic: Unicorn faults (no LSE2), host executes |
| `a64_neon` | 35100 (300 states per line) | 35100 | 0 | |
| `a64_sve` | 2496 | 2464 | 32 | unpredicated SVE2 `mul`: Unicorn has no SVE2 |
| `a64_ext` | 2720 | 2449 | 271 | Unicorn: LDSMAX / LDSMIN swapped (bug); no BF16 / I8MM; unaligned LDAPUR does not fault. Host: no SM3 / SM4 |

The documented entries with the evidence are in `oracle_bugs.txt`. The real Unicorn bug found:
**LDSMAX and LDSMIN (all sizes, acquire / release forms, and STSMAX / STSMIN) are swapped**: the
atomic stores the signed minimum for `ldsmax` and the maximum for `ldsmin` (the unsigned forms
are right). Example: `[x4] = 5`, `w1 = -1`, `ldsmax w1, w0, [x4]` must leave 5 in memory (the
host does), Unicorn stores `0xffffffff`; x0 = 5 on both.

Not bugs but differences between the processors (they affect which states a test may use):
unaligned atomics / exclusives (Unicorn: alignment fault; M4 with LSE2: no fault; the corpus
aligns the atomics), SVE2 / SME / BF16 / I8MM (Unicorn lacks them), SM3 / SM4 (the M4 lacks
them), streaming-mode restrictions (fadda, compact, ffr, NEON are illegal in the streaming mode).

## The A64 base lifter

`corpus/a64_base_all.s` is generated by `tests/arm/tools/gen_semantics_corpus.py` from the A64
encoding corpus (`tests/arm/corpus/a64/*.s`): every mnemonic of `gArm64SemanticTable` with the
register and operand variety of the encoding corpus, without x18 / x28, without the CONSTRAINED
UNPREDICTABLE register overlaps, and the hand written groups (the exclusives joined with their
stores, the system registers, the 16 conditions with the 16 flag values, the arithmetic edge
cases). The table itself is generated by `tests/arm/tools/gen_a64_semantic_table.py` from
`Arm64EncodingTable.cpp`.

```
out/venv/bin/python tests/arm/semantics/run_semantics.py --arch a64 --file a64_base_all --states 12 --strict
out/venv/bin/python tests/arm/semantics/run_semantics.py --arch a64 --file a64_base_all --states 12 --strict --oracle host
```
Both give no FAIL. The lines neither oracle can run (FEAT_CSSC, FEAT_LSUI, FEAT_HBC, FEAT_LS64,
the FEAT_LRCPC3 forms, FEAT_LSE128, FEAT_PCDPHINT) are `ORACLE_BUG` by the entries of
`oracle_bugs.txt` ("neither oracle has"), and `tests/TestArm64Semantics.cpp` checks them: its rows
(`tests/arm/Arm64SemanticRows.inc`, generated by `tests/arm/tools/gen_semantic_rows.py`) have the
expected values computed by an independent model of the Arm ARM pseudocode. The M4 is the arbiter
where Unicorn and dismount disagree (the xpac strip, the FPSR / FPCR bits, the faults of the cache
maintenance, see `oracle_bugs.txt`).

Choices of the lifter:
* The flag registers N, Z, C, V hold the ARM flags: a subtraction sets them as the addition of the
  inverted operand (AddWithCarry), so C is the inverted borrow and `ArmLiftContext::testCondition()`
  (not the IA-32 unsigned conditions) evaluates an ARM condition.
* Pointer authentication: the keys are not modeled. `xpaci`, `xpacd` and `xpaclri` strip the code
  exactly (47 bit user addresses, checked against the M4); the hint space forms (`paciasp`,
  `autiasp`...) are an `OP_HELPER` (`ARM_HELPER_INSTRUCTION`, reported as UNLIFTED by the CLI);
  `pac*`, `aut*`, the branches with authentication and `ldraa` / `ldrab` are not lifted.
* MTE (the allocation tags are not part of the IR memory) and MOPS (the P / M / E split is
  implementation defined, no oracle runs it) are not lifted.
* The exclusive monitor is the pair of registers `ARM64_REG_EXCLUSIVE_ADDRESS` / `_SIZE`: a store
  exclusive succeeds if the monitor is open at the address of the store, and always closes it.
* The cache maintenance by address (`dc`, `ic`) faults on an unmapped address like the M4 does;
  `dc zva` zeroes a 64 byte block (`DCZID_EL0` reads 4).
* An undecodable encoding is `exit=undefined` in `dismount_cli run`, not `trap:undef`: the corpus
  only holds valid instructions, so it is a gap of a table.

## Gates

* `out/bin/test_dismount` passes (`tests/TestArmArchitecture.cpp` tests the descriptions).
* The oracles run the seed corpora without errors: `run_semantics.py --arch a64 --oracle-only`,
  `--oracle host --oracle-only`, `--arch a32 --oracle-only`, `--arch t32 --oracle-only`.
* `run_semantics.py --arch a64 --compare-oracles` runs; the disagreements are all in
  `oracle_bugs.txt`.
* `out/venv/bin/python tests/semantics/run_semantics.py --bits 32 --file flags.asm --strict` (IA-32)
  still passes.
