# RISC-V semantics test plan

Checks the semantics of the RISC-V instructions (RV32 and RV64: I M A F D C, Zba Zbb Zbs Zbc Zbk*, Zfh Zfa
Zfbfmin, Zicond, Zimop, Zawrs, Zacas, Zabha, Zicbo*, V with Zvfh Zvbb Zvbc Zvk*, Zicfilp and Zicfiss) as
dismount lifts them (`dismount_cli run --arch rv64|rv32 [--vlen N]`, running the IR with `SemanticInterpreter`)
against reference processors. The same design as `tests/arm/semantics/TEST_PLAN.md` and
`tests/semantics/TEST_PLAN.md`: every corpus line runs from random and directed states in dismount and in an
oracle; the final registers, the vector state, the floating point status and the written memory must be equal.

The oracles, from the primary one:

| Oracle | What | Covers |
|---|---|---|
| `qemu` | QEMU 11.1.3 (`qemu-system-riscv64|32 -machine virt -cpu max`), a bare metal guest that runs batches | everything QEMU implements: the whole RVA23 profile and more (the primary oracle) |
| `spike` | Spike (riscv-isa-sim, built from source), the same guest | the whole RVA23 profile: the independent golden model |
| `unicorn` | Unicorn 2.1.4 (QEMU 5.0.1, the cpu model "any") | RV32GC / RV64GC only (no V, no B, no Zfa, Zfh, Zicond, Zcb, Zimop, Zicbo, CFI...) |

QEMU and Unicorn share most of their code (a QEMU 5 fork vs QEMU 11); Spike is an independent implementation, so
a disagreement between the three is resolved with the specification (`oracle_bugs.txt`).

## Files

| File | What |
|---|---|
| `run_semantics.py` | The driver (see "Running") |
| `rvstate.py` | The state / result line protocol (parse, format, compare) |
| `rvasm.py` | Assembles the corpus lines with llvm-mc (reads the ELF itself) |
| `states_rv.py` | Random and directed states |
| `gen_corpus.py` | Generates the scalar corpus `corpus/{rv,rv64,rv32}_gen_*.s` from the decoder corpus (`tests/riscv/corpus`) |
| `gen_directed.py` | Generates the directed corpus `corpus/{rv,rv64,rv32}_dir_*.s` (special values, memory edges, atomics, CSRs, CFI) |
| `gen_vector.py` | Generates the vector corpus `corpus/{rv,rv64}_vgen_*.s` (configuration, integer, fixed point, mask, permutation, memory, and the floating point `fadd ffma fwide funary fcmp fmove fcvt fred fdir`, bit manipulation `bits` and cryptography `crypto`) |
| `gen_unit_rows_vector.py` | Generates the rows of `tests/TestRiscvSemanticsVector.cpp` (`tests/riscv/RiscvSemanticVectorRows{64,32}.inc`) from QEMU and Spike at VLEN 128, 256 and 512 |
| `gen_unit_rows.py` | Generates the rows of `tests/TestRiscvSemantics.cpp` (`tests/riscv/RiscvSemanticRows{64,32}.inc`) from QEMU and Spike |
| `build_guest.py`, `guest/runner.c`, `guest/rt.S`, `guest/link.ld` | The bare metal guest of QEMU and Spike (rv32 and rv64, built to `out/riscv_guest/`) |
| `oracle_qemu.py`, `oracle_spike.py`, `oracle_unicorn.py` | The oracles |
| `corpus/*.s` | `rv_*` (RV32 and RV64), `rv64_*`, `rv32_*` (one XLEN): `i m a a_pair f d d_gpr fd c b zfa zfh misc cfi v v2 oracle_diffs`, and the vector corpus `vgen_config int fixed mask perm mem fadd ffma fwide funary fcmp fmove fcvt fred fdir bits crypto` |
| `skip.txt`, `oracle_bugs.txt` | Lines that are not compared / oracle mistakes, with the evidence |

## Setup

* QEMU 11 (`brew install qemu`), Homebrew llvm 23 and lld (`brew install llvm lld`: clang, llvm-mc, ld.lld) and the
  venv with Unicorn (`python3 -m venv out/venv && out/venv/bin/pip install unicorn==2.1.4`).
* Spike (optional, `--oracle spike`): `git clone https://github.com/riscv-software-src/riscv-isa-sim
  out/tools/riscv-isa-sim && git -C out/tools/riscv-isa-sim submodule update --init --depth 1`, then in
  `out/tools/riscv-isa-sim/build`: `../configure --prefix=$PWD/../../spike-install && make -j10` (about 8 minutes on
  an M4). The binary is `out/tools/riscv-isa-sim/build/spike` (or `$SPIKE`). No dtc/boost is needed on macOS.
* A Debug build with the test targets for `dismount_cli` (`cmake .. -DDISMOUNT_BUILD_TESTS=ON`).

## Running

```
out/venv/bin/python tests/riscv/semantics/run_semantics.py [--arch rv64|rv32] [--oracle qemu|unicorn|spike]
        [--vlen 128|256|512] [--file NAME[,NAME]] [--grep REGEX] [--states 16] [--jobs N] [-v] [--strict]
        [--baseline OLD.json] [--oracle-only] [--cross OR1,OR2] [--set x1=5,fcsr=60] [--boxed]
        [--qemu-cpu rva23u64|rva23s64|max] [--no-trap-state] [--cli PATH] [--out DIR]
```

