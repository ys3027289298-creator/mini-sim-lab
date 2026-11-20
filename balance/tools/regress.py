#!/usr/bin/env python3
"""Numeric regression for the deterministic balance sim.

Runs the balance binary over a fixed seed range, computes each seed's damage
distribution and win rate, and compares every item against a committed
baseline. An item is reported only when it drifts past its threshold; each
report names the seed, the item, the baseline value, the observed value and
the allowed delta, and a minimal repro (fewest duels) is written per seed.

Exit codes: 0 within thresholds, 1 regression found, 2 usage/io/baseline/seed
range/table error.

Guarantees:
  * the numeric table is only ever opened read-only and is hashed before and
    after the run; if its bytes change the run fails with exit 2.
  * baselines are hashed before/after every compare run and are only rewritten
    with --write-baseline.
  * repro files go to --repro-dir (a temp dir by default), never into the
    repository data directories.
"""
import argparse
import bisect
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

BASELINE_FORMAT = "balance-baseline/1"
REPRO_FORMAT = "balance-repro/1"
DEFAULT_SEEDS = "1:20"
MAX_SEEDS = 100000
UINT64_MAX = (1 << 64) - 1

# Per-item tolerance. `abs` is an absolute floor, `rel` scales with the
# baseline value; the allowed delta is max(abs, rel * |baseline|). Integer
# counters (wins, hits, ...) are stored in the baseline for context only and
# deliberately have no threshold.
DEFAULT_THRESHOLDS = {
    "win_rate":        {"abs": 0.02,  "rel": 0.0},
    "crit_rate":       {"abs": 0.01,  "rel": 0.05},
    "miss_rate":       {"abs": 0.01,  "rel": 0.10},
    "dmg_dealt_mean":  {"abs": 0.0,   "rel": 0.02},
    "dmg_dealt_p50":   {"abs": 0.0,   "rel": 0.03},
    "dmg_dealt_p90":   {"abs": 0.0,   "rel": 0.03},
    "dmg_dealt_p99":   {"abs": 0.0,   "rel": 0.05},
    "dmg_dealt_max":   {"abs": 0.0,   "rel": 0.05},
    "dmg_taken_mean":  {"abs": 0.0,   "rel": 0.02},
    "avg_rounds":      {"abs": 0.0,   "rel": 0.02},
    "hist_*_rate":     {"abs": 0.01,  "rel": 0.10},
    "hist_*_count":    {"abs": 2.0,   "rel": 0.10},
}
NON_METRIC_SCALARS = {"seed", "duels", "max_rounds", "hist_width", "hist_buckets"}
HIST_KINDS = ("_rate", "_count")

DUEL_RE = re.compile(r"^duel (\d+) rounds=(\d+) win=(\d+) dmg_dealt=(\d+) "
                     r"dmg_taken=(\d+) hits=(\d+) hits_taken=(\d+) crits=(\d+) "
                     r"misses=(\d+) rng=([0-9a-f]+)$")
DHIT_RE = re.compile(r"^dhit (\d+) (\d+)$")
SCALAR_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")


class RegressError(Exception):
    pass


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hash_tree(root: Path) -> dict:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = sha256_file(p)
    return out


def parse_seed_range(spec: str) -> list:
    """Accept 'A:B' (inclusive) or a single 'A'. Reject everything else."""
    parts = spec.split(":")
    if len(parts) == 1:
        lo = hi = parts[0]
    elif len(parts) == 2:
        lo, hi = parts
    else:
        raise RegressError(f"invalid seed range '{spec}': expected A:B")
    for s in (lo, hi):
        # str.isdigit() also accepts non-ASCII digits that int() would parse.
        if not re.fullmatch(r"[0-9]+", s):
            raise RegressError(f"invalid seed range '{spec}': seeds must be non-negative integers")
    a, b = int(lo), int(hi)
    if a > b:
        raise RegressError(f"invalid seed range '{spec}': start {a} > end {b}")
    if b > UINT64_MAX:
        raise RegressError(f"invalid seed range '{spec}': seed exceeds uint64")
    count = b - a + 1
    if count > MAX_SEEDS:
        raise RegressError(f"invalid seed range '{spec}': {count} seeds exceeds limit {MAX_SEEDS}")
    return list(range(a, b + 1))


