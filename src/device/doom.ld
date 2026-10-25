/* polyend-doom linker script: OpenTracker's src/hal/opentracker.ld (itself
 * the Teensyduino mk66fx1m0.ld, MIT, Copyright (c) 2017 PJRC.COM, LLC) for
 * the Doom image. Differences:
 *  - Doom's large .bss goes to the SDRAM (.sdram_doom, NOLOAD, zeroed by
 *    otAppMain once the SDRAM passed its test), and its initialised data too
 *    (.sdram_data, copied from flash by otAppMain); only the renderer's hot
 *    arrays stay in SRAM;
 *  - the rest of the SDRAM below the HAL's .sdram_bss is the malloc heap
 *    (_sheap_sdram.._eheap_sdram, served by the wrapped _sbrk), which holds
 *    Doom's zone;
 *  - Doom's column / span drawers are copied to SRAM_L with .data, like
 *    OpenTracker's .fastrun audio code;
 *  - .dma is placed at the start of SRAM_U whatever the size of .bss.
 *
 * K66 memory map: SRAM_L 0x1FFF0000..0x1FFFFFFF (code bus), SRAM_U
 * 0x20000000..0x2002FFFF (system bus, uncached).
 */

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 1024K
	RAM  (rwx) : ORIGIN = 0x1FFF0000, LENGTH = 256K
	/* SDRAM 16 MB at 0x70000000 (OpenTracker board.h). The boot checks use
	 * the first 1 MB before otAppMain runs; nothing else is there by then. */
	SDRAM_DOOM (rw) : ORIGIN = 0x70000000, LENGTH = 0x00FD9000
	/* board::kSdramBssBase / kSdramBssSize (OpenTracker src/hal/board.h). */
	SDRAM_BSS (rw) : ORIGIN = 0x70FD9000, LENGTH = 156K
}

ENTRY(_VectorsFlash)

SECTIONS
{
	.text : {
		. = 0;
		KEEP(*(.vectors))
		*(.startup*)
		. = 0x400;
		KEEP(*(.flashconfig*))
		*(EXCLUDE_FILE(*doomgeneric/r_draw.c.o) .text*)
		*(.rodata*)
		. = ALIGN(4);
		KEEP(*(.init))
		. = ALIGN(4);
		__preinit_array_start = .;
		KEEP (*(.preinit_array))
		__preinit_array_end = .;
		__init_array_start = .;
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		__init_array_end = .;
	} > FLASH = 0xFF

	.ARM.exidx : {
		__exidx_start = .;
		*(.ARM.exidx* .gnu.linkonce.armexidx.*)
		__exidx_end = .;
	} > FLASH
	_etext = .;

	.usbdescriptortable (NOLOAD) : {
		. = ALIGN(512);
		*(.usbdescriptortable*)
	} > RAM

	.dmabuffers (NOLOAD) : {
		. = ALIGN(4);
		*(.dmabuffers*)
	} > RAM

	.usbbuffers (NOLOAD) : {
		. = ALIGN(4);
		*(.usbbuffers*)
	} > RAM

	.data : AT (_etext) {
		. = ALIGN(4);
		_sdata = .;
		/* Code run from SRAM_L: OpenTracker's OT_FASTRUN_FN functions and
		 * Doom's r_draw.c (R_DrawColumn, R_DrawSpan and friends). */
		__fastrun_start = .;
		/* OpenTracker's boot log groups its audio code by area; none here. */
		__fastrun_smp = .; __fastrun_fx = .; __fastrun_mix = .; __fastrun_vap = .;
		__fastrun_acd = .; __fastrun_perc = .; __fastrun_fat = .;
		__fastrun_other = .;
		*(.fastrun*)
		*doomgeneric/r_draw.c.o(.text*)
		__fastrun_end = .;
		*(EXCLUDE_FILE(*doomgeneric/*.c.o) .data*)
		*doomgeneric/r_*.c.o(.data*)
		. = ALIGN(4);
		_edata = .;
	} > RAM

	/* Doom's other initialised data (info.c states / mobjinfo, sounds.c,
	 * m_config.c ...: ~58 KB) lives in the SDRAM; otAppMain copies it from
	 * flash (after .data's image) before Doom starts. */
	.sdram_data : AT (LOADADDR(.data) + SIZEOF(.data)) ALIGN(16) {
		_ssdram_data = .;
		*doomgeneric/*.c.o(.data*)
		. = ALIGN(4);
		_esdram_data = .;
	} > SDRAM_DOOM
	_sisdram_data = LOADADDR(.sdram_data);
	_eflash = _sisdram_data + SIZEOF(.sdram_data);

	.noinit (NOLOAD) : {
		*(.noinit*)
	} > RAM

	.bss : {
		. = ALIGN(4);
		_sbss = .;
		__bss_start__ = .;
		/* Everything but Doom, plus the renderer's per-frame arrays. */
		*(EXCLUDE_FILE(*doomgeneric/*.c.o) .bss*)
		*(EXCLUDE_FILE(*doomgeneric/*.c.o) COMMON)
		*doomgeneric/r_draw.c.o(.bss* COMMON)
		*doomgeneric/r_main.c.o(.bss* COMMON)
		*doomgeneric/r_segs.c.o(.bss* COMMON)
		*doomgeneric/r_bsp.c.o(.bss* COMMON)
		*doomgeneric/r_sky.c.o(.bss* COMMON)
		. = ALIGN(4);
	} > RAM

	/* DMA buffers: SRAM_U, never across the SRAM_L/SRAM_U boundary. */
	.dma MAX(ALIGN(ADDR(.bss) + SIZEOF(.bss), 32), 0x20000000) (NOLOAD) : {
		_sdma = .;
		*(.dma)
		*(.dma.*)
		. = ALIGN(4);
		_edma = .;
		_ebss = .;
		__bss_end = .;
		__bss_end__ = .;
	} > RAM

	/* Doom's remaining .bss (zeroed by otAppMain), then the heap. */
	.sdram_doom (NOLOAD) : ALIGN(16) {
		_ssdram_doom = .;
		*doomgeneric/*.c.o(.bss* COMMON)
		. = ALIGN(16);
		_esdram_doom = .;
	} > SDRAM_DOOM
	_sheap_sdram = ALIGN(_esdram_doom, 16);
	_eheap_sdram = ORIGIN(SDRAM_DOOM) + LENGTH(SDRAM_DOOM);

	/* OpenTracker HAL scratch in the SDRAM (core/sdram_bss.h). */
	.sdram_bss (NOLOAD) : ALIGN(16) {
		_ssdram_bss = .;
		*(.sdram_bss)
		*(.sdram_bss.*)
		. = ALIGN(16);
		_esdram_bss = .;
	} > SDRAM_BSS

	_estack = ORIGIN(RAM) + LENGTH(RAM);
	ASSERT(_eflash <= ORIGIN(FLASH) + LENGTH(FLASH), "polyend-doom: image does not fit the flash")
	ASSERT(_sdma >= 0x20000000, "polyend-doom: .dma must lie in SRAM_U (uncached)")
	ASSERT(_edma <= 0x20030000, "polyend-doom: .dma overflows SRAM_U")
	ASSERT(__fastrun_end <= 0x20000000, "polyend-doom: fast code must lie in SRAM_L (code bus)")
	ASSERT(_estack - _ebss >= 24576, "polyend-doom: less than 24 KB left for the stack")
	_teensy_model_identifier = 0x22;
}
