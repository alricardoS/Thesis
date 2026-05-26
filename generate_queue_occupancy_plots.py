#!/usr/bin/env python3
"""
Generate queue occupancy plots from MAC queue sampling CSVs.

One PNG is generated per run_label (experience). Each plot shows queue occupancy
in packets over time, sampled every 100 ms by the simulation.
"""

import re
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


def sanitize_filename(text: str) -> str:
    text = str(text).strip().replace("|", "_").replace("+", "plus")
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text)


def parse_priority_classes(output_file: Path) -> dict[str, list[str]]:
    priority_to_stas: dict[str, list[str]] = {}
    if not output_file.exists():
        return priority_to_stas

    pattern = re.compile(r"STA_PRIORITY_CLASS:\s*Sta=(\d+)\s+Class=(\d+)\s+AC=([A-Z]+)\s+TOS=(\d+)")
    with output_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = pattern.search(line)
            if not match:
                continue
            sta_id = match.group(1)
            ac = match.group(3)
            priority_to_stas.setdefault(ac, []).append(f"STA{sta_id}")

    for ac in priority_to_stas:
        priority_to_stas[ac].sort()

    return priority_to_stas


def plot_ap_queue_series(ax, run_df: pd.DataFrame, priority_to_stas: dict[str, list[str]]) -> int:
    """Plot logical per-STA queue lines by expanding AP AC queues to STA labels."""
    ap_df = run_df[run_df["role"] == "AP"].copy()
    if ap_df.empty:
        return 0

    plotted = 0
    for ac, ac_df in ap_df.groupby("ac"):
        ac_df = ac_df.sort_values("time_s")
        if ac_df["packets"].max() <= 0:
            continue

        sta_labels = priority_to_stas.get(ac, [])
        if not sta_labels:
            sta_labels = [ac]

        for sta_label in sta_labels:
            ax.plot(
                ac_df["time_s"],
                ac_df["packets"],
                linewidth=1.8,
                marker="o",
                markersize=2.5,
                label=f"{sta_label}-{ac}",
            )
            plotted += 1

    return plotted


def find_matching_output_file(csv_path: Path, run_label: str) -> Path | None:
    outputs_dir = csv_path.parent
    if "|" not in run_label:
        return None

    # Handle format: [prefix]|freq_pair|protocol or freq_pair|protocol
    parts = run_label.split("|")
    
    # If 3+ parts, last 2 are freq_pair and protocol; first part(s) are prefix (ignore)
    if len(parts) >= 3:
        freq_pair = parts[-2].strip()  # e.g., "2+5" or "2GHz"
        proto = parts[-1].strip()      # e.g., "UDP"
    elif len(parts) == 2:
        freq_pair = parts[0].strip()
        proto = parts[1].strip()
    else:
        return None

    # Try to match the output file
    if "+" in freq_pair:
        freq1, freq2 = [part.strip() for part in freq_pair.split("+", 1)]
        candidates = sorted(outputs_dir.glob(f"mlo_multi_sta_{freq1}_{freq2}_{proto}_*.txt"))
    else:
        freq_match = re.match(r"^(\d+)", freq_pair)
        if not freq_match:
            return None
        freq = freq_match.group(1)
        candidates = sorted(outputs_dir.glob(f"single_multi_sta_{freq}_{proto}_*.txt"))

    return candidates[0] if candidates else None


def generate_queue_occupancy_plots(csv_file: str, out_dir: str, title_prefix: str = "Queue Occupancy") -> bool:
    csv_path = Path(csv_file)
    out_path = Path(out_dir)
    out_path.mkdir(parents=True, exist_ok=True)

    try:
        df = pd.read_csv(csv_path)
    except Exception as exc:
        print(f"Error loading CSV: {exc}")
        return False

    required = {"run_label", "time_s", "role", "node_id", "ac", "packets"}
    missing = required - set(df.columns)
    if missing:
        print(f"Error: missing columns in {csv_file}: {', '.join(sorted(missing))}")
        return False

    df["time_s"] = pd.to_numeric(df["time_s"], errors="coerce")
    df["packets"] = pd.to_numeric(df["packets"], errors="coerce").fillna(0)
    df["node_id"] = pd.to_numeric(df["node_id"], errors="coerce").fillna(-1).astype(int)
    if "link_id" in df.columns:
        df["link_id"] = pd.to_numeric(df["link_id"], errors="coerce")

    for run_label, run_df in df.groupby("run_label"):
        if run_df.empty:
            continue

        output_file = find_matching_output_file(csv_path, run_label)
        priority_to_stas = parse_priority_classes(output_file) if output_file else {}

        fig, ax = plt.subplots(figsize=(13, 6.5))
        plotted = plot_ap_queue_series(ax, run_df, priority_to_stas)

        if plotted == 0:
            run_df = run_df.sort_values(["role", "node_id", "ac", "time_s"])
            series_df = run_df.groupby(["role", "node_id", "ac"], as_index=False).agg(
                max_packets=("packets", "max")
            )
            active_series = {
                (row.role, row.node_id, row.ac)
                for row in series_df.itertuples(index=False)
                if row.max_packets > 0
            }

            for (role, node_id, ac), series in run_df.groupby(["role", "node_id", "ac"]):
                if (role, node_id, ac) not in active_series:
                    continue

                series = series.sort_values("time_s")
                label = f"STA{node_id}-{ac}" if role == "STA" else f"{role}{node_id}-{ac}"

                ax.plot(
                    series["time_s"],
                    series["packets"],
                    linewidth=1.8,
                    marker="o",
                    markersize=2.5,
                    label=label,
                )
                plotted += 1

        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Queue occupancy (packets)")
        ax.set_title(f"{title_prefix} - {run_label}")
        ax.grid(True, alpha=0.3)

        if plotted > 0:
            ax.legend(loc="upper left", bbox_to_anchor=(1.02, 1.0), fontsize=8)
        else:
            ax.text(
                0.5,
                0.5,
                "No queue samples with packets > 0",
                ha="center",
                va="center",
                transform=ax.transAxes,
            )

        plt.tight_layout()
        out_file = out_path / f"{sanitize_filename(title_prefix)}_{sanitize_filename(run_label)}.png"
        plt.savefig(out_file, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"Queue occupancy plot generated: {out_file}")

    return True


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: python3 generate_queue_occupancy_plots.py <csv> <out_dir> [title_prefix]")
        sys.exit(1)

    csv_file = sys.argv[1]
    out_dir = sys.argv[2]
    title_prefix = sys.argv[3] if len(sys.argv) > 3 else "Queue Occupancy"

    sys.exit(0 if generate_queue_occupancy_plots(csv_file, out_dir, title_prefix) else 1)


if __name__ == "__main__":
    main()