def load_thresholds(path):
    th = dict(DEFAULT_THRESHOLDS)
    if path is None:
        return th
    try:
        raw = json.loads(Path(path).read_text())
    except OSError as e:
        raise RegressError(f"cannot read thresholds {path}: {e}")
    except json.JSONDecodeError as e:
        raise RegressError(f"malformed thresholds {path}: {e}")
    if not isinstance(raw, dict):
        raise RegressError(f"thresholds {path}: top level must be an object")
    known = set(DEFAULT_THRESHOLDS) | {"hist_*"}
    for key, val in raw.items():
        if key not in known:
            raise RegressError(f"thresholds {path}: unknown metric '{key}'")
        if not isinstance(val, dict):
            raise RegressError(f"thresholds {path}: '{key}' must be an object")
        for field in ("abs", "rel"):
            if field not in val:
                raise RegressError(f"thresholds {path}: '{key}' missing '{field}'")
            if not isinstance(val[field], (int, float)) or val[field] < 0:
                raise RegressError(f"thresholds {path}: '{key}.{field}' must be a non-negative number")
        if key == "hist_*":
            for kind in HIST_KINDS:
                th["hist_*" + kind] = {"abs": float(val["abs"]), "rel": float(val["rel"])}
        else:
            th[key] = {"abs": float(val["abs"]), "rel": float(val["rel"])}
    return th


def threshold_for(th, metric):
    if metric in th:
        return th[metric]
    if metric.startswith("hist_"):
        for suffix in HIST_KINDS:
            if metric.endswith(suffix) and f"hist_*{suffix}" in th:
                return th[f"hist_*{suffix}"]
    raise RegressError(f"no threshold configured for metric '{metric}'")


def known_threshold(th, metric):
    try:
        threshold_for(th, metric)
        return True
    except RegressError:
        return False


def allowed_delta(th, metric, base):
    t = threshold_for(th, metric)
    return max(t["abs"], t["rel"] * abs(base))


def run_seed(bin_path: Path, table: Path, seed: int, duels: int, max_rounds: int):
    proc = subprocess.run(
        [str(bin_path), "run", "--seed", str(seed), "--duels", str(duels),
         "--max-rounds", str(max_rounds), "--table", str(table)],
        capture_output=True, text=True)
    if proc.returncode != 0:
        msg = (proc.stderr or proc.stdout).strip()
        raise RegressError(f"balance run failed for seed {seed}: {msg}")
    scalars = {}
    duel_rows = []
    dealt_hits = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        m = DUEL_RE.match(line)
        if m:
            duel_rows.append({
                "rounds": int(m.group(2)), "win": int(m.group(3)),
                "dmg_dealt": int(m.group(4)), "dmg_taken": int(m.group(5)),
                "hits": int(m.group(6)), "hits_taken": int(m.group(7)),
                "crits": int(m.group(8)), "misses": int(m.group(9)),
                "rng": m.group(10),
            })
            continue
        m = DHIT_RE.match(line)
        if m:
            dealt_hits.append(int(m.group(2)))
            continue
        m = SCALAR_RE.match(line)
        if m:
            scalars[m.group(1)] = m.group(2)
    if len(duel_rows) != duels:
        raise RegressError(
            f"seed {seed}: binary reported {len(duel_rows)} duels, expected {duels}")
    for key in ("hist_width", "hist_buckets"):
        if key not in scalars:
            raise RegressError(f"seed {seed}: binary output missing '{key}'")
    if sum(d["hits"] for d in duel_rows) != len(dealt_hits):
        raise RegressError(
            f"seed {seed}: {len(dealt_hits)} damage samples do not match "
            f"{sum(d['hits'] for d in duel_rows)} landed hits")
    return scalars, duel_rows, dealt_hits


def hist_names(width, buckets):
    names = []
    for b in range(buckets):
        lo = b * width
        names.append(f"hist_{lo}_{lo + width - 1}")
    names.append(f"hist_{buckets * width}_plus")
    return names


def pct_index(n, p):
    if n == 0:
        return 0
    idx = (p * (n - 1) + 50) // 100
    return idx if idx < n else n - 1


