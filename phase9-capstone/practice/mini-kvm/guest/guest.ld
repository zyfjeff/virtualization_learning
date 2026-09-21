/* SPDX-License-Identifier: GPL-2.0 */
/*
 * guest.ld —— guest 链接脚本：入口 _start，链接到 GPA 0x1000
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
	. = 0x1000;

	.text : {
		*(.text*)
	}

	.rodata : {
		*(.rodata*)
	}

	.data : {
		*(.data*)
	}

	.bss : {
		*(.bss*)
		*(COMMON)
	}

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
