#!/usr/bin/env python3
"""
generate_multi_sta_loss_breakdown_plots.py
Gera gráficos de breakdown granular de packet loss por camada:
- PHY Layer (PhyTxDrop + PhyRxDrop)
- MAC Layer (MacTxDrop + MacRxDrop)
- WiFi Queue (WifiQueueDrop)
- Traffic Control (TcDropBeforeEnqueue + TcDropAfterDequeue + TcDrop)

Gera:
1. Stacked bar charts mostrando % de perdas por camada
2. Comparação Single Link vs MLO
3. Tabelas detalhadas com breakdown por camada
"""
import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

if len(sys.argv) < 4:
    sys.exit("Usage: python3 generate_multi_sta_loss_breakdown_plots.py <single_csv> <mlo_csv> <out_dir> [data_rate] [single_phy_reason_csv] [mlo_phy_reason_csv] [mlo_link_traffic_csv]")

single_csv = sys.argv[1]
mlo_csv = sys.argv[2]
out_dir = sys.argv[3]
data_rate = sys.argv[4] if len(sys.argv) >= 5 else "Unknown"
single_phy_reason_csv = sys.argv[5] if len(sys.argv) >= 6 else None
mlo_phy_reason_csv = sys.argv[6] if len(sys.argv) >= 7 else None
mlo_link_traffic_csv = sys.argv[7] if len(sys.argv) >= 8 else None

os.makedirs(out_dir, exist_ok=True)

# Load data
df_single = pd.read_csv(single_csv)
df_mlo = pd.read_csv(mlo_csv)
df_mlo_link_traffic = None

if mlo_link_traffic_csv and os.path.exists(mlo_link_traffic_csv):
    try:
        df_mlo_link_traffic = pd.read_csv(mlo_link_traffic_csv)
    except Exception:
        df_mlo_link_traffic = None

df_single_phy_reason = None
df_mlo_phy_reason = None

if single_phy_reason_csv and os.path.exists(single_phy_reason_csv):
    try:
        df_single_phy_reason = pd.read_csv(single_phy_reason_csv)
    except Exception:
        df_single_phy_reason = None

if mlo_phy_reason_csv and os.path.exists(mlo_phy_reason_csv):
    try:
        df_mlo_phy_reason = pd.read_csv(mlo_phy_reason_csv)
    except Exception:
        df_mlo_phy_reason = None

# Colors for each layer
COLORS = {
    'PHY': '#e74c3c',        # Red
    'MAC': '#f39c12',        # Orange
    'WiFi Queue': '#3498db',  # Blue
    'Traffic Control': '#9b59b6',  # Purple
    'Unaccounted': '#95a5a6'  # Gray
}

def add_bar_labels(ax, bars, total_height=None):
    """Add percentage labels on stacked bars"""
    for bar in bars:
        height = bar.get_height()
        if height > 2:  # Only show label if > 2%
            ax.annotate(f'{height:.1f}%',
                        xy=(bar.get_x() + bar.get_width() / 2, bar.get_y() + height / 2),
                        ha='center', va='center', fontsize=8, color='white', fontweight='bold')


def allocate_counts(total, weights):
    total = int(round(float(total)))
    if total <= 0 or not weights:
        return [0 for _ in weights]

    clean_weights = [max(0.0, float(weight)) for weight in weights]
    weight_sum = sum(clean_weights)
    if weight_sum <= 0:
        base = total // len(clean_weights)
        remainder = total - (base * len(clean_weights))
        allocations = [base for _ in clean_weights]
        for idx in range(remainder):
            allocations[idx % len(allocations)] += 1
        return allocations

    raw = [total * weight / weight_sum for weight in clean_weights]
    allocations = [int(np.floor(value)) for value in raw]
    remainder = total - sum(allocations)
    if remainder > 0:
        order = sorted(range(len(raw)), key=lambda idx: (raw[idx] - allocations[idx], clean_weights[idx]), reverse=True)
        for idx in order[:remainder]:
            allocations[idx] += 1
    return allocations


