#!/usr/bin/env bash
# verify.sh — 行为保持证据的一键复现脚本
# 用法: ./verify.sh
# 退出码 0 表示全部检查通过；所有哈希/对比均实时计算，不引用预存结论。
set -u
cd "$(dirname "$0")"
B=$PWD
PASS=0; FAIL=0
ok()   { echo "  PASS $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL $1"; FAIL=$((FAIL+1)); }

echo "== 1. 三模块：clean build（-Wall -Wextra 零告警）+ 3 次运行逐位对比基线 =="
for d in fixedstep pool rng; do
    err=$(cd "$B/$d" && make -s clean && make -s 2>&1 >/dev/null)
    [ -z "$err" ] && ok "$d build zero-warning" || { bad "$d build warnings"; echo "$err"; }
    for i in 1 2 3; do
        (cd "$B/$d" && "./$d") > "/tmp/verify-$d-$i.txt"
        h=$(sha256sum "/tmp/verify-$d-$i.txt" | cut -d' ' -f1)
        if cmp -s "/tmp/verify-$d-$i.txt" "$B/artifacts/baseline/$d.txt"; then
            ok "$d run$i bit-identical to baseline ($h)"
        else bad "$d run$i differs"; fi
    done
done

echo "== 2. simlab：线程数 x 重复运行，完整输出 sha256 =="
(cd "$B/simlab" && make -s clean && make -s) || exit 1
ref=""
for t in 1 2 4 8; do
    uniq_hashes=""
    for i in 1 2 3; do
        (cd "$B/simlab" && ./simlab -t "$t") > "/tmp/verify-sim-$t-$i.txt"
        h=$(sha256sum "/tmp/verify-sim-$t-$i.txt" | cut -d' ' -f1)
        uniq_hashes="$uniq_hashes $h"
    done
    n=$(echo "$uniq_hashes" | tr ' ' '\n' | grep -v '^$' | sort -u | wc -l)
    h0=$(echo "$uniq_hashes" | tr ' ' '\n' | grep -v '^$' | head -1)
    [ "$n" = "1" ] && ok "simlab -t $t: 3 runs one hash ($h0)" || bad "simlab -t $t hash unstable"
    [ -z "$ref" ] && ref=$h0
    cmp -s "/tmp/verify-sim-$t-1.txt" "/tmp/verify-sim-1-1.txt" \
        && ok "simlab -t $t output == -t 1 output" || bad "simlab -t $t output differs from -t 1"
done
echo "  reference sha256: $ref"

echo "== 3. 调度压力：100 次随机线程数（1..8）= ="
m=0
for i in $(seq 1 100); do
    t=$(( (RANDOM % 8) + 1 ))
    h=$((cd "$B/simlab" && ./simlab -t "$t") | sha256sum | cut -d' ' -f1)
    [ "$h" = "$ref" ] || { bad "stress iter $i t=$t mismatch"; m=1; }
done
[ "$m" = "0" ] && ok "100/100 stress runs identical"

echo "== 4. 外部调用者兼容：旧 API 名称/签名/结构体字段编译并运行 =="
tmpd=$(mktemp -d)
cat > "$tmpd/caller.c" <<'EOF'
#include <stdio.h>
#include "fixedstep.h"
#include "pool.h"
#include "rng.h"
int main(void) {
    Clock c; clock_init(&c, 0.01, 8);
    int s = clock_advance(&c, 0.025);
    long long ts = c.total_steps; double al = c.alpha; int dr = c.dropped;

    Pool p; pool_init(&p);
    Handle h = pool_alloc(&p, 7);
    int v = 0; pool_get(&p, h, &v);
    int pv = pool_valid(&p, h), used = p.used, hw = p.high_water, cap = POOL_CAP;
    pool_free(&p, h);

    Stream r; stream_init(&r, 42);
    uint64_t x = stream_next(&r);
    uint32_t u = stream_range(&r, 6);
    Stream f = stream_fork(&r, 1);
    printf("%d %lld %.1f %d %d %d %d %d %llu %u %llu\n",
           s, ts, al, dr, pv, used, hw, cap,
           (unsigned long long)x, u, (unsigned long long)f.state);
    return 0;
}
EOF
gcc -O2 -std=c11 -Wall -Wextra -Werror \
    -Ifixedstep/include -Ipool/include -Irng/include \
    "$tmpd/caller.c" fixedstep/src/fixedstep.c pool/src/pool.c rng/src/rng.c \
    -o "$tmpd/caller" -lm && ok "external caller compiles with -Werror against new headers" \
    || bad "external caller compile failed"
"$tmpd/caller" > /tmp/verify-caller.txt && ok "external caller runs: $(cat /tmp/verify-caller.txt)"
rm -rf "$tmpd"

echo "== 5. 实现文件可独立编译（无外部依赖，仅相对包含本模块头）=="
for pair in "fixedstep fixedstep" "pool pool" "rng rng"; do
    set -- $pair
    gcc -O2 -std=c11 -Wall -Wextra -Werror -c "$B/$1/src/$2.c" -o "/tmp/$2.o" \
        && ok "$1/src/$2.c compiles standalone" || bad "$1 standalone compile failed"
done

echo "== 6. 关闭改动：HEAD 临时 worktree 输出与基线一致 =="
wt=/tmp/verify-revert-$$
if git -C "$B" worktree add --detach "$wt" HEAD >/dev/null 2>&1; then
    for d in fixedstep pool rng; do
        (cd "$wt/$d" && make -s 2>/dev/null && "./$d") > "/tmp/verify-rev-$d.txt"
        cmp -s "/tmp/verify-rev-$d.txt" "$B/artifacts/baseline/$d.txt" \
            && ok "reverted $d bit-identical to baseline" || bad "reverted $d differs"
    done
    git -C "$B" worktree remove --force "$wt"
else
    bad "could not create revert worktree"
fi

echo
echo "RESULT pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ]
