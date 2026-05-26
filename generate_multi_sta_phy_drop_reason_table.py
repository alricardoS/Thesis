#!/usr/bin/env python3
"""
Generate PHY RX drop reason table (single-link).
"""
import os
import sys
import pandas as pd


def main():
    if len(sys.argv) < 4:
        sys.exit("Usage: python3 generate_multi_sta_phy_drop_reason_table.py <phy_csv> <out_dir> <scenario>")

    csv_file = sys.argv[1]
    out_dir = sys.argv[2]
    scenario = sys.argv[3]
    out_file = os.path.join(out_dir, "phy_drop_reason_table.txt")

    if not os.path.exists(csv_file):
        print(f"[WARN] Missing file: {csv_file}")
        return

    os.makedirs(out_dir, exist_ok=True)
    df = pd.read_csv(csv_file)
    if df.empty:
        print(f"[WARN] Empty PHY CSV: {csv_file}")
        return

    for c in [
        "freq_band",
        "protocol",
        "total_tx_packets",
        "reason",
        "count",
        "pct_of_phy_rx",
        "pct_of_total_granular",
        "pct_of_total_tx_packets",
    ]:
        if c not in df.columns:
            df[c] = 0

    df = df.sort_values(["freq_band", "protocol", "count"], ascending=[True, True, False])

    with open(out_file, "w") as f:
        f.write("=" * 140 + "\n")
        f.write(f"PHY RX DROP REASONS - {scenario}\n")
        f.write("=" * 140 + "\n\n")
        f.write(
            f"{'Band':<10} {'Proto':<8} {'Reason':<28} {'Count':<12} {'%PhyRx':<12} {'%TotalDrops':<14} {'%TotalTX':<12} {'TotalTX':<12}\n"
        )
        f.write("-" * 140 + "\n")
        for _, r in df.iterrows():
            f.write(
                f"{str(r['freq_band']):<10} {str(r['protocol']):<8} {str(r['reason'])[:28]:<28} "
                f"{int(r['count']):<12} {float(r['pct_of_phy_rx']):<12.3f} {float(r['pct_of_total_granular']):<14.3f} "
                f"{float(r['pct_of_total_tx_packets']):<12.6f} {int(r['total_tx_packets']):<12}\n"
            )

    print(f"PHY reason table generated: {out_file}")


if __name__ == "__main__":
    main()