def normalize_pair_label(value):
    parts = str(value).split("+")
    normalized = []
    for part in parts:
        token = part.strip()
        normalized.append("2.4" if token == "2" else token)
    return "+".join(normalized)


def build_mlo_link_loss_table(df_loss, df_link_traffic):
    base_cols = [
        "pair", "freq_1_gHz", "freq_2_gHz", "protocol", "link_id", "frequency",
        "phy_tx_drop", "phy_rx_drop", "mac_tx_drop", "mac_rx_drop", "wifi_queue_drop",
        "tc_drop_before", "tc_drop_after", "tc_drop", "total_granular", "agg_total_drops", "e2e_lost",
        "phy_pct", "mac_pct", "wifi_queue_pct", "tc_pct", "unaccounted_pct",
    ]

    if df_link_traffic is None or df_link_traffic.empty:
        fallback = df_loss.copy()
        fallback["freq_1_gHz"] = fallback["pair"].astype(str).str.split("+").str[0].fillna("")
        fallback["freq_2_gHz"] = fallback["pair"].astype(str).str.split("+").str[1].fillna("")
        fallback["link_id"] = -1
        fallback["frequency"] = fallback["pair"]
        fallback["agg_total_drops"] = fallback.get("total_granular", 0)
        for c in base_cols:
            if c not in fallback.columns:
                fallback[c] = 0
        return fallback[base_cols].sort_values(["pair", "protocol", "link_id"])

    required = {"pair", "protocol", "link_id", "link_name", "frame_count"}
    if not required.issubset(df_link_traffic.columns):
        return build_mlo_link_loss_table(df_loss, None)

    link_df = df_link_traffic.copy()
    for col in ["frame_count", "bytes", "link_id"]:
        if col in link_df.columns:
            link_df[col] = pd.to_numeric(link_df[col], errors="coerce").fillna(0)

    link_df["pair_norm"] = link_df["pair"].apply(normalize_pair_label)
    link_df = (
        link_df.groupby(["pair_norm", "protocol", "link_id", "link_name"], as_index=False)[["frame_count"]]
        .sum()
        .sort_values(["pair_norm", "protocol", "link_id"])
    )

    loss_cols = [
        "phy_tx_drop", "phy_rx_drop", "mac_tx_drop", "mac_rx_drop", "wifi_queue_drop",
        "tc_drop_before", "tc_drop_after", "tc_drop", "total_granular", "e2e_lost",
    ]

    rows = []
    for _, loss_row in df_loss.iterrows():
        agg_total_drops = int(loss_row["total_granular"])
        pair_links = link_df[(link_df["pair_norm"] == normalize_pair_label(loss_row["pair"])) & (link_df["protocol"] == loss_row["protocol"])].copy()
        if pair_links.empty:
            fallback_row = loss_row.to_dict()
            fallback_row["freq_1_gHz"] = str(loss_row["pair"]).split("+")[0]
            fallback_row["freq_2_gHz"] = str(loss_row["pair"]).split("+")[1] if "+" in str(loss_row["pair"]) else ""
            fallback_row["link_id"] = -1
            fallback_row["frequency"] = str(loss_row["pair"])
            fallback_row["agg_total_drops"] = agg_total_drops
            rows.append(fallback_row)
            continue

        weights = pair_links["frame_count"].tolist()
        link_totals = pair_links["frame_count"].sum()
        allocated = {col: allocate_counts(loss_row[col], weights) for col in loss_cols}
        allocated_e2e = allocate_counts(loss_row["e2e_lost"], weights)

        for idx, (_, link_row) in enumerate(pair_links.iterrows()):
            phy_tx = int(allocated["phy_tx_drop"][idx])
            phy_rx = int(allocated["phy_rx_drop"][idx])
            mac_tx = int(allocated["mac_tx_drop"][idx])
            mac_rx = int(allocated["mac_rx_drop"][idx])
            wifi_q = int(allocated["wifi_queue_drop"][idx])
            tc_before = int(allocated["tc_drop_before"][idx])
            tc_after = int(allocated["tc_drop_after"][idx])
            tc_drop = int(allocated["tc_drop"][idx])
            total = phy_tx + phy_rx + mac_tx + mac_rx + wifi_q + tc_before + tc_after + tc_drop
            e2e = int(allocated_e2e[idx])

            row = {
                "pair": loss_row["pair"],
                "freq_1_gHz": str(loss_row["pair"]).split("+")[0],
                "freq_2_gHz": str(loss_row["pair"]).split("+")[1] if "+" in str(loss_row["pair"]) else "",
                "protocol": loss_row["protocol"],
                "link_id": int(link_row["link_id"]),
                "frequency": str(link_row["link_name"]),
                "phy_tx_drop": phy_tx,
                "phy_rx_drop": phy_rx,
                "mac_tx_drop": mac_tx,
                "mac_rx_drop": mac_rx,
                "wifi_queue_drop": wifi_q,
                "tc_drop_before": tc_before,
                "tc_drop_after": tc_after,
                "tc_drop": tc_drop,
                "total_granular": total,
                "agg_total_drops": agg_total_drops,
                "e2e_lost": e2e,
            }

            if total > 0:
                row["phy_pct"] = ((phy_tx + phy_rx) * 100.0) / total
                row["mac_pct"] = ((mac_tx + mac_rx) * 100.0) / total
                row["wifi_queue_pct"] = (wifi_q * 100.0) / total
                row["tc_pct"] = ((tc_before + tc_after + tc_drop) * 100.0) / total
                row["unaccounted_pct"] = 100.0 - row["phy_pct"] - row["mac_pct"] - row["wifi_queue_pct"] - row["tc_pct"]
            else:
                row["phy_pct"] = 0.0
                row["mac_pct"] = 0.0
                row["wifi_queue_pct"] = 0.0
                row["tc_pct"] = 0.0
                row["unaccounted_pct"] = 0.0

            rows.append(row)

    out_df = pd.DataFrame(rows)
    for col in base_cols:
        if col not in out_df.columns:
            out_df[col] = 0
    return out_df[base_cols].sort_values(["pair", "protocol", "link_id"])

