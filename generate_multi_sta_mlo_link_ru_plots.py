#!/usr/bin/env python3
"""
generate_multi_sta_mlo_link_ru_plots.py
Gera tabela e gráficos para analisar:
- utilização simultânea dos links MLO (STR)
- divisão de RUs por link
"""

import os
import sys
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np


if len(sys.argv) < 4:
    sys.exit("Usage: python3 generate_multi_sta_mlo_link_ru_plots.py <link_activity_csv> <ru_allocation_csv> <out_dir> [scenario] [link_traffic_csv]")

link_csv = sys.argv[1]
ru_csv = sys.argv[2]
out_dir = sys.argv[3]
scenario = sys.argv[4] if len(sys.argv) >= 5 else "Unknown"
link_traffic_csv = sys.argv[5] if len(sys.argv) >= 6 else None

os.makedirs(out_dir, exist_ok=True)


def get_link_frequency(pair, link_id):
    """
    Map (pair, link_id) to frequency band.
    Examples:
      pair="2.4+5", link_id=0 → "2.4 GHz"
      pair="2.4+5", link_id=1 → "5 GHz"
      pair="2.4+6", link_id=1 → "6 GHz"
      pair="5+6", link_id=0 → "5 GHz"
    """
    if not pair:
        return f"Link {link_id}"
    
    parts = str(pair).split('+')
    if len(parts) != 2:
        return f"Link {link_id}"
    
    freq_list = [p.strip() for p in parts]
    if link_id >= len(freq_list):
        return f"Link {link_id}"
    
    freq_str = freq_list[link_id]
    # Append GHz
    try:
        f = float(freq_str)
        if 2.0 <= f < 3.0:
            return "2.4 GHz"
        elif 4.0 <= f < 5.5:
            return "5 GHz"
        elif 5.5 <= f < 7.0:
            return "6 GHz"
    except ValueError:
        pass
    
    return freq_str + " GHz"


def safe_read_csv(path):
    if not path or not os.path.exists(path):
        return pd.DataFrame()
    try:
        return pd.read_csv(path)
    except Exception:
        return pd.DataFrame()


df_link = safe_read_csv(link_csv)
df_ru = safe_read_csv(ru_csv)
df_link_traffic = safe_read_csv(link_traffic_csv)

# Add frequency columns
if not df_link.empty:
    df_link['frequency'] = df_link.apply(lambda row: get_link_frequency(row.get('pair'), row.get('link_id')), axis=1)

if not df_ru.empty:
    df_ru['frequency'] = df_ru.apply(lambda row: get_link_frequency(row.get('pair'), row.get('link_id')), axis=1)

if not df_link_traffic.empty:
    df_link_traffic['frequency'] = df_link_traffic.apply(lambda row: get_link_frequency(row.get('pair'), row.get('link_id')), axis=1)
    df_link_traffic['pair_norm'] = df_link_traffic['pair'].astype(str).str.replace("2+", "2.4+", regex=False)
    for col in ["bytes", "link_id"]:
        if col in df_link_traffic.columns:
            df_link_traffic[col] = pd.to_numeric(df_link_traffic[col], errors="coerce").fillna(0)
    link_bytes = (
        df_link_traffic.groupby(["pair_norm", "protocol", "link_id"], as_index=False)[["bytes"]]
        .sum()
        .rename(columns={"pair_norm": "pair"})
    )
else:
    link_bytes = pd.DataFrame()

