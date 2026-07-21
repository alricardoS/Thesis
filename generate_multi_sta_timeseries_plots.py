#!/usr/bin/env python3
"""
generate_multi_sta_timeseries_plots.py

Gera gráficos de série temporal por STA a partir do CSV de métricas por segundo
produzido pela simulação (--perStaMetricsCsv).

Um PNG por métrica (throughput, delay, jitter, packet loss). Cada figura tem 3
subplots empilhados, um por par de frequências (2.4+5, 2.4+6, 5+6), e cada
subplot tem uma linha por STA, com as mesmas cores usadas em
generate_multi_sta_individual_plots_priority.py.

Uso: generate_multi_sta_timeseries_plots.py <csv> <out_dir> <scenario_tag> [outputs_dir]
"""

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

# Mesma paleta dos gráficos individuais por STA, para a legenda coincidir.
STA_COLORS = ['#E74C3C', '#3498DB', '#2ECC71', '#F39C12']  # Red, Blue, Green, Orange

FREQ_PAIRS = ["2+5", "2+6", "5+6"]
FREQ_PAIR_LABELS = {"2+5": "2.4+5 GHz", "2+6": "2.4+6 GHz", "5+6": "5+6 GHz"}

METRICS = [
    ("throughput_mbps", "throughput", "Throughput", "Throughput (Mbps)"),
    ("delay_ms", "delay", "Delay", "Delay (ms)"),
    ("jitter_ms", "jitter", "Jitter", "Jitter (ms)"),
    ("loss_pct", "loss", "Packet Loss", "Packet loss (%)"),
]


def parse_run_label(label: str):
    """'<scenario>|<traffic>|2+5|UDP' -> ('2+5', 'UDP')."""
    parts = [p for p in str(label).split("|")]
    if len(parts) < 2:
        return None, None
    return parts[-2].strip(), parts[-1].strip()


def load(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    required = {"run_label", "time_s", "sta_id", "ac",
                "throughput_mbps", "delay_ms", "jitter_ms", "loss_pct"}
    missing = required - set(df.columns)
    if missing:
        raise SystemExit(f"[timeseries] Missing columns in {csv_path}: {sorted(missing)}")

    parsed = df["run_label"].apply(parse_run_label)
    df["freq_pair"] = [p[0] for p in parsed]
    df["protocol"] = [p[1] for p in parsed]
    df["sta_id"] = df["sta_id"].astype(int)
    return df


def sta_label(sta_id: int, df: pd.DataFrame) -> str:
    acs = df.loc[df["sta_id"] == sta_id, "ac"].dropna().unique()
    ac = str(acs[0]).strip() if len(acs) else "?"
    return f"STA{sta_id} ({ac})"


def make_figure(df: pd.DataFrame, column: str, metric_title: str, ylabel: str,
                scenario_tag: str, protocol: str, out_path: Path) -> None:
    pairs = [p for p in FREQ_PAIRS if p in set(df["freq_pair"])]
    if not pairs:
        pairs = sorted(set(df["freq_pair"].dropna()))
    if not pairs:
        print(f"[timeseries] No frequency pairs found, skipping {out_path.name}")
        return

    sta_ids = sorted(df["sta_id"].unique())

    fig, axes = plt.subplots(len(pairs), 1, figsize=(12, 3.2 * len(pairs)),
                             sharex=True, sharey=False)
    if len(pairs) == 1:
        axes = [axes]

    handles, labels = [], []
    for ax, pair in zip(axes, pairs):
        pair_df = df[df["freq_pair"] == pair]
        if pair_df.empty:
            ax.text(0.5, 0.5, "no data", ha="center", va="center",
                    transform=ax.transAxes, color="gray")
        for idx, sta_id in enumerate(sta_ids):
            sta_df = pair_df[pair_df["sta_id"] == sta_id].sort_values("time_s")
            if sta_df.empty:
                continue
            line, = ax.plot(sta_df["time_s"], sta_df[column],
                            color=STA_COLORS[idx % len(STA_COLORS)],
                            linewidth=1.6, marker="o", markersize=2.5,
                            label=sta_label(sta_id, df))
            if line.get_label() not in labels:
                handles.append(line)
                labels.append(line.get_label())

        ax.set_title(FREQ_PAIR_LABELS.get(pair, pair), fontsize=11, fontweight="bold",
                     loc="left")
        ax.set_ylabel(ylabel, fontsize=9)
        ax.grid(True, alpha=0.3, linestyle="--")
        ax.margins(x=0.01)

    axes[-1].set_xlabel("Simulation time (s)", fontsize=10)

    if handles:
        fig.legend(handles, labels, loc="upper right", ncol=len(labels),
                   fontsize=9, frameon=True, bbox_to_anchor=(0.99, 0.985))

    fig.suptitle(f"Per-STA {metric_title} over Time — {protocol} {scenario_tag}",
                 fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"[timeseries] Generated: {out_path}")


def main() -> None:
    if len(sys.argv) < 4:
        raise SystemExit(
            "Usage: generate_multi_sta_timeseries_plots.py <csv> <out_dir> <scenario_tag> [outputs_dir]")

    csv_path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    scenario_tag = sys.argv[3]

    if not csv_path.exists():
        raise SystemExit(f"[timeseries] CSV not found: {csv_path}")

    df = load(csv_path)
    if df.empty:
        print(f"[timeseries] CSV is empty, nothing to plot: {csv_path}")
        return

    out_dir.mkdir(parents=True, exist_ok=True)

    protocols = [p for p in df["protocol"].dropna().unique()]
    for protocol in protocols:
        proto_df = df[df["protocol"] == protocol]
        for column, prefix, metric_title, ylabel in METRICS:
            out_path = out_dir / f"mlo_timeseries_{prefix}_{protocol.lower()}_{scenario_tag}.png"
            make_figure(proto_df, column, metric_title, ylabel,
                        scenario_tag, protocol, out_path)


if __name__ == "__main__":
    main()
