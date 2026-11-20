#!/bin/sh
# End-to-end checks for the regression driver beyond the C self-test:
# clean run, real drift detection, minimal repro, and that the numeric table
# and committed baseline are byte-identical before and after.
set -u
cd "$(dirname "$0")"
BIN=./balance
TABLE=data/numbers.txt
B="python3 -I tools/regress.py --balance-bin $BIN --table"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
ok()  { printf 'ok   %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); }

hash_all() { { sha256sum "$TABLE"; find baselines -type f -exec sha256sum {} + | sort; } | sha256sum; }
before=$(hash_all)

# 1. clean regression over the committed baseline
python3 -I tools/regress.py --balance-bin "$BIN" --table "$TABLE" \
  --baselines baselines --seeds 1:5 >"$T/clean.out" 2>&1
if [ $? -eq 0 ] && grep -q '^result=ok' "$T/clean.out"; then
  ok "clean-run"
else
  bad "clean-run"; cat "$T/clean.out"
fi

# 2. a real numeric change is detected, naming seed and item
cp "$TABLE" "$T/numbers.txt"
sed -i 's/^crit_permille 150$/crit_permille 400/' "$T/numbers.txt"
python3 -I tools/regress.py --balance-bin "$BIN" --table "$T/numbers.txt" \
  --baselines baselines --seeds 1:2 --repro-dir "$T/repro" >"$T/drift.out" 2>&1
rc=$?
if [ "$rc" -eq 1 ] && grep -q '^seed 1: regression' "$T/drift.out" \
   && grep -q 'EXCEED crit_rate' "$T/drift.out" \
   && [ -f "$T/repro/seed-1.repro.json" ]; then
  ok "drift-detected-and-reproduced"
else
  bad "drift-detected-and-reproduced (rc=$rc)"; cat "$T/drift.out"
fi

# 3. the minimal repro reproduces the drift with its own table
python3 -I tools/regress.py --balance-bin "$BIN" --table "$T/numbers.txt" \
  --baselines baselines --verify-repro "$T/repro/seed-1.repro.json" >"$T/ver.out" 2>&1
if [ $? -eq 0 ] && grep -q '^repro ok' "$T/ver.out"; then
  ok "repro-minimal"
else
  bad "repro-minimal"; cat "$T/ver.out"
fi

# 4. the repro refuses the wrong table
python3 -I tools/regress.py --balance-bin "$BIN" --table "$TABLE" \
  --baselines baselines --verify-repro "$T/repro/seed-1.repro.json" >/dev/null 2>&1
if [ $? -eq 2 ]; then ok "repro-table-guard"; else bad "repro-table-guard"; fi

# 5. illegal seed ranges are errors, not silent skips
for s in "5:1" "a:b" "1:2:3"; do
  python3 -I tools/regress.py --balance-bin "$BIN" --table "$TABLE" \
    --baselines baselines --seeds "$s" >/dev/null 2>&1
  [ $? -eq 2 ] || bad "seed-range '$s' not rejected"
done
ok "seed-range-validation"

# 6. a seed the baseline does not cover is an error
python3 -I tools/regress.py --balance-bin "$BIN" --table "$TABLE" \
  --baselines baselines --seeds 1:21 >/dev/null 2>&1
if [ $? -eq 2 ]; then ok "seed-outside-baseline"; else bad "seed-outside-baseline"; fi

# 7. table and baseline untouched by the whole run
if [ "$before" = "$(hash_all)" ]; then
  ok "table-and-baseline-untouched"
else
  bad "table-and-baseline-untouched"
fi

printf 'all_ok=%d\n' "$([ "$fails" -eq 0 ] && echo 1 || echo 0)"
[ "$fails" -eq 0 ]