def build_metrics(n, wins, hits_taken, crits, misses, dealt_sum, taken_sum,
                  total_rounds, sorted_hits, counts, width, buckets):
    hits = len(sorted_hits)
    attacks = hits + misses

    def ratio(num, den):
        return num / den if den else 0.0

    def pct(p):
        return float(sorted_hits[pct_index(len(sorted_hits), p)]) if sorted_hits else 0.0

    m = {
        "wins": wins,
        "win_rate": ratio(wins, n),
        "attacks": attacks,
        "hits": hits,
        "hits_taken": hits_taken,
        "crits": crits,
        "misses": misses,
        "crit_rate": ratio(crits, hits),
        "miss_rate": ratio(misses, attacks),
        "dmg_dealt_sum": dealt_sum,
        "dmg_dealt_mean": ratio(dealt_sum, hits),
        "dmg_dealt_p50": pct(50),
        "dmg_dealt_p90": pct(90),
        "dmg_dealt_p99": pct(99),
        "dmg_dealt_max": float(sorted_hits[-1]) if sorted_hits else 0.0,
        "dmg_taken_sum": taken_sum,
        "dmg_taken_mean": ratio(taken_sum, hits_taken),
        "total_rounds": total_rounds,
        "avg_rounds": ratio(total_rounds, n),
    }
    for i, name in enumerate(hist_names(width, buckets)):
        m[name + "_count"] = counts[i]
        m[name + "_rate"] = counts[i] / hits if hits else 0.0
    return m


def metrics_from(duel_rows, dealt_hits, width, buckets):
    counts = [0] * (buckets + 1)
    for v in dealt_hits:
        b = v // width
        b = 0 if b < 0 else (buckets if b > buckets else b)
        counts[b] += 1
    return build_metrics(
        len(duel_rows),
        sum(d["win"] for d in duel_rows),
        sum(d["hits_taken"] for d in duel_rows),
        sum(d["crits"] for d in duel_rows),
        sum(d["misses"] for d in duel_rows),
        sum(d["dmg_dealt"] for d in duel_rows),
        sum(d["dmg_taken"] for d in duel_rows),
        sum(d["rounds"] for d in duel_rows),
        sorted(dealt_hits), counts, width, buckets)


def prefix_series(duel_rows, dealt_hits, width, buckets):
    """Metrics for the first 1..N duels. Index d-1 is the first d duels."""
    series = []
    wins = hits_taken = crits = misses = 0
    dealt_sum = taken_sum = total_rounds = 0
    counts = [0] * (buckets + 1)
    sorted_hits = []
    pos = 0
    for i, d in enumerate(duel_rows):
        wins += d["win"]
        hits_taken += d["hits_taken"]
        crits += d["crits"]
        misses += d["misses"]
        dealt_sum += d["dmg_dealt"]
        taken_sum += d["dmg_taken"]
        total_rounds += d["rounds"]
        for v in dealt_hits[pos:pos + d["hits"]]:
            bisect.insort(sorted_hits, v)
            b = v // width
            counts[0 if b < 0 else (buckets if b > buckets else b)] += 1
        pos += d["hits"]
        series.append(build_metrics(
            i + 1, wins, hits_taken, crits, misses, dealt_sum, taken_sum,
            total_rounds, sorted_hits, counts, width, buckets))
    return series


def rows_to_duels(rows):
    return [{"rounds": r[0], "win": r[1], "dmg_dealt": r[2], "dmg_taken": r[3],
             "hits": r[4], "hits_taken": r[5], "crits": r[6],
             "misses": r[7], "rng": r[8]} for r in rows]


def cross_check(seed, scalars, computed):
    """The binary's printed scalars are authoritative: our recomputation from
    the per-duel/per-hit stream must agree exactly for the full match."""
    for key, val in scalars.items():
        if key in NON_METRIC_SCALARS:
            continue
        if key.startswith("hist_"):
            key = key + "_count"
        if key not in computed:
            raise RegressError(f"seed {seed}: binary printed unknown metric '{key}'")
        try:
            want = float(val)
        except ValueError:
            raise RegressError(f"seed {seed}: non-numeric value for '{key}': {val}")
        if abs(computed[key] - want) > 1e-6:
            raise RegressError(
                f"seed {seed}: metric '{key}' cross-check failed "
                f"(binary {want}, recomputed {computed[key]})")


def load_baseline(path: Path, table_sha: str):
    if not path.is_file():
        raise RegressError(f"baseline missing: {path}")
    try:
        doc = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise RegressError(f"malformed baseline {path}: {e}")
    if not isinstance(doc, dict) or doc.get("format") != BASELINE_FORMAT:
        got = doc.get("format") if isinstance(doc, dict) else type(doc).__name__
        raise RegressError(f"baseline {path}: unsupported format {got!r}")
    for key in ("duels", "max_rounds", "hist_width", "hist_buckets", "seeds"):
        if key not in doc:
            raise RegressError(f"baseline {path}: missing '{key}'")
    doc.setdefault("table_sha256", table_sha)
    if not isinstance(doc["seeds"], dict):
        raise RegressError(f"baseline {path}: 'seeds' must be an object")
    return doc


