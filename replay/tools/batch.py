#!/usr/bin/env python3
"""Batch replay verification: verify every recording against its baseline
and summarize the failure rate.

Usage: batch.py --replay-bin PATH --recordings DIR --baselines DIR
                [--repro-dir DIR]

Exit 0 if every recording matches, 1 if any diverged or errored, 2 on
usage/io errors. Baselines are hashed before and after the run; if any
sample data changed on disk the run is aborted with an error.
"""
import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path


def hash_tree(root: Path) -> dict:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay-bin", required=True)
    ap.add_argument("--recordings", required=True, type=Path)
    ap.add_argument("--baselines", required=True, type=Path)
    ap.add_argument("--repro-dir", type=Path, default=None,
                    help="where to write minimal repros (default: a temp dir)")
    args = ap.parse_args()

    recs = sorted(args.recordings.glob("*.rec"))
    if not recs:
        print(f"error: no .rec files in {args.recordings}", file=sys.stderr)
        return 2

    repro_dir = args.repro_dir or Path(tempfile.mkdtemp(prefix="replay-repro-"))
    repro_dir.mkdir(parents=True, exist_ok=True)

    before = hash_tree(args.baselines)

    passed, diverged, errored = [], [], []
    for rec in recs:
        base = args.baselines / (rec.stem + ".traj")
        if not base.exists():
            errored.append((rec.stem, f"missing baseline {base.name}"))
            continue
        repro = repro_dir / (rec.stem + ".repro.rec")
        proc = subprocess.run(
            [args.replay_bin, "verify", str(rec), str(base), str(repro)],
            capture_output=True, text=True)
        if proc.returncode == 0:
            passed.append(rec.stem)
            repro.unlink(missing_ok=True)  # no divergence -> no repro needed
        elif proc.returncode == 1:
            diverged.append((rec.stem, proc.stdout.strip().splitlines()[0] if proc.stdout else ""))
        else:
            errored.append((rec.stem, (proc.stderr or proc.stdout).strip()))

    after = hash_tree(args.baselines)
    if before != after:
        print("error: baseline sample data changed during verification", file=sys.stderr)
        return 2

    total = len(recs)
    failed = len(diverged) + len(errored)
    print(f"total={total} pass={len(passed)} diverge={len(diverged)} error={len(errored)}")
    print(f"failure_rate={failed / total:.3f}")
    for name, msg in diverged:
        print(f"  DIVERGE {name}: {msg}")
    for name, msg in errored:
        print(f"  ERROR   {name}: {msg}")
    if diverged:
        print(f"repros in {repro_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