# ========== SINGLE LINK: STACKED BAR CHART ==========
print("Generating Single Link Packet Loss Breakdown chart...")

fig, ax = plt.subplots(figsize=(12, 7))

bands = df_single['freq_band'].unique()
x = np.arange(len(bands))
width = 0.6

# Stack the percentages
phy_pct = []
mac_pct = []
wifi_q_pct = []
tc_pct = []
unacc_pct = []

for b in bands:
    row = df_single[df_single['freq_band'] == b]
    if not row.empty:
        phy_pct.append(float(row['phy_pct'].values[0]))
        mac_pct.append(float(row['mac_pct'].values[0]))
        wifi_q_pct.append(float(row['wifi_queue_pct'].values[0]))
        tc_pct.append(float(row['tc_pct'].values[0]))
        unacc_pct.append(float(row['unaccounted_pct'].values[0]))
    else:
        phy_pct.append(0)
        mac_pct.append(0)
        wifi_q_pct.append(0)
        tc_pct.append(0)
        unacc_pct.append(0)

# Create stacked bar chart
bottom = np.zeros(len(bands))

bar_phy = ax.bar(x, phy_pct, width, label='PHY Layer', color=COLORS['PHY'], bottom=bottom)
bottom += np.array(phy_pct)

bar_mac = ax.bar(x, mac_pct, width, label='MAC Layer', color=COLORS['MAC'], bottom=bottom)
bottom += np.array(mac_pct)

bar_wifi = ax.bar(x, wifi_q_pct, width, label='WiFi Queue', color=COLORS['WiFi Queue'], bottom=bottom)
bottom += np.array(wifi_q_pct)

bar_tc = ax.bar(x, tc_pct, width, label='Traffic Control', color=COLORS['Traffic Control'], bottom=bottom)
bottom += np.array(tc_pct)

bar_unacc = ax.bar(x, unacc_pct, width, label='Unaccounted (in-flight)', color=COLORS['Unaccounted'], bottom=bottom)

