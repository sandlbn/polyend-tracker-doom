#!/usr/bin/env python3
"""Folds hot_plugin.c output into functions of the bench-m4 ELF.

usage: hot_funcs.py <elf> <hot.txt> [--budget BYTES] [--nm arm-none-eabi-nm]

Prints the functions by executed M4 instructions (share of the run, code
size, instructions per byte), then a greedy pick by instructions per byte
that fits --budget bytes: the .fastrun candidates (docs/OPTIMIZATION_2X.md
item 6; the firmware's own sizes differ a little, see make -C reimpl)."""
import argparse
import bisect
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("hot")
    ap.add_argument("--budget", type=int, default=14000)
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--top", type=int, default=45)
    a = ap.parse_args()
    syms = []
    for line in subprocess.run([a.nm, "-S", "-C", "--defined-only", a.elf], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        p = line.split(None, 3)
        if len(p) < 4 or p[2] not in "tTwW":
            continue
        syms.append((int(p[0], 16) & ~1, int(p[1], 16), p[3]))
    syms.sort()
    starts = [s[0] for s in syms]
    per = {}
    total = 0
    for line in open(a.hot):
        st, _end, cnt, n = line.split()
        addr, ins = int(st, 16) & ~1, int(cnt) * int(n)
        total += ins
        i = bisect.bisect_right(starts, addr) - 1
        name = syms[i][2] if i >= 0 and addr < syms[i][0] + max(syms[i][1], 2) else "?"
        size = syms[i][1] if name != "?" else 0
        e = per.setdefault(name, [0, size])
        e[0] += ins
    rows = sorted(per.items(), key=lambda kv: -kv[1][0])
    print(f"{total} instructions executed")
    print(f"{'insns':>12} {'share':>6} {'bytes':>6} {'ins/B':>8}  function")
    for name, (ins, size) in rows[:a.top]:
        print(f"{ins:12d} {100.0 * ins / total:5.1f}% {size:6d} {ins / max(size, 1):8.0f}  {name[:110]}")
    pick, used, got = [], 0, 0
    for name, (ins, size) in sorted(per.items(), key=lambda kv: -kv[1][0] / max(kv[1][1], 1)):
        if name == "?" or size == 0 or used + size > a.budget:
            continue
        pick.append((name, size, ins))
        used += size
        got += ins
    print(f"\ngreedy pick for {a.budget} B: {used} B, {100.0 * got / total:.1f}% of the executed instructions")
    for name, size, ins in sorted(pick, key=lambda r: -r[2]):
        print(f"  {size:6d} B {100.0 * ins / total:5.1f}%  {name[:110]}")


if __name__ == "__main__":
    sys.exit(main())
