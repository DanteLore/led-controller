#!/usr/bin/env bash
# Builds the beat tracker on this Mac and runs it on synthetic tracks with
# known beats, checking tempo and timing, then on any recordings with tapped
# beats in test-audio/ (made with the editor page's "Record for tuning"). With audio files as arguments, also
# runs those (anything afconvert reads: mp3, m4a, wav...) and writes a copy of
# each with clicks on the detected beats, to listen to.
set -e
cd "$(dirname "$0")"
OUT="${OUT:-$(mktemp -d)}"
cc -std=c11 -O2 -Wall -Wextra -Werror -I.. ../beat.c beats.c -lm -o "$OUT/beats"
python3 make_tracks.py "$OUT"
for track in "$OUT"/*.beats; do
    name="${track%.beats}"
    "$OUT/beats" "$name.wav" > "$name.out"
    python3 score.py "$name.out" "$track" "$(basename "$name")"
done
# Real recordings with tapped beats, from the editor page's "Record for tuning"
for wav in ../../../test-audio/*.wav; do
    [ -e "$wav" ] || continue
    taps="${wav%.wav}.taps.json"
    [ -e "$taps" ] || continue
    name="$(basename "${wav%.wav}")"
    "$OUT/beats" "$wav" > "$OUT/$name.out"
    python3 score_taps.py "$OUT/$name.out" "$taps" "$name"
done
for file in "$@"; do
    base="$OUT/$(basename "${file%.*}")"
    afconvert -f WAVE -d LEI16@22050 -c 1 "$file" "$base.wav"
    "$OUT/beats" "$base.wav" "$base-clicks.wav" > "$base.out"
    echo "$(basename "$file"): $(grep '^time' "$base.out" | tail -1)   clicks: $base-clicks.wav"
done
