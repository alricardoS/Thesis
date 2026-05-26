#!/usr/bin/env python3
"""
generate_single_link_loss_breakdown_plots.py
Adapter para gerar gráficos de loss breakdown só para single link
(sem MLO)
"""
import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

if len(sys.argv) < 3:
    sys.exit("Usage: python3 generate_single_link_loss_breakdown_plots.py <single_csv> <out_dir> [data_rate] [single_phy_reason_csv]")

single_csv = sys.argv[1]
out_dir = sys.argv[2]
data_rate = sys.argv[3] if len(sys.argv) >= 4 else "Unknown"
single_phy_reason_csv = sys.argv[4] if len(sys.argv) >= 5 else None

os.makedirs(out_dir, exist_ok=True)

# Load data
if not os.path.exists(single_csv):
    print(f"Error: {single_csv} not found")
    sys.exit(1)

df_single = pd.read_csv(single_csv)

df_single_phy_reason = None
if single_phy_reason_csv and os.path.exists(single_phy_reason_csv):
    try:
        df_single_phy_reason = pd.read_csv(single_phy_reason_csv)
    except Exception as e:
        print(f"Warning: Could not load PHY reason CSV: {e}")

# Colors for each layer
COLORS = {
    'PHY': '#e74c3c',        # Red
    'MAC': '#f39c12',        # Orange
    'WiFi Queue': '#3498db',  # Blue
    'Traffic Control': '#9b59b6',  # Purple
    'Unaccounted': '#95a5a6'  # Gray
}

# ========== CALCULATE BREAKDOWN ==========
df_single['phy_total'] = df_single['phy_tx_drop'].astype(int) + df_single['phy_rx_drop'].astype(int)
df_single['mac_total'] = df_single['mac_tx_drop'].astype(int) + df_single['mac_rx_drop'].astype(int)
df_single['wifi_queue_total'] = df_single['wifi_queue_drop'].astype(int)
df_single['tc_total'] = (
    df_single['tc_drop_before'].astype(int) + 
    df_single['tc_drop_after'].astype(int) + 
    df_single['tc_drop'].astype(int)
)

# Calculate percentages
for idx, row in df_single.iterrows():
    total = row['total_granular']
    if total > 0:
        df_single.at[idx, 'phy_pct'] = (row['phy_total'] * 100.0) / total
        df_single.at[idx, 'mac_pct'] = (row['mac_total'] * 100.0) / total
        df_single.at[idx, 'wifi_queue_pct'] = (row['wifi_queue_total'] * 100.0) / total
        df_single.at[idx, 'tc_pct'] = (row['tc_total'] * 100.0) / total
        df_single.at[idx, 'unaccounted_pct'] = 100 - (
            df_single.at[idx, 'phy_pct'] + 
            df_single.at[idx, 'mac_pct'] + 
            df_single.at[idx, 'wifi_queue_pct'] + 
            df_single.at[idx, 'tc_pct']
        )
    else:
        df_single.at[idx, 'phy_pct'] = 0.0
        df_single.at[idx, 'mac_pct'] = 0.0
        df_single.at[idx, 'wifi_queue_pct'] = 0.0
        df_single.at[idx, 'tc_pct'] = 0.0
        df_single.at[idx, 'unaccounted_pct'] = 0.0

