#!/usr/bin/env python3
"""合并 scan_coverage.sh 产出的每关覆盖率表，找出种子里被证伪的 green。

用法：
  python3 dm/android/tools/coverage_scan_report.py --seed dm/gfx/nvn/pass-coverage.tsv \
      --scan coverage-scan
  # 加 --apply 直接把被证伪的 green 改成 red（保守：samples 取最大，uncovered 取最坏一次）
"""
import argparse
import glob
import os
import sys
import tempfile
from pathlib import Path


def parse_row(line):
    f = line.strip().split("\t")
    if len(f) != 9 or f[0] == "pk" or f[0].startswith("#"):
        return None
    try:
        if len(f[0]) != 16:
            return None
        key = (f"{int(f[0], 16):016x}", *(int(x) for x in f[1:5]))
        samples, uncovered, total = map(int, f[6:9])
        if min(key[1], key[2], samples, total) <= 0 or key[4] not in (0, 1) or not 0 <= uncovered <= total:
            return None
        return key, {"verdict": "green" if f[5].lower() == "green" and not uncovered else "red",
                     "samples": samples, "uncovered": uncovered, "total": total}
    except ValueError:
        return None


def read_rows(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as source:
        for line in source:
            row = parse_row(line)
            if row:
                key, value = row
                old = rows.get(key)
                if old:
                    value["verdict"] = "red" if "red" in (old["verdict"], value["verdict"]) else "green"
                    value["samples"] = max(old["samples"], value["samples"])
                    if old["uncovered"] > value["uncovered"]:
                        value["uncovered"], value["total"] = old["uncovered"], old["total"]
                rows[key] = value
    return rows


def merge(scan_dir):
    master = {}
    for path in sorted(glob.glob(os.path.join(scan_dir, "lv*.tsv"))):
        lv = os.path.basename(path)[2:-4]
        for key, r in read_rows(path).items():
            m = master.setdefault(key, {"verdict": "green", "samples": 0, "uncovered": 0, "total": 0, "levels": []})
            # 每张图的表都从种子合并过，样本数不能求和；取最大，未覆盖取最坏一次。
            m["samples"] = max(m["samples"], r["samples"])
            if r["uncovered"] > m["uncovered"]:
                m["uncovered"] = r["uncovered"]
                m["total"] = r["total"]
            m["levels"].append(lv)
            if r["verdict"] == "red" or r["uncovered"] > 0:
                m["verdict"] = "red"
    return master


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", default="dm/gfx/nvn/pass-coverage.tsv")
    ap.add_argument("--scan", default="coverage-scan")
    ap.add_argument("--apply", action="store_true", help="把被证伪的 green 直接改成 red")
    args = ap.parse_args()

    seed = read_rows(args.seed)
    master = merge(args.scan)
    bad = {}
    for key, s in seed.items():
        if s["verdict"] != "green":
            continue
        m = master.get(key)
        if m and m["verdict"] == "red":
            bad[key] = m

    print(f"seed rows: {len(seed)}  merged keys: {len(master)}  contradicted: {len(bad)}")
    for key, m in bad.items():
        print(f"  {key[0]} {key[1]}x{key[2]} fmt={key[3]} d={key[4]} "
              f"uncovered={m['uncovered']}/{m['total']} levels={','.join(m['levels'])}")

    if args.apply and bad:
        lines = open(args.seed, encoding="utf-8").read().splitlines(True)
        out, changed = [], 0
        for line in lines:
            f = line.rstrip("\n").split("\t")
            row = parse_row(line)
            if row:
                key, _ = row
                m = bad.get(key)
                if m:
                    old_unc, old_tot = int(f[7]), int(f[8])
                    f[5] = "red"
                    f[6] = str(max(int(f[6]), m["samples"]))
                    f[7] = str(max(old_unc, m["uncovered"]))
                    f[8] = str(m["total"] if m["uncovered"] >= old_unc else old_tot)
                    line = "\t".join(f) + "\n"
                    changed += 1
            out.append(line)
        seed_path = Path(args.seed).resolve()
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="", dir=seed_path.parent,
                                             prefix=seed_path.name + ".", suffix=".tmp", delete=False) as dest:
                temporary = dest.name
                dest.writelines(out)
                dest.flush()
                os.fsync(dest.fileno())
            os.replace(temporary, seed_path)
        finally:
            if temporary and os.path.exists(temporary):
                os.unlink(temporary)
        print(f"updated {changed} seed row(s)")
    elif bad:
        print("re-run with --apply to write the red verdicts into the seed")


if __name__ == "__main__":
    sys.exit(main())
