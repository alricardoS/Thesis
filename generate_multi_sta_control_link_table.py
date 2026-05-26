#!/usr/bin/env python3
"""
Generate table to verify whether control-frame responses returned on same link.
Input CSV columns:
pair,protocol,response_type,matched,same_link,cross_link,unmatched,same_link_pct
"""

import os
import sys
import pandas as pd


if len(sys.argv) < 3:
    sys.exit("Usage: python3 generate_multi_sta_control_link_table.py <control_link_csv> <out_dir> [scenario]")

csv_path = sys.argv[1]
out_dir = sys.argv[2]
scenario = sys.argv[3] if len(sys.argv) >= 4 else "Unknown"

os.makedirs(out_dir, exist_ok=True)


def safe_read_csv(path):
    if not path or not os.path.exists(path):
        return pd.DataFrame()
    try:
        return pd.read_csv(path)
    except Exception:
        return pd.DataFrame()


df = safe_read_csv(csv_path)
out_file = os.path.join(out_dir, "mlo_control_link_return_table.txt")

with open(out_file, "w") as f:
    f.write("=" * 120 + "\n")
    f.write(f"MLO Control-Frame Return-Link Table - {scenario}\n")
    f.write("=" * 120 + "\n\n")

    if df.empty:
        f.write("No control-link summary data available.\n")
    else:
        needed = ["pair", "protocol", "response_type", "matched", "same_link", "cross_link", "unmatched", "same_link_pct"]
        for c in needed:
            if c not in df.columns:
                df[c] = 0

        df = df[needed].copy()

        f.write("Per Pair / Protocol / Response Type\n")
        f.write("-" * 120 + "\n")
        f.write(df.sort_values(["pair", "protocol", "response_type"]).to_string(index=False))
        f.write("\n\n")

        no_total = df[df["response_type"] != "TOTAL"].copy()
        if not no_total.empty:
            agg = (
                no_total.groupby("response_type", as_index=False)[["matched", "same_link", "cross_link", "unmatched"]]
                .sum()
                .sort_values("response_type")
            )
            agg["same_link_pct"] = agg.apply(
                lambda r: (100.0 * r["same_link"] / r["matched"]) if r["matched"] > 0 else 0.0,
                axis=1,
            )

            f.write("Aggregated by Response Type\n")
            f.write("-" * 120 + "\n")
            f.write(agg.to_string(index=False))
            f.write("\n\n")

            matched = agg["matched"].sum()
            same = agg["same_link"].sum()
            cross = agg["cross_link"].sum()
            unmatch = agg["unmatched"].sum()
            same_pct = (100.0 * same / matched) if matched > 0 else 0.0

            f.write("Overall\n")
            f.write("-" * 120 + "\n")
            f.write(f"Matched={matched}  SameLink={same}  CrossLink={cross}  Unmatched={unmatch}  SameLink_pct={same_pct:.4f}\n")

print(f"Control-link table generated: {out_file}")