def offending(metric, base_val, live_val, th):
    """Return the excess (>=0) and the allowed delta; excess > 0 means drift."""
    allowed = allowed_delta(th, metric, base_val)
    return abs(live_val - base_val) - allowed, allowed


def compare_seed(seed, base_seed, live, th):
    base_metrics = base_seed["metrics"]
    violations = []
    for metric in base_metrics:
        if not known_threshold(th, metric):
            continue
        if metric not in live:
            raise RegressError(f"seed {seed}: baseline item '{metric}' not produced by binary")
        diff = live[metric] - base_metrics[metric]
        excess, allowed = offending(metric, base_metrics[metric], live[metric], th)
        if excess > 1e-12:
            violations.append({
                "seed": seed, "metric": metric,
                "baseline": base_metrics[metric], "live": live[metric],
                "diff": diff, "allowed": allowed,
                "excess": excess / allowed if allowed > 0 else float("inf"),
            })
    return violations


def first_violating_duels(live_series, base_series, metric, th):
    for i, (lv, bv) in enumerate(zip(live_series, base_series)):
        excess, _allowed = offending(metric, bv[metric], lv[metric], th)
        if excess > 1e-12:
            return i + 1
    return None


def cmd_compare(args, th):
    table = Path(args.table)
    baselines = Path(args.baselines)
    if not table.is_file():
        raise RegressError(f"numeric table missing: {table}")
    if not baselines.is_dir():
        raise RegressError(f"baselines directory missing: {baselines}")
    baseline_path = baselines / "baseline.json"

    seeds = parse_seed_range(args.seeds)
    table_sha_before = sha256_file(table)
    base_before = hash_tree(baselines)

    doc = load_baseline(baseline_path, table_sha_before)
    duels = doc["duels"]
    max_rounds = doc["max_rounds"]
    width = doc["hist_width"]
    buckets = doc["hist_buckets"]
    if args.duels is not None and args.duels != duels:
        raise RegressError(
            f"--duels {args.duels} does not match baseline duels {duels}; "
            "regenerate the baseline for a different match shape")
    if args.max_rounds is not None and args.max_rounds != max_rounds:
        raise RegressError(
            f"--max-rounds {args.max_rounds} does not match baseline {max_rounds}; "
            "regenerate the baseline for a different match shape")

    for seed in seeds:
        if str(seed) not in doc["seeds"]:
            raise RegressError(f"baseline missing for seed {seed} (range {args.seeds})")

    if doc["table_sha256"] != table_sha_before:
        print("note: numeric table differs from the one the baseline was taken "
              "with; this is expected when checking a balance change")

    repro_dir = Path(args.repro_dir) if args.repro_dir else None
    regressed = []
    total_items = 0
    for seed in seeds:
        base_seed = doc["seeds"][str(seed)]
        scalars, live_duels, live_hits = run_seed(
            Path(args.balance_bin), table, seed, duels, max_rounds)
        if int(scalars["hist_width"]) != width or int(scalars["hist_buckets"]) != buckets:
            raise RegressError(
                f"seed {seed}: table histogram shape changed "
                f"(baseline {width}x{buckets}, live {scalars['hist_width']}x"
                f"{scalars['hist_buckets']}); regenerate the baseline")
        computed = metrics_from(live_duels, live_hits, width, buckets)
        cross_check(seed, scalars, computed)

        violations = compare_seed(seed, base_seed, computed, th)
        if not violations:
            continue

        base_rows = rows_to_duels(base_seed["duels"])
        live_series = prefix_series(live_duels, live_hits, width, buckets)
        base_series = prefix_series(base_rows, base_seed["dealt_hits"], width, buckets)

        items = []
        for v in violations:
            fvd = first_violating_duels(live_series, base_series, v["metric"], th)
            v["first_violating_duels"] = fvd
            items.append(v)
        # Minimal repro: the smallest duel count at which any item still
        # drifts on its own.
        duel_param = min(v["first_violating_duels"] for v in items)
        at_param = sorted(v["metric"] for v in items
                          if v["first_violating_duels"] <= duel_param)

        if repro_dir is None:
            repro_dir = Path(tempfile.mkdtemp(prefix="balance-repro-"))
        path = write_repro(repro_dir, seed, duel_param, max_rounds, table,
                           table_sha_before, items, at_param)
        regressed.append(seed)
        total_items += len(items)
        print(f"seed {seed}: regression")
        for v in sorted(items, key=lambda x: -x["excess"]):
            print(f"  EXCEED {v['metric']}: baseline={v['baseline']:.6f} "
                  f"live={v['live']:.6f} diff={v['diff']:+.6f} "
                  f"allowed=±{v['allowed']:.6f} "
                  f"first_violating_duels={v['first_violating_duels']}")
        print(f"  repro: seed={seed} duels={duel_param} max_rounds={max_rounds} "
              f"table={table} -> {path}")

    table_sha_after = sha256_file(table)
    if table_sha_after != table_sha_before:
        raise RegressError("numeric table changed during regression")
    if hash_tree(baselines) != base_before:
        raise RegressError("baseline data changed during regression")

    if args.json:
        print(json.dumps({"regressed": regressed,
                          "seeds_checked": len(seeds),
                          "items_exceeded": total_items}))
    print(f"seeds={len(seeds)} checked={len(seeds) - len(regressed)} "
          f"regressed={len(regressed)} items_exceeded={total_items}")
    if regressed:
        print(f"repros in {repro_dir}")
        return 1
    print("result=ok")
    return 0


