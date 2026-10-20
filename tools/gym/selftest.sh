#!/bin/sh
# Play the gym self-test twice and check that lock-step mode is exact and
# reproducible: every step mixes exactly its share of audio, and two runs write
# the same bytes (audio, per-step state, frames).
#
# Usage: tools/gym/selftest.sh OUTDIR [FPS] [STEPS]
# Needs a display: on a headless Linux machine, run it under xvfb-run.
set -eu

OUT=${1:?usage: tools/gym/selftest.sh OUTDIR [FPS] [STEPS]}
FPS=${2:-60}
STEPS=${3:-900}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
case "$(uname)" in
  Darwin) BIN="$ROOT/ITGmania.app/Contents/MacOS/ITGmania" ;;
  *) BIN="$ROOT/itgmania" ;;
esac

run() {
  rm -rf "$OUT/$1"
  mkdir -p "$OUT/$1/data"
  ITGMANIA_GYM=1 "$BIN" --gym-selftest="$STEPS" --gym-selftest-dir="$OUT/$1/out" \
    --gym-selftest-frame-every=60 --gym-fps="$FPS" --gym-screensize=854x480 \
    --gym-seed=1 --gym-datadir="$OUT/$1/data" \
    "--gym-song=StepMania 5/MechaTribe Assault" --gym-steps-type=dance-single \
    --gym-difficulty=Medium --gym-mods=M250,clap,failoff --gym-game-mode=ITG
}

run a
run b

python3 - "$OUT" "$FPS" "$STEPS" <<'EOF'
import csv, hashlib, os, sys

out, fps, steps = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

def digests(run):
    root = os.path.join(out, run, "out")
    result = {}
    for folder, _, files in os.walk(root):
        for name in files:
            path = os.path.join(folder, name)
            with open(path, "rb") as f:
                result[os.path.relpath(path, root)] = hashlib.sha256(f.read()).hexdigest()
    return result

a, b = digests("a"), digests("b")
if a != b:
    sys.exit("the two runs differ: %s" % sorted(k for k in a if a.get(k) != b.get(k)))

with open(os.path.join(out, "a", "out", "audio.wav"), "rb") as f:
    head = f.read(44)
frames = int.from_bytes(head[40:44], "little") // 4
expected = (steps + 1) * 48000 // fps
if frames != expected:
    sys.exit("audio has %d frames, expected %d" % (frames, expected))

rows = list(csv.DictReader(open(os.path.join(out, "a", "out", "steps.csv"))))
seconds = [float(r["music_seconds"]) for r in rows]
span = seconds[-1] - seconds[0]
if abs(span - steps / fps) > 1e-3:
    sys.exit("music advanced %.6f s over %d steps, expected %.6f" % (span, steps, steps / fps))

print("selftest ok: %d files identical across runs, %d audio frames, %.3f s of music"
      % (len(a), frames, span))
EOF
