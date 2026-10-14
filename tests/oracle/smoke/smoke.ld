/*
 * Linker script for the PCSX2-oracle smoke test (PS2 EE, R5900).
 * One PT_LOAD segment at 0x00100000 (the usual EE user-program base, above the
 * kernel), entry `_start`.  Used with: psp-ld -m elf32elmip -T smoke.ld
 */
OUTPUT_FORMAT("elf32-littlemips")
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(7);
}

SECTIONS
{
	. = 0x00100000;

	.text : {
		*(.text .text.*)
	} :text

	.data ALIGN(64) : {
		*(.data .data.*)
		*(.rodata .rodata.*)
		*(.sdata .sdata.*)
	} :text

	.bss ALIGN(64) : {
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
	} :text

	/DISCARD/ : {
		*(.MIPS.abiflags)
		*(.reginfo)
		*(.pdr)
		*(.comment)
		*(.gnu.attributes)
		*(.mdebug.*)
	}
}