# Add labels
for i, b in enumerate(bands):
    if tc_pct[i] > 2:
        ax.annotate(f'{tc_pct[i]:.1f}%', xy=(i, np.sum([phy_pct[i], mac_pct[i], wifi_q_pct[i], tc_pct[i]/2])), 
                    ha='center', va='center', fontsize=9, color='white', fontweight='bold')

ax.set_xticks(x)
ax.set_xticklabels(bands)
ax.set_ylabel('Percentage of Total Granular Drops (%)')
ax.set_xlabel('Frequency Band')
ax.set_title(f'WiFi 7 Single Link - Packet Loss Attribution by Layer\nData Rate: {data_rate}')
ax.legend(loc='upper right')
ax.set_ylim(0, 105)

plt.tight_layout()
plt.savefig(os.path.join(out_dir, 'single_link_loss_breakdown_stacked.png'), dpi=150)
print('Single Link Loss Breakdown Plot generated:', os.path.join(out_dir, 'single_link_loss_breakdown_stacked.png'))
plt.close()

# ========== MLO: STACKED BAR CHART ==========
print("Generating MLO Packet Loss Breakdown chart...")

fig, ax = plt.subplots(figsize=(12, 7))

pairs = df_mlo['pair'].unique()
x = np.arange(len(pairs))

# Stack the percentages
phy_pct = []
mac_pct = []
wifi_q_pct = []
tc_pct = []
unacc_pct = []

for p in pairs:
    row = df_mlo[df_mlo['pair'] == p]
    if not row.empty:
        phy_pct.append(float(row['phy_pct'].values[0]))
        mac_pct.append(float(row['mac_pct'].values[0]))
        wifi_q_pct.append(float(row['wifi_queue_pct'].values[0]))
        tc_pct.append(float(row['tc_pct'].values[0]))
        unacc_pct.append(float(row['unaccounted_pct'].values[0]))
    else:
        phy_pct.append(0)
        mac_pct.append(0)
        wifi_q_pct.append(0)
        tc_pct.append(0)
        unacc_pct.append(0)

# Create stacked bar chart
bottom = np.zeros(len(pairs))

bar_phy = ax.bar(x, phy_pct, width, label='PHY Layer', color=COLORS['PHY'], bottom=bottom)
bottom += np.array(phy_pct)

bar_mac = ax.bar(x, mac_pct, width, label='MAC Layer', color=COLORS['MAC'], bottom=bottom)
bottom += np.array(mac_pct)

bar_wifi = ax.bar(x, wifi_q_pct, width, label='WiFi Queue', color=COLORS['WiFi Queue'], bottom=bottom)
bottom += np.array(wifi_q_pct)

bar_tc = ax.bar(x, tc_pct, width, label='Traffic Control', color=COLORS['Traffic Control'], bottom=bottom)
bottom += np.array(tc_pct)

bar_unacc = ax.bar(x, unacc_pct, width, label='Unaccounted (in-flight)', color=COLORS['Unaccounted'], bottom=bottom)

# Add labels
for i, p in enumerate(pairs):
    if tc_pct[i] > 2:
        ax.annotate(f'{tc_pct[i]:.1f}%', xy=(i, np.sum([phy_pct[i], mac_pct[i], wifi_q_pct[i], tc_pct[i]/2])), 
                    ha='center', va='center', fontsize=9, color='white', fontweight='bold')

ax.set_xticks(x)
ax.set_xticklabels(pairs)
ax.set_ylabel('Percentage of Total Granular Drops (%)')
ax.set_xlabel('MLO Frequency Pair')
ax.set_title(f'WiFi 7 MLO - Packet Loss Attribution by Layer\nData Rate: {data_rate}')
ax.legend(loc='upper right')
ax.set_ylim(0, 105)

plt.tight_layout()
plt.savefig(os.path.join(out_dir, 'mlo_loss_breakdown_stacked.png'), dpi=150)
print('MLO Loss Breakdown Plot generated:', os.path.join(out_dir, 'mlo_loss_breakdown_stacked.png'))
plt.close()

# ========== COMPARISON: SINGLE LINK vs MLO ==========
print("Generating Single Link vs MLO Loss Breakdown Comparison...")

fig, axes = plt.subplots(1, 2, figsize=(16, 7))

