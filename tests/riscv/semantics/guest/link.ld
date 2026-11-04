/* The linker script of the bare metal guest runner (tests/riscv/semantics/TEST_PLAN.md) */
ENTRY(_start)
SECTIONS
{
    . = 0x80000000;
    .text : { *(.text.start) *(.text .text.*) }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    . = ALIGN(0x1000);
    .tohost : { *(.tohost) }
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4096); }
    . = ALIGN(4096);
    _pt_start = .;
    . += 0x10000;
    _stack_bottom = .;
    . += 0x10000;
    _stack_top = .;
}
