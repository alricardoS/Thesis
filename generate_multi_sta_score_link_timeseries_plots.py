#!/usr/bin/env python3
"""
generate_multi_sta_score_link_timeseries_plots.py

Gera dois gráficos de série temporal a partir do CSV de estado por-fluxo emitido
pelo scheduler (--schedulerStateCsv):

  1. SCORE do link atual over time, por (STA, AC)   -> mlo_timeseries_score_*.png
  2. LINK utilizado over time, por (STA, AC)        -> mlo_timeseries_link_*.png

Reutiliza o estilo e a lógica de deteção/trim de switch de
generate_multi_sta_timeseries_plots.py (mesma paleta por STA, estilo por AC,
subplots por par de frequências, marcador de switch).

O trim pós-switch (e o corte de "caudas" obsoletas — o scheduler mantém a
entrada de um AC no seu mapa mesmo depois de a app parar) usa as JANELAS ATIVAS
derivadas do CSV per-STA de métricas (throughput>1), passado como 4º argumento.

Uso: generate_multi_sta_score_link_timeseries_plots.py <state_csv> <out_dir> <tag> [per_sta_csv]
"""

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

import generate_multi_sta_timeseries_plots as ts

STA_COLORS = ts.STA_COLORS
AC_LINESTYLE = ts.AC_LINESTYLE
FREQ_PAIRS = ts.FREQ_PAIRS
FREQ_PAIR_LABELS = ts.FREQ_PAIR_LABELS
flow_label = ts.flow_label

FREQ_LABEL = {2: "2.4 GHz", 5: "5 GHz", 6: "6 GHz"}


