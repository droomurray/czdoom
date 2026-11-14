#!/bin/sh
# Usage: tests/smoke.sh <czdoom-binary> <output-dir>
#
# Runs the game off-device: a plain file stands in for the 320x170 RGB565
# panel and a file of raw evdev events stands in for the keyboard. Writes
# PNG screenshots and fails if the picture is blank or the keys had no effect.
set -eu

BIN="$(realpath "$1")"
OUT="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
WAD=/usr/share/games/doom/freedoom1.wad
WORK="$(mktemp -d)"
mkdir -p "$OUT"

export SDL_AUDIODRIVER=dummy CZDOOM_FAKEFB=320x170x16

run() { # name frames keys... -- engine args
    name="$1"; frames="$2"; shift 2
    keys=""
    while [ "$1" != "--" ]; do keys="$keys $1"; shift; done
    shift
    python3 "$HERE/evkeys.py" "$WORK/$name.ev" $keys
    : >"$WORK/$name.fb"
    ( cd "$WORK" && CZDOOM_FB="$WORK/$name.fb" CZDOOM_KBD="$WORK/$name.ev" CZDOOM_MAX_FRAMES="$frames" \
        "$BIN" -iwad "$WAD" "$@" >"$OUT/$name.log" 2>&1 ) || { cat "$OUT/$name.log"; exit 1; }
    python3 "$HERE/fb2png.py" "$WORK/$name.fb" 320 170 "$OUT/$name.png"
}

# 1. title screen
run title 60 -- 
# 2. first level, no input
run level-idle 120 -- -warp 1 1 -skill 2 -nomonsters
# 3. first level, holding D (turn right): the view must differ from run 2
run level-turn 120 32 -- -warp 1 1 -skill 2 -nomonsters
# 4. first level, holding K (fire): the weapon must differ from run 2
run level-fire 120 37 -- -warp 1 1 -skill 2 -nomonsters
# 5. 4:3 mode
CZDOOM_ASPECT=4:3 run aspect43 120 -- -warp 1 1 -skill 2 -nomonsters

python3 "$HERE/check.py" "$WORK"
grep -h "^czdoom:" "$OUT/level-turn.log"
echo "smoke test passed"