# Single Link subplot
ax1 = axes[0]
bands = df_single['freq_band'].unique()
x1 = np.arange(len(bands))

phy_pct_s = [float(df_single[df_single['freq_band'] == b]['phy_pct'].values[0]) if len(df_single[df_single['freq_band'] == b]) > 0 else 0 for b in bands]
mac_pct_s = [float(df_single[df_single['freq_band'] == b]['mac_pct'].values[0]) if len(df_single[df_single['freq_band'] == b]) > 0 else 0 for b in bands]
wifi_q_pct_s = [float(df_single[df_single['freq_band'] == b]['wifi_queue_pct'].values[0]) if len(df_single[df_single['freq_band'] == b]) > 0 else 0 for b in bands]
tc_pct_s = [float(df_single[df_single['freq_band'] == b]['tc_pct'].values[0]) if len(df_single[df_single['freq_band'] == b]) > 0 else 0 for b in bands]
unacc_pct_s = [float(df_single[df_single['freq_band'] == b]['unaccounted_pct'].values[0]) if len(df_single[df_single['freq_band'] == b]) > 0 else 0 for b in bands]

bottom = np.zeros(len(bands))
ax1.bar(x1, phy_pct_s, width, label='PHY Layer', color=COLORS['PHY'], bottom=bottom)
bottom += np.array(phy_pct_s)
ax1.bar(x1, mac_pct_s, width, label='MAC Layer', color=COLORS['MAC'], bottom=bottom)
bottom += np.array(mac_pct_s)
ax1.bar(x1, wifi_q_pct_s, width, label='WiFi Queue', color=COLORS['WiFi Queue'], bottom=bottom)
bottom += np.array(wifi_q_pct_s)
ax1.bar(x1, tc_pct_s, width, label='Traffic Control', color=COLORS['Traffic Control'], bottom=bottom)
bottom += np.array(tc_pct_s)
ax1.bar(x1, unacc_pct_s, width, label='Unaccounted', color=COLORS['Unaccounted'], bottom=bottom)

ax1.set_xticks(x1)
ax1.set_xticklabels(bands)
ax1.set_ylabel('Percentage of Total Granular Drops (%)')
ax1.set_xlabel('Frequency Band')
ax1.set_title('Single Link')
ax1.set_ylim(0, 105)

# MLO subplot
ax2 = axes[1]
pairs = df_mlo['pair'].unique()
x2 = np.arange(len(pairs))

phy_pct_m = [float(df_mlo[df_mlo['pair'] == p]['phy_pct'].values[0]) if len(df_mlo[df_mlo['pair'] == p]) > 0 else 0 for p in pairs]
mac_pct_m = [float(df_mlo[df_mlo['pair'] == p]['mac_pct'].values[0]) if len(df_mlo[df_mlo['pair'] == p]) > 0 else 0 for p in pairs]
wifi_q_pct_m = [float(df_mlo[df_mlo['pair'] == p]['wifi_queue_pct'].values[0]) if len(df_mlo[df_mlo['pair'] == p]) > 0 else 0 for p in pairs]
tc_pct_m = [float(df_mlo[df_mlo['pair'] == p]['tc_pct'].values[0]) if len(df_mlo[df_mlo['pair'] == p]) > 0 else 0 for p in pairs]
unacc_pct_m = [float(df_mlo[df_mlo['pair'] == p]['unaccounted_pct'].values[0]) if len(df_mlo[df_mlo['pair'] == p]) > 0 else 0 for p in pairs]

bottom = np.zeros(len(pairs))
ax2.bar(x2, phy_pct_m, width, label='PHY Layer', color=COLORS['PHY'], bottom=bottom)
bottom += np.array(phy_pct_m)
ax2.bar(x2, mac_pct_m, width, label='MAC Layer', color=COLORS['MAC'], bottom=bottom)
bottom += np.array(mac_pct_m)
ax2.bar(x2, wifi_q_pct_m, width, label='WiFi Queue', color=COLORS['WiFi Queue'], bottom=bottom)
bottom += np.array(wifi_q_pct_m)
ax2.bar(x2, tc_pct_m, width, label='Traffic Control', color=COLORS['Traffic Control'], bottom=bottom)
bottom += np.array(tc_pct_m)
ax2.bar(x2, unacc_pct_m, width, label='Unaccounted', color=COLORS['Unaccounted'], bottom=bottom)

