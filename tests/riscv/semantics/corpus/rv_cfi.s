# Zicfilp / Zicfiss with the enables on (the state has cfi: menvcfg / senvcfg LPE and SSE, ssp in the shadow stack
# page, the expected landing pad state elp). The lifter lifts the enabled behavior. QEMU and Spike only.
# @mode: cfi is implied by the mnemonic, the tag is spelled out for the lines which are not CFI instructions.
lpad 0                   # @mode: cfi
lpad 1                   # @mode: cfi
lpad 0x12345             # @mode: cfi
lpad 0xfffff             # @mode: cfi
sspush ra                # @mode: cfi
sspush t0                # @mode: cfi
c.sspush ra              # @mode: cfi
sspopchk ra              # @mode: cfi
sspopchk t0              # @mode: cfi
c.sspopchk t0            # @mode: cfi
ssrdp a0                 # @mode: cfi
ssrdp t1                 # @mode: cfi
ssamoswap.w a0, a2, (a1) # @mode: cfi
ssamoswap.w.aq a0, a2, (a1) # @mode: cfi
ssamoswap.w.rl a0, a2, (a1) # @mode: cfi
ssamoswap.w.aqrl a0, a2, (a1) # @mode: cfi
add a0, a1, a2           # @mode: cfi
addi a0, a1, 1           # @mode: cfi
c.nop                    # @mode: cfi
nop                      # @mode: cfi
jalr ra, 0(a1)           # @mode: cfi
jalr zero, 0(a1)         # @mode: cfi
jalr zero, 0(t2)         # @mode: cfi
jalr zero, 0(ra)         # @mode: cfi
jalr t0, 0(a1)           # @mode: cfi
c.jr a1                  # @mode: cfi
c.jalr a1                # @mode: cfi
c.jr ra                  # @mode: cfi
jal ra, 8                # @mode: cfi
beq a0, a1, 8            # @mode: cfi
ecall                    # @mode: cfi
ebreak                   # @mode: cfi
