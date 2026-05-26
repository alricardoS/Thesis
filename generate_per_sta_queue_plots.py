#!/usr/bin/env python3
"""Generate per-STA queue/backlog plots from the per-STA metrics CSVs.

This script plots the per-STA backlog metric produced by the per-STA test
variants, one PNG per run_label.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


def sanitize_filename(text: str) -> str:
    text = str(text).strip().replace("|", "_").replace("+", "plus")
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text)


def format_label(sta_id: int, ac: str) -> str:
    return f"STA{sta_id}-{ac}"


def generate_per_sta_plots(csv_file: str, out_dir: str, title_prefix: str = "Per-STA Queue Occupancy") -> bool:
    csv_path = Path(csv_file)
    out_path = Path(out_dir)
    out_path.mkdir(parents=True, exist_ok=True)

    try:
        df = pd.read_csv(csv_path)
    except Exception as exc:
        print(f"Error loading CSV: {exc}")
        return False

    required = {"run_label", "time_s", "sta_id", "ac", "backlog_packets"}
    missing = required - set(df.columns)
    if missing:
        print(f"Error: missing columns in {csv_file}: {', '.join(sorted(missing))}")
        return False

    df["time_s"] = pd.to_numeric(df["time_s"], errors="coerce")
    df["sta_id"] = pd.to_numeric(df["sta_id"], errors="coerce").fillna(-1).astype(int)
    df["backlog_packets"] = pd.to_numeric(df["backlog_packets"], errors="coerce").fillna(0)

    for run_label, run_df in df.groupby("run_label"):
        if run_df.empty:
            continue

        fig, ax = plt.subplots(figsize=(13, 6.5))
        plotted = 0

        for (sta_id, ac), series in run_df.groupby(["sta_id", "ac"]):
            series = series.sort_values("time_s")
            if series["backlog_packets"].max() <= 0:
                continue

            ax.plot(
                series["time_s"],
                series["backlog_packets"],
                linewidth=1.8,
                marker="o",
                markersize=2.5,
                label=format_label(int(sta_id), str(ac)),
            )
            plotted += 1

        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Backlog (packets)")
        ax.set_title(f"{title_prefix} - {run_label}")
        ax.grid(True, alpha=0.3)

        if plotted > 0:
            ax.legend(loc="upper left", bbox_to_anchor=(1.02, 1.0), fontsize=8)
        else:
            ax.text(
                0.5,
                0.5,
                "No per-STA backlog samples with packets > 0",
                ha="center",
                va="center",
                transform=ax.transAxes,
            )

        plt.tight_layout()
        out_file = out_path / f"{sanitize_filename(title_prefix)}_{sanitize_filename(run_label)}.png"
        plt.savefig(out_file, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"Per-STA queue plot generated: {out_file}")

    return True


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: python3 generate_per_sta_queue_plots.py <csv> <out_dir> [title_prefix]")
        sys.exit(1)

    csv_file = sys.argv[1]
    out_dir = sys.argv[2]
    title_prefix = sys.argv[3] if len(sys.argv) > 3 else "Per-STA Queue Occupancy"

    sys.exit(0 if generate_per_sta_plots(csv_file, out_dir, title_prefix) else 1)


if __name__ == "__main__":
    main()
