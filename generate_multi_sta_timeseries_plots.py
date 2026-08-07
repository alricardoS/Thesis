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
# Alargada até 12 STAs (mantém as 4 primeiras cores originais).
STA_COLORS = ['#E74C3C', '#3498DB', '#2ECC71', '#F39C12',
              '#9B59B6', '#1ABC9C', '#E67E22', '#34495E',
              '#16A085', '#C0392B', '#2980B9', '#8E44AD']

# Estilo de linha por AC — distingue as apps quando um STA muda de AC a meio (switch).
AC_LINESTYLE = {"VO": "-", "VI": "--", "BE": ":", "BK": "-."}

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


def flow_label(sta_id: int, ac: str) -> str:
    return f"STA{sta_id} ({ac})"


def detect_switch_times(df: pd.DataFrame):
    """Instante real do switch de AC (≈ simTime/2). Detetado como o 1º instante em que
    o AC "depois" fica ativo (throughput>1), menos meio intervalo de amostragem — porque
    a amostra que contém o switch já apanha a app nova a meio dessa janela.
    (Ex.: BE aparece em t=9 → switch = 9 − 0.5 = 8.5.)"""
    times = set()
    ts = sorted(df["time_s"].dropna().unique())
    dt = 1.0
    if len(ts) >= 2:
        diffs = sorted(ts[i + 1] - ts[i] for i in range(len(ts) - 1))
        dt = float(diffs[len(diffs) // 2])  # mediana dos intervalos
    for sta_id, sdf in df.groupby("sta_id"):
        if sdf["ac"].nunique() < 2:
            continue
        first_active = {}
        for ac, g in sdf.groupby("ac"):
            act = g[g["throughput_mbps"] > 1.0]["time_s"]
            if not act.empty:
                first_active[ac] = float(act.min())
        if len(first_active) < 2:
            continue
        after_t = sorted(first_active.values())[1]  # o AC que fica ativo mais tarde
        times.add(after_t - dt / 2.0)
    return sorted(times)


def make_figure(df: pd.DataFrame, column: str, metric_title: str, ylabel: str,
                scenario_tag: str, protocol: str, out_path: Path) -> None:
    pairs = [p for p in FREQ_PAIRS if p in set(df["freq_pair"])]
    if not pairs:
        pairs = sorted(set(df["freq_pair"].dropna()))
    if not pairs:
        print(f"[timeseries] No frequency pairs found, skipping {out_path.name}")
        return

    sta_ids = sorted(df["sta_id"].unique())
    sta_color = {sid: STA_COLORS[i % len(STA_COLORS)] for i, sid in enumerate(sta_ids)}
    switch_times = detect_switch_times(df)

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
        # Uma linha por (STA, AC): cor por STA, estilo por AC → distingue o switch.
        for (sta_id, ac), g in pair_df.groupby(["sta_id", "ac"]):
            g = g.sort_values("time_s")
            if g.empty:
                continue
            ls = AC_LINESTYLE.get(str(ac).strip().upper(), "-")
            line, = ax.plot(g["time_s"], g[column],
                            color=sta_color.get(sta_id, "#555555"),
                            linestyle=ls, linewidth=1.6, marker="o", markersize=2.5,
                            label=flow_label(sta_id, ac))
            if line.get_label() not in labels:
                handles.append(line)
                labels.append(line.get_label())

        # Marcar o(s) instante(s) de switch de AC.
        for st in switch_times:
            ax.axvline(st, color="black", linestyle=(0, (4, 3)), linewidth=1.2, alpha=0.7)
            ax.text(st, ax.get_ylim()[1], " switch", color="black", fontsize=8,
                    va="top", ha="left", alpha=0.8)

        ax.set_title(FREQ_PAIR_LABELS.get(pair, pair), fontsize=11, fontweight="bold",
                     loc="left")
        ax.set_ylabel(ylabel, fontsize=9)
        ax.grid(True, alpha=0.3, linestyle="--")
        ax.margins(x=0.01)

    axes[-1].set_xlabel("Simulation time (s)", fontsize=10)

    if handles:
        fig.legend(handles, labels, loc="lower center", ncol=min(len(labels), 6),
                   fontsize=8, frameon=True, bbox_to_anchor=(0.5, 0.0))

    fig.suptitle(f"Per-STA {metric_title} over Time — {protocol} {scenario_tag}",
                 fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0, 0.07, 1, 0.95])
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