* default: dismount_cli against the oracle (`--oracle qemu`). Writes `OUT/report.json`, `summary.md`, `failures.txt`
  (`out/semantics_riscv`). The exit status is 0 unless the harness breaks; `--strict` exits 1 on any FAIL or DIS_ERR,
  `--baseline` exits 1 when a PASS line regressed. Result classes: `PASS`, `FAIL`, `UNLIFTED` (dismount says
  `exit=undefined` or `helper:N`), `SKIP` (skip.txt, or `@oracle:` excludes the oracle), `ORACLE_BUG` (a FAIL
  line which matches `oracle_bugs.txt`), `ASM_ERR` (llvm-mc), `DIS_ERR` (dismount_cli: `ERR` or no `run`
  command), `ORACLE_ERR` (the oracle couldn't run any state).
* `--oracle-only`: what the oracle does (the registers it changed) for every state; `-v` prints the input too.
* `--cross A,B`: the same states on two oracles and the disagreements: the way to find oracle bugs, and what
  `oracle_bugs.txt` cites. No dismount_cli. `-v` prints every differing state.
* The summary lists the lines which are an illegal instruction in every state of an oracle (a feature the oracle
  lacks, or a wrong corpus line).
* `--states` per line (default 16); the states are a function of the instruction text, so a run is reproducible.
  `--vlen` is VLEN of QEMU (`-cpu max,vlen=N`) / Spike (`zvlNb`) and of `dismount_cli run --vlen`.

The corpus line syntax (`rvasm.py`): `<instruction>  # @feature: zba  # @state: x11=win+100 f1=7ff8000000000000
# @mode: cfi # @oracle: qemu spike`; `;` separates the instructions of one unit (an `lr`/`sc` pair); the
instructions are not compressed by the assembler (`.option norvc`), a `c.` line is assembled as itself.
`@state:` tokens replace the drawn values (`win+N` is the window base plus N; `frm=`, `fflags=` set parts of fcsr),
`@mode: vector` forces vector state, `@mode: cfi` runs with Zicfilp / Zicfiss enabled.

## The run protocol

`dismount_cli run --arch rv64|rv32 [--vlen N]` reads one line per state and prints one line per result (the
ARM protocol). Values are hex without `0x`.

```
<hex bytes of the code, one or more instructions> ; token=value ...
```

| Token | Meaning |
|---|---|
| `x1`..`x31` | XLEN bits (`x0` is never given) |
| `pc` | the address of the code; the default is 80001000 (the middle page of the 3 pages of the code region) |
| `f0`..`f31` | 64 bits raw, the NaN boxing is in the bits |
| `fcsr` | frm = bits 5-7, fflags = bits 0-4 |
| `vlen` | VLEN in bits (hex: `80` = 128) |
| `v0`..`v31` | VLEN/4 hex digits |
| `vl`, `vtype`, `vstart`, `vxrm`, `vxsat` | vtype is the raw CSR (vill = bit XLEN-1). Any of these, or v*, or `vlen` makes the state a vector state |
| `ssp`, `elp` | Zicfiss / Zicfilp (see below) |
| `win=BASE:SIZE` | the data window (the tests pass `win=80100000:4000`) |
| `mem=ADDR:HEX,ADDR:HEX` | initial bytes; the other bytes of the window are `((uint32)(addr * 2654435761) >> 24)` |

The code region is 3 pages around the page of the pc, zero bytes but the instructions. An access outside of the
window and the code region faults. The result is `OK exit=<exit> x1=.. .. x31=.. pc=.. f0=.. .. f31=.. fcsr=..
ssp=.. elp=N` and, for vector states, `vlen=.. vl=.. vtype=.. vstart=.. vxrm=.. vxsat=.. v0=.. .. v31=..`, then
`mem=ADDR:HEX,..` (the bytes written, `-` for none). `exit` is `fall`, `branch` (pc = the target), `trap:ecall`,
`trap:ebreak`, `trap:illegal`, `trap:fault` (every access fault, page fault and address misaligned exception),
`trap:swcheck` (Zicfilp / Zicfiss software check), or `undefined` / `helper:N` (not lifted). After a trap the
registers are the ones at the trap and `pc` is the address of the instruction: the exceptions are precise, so the
comparison checks the registers too (`--no-trap-state` compares the exit only).

The comparison (`rvstate.compare_results`): exit class (`fall` and `branch` are both flow), x1..x31, pc (not after a
trap), f0..f31, fcsr (the low 8 bits), for vector states vl, vtype, vstart, vxrm, vxsat and v0..v31, for CFI states ssp
and elp, and the written bytes. A store which writes the value already there is not a difference.

## The QEMU and Spike guest

`guest/runner.c` + `guest/rt.S` (the same source for rv32 and rv64; `-DSPIKE` selects the Spike I/O): a bare metal
M-mode program that runs a batch of tests, one record per test. The host packs the records into a flat image
(`oracle_qemu.build_input`): a header (magic, count, XLEN, VLENB, window, flags, the output path), then per test the
instruction bytes (up to 40), pc, vtype, vl, vstart, fcsr, vcsr, ssp, elp, x0-x31, f0-f31, v0-v31 and the memory
runs. QEMU loads it with `-device loader,file=in.bin,addr=0x90000000,force-raw=on`, Spike with `--kernel` (flat image at
0x80200000 on RV64, 0x80400000 on RV32). The guest, per test:

1. restores the window from a pristine copy and applies the memory runs of the record; writes the instruction bytes
   at the pc of the code region followed by an `ecall` (the marker), and executes `fence.i`;
2. loads f0-f31 and v0-v31 (whole register loads), fcsr, vcsr, then `vsetvl` for vtype / vl and `csrw vstart`;
3. arms a machine timer (100 ms) and `mret`s to U-mode at the pc with x1-x31 loaded;
4. the M-mode trap handler stores x1-x31, f0-f31, v0-v31 (`vs8r.v`), mcause, mepc, mtval, vstart (then clears it), vl,
   vtype, fcsr, vcsr, ssp and mstatus (MPELP is the ELP at the trap) into the result record, and returns to the loop;
5. diffs the window against the pristine copy (bytes that differ from the initial window are reported), restores it.

The U-mode runs under paging (Sv39 / Sv32): only the 3 code pages and the window (and, with CFI, a shadow stack page)
are mapped, so everything else faults and a branch outside of the code region is an instruction page fault whose
mepc is the target. How the trap is classified (`oracle_qemu.decode`): mcause 8 at pc + length is `fall` (the
marker; the pc is mepc); mcause 8 inside the instruction is `trap:ecall`; 3 `trap:ebreak`; 2 inside the instruction
`trap:illegal`; 2 / 1 / 12 outside of it (the zero bytes of the code region, an unmapped target) is `branch` to
mepc; 4, 5, 6, 7, 13, 15 `trap:fault`; 18 `trap:swcheck`; the timer interrupt is an error (an endless loop). A
trap kills no batch: QEMU assertions (a QEMU bug in `vector_helper.c` was found this way) are isolated by
splitting the batch down to the test that kills it (`run_isolating`, that state is `ORACLE_ERR`).

* CPU: `-cpu max,vlen=N,rvv_vsetvl_x0_vill=true` (RV32: the `max` cpu of qemu-system-riscv32), `rvv_ta_all_1s` and
  `rvv_ma_all_1s` are false, so the tail and the masked-off elements stay undisturbed (QEMU's default).
  `--qemu-cpu rva23u64` restricts QEMU to the profile. Spike: `rv64imafdcv_<extensions>` with `zvlNb` for VLEN. Spike
  never writes ones into the tail / masked-off elements either. Spike raises an illegal instruction for a nonzero
  vstart in the vector arithmetic instructions (permitted by the specification): those states are dropped
  (`ORACLE_ERR` for the state only).
* The counters (`mcounteren`, `scounteren` = all ones) and `menvcfg`/`senvcfg` CBIE/CBCFE/CBZE (the `cbo.*`) are on.
* Zicfilp / Zicfiss (`@mode: cfi`, or `state['cfi']`): the guest sets `menvcfg.LPE|SSE` and `senvcfg.LPE|SSE`, maps a
  shadow stack page at 80200000 (a page with R = 0, W = 1: only `sspush`, `ssamoswap` and the like can access it), loads
  `ssp` (CSR 0x011) and starts with ELP = `elp` (`mstatus.MPELP`: bit 41 of mstatus, bit 9 of mstatush on RV32; the
  `mret` copies it to ELP). The state generator puts ssp into the page, x7 (the label of lpad) equal to the label
  half of the time, and the top of the shadow stack equal to ra / t0 (sspopchk). The shadow stack page is part of the
  memory the result reports (`mem=` addresses 80200000..80200fff). The mode is per batch (QEMU's CFI enables), so the
  CFI states run in their own batches. Without the mode the instructions are Zimop (`sspush`, `ssrdp`, `lpad` (an
  `auipc x0`)...) and the lifter's enabled behavior doesn't apply: don't run CFI lines without the mode.
  A jalr / jal sets ELP when it is an indirect jump (not `jalr x0, 0(x1|x5)` and not through x7); the instruction
  after it must be an lpad else the software check exception: this is checked on the fetch of the next instruction, so
  one-instruction tests see it as `elp=1` after a `branch` (the fetch of the unmapped target faults first) and as
  `trap:swcheck` for any non-lpad instruction started with `elp=1`.
* Timeouts: an endless loop (`jal x0, 0`) is a timer interrupt after 100 ms and an `ORACLE_ERR` state; skip.txt
  lists the lines which are such a loop for every state.

## Unicorn

`oracle_unicorn.py`: the cpu model "any" in M-mode with `mstatus.FS` dirty, the 3 code pages (RWX) and the window
(RW, not executable, so a jump into it is a fetch fault = `branch`). Exceptions come through `UC_HOOK_INTR` (2
illegal, 3 ebreak, 8/9/11 ecall), unmapped accesses and unaligned accesses through `UcError`. A state with vector
state is an error (`ORACLE_ERR`; the V lines are in skip.txt). Unicorn doesn't NaN box: the cross-check against it
draws properly boxed f registers (`--boxed`, implied) and compares the low 32 bits of the results of the
single precision instructions only.

## The corpus

* Starter corpus (`corpus/rv_*.s`, `rv64_*.s`, `rv32_*.s`, about 1300 lines per XLEN, by hand): the base integer
  instructions with boundary immediates, M, A (all amo orderings, lr/sc pairs), F, D, Zfh, Zfa, Zfbfmin, C, B, the
  scalar cryptography, the hints and cache block ops, Zimop/Zcmop, Zabha/Zacas, the CFI instructions, and about 400
  vector instructions. `oracle_diffs` is the evidence for oracle_bugs.txt.
* The generated scalar corpus (`gen_corpus.py`: `corpus/*_gen_*.s`, about 42 000 lines): every instruction of the
  base, compressed and scalar extension tables, as the decoder corpus has it (`tests/riscv/corpus`): every register of
  each operand (x0 as destination and as source, the same register everywhere), the edge values of the immediates,
  every rounding mode (`rne rtz rdn rup rmm dyn`; the reserved static modes can't be encoded: the decoder rejects
  them like LLVM), every aq / rl combination, every fence set, the CSRs of the description. The Zicfilp / Zicfiss
  lines run twice: as Zimop instructions and with the control flow integrity enabled (`@mode: cfi`).
* The directed scalar corpus (`gen_directed.py`: `corpus/*_dir_*.s`, about 76 000 lines): the special values of
  every operand position of the floating point instructions (zeros, denormals, infinities, quiet and signaling
  NaNs, the integer conversion boundaries, ties) in the three formats, badly NaN boxed inputs, every `frm` with
  `dyn`, `fcvtmod.w.d`, `fli`; the integer instructions over boundary values (division by zero and overflow, shifts
  by the width, `clz` of zero, `xperm` out of range indexes...); loads, stores and AMOs at every alignment next to
  a page and the end of the window; `lr` / `sc` sequences (other address, other size, in between stores, x0);
  Zacas pairs; the cache block operations at the edges; the CSR instructions with read back; the CFI states.

* The vector corpus (`gen_vector.py`: `corpus/rv_vgen_*.s` and `rv64_vgen_*.s` for the `ei64` forms, about 13 700 lines):
  every integer vector instruction the lifter lifts (526 mnemonics and the aliases `vneg.v`, `vnot.v`, `vwcvt*.x.x.v`,
  `vncvt.x.x.w`, `vmmv.m`, `vmclr.m`, `vmset.m`, `vmnot.m`, `vl<n>r.v`) in every operand form (`.vv .vx .vi .wv .wx
  .vvm .vxm .vim .vs .mm .m`), with and without `v0.t`, over register patterns: the aligned groups of LMUL 8, the
  overlaps the architecture allows and the ones it reserves (the destination with a source, the top of a wide
  destination, v0 as a source, as an unmasked destination and as a masked one), odd register numbers, the unaligned
  groups (a forced LMUL: `@state: lmul=m2`); the immediates at their edges; the slide offsets (0, 1, VL, VLMAX, beyond,
  -1), the gather indexes, the four `vxrm` modes; every `vsetvli` / `vsetivli` configuration (28 legal, the reserved
  ones as raw immediates), every `rd` / `rs1` combination and `vsetvl` over drawn AVL and vtype values; loads and
  stores of the four element widths in every kind (unit stride, fault only first, strided, ordered and unordered
  indexed, segments 2 to 8, whole register, mask) at the edges of the window.
  The encodings llvm-mc refuses to assemble (a reserved overlap) carry the word in a `@word:` tag, built by the script
  from the encoding table, and are run as such: the illegal instruction paths are tested too.
  The floating point families (`vgen_f*`: all 105 OPFVV / OPFVF mnemonics, the BFloat16 ones of Zvfbfmin / Zvfbfwma and
  the aliases `vfneg.v`, `vfabs.v`) use the same register patterns with the `.vv .vf .wv .wf .vfm .v .w .vs` forms. The
  states draw the vector registers by the role of each operand (`states_rv.float_roles()`: the wide operand of a
  widening instruction has elements of 2 * SEW, the integer source of `vfcvt.f.x.v` is drawn as integers, the BFloat16
  sources as BFloat16), SEW 16, 32 and 64 (8 too for the conversions, whose integer side has 8 bit elements), the scalar
  of the `.vf` forms is a float of SEW bits boxed 80% of the time. `vgen_fdir` is the directed part: every rounding
  instruction at each of the five frm values and at the reserved 5 and 7, SEW 8 (reserved), and the scalars that are not
  NaN boxed (the half, single and BFloat16 patterns with a bad box). `vgen_bits` (Zvbb, Zvkb, Zvbc) and `vgen_crypto`
  (Zvkg, Zvkned, Zvknha / Zvknhb, Zvksed, Zvksh: the `.vv`, `.vs` and `.vi` forms, every round number of the key
  schedules, LMUL from 1/8 to 8) draw SEW 32 (64 for SHA-512) most of the time and a `vl` and `vstart` that are multiples
  of EGS 85% of the time (`states_rv.crypto_group_size()`), plus directed `vl` / `vstart` that are not, and SEW 8, 16, 64.

The states: random integers (boundary values and random widths), floating point (zeros, infinities, quiet and
signaling NaNs, denormals, the integer conversion boundaries, random normals) boxed 80% of the time (else the upper
bits are zero, partly set or random), `fcsr` with the rounding modes 0-4 (and, rarely, the reserved 5-7) and the
flags, vtype over every SEW and LMUL (the states of a line cycle through the combinations the named registers allow,
from an offset drawn per line: 16 states cover each SEW and 4 LMULs; 6% any raw value, which the processor makes
vill, as a vtype value with a reserved bit or the vill bit: those states carry the vill vtype and vl = 0 as the
processor ends up with) and policy, vl at 0, 1, VLMAX, VLMAX - 1, odd, random; vstart nonzero in 20% (below VLEN,
the CSR is log2(VLEN) bits wide), a `@state: sew=32 lmul=m2 vlsel=max` tag forces the configuration;
vxrm / vxsat, masks in v0 (all ones, all zeros, 0x55.., random), elements typed by SEW (floating point values for
the vf*, small offsets for the index vectors); base registers point into the window (8% misaligned, 4% outside to
fault, 17% of the vector memory ones at the edges of the window, atomics always aligned), strides small; random bytes and floating point constants around the pointers.

## Oracle disagreements found (details and evidence in `oracle_bugs.txt`)

| # | Who | What | Verdict |
|---|---|---|---|
| 1 | Unicorn | fmin/fmax with a signaling NaN and a number return the canonical NaN | Unicorn wrong (F 2.2: the number) |
| 2 | Unicorn | single precision results are not NaN boxed, inputs are not checked | Unicorn wrong |
| 3 | Unicorn | misaligned AMO/LR/SC always trap (no Zama16b) | Unicorn lacks RVA23 behavior |
| 4 | Unicorn | a failing sc to an unmapped address doesn't fault | Unicorn wrong (QEMU 11 and Spike fault) |
| 5 | Unicorn | no B, Zfa, Zfh, Zicond, Zcb, Zimop, Zicbo*, V, CFI | missing features |
| 6 | QEMU | an AMO that crosses a 16 byte granule at a misaligned address completes | QEMU wrong (Spike: access fault) |
| 7 | QEMU | `vfwmaccbf16.vf` doesn't NaN box check the scalar | QEMU wrong (Spike right) |
| 8 | QEMU | RV32: a write of `mstatush.MPELP` is ignored | QEMU bug, those states are skipped |
| 9 | QEMU | `vl<n>re<w>.v` / `vs<n>r.v` with vstart >= the number of elements: `vext_page_ldst_us: assertion failed: (env->vstart < UINT16_MAX && UINT16_MAX - env->vstart >= elems)` aborts the process | QEMU bug; isolated per state (that state is ORACLE_ERR) |
| 10 | QEMU/Spike | `vsetvl{i} x0, x0` with a changed SEW/LMUL ratio: QEMU keeps vl if it fits, Spike sets vill | reserved by the specification |
| 11 | Spike | illegal instruction for vector arithmetic with vstart != 0 | permitted by the specification |
| 12 | QEMU | a masked instruction with v0 as a source, or vmerge / vadc / vsbc with a v0 source, is illegal | QEMU wrong (only the destination may not overlap v0) |
| 13 | QEMU | stricter overlap rules (the floating point twins too: `vfwmacc`, `vfwnmsac`, `vfwmaccbf16`, `vfwadd.wv`): a source in the top of a wide destination of a widening multiply-add, the narrow vs1 inside a wide group (`.wv`), vs1 inside the vs2 group of a narrowing instruction or of a widening reduction, the data and the index group of an indexed store, the index group and vs2 of `vrgatherei16` | QEMU stricter than the specification (rule 5.2); Spike executes them |
| 14 | QEMU | a whole register load or store that faults reports a vstart rounded down to a chunk of 8 bytes and transfers fewer elements | QEMU imprecise; Spike and dismount are exact |
| 15 | QEMU | `vtype` bit 8 (altfmt of Zvfbfa) is valid for e8 / e16 | an extension dismount doesn't model yet; Spike without it: vill |
| 17 | Spike | `vfredusum.vs` / `vfwredusum.vs` with vl > 0 and no active element and a NaN in `vs1[0]`: Spike writes the canonical NaN (and raises invalid for a signaling one), QEMU passes the scalar through | both permitted (V 1.0, 14.3: "permitted to canonicalize the NaN ... alternatively permitted to pass through the original NaN and set no exception flags"); dismount passes it through |
| 16 | Spike | a vector store that faults on the upper bytes of an element writes the lower ones; a fault only first segment load writes the fields before the one that faults | implementation defined (dismount follows QEMU: the element is not written) |

Other than these, QEMU and Spike agree on every scalar line (RV32 and RV64, 800 states per FP line, 128 per other
line) and on the vector sample (VLEN 128, 256 and 512, 400 states per line): see "Throughput" for the runs.

## The vector lifter

`RiscvSemanticLiftVector*.cpp` and `RiscvSemanticVector.cpp` lift the configuration (`vsetvli`, `vsetivli`, `vsetvl`),
the loads and stores, the integer arithmetic, the fixed point instructions, the mask instructions, the permutations
and the integer reductions (Zve32x and Zve64x: 526 mnemonics), and the rest of V (`RiscvSemanticLiftVectorFloat.cpp`: the
floating point of Zve32f / Zve64d / Zvfh / Zvfhmin and the BFloat16 of Zvfbfmin / Zvfbfwma, 105 + 4 mnemonics;
`...Bits.cpp`: Zvbb, Zvkb, Zvbc; `...Crypto.cpp`: Zvkg, Zvkned, Zvknha, Zvknhb, Zvksed, Zvksh). The infrastructure is
documented in
`Include/dismount/proc/riscv/RiscvSemanticVector.h`; the model, in short:

* vtype, vl and vstart are registers read at runtime. Every instruction snapshots vl and vstart, traps if `vtype.vill`
  is set and dispatches on (vsew, vlmul): the 28 legal combinations each get their own body in which SEW, LMUL,
  EEW, EMUL, the number of registers of a group and the legality of the register numbers are lift-time constants
  (a reserved group is an `OP_TRAP` in that body), then vstart = 0.
* The element-wise instructions (`RvvLift::elementwise`) cut the elements into chunks of VLEN / (the widest EEW)
  lanes so that a chunk of any operand lives in one register, compute the whole chunk with vector IR operations
  (`RvvElementOp`), merge it with the old destination by the active lanes (vstart <= i < vl, v0), and write the
  destination after all the chunks (the overlaps the architecture allows read the old values). Fixed point
  rounding (vxrm) is computed per lane and selected by the runtime mode; vxsat accumulates the active lanes that
  saturated.
* The instructions with a data dependent order or address (loads, stores, vrgather, the slides, vcompress, viota)
  are element loops whose counter is the vstart register itself, so a fault leaves the architectural vstart (the
  elements before it are written / stored). A register group is accessed by `OP_READ_REGISTER_INDEXED` /
  `OP_WRITE_REGISTER_INDEXED` over v0..v31. The fault only first loads probe each field of the element with
  `OP_PROBE_ACCESS` and set vl to the element index.
* The tail, the prestart and the masked off elements are left undisturbed (the agnostic policies allow it, and the
  reference processors are configured to it).

| Topic | dismount | Why |
|---|---|---|
| `vsetvl{i} x0, x0` with a vtype whose VLMAX differs from the current one (also from an illegal current vtype) | vill (vtype = the vill bit, vl = 0) | Spike; reserved by the specification. QEMU keeps vl when it fits the new VLMAX (`oracle_bugs.txt`) |
| `vl` | min(AVL, VLMAX) | both processors; the specification allows ceil(AVL / 2) <= vl <= VLMAX for AVL < 2 * VLMAX |
| vtype bit 8 (altfmt of Zvfbfa) and bits 9 and up | reserved: vill | Spike; QEMU 11 accepts bit 8 for e8 / e16 (`oracle_bugs.txt`). The next lifter adds altfmt (`RvvLift::vtypeIsValid`) |
| vstart != 0 | the arithmetic honors it (the elements from vstart); the reductions, `vcpop.m`, `vfirst.m`, `vmsbf/sif/sof.m`, `viota.m`, `vcompress.vm` raise illegal instruction | specification; Spike raises it for all of them (those states are dropped), QEMU runs the arithmetic from vstart |
| vstart written with a value above what the CSR holds | the states keep vstart below VLEN (the CSR is log2(VLEN) bits wide) | both processors truncate |
| a masked instruction with v0 as a source, widening overlaps in the top of the destination, the narrow vs1 inside a wide group, indexed stores whose data and index groups overlap, `vrgatherei16` with the index overlapping vs2 | executed | specification; QEMU rejects them, Spike executes them (`oracle_bugs.txt`) |
| `vmv<n>r.v` | copies the elements from vstart (SEW sized) of n registers; vill traps | Spike |
| a vector store that faults in an element | the element is not written at all, the elements before it are | QEMU. Spike writes the lower bytes of a misaligned element (`oracle_bugs.txt`) |
| the memory accesses | element by element, byte accurate, misaligned elements allowed | Zicclsm / QEMU / Spike |
| `ta` / `ma` | ignored: the tail and the masked off elements stay undisturbed | allowed; both processors are configured so |
| the floating point flags and `frm` | every OPFVV / OPFVF instruction (also `vfmin`, `vfsgnj`, `vfclass`, the compares, `vfmerge`, `vfmv.*`, `vfslide1*`, the reductions and the `rtz` / `rod` conversions) raises illegal instruction for a reserved `frm` (5 - 7); `fflags` gets the flags of the active elements only (an inactive lane is replaced by a value that raises nothing before the operation) | QEMU and Spike agree (every vector floating point instruction checks `frm`) |
| NaN results and `fmin` / `fmax` | the canonical NaN; `vfmin` / `vfmax` / `vfredmin` / `vfredmax` are minimumNumber / maximumNumber (a signaling NaN raises invalid) | V 1.0, 13.11; both processors |
| `vmfeq` / `vmfne` / `vmflt` / `vmfle` / `vmfgt` / `vmfge` | `vmfeq`, `vmfne` signal invalid for a signaling NaN only, the others for any NaN | V 1.0, 13.13 |
| `vfcvt.x.f.v` of a NaN or an out of range value | the largest (or smallest) integer, and invalid, as the scalar `fcvt` | V 1.0, 13.17 |
| `vfrec7.v`, `vfrsqrt7.v` | the tables of the specification (`SemanticFloat::riscvEstimate`, `BEHAVIOR_ESTIMATE_RISCV`): exact, no estimate flag; the reciprocal of a subnormal that overflows gives infinity or the largest finite number by `frm` with overflow and inexact | V 1.0, 13.9 and 13.10; both processors |
| `vfredosum.vs`, `vfwredosum.vs` | sequential in element order from `vs1[0]`, one rounding per element (an element loop) | V 1.0, 14.3 |
| `vfredusum.vs`, `vfwredusum.vs` | the same sequential sum (the unordered sum may use any order), and `vs1[0]` passes through unchanged when no element is active | V 1.0, 14.3; QEMU does the same, Spike canonicalizes a NaN there (permitted, rule 17 above) |
| `vfwcvt.f.f.v`, `vfncvt.f.f.w`, `vfncvt.rod.f.f.w` | exact widening; the narrowing rounds by `frm`, `rod` is round to odd; SEW 16 needs Zvfhmin (SEW 8 widening / narrowing integer sides need Zvfh), all enabled | V 1.0, 13.18 |
| the scalar of the `.vf` forms | NaN boxed: the f register is read as a float of SEW bits, a value whose upper bits aren't all ones is the canonical NaN of the format (BFloat16: 0x7fc0) | V 1.0, 13.2 |
| the crypto instructions | SEW 32 (64 for SHA-512 with Zvknhb), `vl` and `vstart` multiples of EGS (else illegal instruction), the element group must fit `LMUL * VLEN`, `vd` / `vs2` (`vs1`) aligned to LMUL, the `.vs` forms: `vs2` is the first element group, not in the group of `vd`; `vsha2*`: `vd` is not `vs1` / `vs2`; `vsm3*`: `vd` is not `vs2`; masked encodings don't exist | Zvk 1.0; both processors |
| the SM4 S-box | the helper `RISCV_HELPER_SM4_SBOX` (RiscvHelpers) like the scalar `sm4ed` / `sm4ks` | the IR has no S-box operation |
| Spike and the crypto instructions with `vstart != 0` | Spike raises illegal instruction for them like for all the vector arithmetic (permitted); the 68 directed lines of `vgen_crypto` with a `vstart` are `ORACLE_ERR` on Spike and are verified by QEMU only | `oracle_spike.py` |

## Throughput (Apple M4, 10 cores, `--jobs 8`)

* One QEMU process: ~2 500 tests/s including the boot (0.15 s); a batch is 2 000 tests (~4.5 MB of input at
  VLEN 128). 8 processes: 12-15 000 states/s, Spike ~10 000 states/s (8 processes), Unicorn ~9 000 states/s in one
  Python process. State drawing: ~200 000 states/s (parallel); the comparison and the dismount_cli parse are the
  Python cost of ~3 000 states/s per core.
* `--cross qemu,spike` over the whole starter corpus (1214 lines, 128 states: 155 000 states per oracle): 50 s.
  FP lines at 800 states: 279 000 states per oracle in 49 s. So the planned ~50k lines x 16 states = 800 000
  states take 1-2 minutes per oracle for the oracle side (the dismount_cli side is the lifter's speed).

## The scalar lifter: decisions where the processors or the specification leave a choice

The scalar instructions (`RiscvSemanticLift*.cpp`) are verified with QEMU and Spike (`corpus/*_gen_*.s`, the
decoder corpus of every register / immediate / rounding mode; `corpus/*_dir_*.s`, the directed values) and Unicorn
for the base. Where the oracles disagree, or the specification gives an implementation the choice, dismount's
behavior and the reason are:

| Topic | dismount | Why |
|---|---|---|
| `rm` of the conversions that don't round (`fcvt.d.w`, `fcvt.d.s`, `fcvt.s.h`...) | the field is read from the encoding like every rounding mode: `dyn` with a reserved `frm` (5-7) is an illegal instruction, a static mode is not checked (the decoder never produces 5 / 6) | QEMU and Spike agree (every instruction with an rm field calls `set_rm`) |
| NaN results | the canonical NaN (positive, quiet bit only) for every operation, including the conversions of a NaN | F 2.2; `FloatEnvironment::NAN_DEFAULT` |
| `fmin` / `fmax` / `fminm` / `fmaxm` | `BEHAVIOR_MINMAX_NUMBER` / `BEHAVIOR_MINMAX_PROPAGATE_NAN`; -0 < +0 | F 2.2 (minimumNumber), Zfa (minimum) |
| float to integer | saturate, a NaN is the largest positive value, invalid raised; `fcvt.wu` of a negative is 0; the 32 bit results are sign extended on RV64 | F 2.2 |
| `fcvtmod.w.d` | the integer part modulo 2^32, as a signed word; NaN and infinity give 0; invalid for those and for a value outside of [-2^31, 2^31) (and no inexact then), inexact for a fraction | QEMU and Spike agree |
| NaN boxing | a read of an f register as a narrower format whose upper bits aren't all ones is the canonical NaN of the format; `fsw`, `fsh`, `fmv.x.w`, `fmv.x.h` use the raw bits | F 2.2; QEMU and Spike agree |
| Misaligned loads and stores | complete (Zicclsm), byte accurate, a page / window crossing access faults without writing | QEMU. Spike writes the bytes below the faulting one (`oracle_bugs.txt`: implementation defined) |
| Misaligned AMOs | an AMO whose access stays inside a naturally aligned 16 byte region completes (Zama16b), one that crosses a region raises the access fault (`RISCV_TRAP_ACCESS_FAULT`) | Zama16b; Spike. QEMU completes a crossing AMO (rule 6 of the table below) and faults a misaligned `amo*.b/.h` / `amocas.*` even inside the region |
| `lr` / `sc` | a natural alignment is required (a misaligned lr / sc faults, as QEMU and Spike do); the reservation is the address and the size, ended by any `sc`; an `sc` to an unmapped address faults even when it fails | QEMU and Spike agree on the alignment and the fault. Same-hart stores between lr and sc don't invalidate it, an `sc` of another size fails (the two processors differ on each: `oracle_bugs.txt`) |
| `amocas` pairs | `amocas.d` on RV32 and `amocas.q` on RV64 use the even register pair (x0 reads as a zero pair, nothing is written) | Zacas |
| Zicfilp (`RiscvArchitecture::isControlFlowIntegrity()`) | `jalr` / `c.jr` / `c.jalr` set elp unless the base is ra, t0 (whatever rd is) or t2; every instruction but `lpad` begins with a trap if elp is set; `lpad` traps if the pc isn't 4 aligned or the label is not 0 and not x7[31:12], clears elp otherwise; without the enable `lpad` is a nop | QEMU and Spike agree (the jalr forms are in `corpus/rv_dir_cfi.s`) |
| Zicfiss | `sspush` / `sspopchk` / `ssrdp` / `ssamoswap` / `c.sspush` / `c.sspopchk` use ssp (misaligned: access fault, a mismatch of `sspopchk` is the software check exception); `csr ssp` is a plain register; without the enable they are Zimop (`ssamoswap` and the ssp CSR are illegal instructions) | Zicfiss. Spike clears the low bits of ssp when the guest writes the CSR (`oracle_bugs.txt`) |
| Zimop / Zcmop | `mop.r.N` / `mop.rr.N` / `c.mop.N` write 0 to rd | Zimop |
| `fence` | an OP_FENCE with the sets: `i` and `r` are loads, `o` and `w` stores; `fence.tso` is the full fence | no finer description in the IR |
| `fence.i` | OP_FENCE_INSTRUCTION; `pause` is HINT_SPIN, `wrs.*` HINT_WAIT; the hints are no-ops | |
| `cbo.clean` / `flush` / `inval` | a one byte load of the aligned 64 byte block (the fault) and OP_CACHE_MAINTENANCE | Zic64b |
| `cbo.zero` | 8 stores of 64 bits of zero to the aligned block (the first one faults) | Zic64b |
| `ecall` / `ebreak` / `unimp` | OP_TRAP (system call / breakpoint / undefined) | |
| CSRs | `fflags`, `frm`, `fcsr`, `vstart`, `vxsat`, `vxrm`, `vcsr`, `vl`, `vtype`, `vlenb` (a constant VLEN / 8), `ssp` are inline; the others (the counters, `seed`, the supervisor / machine CSRs) are `RISCV_HELPER_READ_CSR` / `WRITE_CSR`. A write to a read only CSR (`csr[11:10] == 3`, `vl`, `vtype`, `vlenb`) is an illegal instruction, `csrrs` / `csrrc` with x0 (and `csrrsi` / `csrrci` with 0) don't write, `csrrw` to x0 doesn't read | Zicsr |
| Scalar cryptography | AES, SHA-2, SM3 in the IR (the AES steps of the IR for a lookup of the S-box), SM4 (`sm4ed`, `sm4ks`) the helper `RISCV_HELPER_INSTRUCTION` implemented by `RiscvHelpers` | the S-box of SM4 has no IR operation |
| The helpers on the reference runs | `RiscvHelpers::execute` (SM4, the counters read 0, `seed`); the user mode `dismount_cli run` answers the helper of any other CSR with an illegal instruction (user mode can't access it) | |

What the reference runs do not cover, and why (the lines are in `skip.txt` or not in the corpus):

* The privileged instructions (`mret`, `sret`, `wfi`, `sfence.vma`, the Svinval and H instructions) trap in user
  mode: their semantics are the helper `RISCV_HELPER_PRIVILEGED`; they are lifted and not verified
  (`SUPPORT_LIFT` without `SUPPORT_LIFT_VERIFIED`, and `privileged.s` of the decoder corpus is not run).
* The counters (`rdcycle`, `rdtime`, `rdinstret`, `csrr*` of `cycle`, `time`, `instret`, `hpmcounter*`) and `seed`:
  the values differ at every run (the write attempts to them, an illegal instruction, are in `rv_dir_csr.s`).
* `wrs.nto`, `wrs.sto`: the reference would wait for a reservation to be invalidated.
* The encodings the decoder rejects like LLVM and which Zimop could use (`mop.r.28` with rd = x0 that isn't
  `sspopchk`, `mop.rr.7` with rd = rs1 = x0).
* Branches and jumps to their own address and into the middle of the instruction (`gen_corpus.py`).

## Results of the scalar gate (16 states per line; starter + generated + directed corpus)

| Arch | Oracle | Lines | PASS | FAIL | UNLIFTED | SKIP | ORACLE_BUG | ORACLE_ERR |
|---|---|---|---|---|---|---|---|---|
| rv64 | qemu | 117574 | 117344 | 0 | 3 | 157 | 70 | 0 |
| rv64 | spike | 117574 | 117354 | 0 | 3 | 167 | 50 | 0 |
| rv32 | qemu | 87156 | 86839 | 0 | 3 | 268 | 32 | 14 |
| rv32 | spike | 87156 | 86836 | 0 | 3 | 278 | 39 | 0 |

* The 3 lines of `rv_oracle_diffs.s` that were UNLIFTED (the floating point and BFloat16 vector instructions) are lifted now (0 UNLIFTED). The 14 ORACLE_ERR are QEMU's RV32 `mstatush.MPELP` bug (table above, 8).
* Unicorn, the RV64GC / RV32GC base files of the generated corpus and the starter files: rv64 19652 PASS, 455
  ORACLE_BUG, 0 FAIL; rv32 15728 PASS, 382 ORACLE_BUG, 0 FAIL.
* `run_compat.py` (decoder): 0 failures. `test_dismount`: Debug and Release pass.

## Results of the vector gate (the vector corpus `rv_vgen_*`, `rv64_vgen_*`)

The whole vector corpus (integer, fixed point, mask, permutation, memory, floating point, bit manipulation and
cryptography), 16 states per line, every arch / oracle / VLEN (`dismount_cli` against QEMU 11.1.3 `-cpu max` and
Spike with zvfh, zvfbfmin, zvfbfwma, zvbb, zvbc, zvkg, zvkned, zvknhb, zvksed, zvksh):

| Arch | Oracle | VLEN | Lines | States | PASS | FAIL | UNLIFTED | SKIP | ORACLE_BUG | ORACLE_ERR |
|---|---|---|---|---|---|---|---|---|---|---|
| rv64 | qemu | 128 | 13744 | 16 | 13091 | 0 | 0 | 0 | 653 | 0 |
| rv64 | qemu | 256 | 13744 | 16 | 13081 | 0 | 0 | 0 | 663 | 0 |
| rv64 | qemu | 512 | 13744 | 16 | 13064 | 0 | 0 | 0 | 680 | 0 |
| rv64 | spike | 128 | 13744 | 16 | 13600 | 0 | 0 | 0 | 76 | 68 |
| rv64 | spike | 256 | 13744 | 16 | 13571 | 0 | 0 | 0 | 105 | 68 |
| rv64 | spike | 512 | 13744 | 16 | 13481 | 0 | 0 | 0 | 195 | 68 |
| rv32 | qemu | 128 | 13304 | 16 | 12679 | 0 | 0 | 0 | 625 | 0 |
| rv32 | qemu | 256 | 13304 | 16 | 12665 | 0 | 0 | 0 | 639 | 0 |
| rv32 | qemu | 512 | 13304 | 16 | 12640 | 0 | 0 | 0 | 664 | 0 |
| rv32 | spike | 128 | 13304 | 16 | 13163 | 0 | 0 | 0 | 73 | 68 |
| rv32 | spike | 256 | 13304 | 16 | 13113 | 0 | 0 | 0 | 123 | 68 |
| rv32 | spike | 512 | 13304 | 16 | 13053 | 0 | 0 | 0 | 183 | 68 |

The stress run of the new families (`vgen_bits crypto fadd fcmp fcvt fdir ffma fmove fred funary fwide`: 4 096 lines,
64 states per line):

| Arch | Oracle | VLEN | Lines | States | PASS | FAIL | UNLIFTED | SKIP | ORACLE_BUG | ORACLE_ERR |
|---|---|---|---|---|---|---|---|---|---|---|
| rv64 | qemu | 128 | 4096 | 64 | 3874 | 0 | 0 | 0 | 222 | 0 |
| rv64 | spike | 128 | 4096 | 64 | 4025 | 0 | 0 | 0 | 3 | 68 |
| rv64 | qemu | 512 | 4096 | 64 | 3873 | 0 | 0 | 0 | 223 | 0 |
| rv64 | spike | 512 | 4096 | 64 | 4025 | 0 | 0 | 0 | 3 | 68 |
| rv32 | qemu | 256 | 4096 | 64 | 3873 | 0 | 0 | 0 | 223 | 0 |
| rv32 | spike | 256 | 4096 | 64 | 4026 | 0 | 0 | 0 | 2 | 68 |

* The ORACLE_BUG lines are the rules of `oracle_bugs.txt` (table of the disagreements above): the strictness of QEMU
  (a v0 source of a masked instruction or of `vfmerge`, the overlaps of the integer and floating point widening and narrowing
  instructions, 60 lines of `vsetvl{i} x0, x0`), the 8 byte chunks of the whole register accesses of QEMU, the partial writes of a
  faulting store and of a fault only first segment of Spike, and the NaN of `vfredusum` without an active element for Spike.
  Every line is verified by the other oracle at least for the states that do not hit its own quirk (for the new floating
  point, bit manipulation and cryptography families Spike has 3 ORACLE_BUG lines at most, so Spike verifies them all but
  those, and QEMU those; nothing is hidden by a rule: the QEMU rules match the strictness of the overlaps only).
* ORACLE_ERR on Spike is the 68 directed lines of `vgen_crypto` with a nonzero `vstart` (Spike raises illegal instruction for
  every state; QEMU runs them and verifies them).
* UNLIFTED is 0.

## Results of the whole gate (scalar and vector corpus, 16 states per line, VLEN 128)

| Arch | Oracle | Lines | PASS | FAIL | UNLIFTED | SKIP | ORACLE_BUG | ORACLE_ERR |
|---|---|---|---|---|---|---|---|---|
| rv64 | qemu | 131721 | 130837 | 0 | 0 | 157 | 727 | 0 |
| rv64 | spike | 131721 | 131360 | 0 | 0 | 167 | 126 | 68 |
| rv32 | qemu | 100859 | 99915 | 0 | 0 | 268 | 662 | 14 |
| rv32 | spike | 100859 | 100399 | 0 | 0 | 278 | 114 | 68 |

* UNLIFTED is 0: the vector starter lines (`rv_v.s`, `rv_v2.s`) and the lines of `rv_oracle_diffs.s` that use the floating
  point, BFloat16, Zvbb and cryptography instructions are lifted now. The scalar files have the PASS / SKIP counts of the
  scalar gate above (SKIP 157 / 167 and 268 / 278 are the same; the ORACLE_BUG counts of the scalar files are those of the
  tables above plus the vector lines).
* `run_compat.py` (decoder): 0 failures. `test_dismount`: Debug and Release (-O2) pass (5 049 vector rows: 2 567 RV64 and
  2 482 RV32, and 4 814 scalar rows).
* The shared code changed once: `SemanticFloat::riscvEstimate` and the `BEHAVIOR_ESTIMATE_RISCV` case of
  `SemanticInterpreter` (vfrec7 / vfrsqrt7, new code only reached by the RISC-V behavior). The ARM and IA-32 gates were
  re-run: see the results below.

## The shared-code gates (the estimates of `SemanticFloat` / `SemanticInterpreter`)

Re-run after the change (this branch, Debug): A64 against Unicorn PASS 48 026, FAIL 2 (`addsvl`, `rdsvl`: Unicorn has no SME
streaming vector length) and against the host PASS 51 622, FAIL 1 (`ldxrb`); both are the same on the build before the
change (`dismount_cli` of master, same lines), so there is no regression. A32 PASS 4 452 and T32 PASS 2 136, FAIL 0. IA-32
(`tests/semantics/run_semantics.py --strict`): PASS 53 680, FAIL 0, UNLIFTED 779, ORACLE_BUG 694. `test_dismount` passes
(the ARM and IA-32 unit tests).