ax2.set_xticks(x2)
ax2.set_xticklabels(pairs)
ax2.set_xlabel('MLO Frequency Pair')
ax2.set_title('MLO (Multi-Link Operation)')
ax2.set_ylim(0, 105)

# Single legend for both
handles, labels = ax1.get_legend_handles_labels()
fig.legend(handles, labels, loc='upper center', ncol=5, bbox_to_anchor=(0.5, 0.02))

fig.suptitle(f'WiFi 7 Packet Loss Attribution Comparison - Data Rate: {data_rate}', fontsize=14, fontweight='bold')
plt.tight_layout(rect=[0, 0.05, 1, 0.95])
plt.savefig(os.path.join(out_dir, 'loss_breakdown_comparison_single_vs_mlo.png'), dpi=150)
print('Comparison Plot generated:', os.path.join(out_dir, 'loss_breakdown_comparison_single_vs_mlo.png'))
plt.close()

# ========== DETAILED TABLE: TEXT OUTPUT ==========
print("\nGenerating detailed loss breakdown tables...")

table_file = os.path.join(out_dir, 'loss_breakdown_table.txt')
with open(table_file, 'w') as f:
    f.write("=" * 100 + "\n")
    f.write(f"PACKET LOSS BREAKDOWN TABLE - Data Rate: {data_rate}\n")
    f.write("=" * 100 + "\n\n")
    
    # Single Link Table
    f.write("SINGLE LINK (SLO)\n")
    f.write("-" * 100 + "\n")
    f.write(f"{'Band':<12} {'PHY Drops':<12} {'MAC Drops':<12} {'WiFi Queue':<12} {'TC Drops':<15} {'Total':<12} {'E2E Lost':<12}\n")
    f.write("-" * 100 + "\n")
    
    for _, row in df_single.iterrows():
        phy_total = int(row['phy_tx_drop']) + int(row['phy_rx_drop'])
        mac_total = int(row['mac_tx_drop']) + int(row['mac_rx_drop'])
        wifi_q = int(row['wifi_queue_drop'])
        tc_total = int(row['tc_drop_before']) + int(row['tc_drop_after']) + int(row['tc_drop'])
        total = int(row['total_granular'])
        e2e = int(row['e2e_lost'])
        f.write(f"{row['freq_band']:<12} {phy_total:<12} {mac_total:<12} {wifi_q:<12} {tc_total:<15} {total:<12} {e2e:<12}\n")
    
    f.write("\nPercentages (relative to Total Granular Drops):\n")
    f.write(f"{'Band':<12} {'PHY %':<12} {'MAC %':<12} {'WiFi Q %':<12} {'TC %':<12} {'Unaccounted %':<15}\n")
    f.write("-" * 75 + "\n")
    for _, row in df_single.iterrows():
        f.write(f"{row['freq_band']:<12} {row['phy_pct']:<12.2f} {row['mac_pct']:<12.2f} {row['wifi_queue_pct']:<12.2f} {row['tc_pct']:<12.2f} {row['unaccounted_pct']:<15.2f}\n")
    
    f.write("\n\n")
    
    # MLO Table (by link)
    mlo_link_table = build_mlo_link_loss_table(df_mlo, df_mlo_link_traffic)
    f.write("MLO (Multi-Link Operation)\n")
    f.write("-" * 125 + "\n")
    f.write(
        f"{'Pair':<12} {'Link':<6} {'Frequency':<10} {'PHY Drops':<12} {'MAC Drops':<12} "
        f"{'WiFi Queue':<12} {'TC Drops':<15} {'Link Total':<12} {'Agg Total':<12} {'E2E Lost':<12}\n"
    )
    f.write("-" * 125 + "\n")

    for _, row in mlo_link_table.iterrows():
        phy_total = int(row['phy_tx_drop']) + int(row['phy_rx_drop'])
        mac_total = int(row['mac_tx_drop']) + int(row['mac_rx_drop'])
        wifi_q = int(row['wifi_queue_drop'])
        tc_total = int(row['tc_drop_before']) + int(row['tc_drop_after']) + int(row['tc_drop'])
        total = int(row['total_granular'])
        agg_total = int(row['agg_total_drops'])
        e2e = int(row['e2e_lost'])
        f.write(
            f"{str(row['pair']):<12} {int(row['link_id']):<6} {str(row['frequency']):<10} "
            f"{phy_total:<12} {mac_total:<12} {wifi_q:<12} {tc_total:<15} {total:<12} {agg_total:<12} {e2e:<12}\n"
        )

    f.write("\nPercentages (relative to each link's total granular drops):\n")
    f.write(
        f"{'Pair':<12} {'Link':<6} {'Frequency':<10} {'PHY %':<12} {'MAC %':<12} {'WiFi Q %':<12} {'TC %':<12} {'Unaccounted %':<15}\n"
    )
    f.write("-" * 105 + "\n")
    for _, row in mlo_link_table.iterrows():
        f.write(
            f"{str(row['pair']):<12} {int(row['link_id']):<6} {str(row['frequency']):<10} "
            f"{float(row['phy_pct']):<12.2f} {float(row['mac_pct']):<12.2f} {float(row['wifi_queue_pct']):<12.2f} "
            f"{float(row['tc_pct']):<12.2f} {float(row['unaccounted_pct']):<15.2f}\n"
        )
    
    f.write("\n\n")
    f.write("=" * 100 + "\n")
    f.write("LEGEND:\n")
    f.write("  PHY Drops: Packets dropped at Physical layer (TX interference + RX low SNR/collision)\n")
    f.write("  MAC Drops: Packets dropped at MAC layer (TX retry limit exceeded + RX CRC errors)\n")
    f.write("  WiFi Queue: Packets dropped from WiFi MAC queue (queue full or expired)\n")
    f.write("  TC Drops: Packets dropped at Traffic Control layer (queue overflow before reaching WiFi)\n")
    f.write("  Unaccounted: Remaining percentage to close 100% (typically rounding residue)\n")

    # Optional: detailed PHY RX drop reasons
    if df_single_phy_reason is not None and not df_single_phy_reason.empty:
        f.write("\n")
        f.write("PHY RX DROP REASONS - SINGLE LINK\n")
        f.write("-" * 100 + "\n")
        f.write(f"{'Band':<12} {'Protocol':<10} {'Reason':<40} {'Count':<12} {'% of PhyRx':<14} {'% of TotalDrops':<16}\n")
        f.write("-" * 100 + "\n")
        for _, row in df_single_phy_reason.sort_values(by=['freq_band', 'protocol', 'count'], ascending=[True, True, False]).iterrows():
            f.write(f"{str(row['freq_band']):<12} {str(row['protocol']):<10} {str(row['reason']):<40} {int(row['count']):<12} {float(row['pct_of_phy_rx']):<14.2f} {float(row['pct_of_total_granular']):<16.2f}\n")

    if df_mlo_phy_reason is not None and not df_mlo_phy_reason.empty:
        f.write("\n")
        f.write("PHY RX DROP REASONS - MLO\n")
        f.write("-" * 100 + "\n")
        f.write(f"{'Pair':<12} {'Protocol':<10} {'Reason':<40} {'Count':<12} {'% of PhyRx':<14} {'% of TotalDrops':<16}\n")
        f.write("-" * 100 + "\n")
        for _, row in df_mlo_phy_reason.sort_values(by=['pair', 'protocol', 'count'], ascending=[True, True, False]).iterrows():
            f.write(f"{str(row['pair']):<12} {str(row['protocol']):<10} {str(row['reason']):<40} {int(row['count']):<12} {float(row['pct_of_phy_rx']):<14.2f} {float(row['pct_of_total_granular']):<16.2f}\n")
    f.write("=" * 100 + "\n")

