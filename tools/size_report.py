#!/usr/bin/env python3
"""polyend-doom image size report: flash, SRAM, stack room and SDRAM use of
build/doom.elf from the linker symbols of src/device/doom.ld.

    size_report.py [--size-tool arm-none-eabi-size] build/doom.elf
"""
import argparse
import subprocess
import sys

FLASH = 1024 * 1024
SRAM_BASE, SRAM_END = 0x1FFF0000, 0x20030000


def symbols(elf, nm):
    out = subprocess.run([nm, elf], check=True, capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size-tool", default="arm-none-eabi-size")
    ap.add_argument("elf")
    args = ap.parse_args()
    nm = args.size_tool[: -len("size")] + "nm" if args.size_tool.endswith("size") else "arm-none-eabi-nm"
    s = symbols(args.elf, nm)

    flash = s["_eflash"]
    fast = s["__fastrun_end"] - s["__fastrun_start"]
    sram = s["_ebss"] - SRAM_BASE
    stack = s["_estack"] - s["_ebss"]
    sdram_bss = s["_esdram_doom"] - s["_ssdram_doom"]
    sdram_data = s["_esdram_data"] - s["_ssdram_data"]
    heap = s["_eheap_sdram"] - s["_sheap_sdram"]

    print(f"flash : {flash:8,d} B of {FLASH:,d} ({100 * flash / FLASH:.1f} %)")
    print(f"sram  : {sram:8,d} B of {SRAM_END - SRAM_BASE:,d} (fast code {fast:,d} B), stack room {stack:,d} B")
    print(f"sdram : Doom .data {sdram_data:,d} B, .bss {sdram_bss:,d} B, heap {heap / 1048576:.2f} MB")
    if flash > FLASH:
        print("size_report: image does not fit the flash", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
