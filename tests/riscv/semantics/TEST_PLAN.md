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
| `build_guest.py`, `guest/runner.c`, `guest/rt.S`, `guest/link.ld` | The bare metal guest of QEMU and Spike (rv32 and rv64, built to `out/riscv_guest/`) |
| `oracle_qemu.py`, `oracle_spike.py`, `oracle_unicorn.py` | The oracles |
| `corpus/*.s` | `rv_*` (RV32 and RV64), `rv64_*`, `rv32_*` (one XLEN): `i m a a_pair f d d_gpr fd c b zfa zfh misc cfi v v2 oracle_diffs` |
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

Starter corpus (`corpus/`, about 1300 lines per XLEN): the base integer instructions with boundary immediates (all
shifts, loads and stores with offsets, branches, jal, jalr, csr instructions of fcsr/fflags/frm, fences), M, A (all amo
orderings, lr/sc pairs), F, D, Zfh, Zfa, Zfbfmin (every rounding mode on the arithmetic, the conversions and the
comparisons), C (all the Zca/Zcb/Zcd forms, assembled as themselves), Zba/Zbb/Zbs/Zbc/Zbkb/Zbkx/Zicond, the scalar
cryptography, the hints and cache block ops, Zimop/Zcmop, Zabha/Zacas, the CFI instructions, and about 400 vector
instructions (vsetvl*, the integer, fixed point, floating point, widening/narrowing, mask, reduction, permutation,
load/store (unit stride, strided, indexed, segment, fault-only-first, whole register) forms, Zvbb, Zvbc, Zvk*,
Zvfbf*). `oracle_diffs` is the evidence for oracle_bugs.txt.

The states: random integers (boundary values and random widths), floating point (zeros, infinities, quiet and
signaling NaNs, denormals, the integer conversion boundaries, random normals) boxed 80% of the time (else the upper
bits are zero, partly set or random), `fcsr` with the rounding modes 0-4 (and, rarely, the reserved 5-7) and the
flags, vtype over every SEW and LMUL (those the named registers allow; 6% any raw value, 2% vill, 2% a reserved bit)
and policy, vl at 0, 1, VLMAX, VLMAX - 1, random; vstart nonzero in 20% (less than vl, rarely beyond VLMAX),
vxrm / vxsat, masks in v0 (all ones, all zeros, 0x55.., random), elements typed by SEW (floating point values for
the vf*, small offsets for the index vectors); base registers point into the window (8% misaligned, 4% outside to
fault, atomics always aligned), strides small; random bytes and floating point constants around the pointers.

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

Other than these, QEMU and Spike agree on every scalar line (RV32 and RV64, 800 states per FP line, 128 per other
line) and on the vector sample (VLEN 128, 256 and 512, 400 states per line): see "Throughput" for the runs.

## Throughput (Apple M4, 10 cores, `--jobs 8`)

* One QEMU process: ~2 500 tests/s including the boot (0.15 s); a batch is 2 000 tests (~4.5 MB of input at
  VLEN 128). 8 processes: 12-15 000 states/s, Spike ~10 000 states/s (8 processes), Unicorn ~9 000 states/s in one
  Python process. State drawing: ~200 000 states/s (parallel); the comparison and the dismount_cli parse are the
  Python cost of ~3 000 states/s per core.
* `--cross qemu,spike` over the whole starter corpus (1214 lines, 128 states: 155 000 states per oracle): 50 s.
  FP lines at 800 states: 279 000 states per oracle in 49 s. So the planned ~50k lines x 16 states = 800 000
  states take 1-2 minutes per oracle for the oracle side (the dismount_cli side is the lifter's speed).