print(f'Detailed table saved to: {table_file}')

def generate_reason_plot(df_reason, group_col, title, out_name):
    if df_reason is None or df_reason.empty:
        return

    grouped = (df_reason.groupby(['reason'])["count"]
               .sum()
               .sort_values(ascending=False)
               .head(10))
    if grouped.empty:
        return

    plt.figure(figsize=(12, 6))
    grouped.plot(kind='bar', color='#7f8c8d', edgecolor='black')
    plt.ylabel('Drop Count')
    plt.xlabel('PHY RX Drop Reason')
    plt.title(title)
    plt.xticks(rotation=35, ha='right')
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, out_name), dpi=150)
    plt.close()

# Reason plots (optional)
generate_reason_plot(df_single_phy_reason,
                     'freq_band',
                     f'Single Link PHY RX Drop Reasons (Top 10) - {data_rate}',
                     'single_link_phy_rx_reason_top10.png')

generate_reason_plot(df_mlo_phy_reason,
                     'pair',
                     f'MLO PHY RX Drop Reasons (Top 10) - {data_rate}',
                     'mlo_phy_rx_reason_top10.png')

# ========== PIE CHARTS: AVERAGE LOSS DISTRIBUTION ==========
print("Generating pie charts for average loss distribution...")

fig, axes = plt.subplots(1, 2, figsize=(14, 6))

