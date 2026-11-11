#!/usr/bin/env python3
"""Summarize (and optionally plot) a latency histogram CSV written by the benchmarks.

Usage:
    python3 scripts/latency_report.py build/pipeline_latency.csv
    python3 scripts/latency_report.py build/pipeline_latency.csv --plot latency.png

The CSV has columns bucket_ns,count where each bucket is 10 ns wide.
"""

import argparse
import csv
import sys

BUCKET_NS = 10
PERCENTILES = [50, 90, 99, 99.9, 99.99]


def load(path):
    buckets, counts = [], []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            buckets.append(int(row["bucket_ns"]))
            counts.append(int(row["count"]))
    return buckets, counts


def percentile(buckets, counts, p):
    total = sum(counts)
    target = int(p / 100 * (total - 1)) + 1
    seen = 0
    for b, c in zip(buckets, counts):
        seen += c
        if seen >= target:
            return b + BUCKET_NS
    return buckets[-1] + BUCKET_NS


OVERFLOW_NS = 1_000_000  # the C++ histogram lumps everything >= 1 ms into one bucket


def fmt(ns):
    if ns > OVERFLOW_NS:
        return ">= 1 ms"
    return f"{ns / 1000:.2f} us" if ns >= 1000 else f"{ns} ns"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--plot", metavar="PNG", help="write a log-scale CDF plot (needs matplotlib)")
    args = ap.parse_args()

    buckets, counts = load(args.csv)
    if not counts:
        sys.exit("empty histogram")
    total = sum(counts)
    mean = sum((b + BUCKET_NS / 2) * c for b, c in zip(buckets, counts)) / total

    print(f"{args.csv}: {total:,} samples, mean {fmt(int(mean))}")
    for p in PERCENTILES:
        print(f"  p{p:<6} {fmt(percentile(buckets, counts, p))}")

    if args.plot:
        try:
            import matplotlib.pyplot as plt
        except ImportError:
            sys.exit("matplotlib is not installed: pip install matplotlib")
        cum, xs, ys = 0, [], []
        for b, c in zip(buckets, counts):
            cum += c
            xs.append((b + BUCKET_NS) / 1000)
            ys.append(cum / total * 100)
        fig, ax = plt.subplots(figsize=(7, 4))
        ax.plot(xs, ys)
        ax.set_xscale("log")
        ax.set_xlabel("latency (us, log scale)")
        ax.set_ylabel("% of messages at or below")
        ax.set_title("Apex latency CDF")
        ax.grid(True, which="both", alpha=0.3)
        fig.tight_layout()
        fig.savefig(args.plot, dpi=150)
        print(f"wrote {args.plot}")


if __name__ == "__main__":
    main()
