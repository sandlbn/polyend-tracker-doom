// OpenTracker -- placement of DMA buffers (docs/PLAN_2X.md performance workstream).
//
// OT_DMA_MEM puts a buffer into the `.dma` output section of src/hal/opentracker.ld:
// zero-initialised (it lies between _sbss and _ebss), 32-byte aligned, and always
// inside SRAM_U (0x20000000..0x2002FFFF; the linker script asserts it). SRAM_U is
// reached over the system bus and is never cached: the LMEM code-bus cache only
// covers SRAM_L, and the system cache (sdram_cache.h) caches the SDRAM region only.
// So the CPU and the DMA masters (eDMA for I2S, SDHC ADMA2) always see the same
// bytes there and no cache maintenance is needed. A buffer here also never
// crosses the SRAM_L/SRAM_U boundary, which a DMA burst must not do.
#pragma once

#if defined(OT_TARGET_DEVICE)
#define OT_DMA_MEM __attribute__((section(".dma"), aligned(32)))
#else
#define OT_DMA_MEM alignas(32)
#endif