# Single Link average
ax1 = axes[0]
avg_phy = df_single['phy_pct'].mean()
avg_mac = df_single['mac_pct'].mean()
avg_wifi = df_single['wifi_queue_pct'].mean()
avg_tc = df_single['tc_pct'].mean()
avg_unacc = df_single['unaccounted_pct'].mean()

sizes = [avg_phy, avg_mac, avg_wifi, avg_tc, avg_unacc]
labels = ['PHY Layer', 'MAC Layer', 'WiFi Queue', 'Traffic Control', 'Unaccounted']
colors = [COLORS['PHY'], COLORS['MAC'], COLORS['WiFi Queue'], COLORS['Traffic Control'], COLORS['Unaccounted']]

# Filter out zero values
filtered = [(s, l, c) for s, l, c in zip(sizes, labels, colors) if s > 0.1]
if filtered:
    sizes_f, labels_f, colors_f = zip(*filtered)
    wedges, texts, autotexts = ax1.pie(sizes_f, labels=labels_f, colors=colors_f, autopct='%1.1f%%',
                                        startangle=90, pctdistance=0.75)
    ax1.set_title('Single Link\nAverage Loss Distribution')
else:
    ax1.text(0.5, 0.5, 'No Losses', ha='center', va='center')
    ax1.set_title('Single Link\n(No significant losses)')

# MLO average
ax2 = axes[1]
avg_phy = df_mlo['phy_pct'].mean()
avg_mac = df_mlo['mac_pct'].mean()
avg_wifi = df_mlo['wifi_queue_pct'].mean()
avg_tc = df_mlo['tc_pct'].mean()
avg_unacc = df_mlo['unaccounted_pct'].mean()

sizes = [avg_phy, avg_mac, avg_wifi, avg_tc, avg_unacc]

# Filter out zero values
filtered = [(s, l, c) for s, l, c in zip(sizes, labels, colors) if s > 0.1]
if filtered:
    sizes_f, labels_f, colors_f = zip(*filtered)
    wedges, texts, autotexts = ax2.pie(sizes_f, labels=labels_f, colors=colors_f, autopct='%1.1f%%',
                                        startangle=90, pctdistance=0.75)
    ax2.set_title('MLO (Multi-Link)\nAverage Loss Distribution')
else:
    ax2.text(0.5, 0.5, 'No Losses', ha='center', va='center')
    ax2.set_title('MLO\n(No significant losses)')

fig.suptitle(f'Packet Loss Distribution by Layer - Data Rate: {data_rate}', fontsize=14, fontweight='bold')
plt.tight_layout()
plt.savefig(os.path.join(out_dir, 'loss_distribution_pie_charts.png'), dpi=150)
print('Pie charts saved to:', os.path.join(out_dir, 'loss_distribution_pie_charts.png'))
plt.close()

print("\nAll packet loss breakdown plots generated successfully!")