# ========== GENERATE TABLE ==========
output_file = os.path.join(out_dir, 'loss_breakdown_table.txt')
with open(output_file, 'w') as f:
    f.write("=" * 90 + "\n")
    f.write(f"PACKET LOSS BREAKDOWN TABLE - Data Rate: {data_rate}\n")
    f.write("=" * 90 + "\n\n")
    
    # Single Link Table
    f.write("SINGLE LINK (SLO)\n")
    f.write("-" * 90 + "\n")
    f.write(f"{'Band':<12} {'PHY Drops':<12} {'MAC Drops':<12} {'WiFi Queue':<12} {'TC Drops':<15} {'Total':<12} {'E2E Lost':<12}\n")
    f.write("-" * 90 + "\n")
    
    for _, row in df_single.iterrows():
        f.write(f"{row['freq_band']:<12} {int(row['phy_total']):<12} {int(row['mac_total']):<12} {int(row['wifi_queue_total']):<12} {int(row['tc_total']):<15} {int(row['total_granular']):<12} {int(row['e2e_lost']):<12}\n")
    
    f.write("\nPercentages (relative to Total Granular Drops):\n")
    f.write(f"{'Band':<12} {'PHY %':<12} {'MAC %':<12} {'WiFi Q %':<12} {'TC %':<12} {'Unaccounted %':<15}\n")
    f.write("-" * 75 + "\n")
    for _, row in df_single.iterrows():
        f.write(f"{row['freq_band']:<12} {row['phy_pct']:<12.2f} {row['mac_pct']:<12.2f} {row['wifi_queue_pct']:<12.2f} {row['tc_pct']:<12.2f} {row['unaccounted_pct']:<15.2f}\n")
    
    f.write("\n" + "=" * 90 + "\n")
    f.write("LEGEND:\n")
    f.write("  PHY Drops: Packets dropped at Physical layer (TX interference + RX low SNR/collision)\n")
    f.write("  MAC Drops: Packets dropped at MAC layer (TX retry limit exceeded + RX CRC errors)\n")
    f.write("  WiFi Queue: Packets dropped from WiFi MAC queue (queue full or expired)\n")
    f.write("  TC Drops: Packets dropped at Traffic Control layer (queue overflow before reaching WiFi)\n")
    f.write("  Unaccounted: Remaining percentage to close 100% (typically rounding residue)\n")
    
    # PHY RX drop reasons if available
    if df_single_phy_reason is not None and not df_single_phy_reason.empty:
        f.write("\n" + "=" * 90 + "\n")
        f.write("PHY RX DROP REASONS - SINGLE LINK\n")
        f.write("-" * 90 + "\n")
        f.write(f"{'Band':<12} {'Protocol':<10} {'Reason':<40} {'Count':<12} {'% of PhyRx':<12}\n")
        f.write("-" * 90 + "\n")
        
        for _, row in df_single_phy_reason.sort_values(by=['freq_band', 'protocol', 'count'], ascending=[True, True, False]).iterrows():
            reason = str(row['reason'])[:38] if 'reason' in row else "N/A"
            f.write(f"{row['freq_band']:<12} {row['protocol']:<10} {reason:<40} {int(row['count']):<12} {float(row.get('pct_of_phy_rx', 0)):<12.2f}\n")

print(f"✓ Loss breakdown table generated: {output_file}")

# ========== STACKED BAR CHART - SINGLE LINK ==========
fig, ax = plt.subplots(figsize=(12, 6))

bands = df_single['freq_band'].values
phy_pcts = df_single['phy_pct'].values
mac_pcts = df_single['mac_pct'].values
wifi_pcts = df_single['wifi_queue_pct'].values
tc_pcts = df_single['tc_pct'].values
unacc_pcts = df_single['unaccounted_pct'].values

x = np.arange(len(bands))
width = 0.6

p1 = ax.bar(x, phy_pcts, width, label='PHY', color=COLORS['PHY'])
p2 = ax.bar(x, mac_pcts, width, bottom=phy_pcts, label='MAC', color=COLORS['MAC'])
p3 = ax.bar(x, wifi_pcts, width, bottom=phy_pcts+mac_pcts, label='WiFi Queue', color=COLORS['WiFi Queue'])
p4 = ax.bar(x, tc_pcts, width, bottom=phy_pcts+mac_pcts+wifi_pcts, label='Traffic Control', color=COLORS['Traffic Control'])
p5 = ax.bar(x, unacc_pcts, width, bottom=phy_pcts+mac_pcts+wifi_pcts+tc_pcts, label='Unaccounted', color=COLORS['Unaccounted'])

ax.set_ylabel('Percentage (%)', fontsize=12)
ax.set_xlabel('Frequency Band', fontsize=12)
ax.set_title(f'Single Link Packet Loss Breakdown - {data_rate}', fontsize=14, fontweight='bold')
ax.set_xticks(x)
ax.set_xticklabels(bands)
ax.legend(loc='upper right')
ax.set_ylim([0, 100])