# --------- TEXT TABLE ---------
table_file = os.path.join(out_dir, "mlo_link_ru_usage_table.txt")
with open(table_file, "w") as f:
    f.write("=" * 130 + "\n")
    f.write(f"MLO Link/RU Usage Summary - {scenario}\n")
    f.write("=" * 130 + "\n\n")

    if df_link.empty:
        f.write("No link activity data available.\n")
    else:
        f.write("Per Pair/Protocol/Link Activity\n")
        f.write("-" * 130 + "\n")
        cols = [
            "pair",
            "protocol",
            "link_id",
            "frequency",
            "tx_time_s",
            "bytes",
            "duty_pct",
            "overlap_time_s",
            "overlap_pct",
            "mu_tx_count",
            "su_tx_count",
        ]
        tmp = df_link.copy()
        if not link_bytes.empty:
            tmp = tmp.merge(link_bytes, on=["pair", "protocol", "link_id"], how="left")
        else:
            tmp["bytes"] = 0
        for c in cols:
            if c not in tmp.columns:
                tmp[c] = 0
        tmp = tmp[cols].sort_values(["pair", "protocol", "link_id"])
        f.write(tmp.to_string(index=False))
        f.write("\n\n")

        # Aggregated by Link - now include frequency info from pairs
        agg = df_link.groupby(["link_id", "frequency"], as_index=False)[["tx_time_s", "mu_tx_count", "su_tx_count"]].sum().sort_values("link_id")
        if not link_bytes.empty:
            agg_bytes = (
                df_link_traffic.groupby(["link_id", "frequency"], as_index=False)[["bytes"]]
                .sum()
            )
            agg = agg.merge(agg_bytes, on=["link_id", "frequency"], how="left")
        else:
            agg["bytes"] = 0
        f.write("Aggregated by Link\n")
        f.write("-" * 130 + "\n")
        if not agg.empty:
            total_time = agg["tx_time_s"].sum()
            if total_time > 0:
                agg["tx_share_pct"] = (agg["tx_time_s"] * 100.0 / total_time).round(2)
            else:
                agg["tx_share_pct"] = 0.0
            f.write(agg[["link_id", "frequency", "tx_time_s", "bytes", "mu_tx_count", "su_tx_count", "tx_share_pct"]].to_string(index=False))
            f.write("\n\n")

    if df_ru.empty:
        f.write("No RU allocation data available (likely no MU/OFDMA allocations in this scenario).\n")
    else:
        ru_pivot = (
            df_ru.groupby(["link_id", "frequency", "ru_type"], as_index=False)["count"].sum()
            .sort_values(["link_id", "ru_type"])
        )
        f.write("RU Allocation by Link\n")
        f.write("-" * 130 + "\n")
        f.write(ru_pivot.to_string(index=False))
        f.write("\n")

print(f"Table generated: {table_file}")


# --------- LINK DUTY / OVERLAP PLOT ---------
if not df_link.empty:
    tmp = df_link.copy()
    tmp["pair_proto"] = tmp["pair"].astype(str) + " (" + tmp["protocol"].astype(str) + ")"

    duty_pivot = tmp.pivot_table(index="pair_proto", columns="link_id", values="duty_pct", aggfunc="mean").fillna(0)
    overlap_series = tmp.groupby("pair_proto")["overlap_pct"].max()

    x = np.arange(len(duty_pivot.index))
    width = 0.35

    fig, ax1 = plt.subplots(figsize=(12, 6))

    links = sorted(duty_pivot.columns.tolist())
    for i, link in enumerate(links):
        vals = duty_pivot[link].values
        ax1.bar(x + (i - (len(links) - 1) / 2) * width, vals, width=width * 0.9, label=f"Link {int(link)} Duty")

    ax1.set_ylabel("Link Duty (%)")
    ax1.set_xticks(x)
    ax1.set_xticklabels(duty_pivot.index, rotation=20, ha="right")
    ax1.set_title(f"MLO Link Usage and Overlap - {scenario}")
    ax1.grid(axis="y", alpha=0.3)

    ax2 = ax1.twinx()
    ov_vals = [overlap_series.get(idx, 0.0) for idx in duty_pivot.index]
    ax2.plot(x, ov_vals, color="black", marker="o", linewidth=2, label="Overlap % (both links active)")
    ax2.set_ylabel("Overlap (%)")

    h1, l1 = ax1.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax1.legend(h1 + h2, l1 + l2, loc="upper right")

    plt.tight_layout()
    out = os.path.join(out_dir, "mlo_link_activity_overview.png")
    plt.savefig(out, dpi=150)
    plt.close()
    print(f"Plot generated: {out}")


# --------- RU DISTRIBUTION PLOT ---------
if not df_ru.empty:
    ru_agg = df_ru.groupby(["link_id", "frequency", "ru_type"], as_index=False)["count"].sum()
    pivot = ru_agg.pivot_table(index=["link_id", "frequency"], columns="ru_type", values="count").fillna(0)

    fig, ax = plt.subplots(figsize=(11, 6))
    bottom = np.zeros(len(pivot.index))
    x = np.arange(len(pivot.index))

    for ru_type in pivot.columns:
        vals = pivot[ru_type].values
        ax.bar(x, vals, bottom=bottom, label=str(ru_type))
        bottom += vals

    ax.set_xticks(x)
    # Create labels with both link_id and frequency
    labels = [f"Link {int(idx[0])} ({idx[1]})" for idx in pivot.index]
    ax.set_xticklabels(labels)
    ax.set_ylabel("RU Allocation Count")
    ax.set_title(f"RU Type Distribution per Link (MLO STR) - {scenario}")
    ax.legend(title="RU Type", bbox_to_anchor=(1.02, 1), loc="upper left")
    ax.grid(axis="y", alpha=0.3)

    plt.tight_layout()
    out = os.path.join(out_dir, "mlo_ru_distribution_by_link.png")
    plt.savefig(out, dpi=150)
    plt.close()
    print(f"Plot generated: {out}")

print("MLO link/RU usage report generation completed.")
