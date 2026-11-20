#!/bin/sh
# Self-test for the replay harness. Everything that writes goes to a tmp dir;
# the committed sample data (recordings/, baselines/) must be byte-identical
# before and after the run.
set -u
cd "$(dirname "$0")"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0

ok()   { printf 'ok   %s\n' "$1"; }
bad()  { printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); }

sample_hash() { find recordings baselines -type f -exec sha256sum {} + | sort | sha256sum; }
before=$(sample_hash)

# flip one direction bit of the input at frame N of a recording (guaranteed
# to change the trajectory, unlike overwriting with a fixed value)
flip_input() { # <file> <frame>
  python3 - "$1" "$2" <<'EOF'
import sys
path, target = sys.argv[1], sys.argv[2]
lines = open(path).read().splitlines(keepends=True)
for i, ln in enumerate(lines):
    p = ln.split()
    if len(p) == 6 and p[0] == "frame" and p[1] == target:
        p[5] = format(int(p[5], 16) ^ 1, "x")
        lines[i] = " ".join(p) + "\n"
        break
else:
    sys.exit("frame not found")
open(path, "w").writelines(lines)
EOF
}

# 1. round trip: gen -> record -> verify matches
./replay gen 11 300 "$T/a.rec" \
  && ./replay record "$T/a.rec" "$T/a.traj" >/dev/null \
  && ./replay verify "$T/a.rec" "$T/a.traj" >"$T/v.out"
if [ $? -eq 0 ] && grep -q '^match 300 frames' "$T/v.out"; then
  ok "roundtrip"
else
  bad "roundtrip"
fi

# 2. corrupted input diverges, repro is truncated to the first diverging frame
cp "$T/a.rec" "$T/b.rec"
flip_input "$T/b.rec" 150
./replay verify "$T/b.rec" "$T/a.traj" "$T/b.repro.rec" >"$T/v2.out"
rc=$?
div_frame=$(sed -n 's/^diverge frame \([0-9]*\).*/\1/p' "$T/v2.out")
repro_frames=$(grep -c '^frame ' "$T/b.repro.rec" 2>/dev/null || echo 0)
if [ "$rc" -eq 1 ] && [ -n "$div_frame" ] && [ "$repro_frames" -eq $((div_frame + 1)) ]; then
  ok "diverge+repro (frame $div_frame, repro $repro_frames frames)"
else
  bad "diverge+repro (rc=$rc frame=$div_frame repro=$repro_frames)"
fi
# the repro itself still reproduces the divergence at the same frame
./replay verify "$T/b.repro.rec" "$T/a.traj" >"$T/v3.out" 2>/dev/null
rc=$?
div2=$(sed -n 's/^diverge frame \([0-9]*\).*/\1/p' "$T/v3.out")
if [ "$rc" -eq 1 ] && [ "$div2" = "$div_frame" ]; then
  ok "repro-reproduces"
else
  bad "repro-reproduces (rc=$rc frame=$div2 want=$div_frame)"
fi

# 3. missing frame is an error, not a divergence
grep -v '^frame 100 ' "$T/a.rec" > "$T/missing.rec"
./replay verify "$T/missing.rec" "$T/a.traj" >"$T/v4.out" 2>"$T/e4.out"
if [ $? -eq 2 ] && grep -q 'missing frame' "$T/e4.out"; then
  ok "missing-frame"
else
  bad "missing-frame"
fi

# 4. backwards timestamp is an error
sed -E 's/^(frame 200 t )[0-9.]+/\10.000001/' "$T/a.rec" > "$T/back.rec"
./replay verify "$T/back.rec" "$T/a.traj" >/dev/null 2>"$T/e5.out"
if [ $? -eq 2 ] && grep -q 'backwards' "$T/e5.out"; then
  ok "backward-timestamp"
else
  bad "backward-timestamp"
fi

# 5. batch over committed samples: all pass, failure rate 0
python3 tools/batch.py --replay-bin ./replay \
  --recordings recordings --baselines baselines --repro-dir "$T/repro" >"$T/b1.out"
if [ $? -eq 0 ] && grep -q '^failure_rate=0.000' "$T/b1.out"; then
  ok "batch-clean"
else
  bad "batch-clean"
fi

# 6. batch with one corrupted recording: failure rate 1/4, repro written
mkdir -p "$T/recs"
cp recordings/*.rec "$T/recs/"
flip_input "$T/recs/run2.rec" 60
python3 tools/batch.py --replay-bin ./replay \
  --recordings "$T/recs" --baselines baselines --repro-dir "$T/repro2" >"$T/b2.out"
rc=$?
if [ "$rc" -eq 1 ] && grep -q '^failure_rate=0.250' "$T/b2.out" \
  && [ -f "$T/repro2/run2.repro.rec" ] && [ ! -f "$T/repro2/run1.repro.rec" ]; then
  ok "batch-failure-rate"
else
  bad "batch-failure-rate (rc=$rc)"
  cat "$T/b2.out"
fi

# 7. sample data untouched
after=$(sample_hash)
if [ "$before" = "$after" ]; then
  ok "samples-untouched"
else
  bad "samples-untouched"
fi

printf 'all_ok=%d\n' "$([ "$fails" -eq 0 ] && echo 1 || echo 0)"
[ "$fails" -eq 0 ]