# Add percentage labels
for i, band in enumerate(bands):
    y_offset = 0
    percentages = [phy_pcts[i], mac_pcts[i], wifi_pcts[i], tc_pcts[i], unacc_pcts[i]]
    for pct in percentages:
        if pct > 3:
            ax.text(i, y_offset + pct/2, f'{pct:.1f}%', ha='center', va='center', fontweight='bold', fontsize=9)
        y_offset += pct

plt.tight_layout()
plt.savefig(os.path.join(out_dir, 'single_link_loss_breakdown_stacked.png'), dpi=150)
print(f"✓ Stacked loss breakdown chart generated: {os.path.join(out_dir, 'single_link_loss_breakdown_stacked.png')}")
plt.close()

# ========== PIE CHARTS - LOSS DISTRIBUTION ==========
fig, axes = plt.subplots(1, len(bands), figsize=(6*len(bands), 5))
if len(bands) == 1:
    axes = [axes]

for idx, band in enumerate(bands):
    row = df_single[df_single['freq_band'] == band].iloc[0]
    
    sizes = [row['phy_total'], row['mac_total'], row['wifi_queue_total'], row['tc_total']]
    labels = ['PHY', 'MAC', 'WiFi Queue', 'TC']
    colors_list = [COLORS['PHY'], COLORS['MAC'], COLORS['WiFi Queue'], COLORS['Traffic Control']]
    
    # Filter out zero values
    sizes_filtered = []
    labels_filtered = []
    colors_filtered = []
    for s, l, c in zip(sizes, labels, colors_list):
        if s > 0:
            sizes_filtered.append(s)
            labels_filtered.append(l)
            colors_filtered.append(c)
    
    if sizes_filtered:
        axes[idx].pie(sizes_filtered, labels=labels_filtered, colors=colors_filtered, autopct='%1.1f%%', startangle=90)
        axes[idx].set_title(f'{band}\nTotal Drops: {int(row["total_granular"])}', fontweight='bold')
    else:
        axes[idx].text(0.5, 0.5, 'No Drops', ha='center', va='center', fontsize=12)
        axes[idx].set_title(f'{band}\n(No Drops)', fontweight='bold')

plt.suptitle(f'Packet Loss Distribution by Layer - {data_rate}', fontsize=14, fontweight='bold', y=1.02)
plt.tight_layout()
plt.savefig(os.path.join(out_dir, 'loss_distribution_pie_charts.png'), dpi=150, bbox_inches='tight')
print(f"✓ Loss distribution pie charts generated: {os.path.join(out_dir, 'loss_distribution_pie_charts.png')}")
plt.close()

# ========== PHY RX DROP REASONS - TOP 10 ==========
if df_single_phy_reason is not None and not df_single_phy_reason.empty:
    # Group by reason and sum counts
    reason_totals = df_single_phy_reason.groupby('reason')['count'].sum().sort_values(ascending=False).head(10)
    
    if not reason_totals.empty:
        fig, ax = plt.subplots(figsize=(12, 6))
        
        reasons = [r[:50] for r in reason_totals.index]  # Truncate long reasons
        counts = reason_totals.values
        
        bars = ax.barh(reasons, counts, color=COLORS['PHY'])
        ax.set_xlabel('Count', fontsize=12)
        ax.set_title(f'Single Link PHY RX Drop Reasons (Top 10) - {data_rate}', fontsize=14, fontweight='bold')
        
        # Add count labels
        for bar in bars:
            width = bar.get_width()
            ax.text(width, bar.get_y() + bar.get_height()/2, f'{int(width)}', 
                   ha='left', va='center', fontsize=10)
        
        plt.tight_layout()
        plt.savefig(os.path.join(out_dir, 'single_link_phy_rx_reason_top10.png'), dpi=150)
        print(f"✓ PHY RX reason chart generated: {os.path.join(out_dir, 'single_link_phy_rx_reason_top10.png')}")
        plt.close()

print("\n✓ All single-link loss breakdown plots generated successfully!")