def write_repro(repro_dir: Path, seed, duels, max_rounds, table, table_sha,
                items, at_param):
    repro_dir.mkdir(parents=True, exist_ok=True)
    path = repro_dir / f"seed-{seed}.repro.json"
    doc = {
        "format": REPRO_FORMAT,
        "seed": seed,
        "duels": duels,
        "max_rounds": max_rounds,
        "table": str(table),
        "table_sha256": table_sha,
        "exceeds_at_duels": at_param,
        "items": [{
            "metric": v["metric"],
            "baseline": v["baseline"],
            "live": v["live"],
            "diff": v["diff"],
            "allowed": v["allowed"],
            "first_violating_duels": v["first_violating_duels"],
        } for v in items],
    }
    path.write_text(json.dumps(doc, indent=2) + "\n")
    return path


def cmd_verify_repro(args, th):
    repro = Path(args.verify_repro)
    try:
        doc = json.loads(repro.read_text())
    except OSError as e:
        raise RegressError(f"cannot read repro {repro}: {e}")
    except json.JSONDecodeError as e:
        raise RegressError(f"malformed repro {repro}: {e}")
    if doc.get("format") != REPRO_FORMAT:
        raise RegressError(f"repro {repro}: unsupported format")
    for key in ("seed", "duels", "max_rounds", "table_sha256", "items"):
        if key not in doc:
            raise RegressError(f"repro {repro}: missing '{key}'")
    seed, d, max_rounds = doc["seed"], doc["duels"], doc["max_rounds"]
    table = Path(args.table)
    if not table.is_file():
        raise RegressError(f"numeric table missing: {table}")
    if sha256_file(table) != doc["table_sha256"]:
        raise RegressError(
            f"repro {repro} was produced with a different numeric table; "
            "pass the table that reproduces the drift")

    baseline_path = Path(args.baselines) / "baseline.json"
    base_doc = load_baseline(baseline_path, sha256_file(table))
    if str(seed) not in base_doc["seeds"]:
        raise RegressError(f"baseline missing for seed {seed}")
    base_seed = base_doc["seeds"][str(seed)]
    width, buckets = base_doc["hist_width"], base_doc["hist_buckets"]
    if base_doc["max_rounds"] != max_rounds:
        raise RegressError(
            f"repro max_rounds {max_rounds} does not match baseline {base_doc['max_rounds']}")
    if d < 1 or d > base_doc["duels"]:
        raise RegressError(f"repro duels {d} outside baseline range 1..{base_doc['duels']}")

    scalars, rows, hits = run_seed(
        Path(args.balance_bin), table, seed, d, max_rounds)
    live_series = prefix_series(rows, hits, width, buckets)
    base_series = prefix_series(rows_to_duels(base_seed["duels"]),
                                base_seed["dealt_hits"], width, buckets)
    live = live_series[d - 1]
    base = base_series[d - 1]
    item_metrics = [it["metric"] for it in doc["items"]]

    problems = []
    over_here = [name for name in item_metrics
                 if offending(name, base[name], live[name], th)[0] > 1e-12]
    if not over_here:
        problems.append(f"no reported item exceeds its threshold at duels={d}")
    for name in doc.get("exceeds_at_duels", []):
        if name not in over_here:
            problems.append(f"{name} listed at duels={d} but no longer exceeds")
    if d > 1:
        prev_live, prev_base = live_series[d - 2], base_series[d - 2]
        early = [name for name in item_metrics
                 if offending(name, prev_base[name], prev_live[name], th)[0] > 1e-12]
        if early:
            problems.append(f"duels={d - 1} already exceeds ({', '.join(sorted(early))}); "
                            "repro is not minimal")

    for msg in problems:
        print(f"repro FAIL: {msg}")
    if problems:
        return 1
    print(f"repro ok: seed={seed} duels={d} max_rounds={max_rounds} "
          f"exceeds={','.join(sorted(over_here))}")
    return 0


