/* example.ld - the example program's memory map (the numbers games/example/pack.toml [memory] repeats):
 *   0x00100000-0x00108000  code (one loadable segment, padded)   .text.frame at +0xE00, the library stand-ins at +0xF00
 *   0x00108000-0x00109000  data (padded to 4 KB: the end of the file data is fixed)
 *   0x00109000-0x0010A000  bss
 * One PT_LOAD for everything, like a retail PS2 program: the PSP loader reads the data part of it. */
ENTRY(_start)
PHDRS
{
  all PT_LOAD FLAGS(7);
}
SECTIONS
{
  .text 0x00100000 : {
    *(.text.start)
    *(.text)
    . = 0x0E00;
    *(.text.frame)
    . = 0x0F00;
    *(.text.lib)
    . = 0x8000;
  } :all
  .data 0x00108000 : {
    *(.data) *(.rodata*)
    . = 0x1000;
  } :all
  _gp = ADDR(.data) + 0x7FF0;
  .bss 0x00109000 (NOLOAD) : {
    *(.bss) *(COMMON)
    . = 0x1000;
  } :all
  _end = .;
  /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.comment) *(.gnu.attributes) }
}
