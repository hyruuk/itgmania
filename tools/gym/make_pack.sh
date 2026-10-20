#!/bin/sh
# Lay out the game pack that itgmania-gym downloads: the built executable and
# the folders it reads at run time, arranged as in a checkout (ITGmania runs in
# place), with the CC-NC songs of Songs/StepMania 5.
#
# Usage: tools/gym/make_pack.sh OUTDIR   (after a Release build)
set -eu

OUT=${1:?usage: tools/gym/make_pack.sh OUTDIR}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

rm -rf "$OUT"
mkdir -p "$OUT/Songs"
case "$(uname)" in
  Darwin) cp -R "$ROOT/ITGmania.app" "$OUT/" ;;
  *) cp "$ROOT/itgmania" "$OUT/" ;;
esac
# The folders upstream's own install puts next to the executable, minus Docs.
for dir in Announcers BGAnimations BackgroundEffects BackgroundTransitions \
    Characters Courses Data NoteSkins Scripts Themes; do
  rsync -a --exclude .git "$ROOT/$dir" "$OUT/"
done
rsync -a "$ROOT/Songs/StepMania 5" "$OUT/Songs/"
cp "$ROOT/LICENSE" "$OUT/LICENSE"
cp "$ROOT/Docs/Licenses.txt" "$OUT/Licenses.txt"
printf '%s\n' \
  "ITGmania with gym mode, for itgmania-gym: https://github.com/hyruuk/itgmania" \
  "The game is GPL-3.0-or-later (LICENSE); Licenses.txt lists its libraries." \
  "Songs/StepMania 5 is licensed CC-NC; Simply Love is GPL-3.0-or-later." \
  > "$OUT/README.txt"