def cmd_write_baseline(args, th):
    table = Path(args.table)
    if not table.is_file():
        raise RegressError(f"numeric table missing: {table}")
    seeds = parse_seed_range(args.seeds)
    table_sha_before = sha256_file(table)

    # With no explicit shape, let the table's defaults decide by probing the
    # binary with one seed.
    if args.duels is None or args.max_rounds is None:
        proc = subprocess.run(
            [args.balance_bin, "run", "--seed", str(seeds[0]), "--table", str(table)],
            capture_output=True, text=True)
        if proc.returncode != 0:
            raise RegressError(f"balance run failed while probing shape: "
                               f"{(proc.stderr or proc.stdout).strip()}")
        shape = dict(re.findall(r"^(duels|max_rounds)=(\d+)$",
                                proc.stdout, re.MULTILINE))
        duels = args.duels if args.duels is not None else int(shape["duels"])
        max_rounds = (args.max_rounds if args.max_rounds is not None
                      else int(shape["max_rounds"]))
    else:
        duels, max_rounds = args.duels, args.max_rounds

    out = {
        "format": BASELINE_FORMAT,
        "table_sha256": table_sha_before,
        "duels": duels,
        "max_rounds": max_rounds,
        "hist_width": None,
        "hist_buckets": None,
        "seeds": {},
    }
    for seed in seeds:
        scalars, rows, hits = run_seed(
            Path(args.balance_bin), table, seed, duels, max_rounds)
        width = int(scalars["hist_width"])
        buckets = int(scalars["hist_buckets"])
        if out["hist_width"] is None:
            out["hist_width"], out["hist_buckets"] = width, buckets
        elif (width, buckets) != (out["hist_width"], out["hist_buckets"]):
            raise RegressError(f"seed {seed}: inconsistent histogram shape")
        computed = metrics_from(rows, hits, width, buckets)
        cross_check(seed, scalars, computed)
        out["seeds"][str(seed)] = {
            "metrics": computed,
            "duels": [[r["rounds"], r["win"], r["dmg_dealt"], r["dmg_taken"],
                       r["hits"], r["hits_taken"], r["crits"], r["misses"],
                       r["rng"]] for r in rows],
            "dealt_hits": hits,
        }

    if sha256_file(table) != table_sha_before:
        raise RegressError("numeric table changed while writing baseline")
    baselines = Path(args.baselines)
    baselines.mkdir(parents=True, exist_ok=True)
    path = baselines / "baseline.json"
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(out, separators=(",", ":")) + "\n")
    tmp.replace(path)
    print(f"wrote baseline {path}: {len(seeds)} seeds, {duels} duels, "
          f"{max_rounds} max rounds")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--balance-bin", required=True)
    ap.add_argument("--table", required=True)
    ap.add_argument("--baselines", required=True)
    ap.add_argument("--seeds", default=DEFAULT_SEEDS,
                    help=f"inclusive seed range A:B (default {DEFAULT_SEEDS})")
    ap.add_argument("--duels", type=int, default=None)
    ap.add_argument("--max-rounds", type=int, default=None)
    ap.add_argument("--thresholds", default=None)
    ap.add_argument("--repro-dir", default=None)
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--verify-repro", default=None, metavar="FILE")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    try:
        th = load_thresholds(args.thresholds)
        # Path("./balance") stringifies to "balance", which subprocess would
        # look up on PATH; pin the binary to an absolute path instead.
        args.balance_bin = str(Path(args.balance_bin).resolve())
        if args.write_baseline and args.verify_repro:
            raise RegressError("--write-baseline and --verify-repro are mutually exclusive")
        if args.verify_repro:
            return cmd_verify_repro(args, th)
        if args.write_baseline:
            return cmd_write_baseline(args, th)
        return cmd_compare(args, th)
    except RegressError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
