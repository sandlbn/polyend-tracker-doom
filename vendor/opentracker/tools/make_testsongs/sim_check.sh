#!/bin/sh
# Opens every generated test song in the headless simulator and plays it.
#   sim_check.sh <opentracker-sim> <testsongs-sd-root> <work-dir>
# Each project gets its own card laid out like the stock-project fixture
# (only Projects/<name>/, booted to an empty Workspace). The script opens it
# through File -> Open Project (the firmware's own Open job), checks that the
# job ended without errors or a popup and that the project is current, then
# plays the song from the Song screen and expects sound.
set -eu
SIM=$1
SRC=$2
WORK=$3
rm -rf "$WORK"
mkdir -p "$WORK"
n=0
fail=0
for dir in "$SRC"/Projects/*/; do
    [ -f "$dir/project.mt" ] || continue
    name=$(basename "$dir")
    n=$((n + 1))
    card="$WORK/$n/card"
    mkdir -p "$card/Projects"
    cp -R "$dir" "$card/Projects/$name"
    cat > "$WORK/$n/script.txt" <<EOF
# Open "$name" (File, F2 Open Project, the only project, F8 Open).
wait 300
tap FILE
tap F2
wait 300
tap DOWN
tap F8
wait 6000
expect-open-ok
expect-project $name
screenshot $WORK/$n/opened.png
# Play the song from the Song screen.
tap SONG
wait 200
sound-reset
tap PLAY
wait 4000
expect-playing
expect-sound 1000
tap PATTERN
wait 1000
expect-playing
screenshot $WORK/$n/playing.png
tap PLAY
wait 200
EOF
    if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$SIM" --sd "$card" --script "$WORK/$n/script.txt" \
        > "$WORK/$n/sim.log" 2>&1; then
        echo "sim: opened and played \"$name\""
    else
        echo "sim: FAILED \"$name\" (log: $WORK/$n/sim.log)"
        sed 's/^/  /' "$WORK/$n/sim.log" | tail -5
        fail=1
    fi
done
if [ "$n" -eq 0 ]; then
    echo "sim: no test songs in $SRC/Projects"
    exit 1
fi
exit $fail
