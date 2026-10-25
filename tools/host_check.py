#!/usr/bin/env python3
"""polyend-doom host checks: scripted runs of build/host/doom-host on a WAD.

    host_check.py build/host/doom-host doom1.wad tests/scripts

  - the attract demo runs 1000 frames without an error, and sends MIDI;
  - idle.txt: a new game by the Tracker controls reaches E1M1 and the player
    stays at the start;
  - new_game.txt: arrow keys, Shift and the encoder move the player;
  - jog.txt: encoder push held + encoder turns walk the player forward;
  - save_name.txt: the on-screen keyboard (pads, encoder) names a savegame.
"""
import os
import re
import shutil
import subprocess
import sys

START = (1056, -3616)   # E1M1 player 1 start


def run(exe, wad, *args):
    r = subprocess.run([exe, "--headless", *args, "-iwad", wad], capture_output=True, text=True, timeout=300)
    out = r.stdout + r.stderr
    if r.returncode != 0:
        raise SystemExit(f"host_check: {' '.join(args)}: exit {r.returncode}\n{out[-2000:]}")
    stats = re.search(r"host: (\d+) frames in \d+ ms, (\d+) MIDI bytes, error \"(.*)\"", out)
    hud = re.search(r"host: in level (\d), health (\d+), weapon slot (\d), position (-?\d+) (-?\d+)", out)
    if not stats or not hud:
        raise SystemExit(f"host_check: no summary\n{out[-2000:]}")
    return {
        "frames": int(stats[1]), "midi": int(stats[2]), "error": stats[3],
        "inLevel": hud[1] == "1", "pos": (int(hud[4]), int(hud[5])),
    }


def main():
    exe, wad, scripts = sys.argv[1:4]
    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    demo = run(exe, wad, "--frames", "1000", "--shot", "check.ppm")
    check(demo["frames"] == 1000 and not demo["error"], "attract demo: 1000 frames, no error")
    check(demo["midi"] > 0, f"attract demo: music sent as MIDI ({demo['midi']} bytes)")

    idle = run(exe, wad, "--frames", "400", "--script", f"{scripts}/idle.txt")
    check(idle["inLevel"] and idle["pos"] == START, f"idle: in E1M1 at the start {idle['pos']}")

    keys = run(exe, wad, "--frames", "400", "--script", f"{scripts}/new_game.txt")
    moved = abs(keys["pos"][0] - START[0]) + abs(keys["pos"][1] - START[1])
    check(keys["inLevel"] and moved > 200, f"keys: player moved {moved} units")

    jog = run(exe, wad, "--frames", "400", "--script", f"{scripts}/jog.txt")
    forward = jog["pos"][1] - START[1]   # E1M1 starts facing north (+y)
    check(jog["inLevel"] and forward > 300, f"jog: player walked {forward} units forward")

    shutil.rmtree(".savegame", ignore_errors=True)
    run(exe, wad, "--frames", "360", "--script", f"{scripts}/save_name.txt")
    name = b""
    if os.path.exists(".savegame/doomsav0.dsg"):
        with open(".savegame/doomsav0.dsg", "rb") as f:
            name = f.read(24).split(b"\0")[0]
    check(name == b"DOOMN", f"save name typed on the keyboard: {name!r}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
