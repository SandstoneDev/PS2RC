/* PoC-1 guest: single image at 0x00100000 (like a retail PS2 ELF). */
ENTRY(_start)
SECTIONS
{
  . = 0x00100000;
  .text : { *(.text.start) *(.text) *(.text.*) }
  . = ALIGN(64);
  .data : { *(.data) *(.rodata*) }
  . = ALIGN(64);
  .bss (NOLOAD) : { *(.bss) *(COMMON) }
  /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.comment) *(.gnu.attributes) }
}
