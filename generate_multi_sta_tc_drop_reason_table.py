#!/usr/bin/env python3
"""
Generate Traffic Control drop-reason table(s) from single-link and MLO CSV files.
"""

import os
import sys
import pandas as pd

if len(sys.argv) < 4:
    sys.exit("Usage: python3 generate_multi_sta_tc_drop_reason_table.py <single_tc_csv> <mlo_tc_csv> <out_dir> [scenario]")

single_csv = sys.argv[1]
mlo_csv = sys.argv[2]
out_dir = sys.argv[3]
scenario = sys.argv[4] if len(sys.argv) >= 5 else "Unknown"

os.makedirs(out_dir, exist_ok=True)
out_file = os.path.join(out_dir, "tc_drop_reason_table.txt")


def safe_read(path):
    if not path or not os.path.exists(path):
        return pd.DataFrame()
    try:
        return pd.read_csv(path)
    except Exception:
        return pd.DataFrame()


def normalize(df: pd.DataFrame, mode: str) -> pd.DataFrame:
    if df.empty:
        return df
    if mode == "single":
        if "freq_band" in df.columns:
            df = df.rename(columns={"freq_band": "scenario_item"})
        else:
            df["scenario_item"] = "UNKNOWN"
    else:
        if "pair" in df.columns:
            df = df.rename(columns={"pair": "scenario_item"})
        else:
            df["scenario_item"] = "UNKNOWN"

    needed = [
        "scenario_item",
        "protocol",
        "total_tx_packets",
        "stage",
        "reason",
        "count",
        "pct_of_tc_stage",
        "pct_of_total_granular",
        "pct_of_total_tx_packets",
    ]
    for c in needed:
        if c not in df.columns:
            df[c] = 0
    return df[needed].copy()


single_df = normalize(safe_read(single_csv), "single")
mlo_df = normalize(safe_read(mlo_csv), "mlo")

with open(out_file, "w") as f:
    f.write("=" * 120 + "\n")
    f.write(f"Traffic Control Drop Reason Table - {scenario}\n")
    f.write("=" * 120 + "\n\n")

    if single_df.empty and mlo_df.empty:
        f.write("No TC drop-reason data available.\n")
    else:
        if not single_df.empty:
            f.write("Single-Link TC Drop Reasons\n")
            f.write("-" * 120 + "\n")
            f.write(single_df.sort_values(["scenario_item", "protocol", "stage", "count"], ascending=[True, True, True, False]).to_string(index=False))
            f.write("\n\n")

        if not mlo_df.empty:
            f.write("MLO TC Drop Reasons\n")
            f.write("-" * 120 + "\n")
            f.write(mlo_df.sort_values(["scenario_item", "protocol", "stage", "count"], ascending=[True, True, True, False]).to_string(index=False))
            f.write("\n\n")

        combined = pd.concat([single_df.assign(mode="single"), mlo_df.assign(mode="mlo")], ignore_index=True)
        if not combined.empty:
            agg = (
                combined.groupby(["mode", "stage", "reason"], as_index=False)["count"]
                .sum()
                .sort_values(["mode", "stage", "count"], ascending=[True, True, False])
            )
            f.write("Aggregated TC Drop Reasons (by mode/stage/reason)\n")
            f.write("-" * 120 + "\n")
            f.write(agg.to_string(index=False))
            f.write("\n")

print(f"TC drop-reason table generated: {out_file}")
