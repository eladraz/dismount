/*
 * rt.S
 *
 * The assembly part of the bare metal guest runner of the RISC-V semantics
 * tests (tests/riscv/semantics/TEST_PLAN.md): the start, the entry into the
 * user mode with the registers of a test and the M-mode trap handler which
 * records the registers the instruction left.
 *
 * Built for rv32 and rv64 (the preprocessor selects the loads), with the D and
 * V extensions on (runner.c is integer only).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#if __riscv_xlen == 64
#define LREG ld
#define SREG sd
#define SZ 8
#else
#define LREG lw
#define SREG sw
#define SZ 4
#endif

/* The registers are 8 bytes slots in the records of the host in both widths */
#define XS 8

    .section .text.start
    .globl _start
_start:
    .option push
    .option norelax
    la sp, _stack_top
    .option pop
    call main
1:  j 1b

    .text
    .balign 4
/*
 * enter_user(const u64* xs, u64* outx)
 *
 * Saves the callee saved registers, sets mscratch to the output x array, loads
 * x1..x31 and returns to the user mode at mepc. The trap handler restores the
 * callee saved registers and returns to the caller of enter_user.
 */
    .globl enter_user
enter_user:
    la t0, m_ctx
    SREG s0, 0*SZ(t0)
    SREG s1, 1*SZ(t0)
    SREG s2, 2*SZ(t0)
    SREG s3, 3*SZ(t0)
    SREG s4, 4*SZ(t0)
    SREG s5, 5*SZ(t0)
    SREG s6, 6*SZ(t0)
    SREG s7, 7*SZ(t0)
    SREG s8, 8*SZ(t0)
    SREG s9, 9*SZ(t0)
    SREG s10, 10*SZ(t0)
    SREG s11, 11*SZ(t0)
    SREG ra, 12*SZ(t0)
    SREG sp, 13*SZ(t0)
    csrw mscratch, a1
    mv t6, a0
    .irp i,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
    LREG x\i, XS*\i(t6)
    .endr
    LREG t6, XS*31(t6)
    mret

    .balign 4
    .globl trap_entry
trap_entry:
    csrrw t6, mscratch, t6
    .irp i,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
    SREG x\i, XS*\i(t6)
    .endr
    csrr t5, mscratch
    SREG t5, XS*31(t6)
    csrw mscratch, t6
    csrr t5, mcause
    SREG t5, -80(t6)
    csrr t5, mepc
    SREG t5, -72(t6)
    csrr t5, mtval
    SREG t5, -64(t6)
    csrr t5, 0x008              /* vstart */
    SREG t5, -56(t6)
    csrw 0x008, zero
    csrr t5, 0xC20              /* vl */
    SREG t5, -48(t6)
    csrr t5, 0xC21              /* vtype */
    SREG t5, -40(t6)
    csrr t5, 0x003              /* fcsr */
    SREG t5, -32(t6)
    csrr t5, 0x00F              /* vcsr */
    SREG t5, -24(t6)
    csrr t5, mstatus            /* (MPELP: the expected landing pad state at the trap) */
    SREG t5, -8(t6)
#if __riscv_xlen == 32
    csrr t5, 0x310              /* mstatush */
    sw t5, -4(t6)
#endif
    la t4, cfi_enabled
    lw t4, 0(t4)
    beqz t4, 1f
    csrr t5, 0x011              /* ssp */
    SREG t5, -16(t6)
1:
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    fsd f\i, 256+8*\i(t6)
    .endr
    addi t5, t6, 512
    csrr t4, 0xC22              /* vlenb */
    slli t4, t4, 3
    vs8r.v v0, (t5)
    add t5, t5, t4
    vs8r.v v8, (t5)
    add t5, t5, t4
    vs8r.v v16, (t5)
    add t5, t5, t4
    vs8r.v v24, (t5)
    la t0, m_ctx
    LREG s0, 0*SZ(t0)
    LREG s1, 1*SZ(t0)
    LREG s2, 2*SZ(t0)
    LREG s3, 3*SZ(t0)
    LREG s4, 4*SZ(t0)
    LREG s5, 5*SZ(t0)
    LREG s6, 6*SZ(t0)
    LREG s7, 7*SZ(t0)
    LREG s8, 8*SZ(t0)
    LREG s9, 9*SZ(t0)
    LREG s10, 10*SZ(t0)
    LREG s11, 11*SZ(t0)
    LREG ra, 12*SZ(t0)
    LREG sp, 13*SZ(t0)
    ret

/*
 * load_state(const u64* f, const u8* v, unsigned long vlenb)
 *
 * Loads f0..f31 and v0..v31. The whole register loads don't depend on vtype.
 */
    .globl load_state
load_state:
    .irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    fld f\i, 8*\i(a0)
    .endr
    slli t0, a2, 3
    vl8re8.v v0, (a1)
    add a1, a1, t0
    vl8re8.v v8, (a1)
    add a1, a1, t0
    vl8re8.v v16, (a1)
    add a1, a1, t0
    vl8re8.v v24, (a1)
    ret

/* set_vtype(avl, vtype): vsetvl x0, a0, a1 */
    .globl set_vtype
set_vtype:
    vsetvl x0, a0, a1
    ret

    .section .bss
    .balign 16
m_ctx:
    .space 16*8
    .globl cfi_enabled
cfi_enabled:
    .space 8