def load_state(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    required = {"run_label", "time_s", "sta_id", "ac", "link_id", "link_freq", "score"}
    missing = required - set(df.columns)
    if missing:
        raise SystemExit(f"[score/link] Missing columns in {csv_path}: {sorted(missing)}")
    parsed = df["run_label"].apply(ts.parse_run_label)
    df["freq_pair"] = [p[0] for p in parsed]
    df["protocol"] = [p[1] for p in parsed]
    df["sta_id"] = df["sta_id"].astype(int)
    df["link_freq"] = pd.to_numeric(df["link_freq"], errors="coerce").fillna(0).astype(int)
    df["score"] = pd.to_numeric(df["score"], errors="coerce")
    return df


def active_windows(persta_df: pd.DataFrame):
    """(freq_pair, sta_id, ac) -> (first, last) dos instantes com throughput>1."""
    win = {}
    if persta_df is None or persta_df.empty:
        return win
    for (fp, sta, ac), g in persta_df.groupby(["freq_pair", "sta_id", "ac"]):
        act = g[g["throughput_mbps"] > 1.0]["time_s"]
        if not act.empty:
            win[(str(fp), int(sta), str(ac))] = (float(act.min()), float(act.max()))
    return win


def make_figure(state_df, persta_df, value_col, kind, tag, protocol, out_path):
    pairs = [p for p in FREQ_PAIRS if p in set(state_df["freq_pair"])]
    if not pairs:
        pairs = sorted(set(state_df["freq_pair"].dropna()))
    if not pairs:
        print(f"[score/link] No frequency pairs found, skipping {out_path.name}")
        return

    sta_ids = sorted(state_df["sta_id"].unique())
    sta_color = {sid: STA_COLORS[i % len(STA_COLORS)] for i, sid in enumerate(sta_ids)}
    win = active_windows(persta_df)
    dt = ts._median_dt(state_df)
    trail = 1.5 * dt
    switch_times = ts.detect_switch_times(ts.detect_switch_info(persta_df)) if persta_df is not None else []

    fig, axes = plt.subplots(len(pairs), 1, figsize=(12, 3.2 * len(pairs)),
                             sharex=True, sharey=(kind == "link"))
    if len(pairs) == 1:
        axes = [axes]

    all_freqs = sorted(state_df["link_freq"].unique())
    handles, labels = [], []
    for ax, pair in zip(axes, pairs):
        pair_df = state_df[state_df["freq_pair"] == pair]
        if pair_df.empty:
            ax.text(0.5, 0.5, "no data", ha="center", va="center",
                    transform=ax.transAxes, color="gray")
        for (sta_id, ac), g in pair_df.groupby(["sta_id", "ac"]):
            g = g.sort_values("time_s")
            # Trim à janela ativa (corta caudas obsoletas + resolve o switch).
            w = win.get((pair, sta_id, str(ac)))
            if w is not None:
                g = g[(g["time_s"] >= w[0] - dt) & (g["time_s"] <= w[1] + trail)]
            elif win:  # há janelas conhecidas mas esta (sta,ac) nunca teve tráfego → salta
                continue
            if g.empty:
                continue
            ls = AC_LINESTYLE.get(str(ac).strip().upper(), "-")
            line, = ax.plot(g["time_s"], g[value_col],
                            color=sta_color.get(sta_id, "#555555"),
                            linestyle=ls, linewidth=1.6,
                            marker="o", markersize=2.5,
                            drawstyle=("steps-post" if kind == "link" else "default"),
                            label=flow_label(sta_id, ac))
            if line.get_label() not in labels:
                handles.append(line)
                labels.append(line.get_label())

        for st in switch_times:
            ax.axvline(st, color="black", linestyle=(0, (4, 3)), linewidth=1.2, alpha=0.7)
            ax.text(st, ax.get_ylim()[1], " switch", color="black", fontsize=8,
                    va="top", ha="left", alpha=0.8)

        ax.set_title(FREQ_PAIR_LABELS.get(pair, pair), fontsize=11, fontweight="bold", loc="left")
        ax.grid(True, alpha=0.3, linestyle="--")
        ax.margins(x=0.01)
        if kind == "score":
            ax.set_ylabel("Score (satisfação)", fontsize=9)
            ax.set_ylim(-0.02, 1.05)
        else:
            ax.set_ylabel("Link", fontsize=9)
            ax.set_yticks(all_freqs)
            ax.set_yticklabels([FREQ_LABEL.get(f, f"{f} GHz") for f in all_freqs])
            ax.set_ylim(min(all_freqs) - 0.5, max(all_freqs) + 0.5)

    axes[-1].set_xlabel("Simulation time (s)", fontsize=10)
    if handles:
        fig.legend(handles, labels, loc="lower center", ncol=min(len(labels), 6),
                   fontsize=8, frameon=True, bbox_to_anchor=(0.5, 0.0))

    title = "Current-link Score" if kind == "score" else "Link in use"
    fig.suptitle(f"Per-(STA,AC) {title} over Time — {protocol} {tag}",
                 fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0, 0.07, 1, 0.95])
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"[score/link] Generated: {out_path}")


def main() -> None:
    if len(sys.argv) < 4:
        raise SystemExit(
            "Usage: generate_multi_sta_score_link_timeseries_plots.py <state_csv> <out_dir> <tag> [per_sta_csv]")

    state_path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    tag = sys.argv[3]
    persta_path = Path(sys.argv[4]) if len(sys.argv) > 4 else None

    if not state_path.exists():
        print(f"[score/link] State CSV not found, skipping: {state_path}")
        return

    state_df = load_state(state_path)
    if state_df.empty:
        print(f"[score/link] State CSV empty, nothing to plot: {state_path}")
        return

    persta_df = None
    if persta_path is not None and persta_path.exists():
        try:
            persta_df = ts.load(persta_path)
        except SystemExit:
            persta_df = None

    out_dir.mkdir(parents=True, exist_ok=True)
    for protocol in [p for p in state_df["protocol"].dropna().unique()]:
        pdf = state_df[state_df["protocol"] == protocol]
        pd_persta = persta_df[persta_df["protocol"] == protocol] if persta_df is not None else None
        make_figure(pdf, pd_persta, "score", "score", tag, protocol,
                    out_dir / f"mlo_timeseries_score_{protocol.lower()}_{tag}.png")
        make_figure(pdf, pd_persta, "link_freq", "link", tag, protocol,
                    out_dir / f"mlo_timeseries_link_{protocol.lower()}_{tag}.png")


if __name__ == "__main__":
    main()
