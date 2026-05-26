#!/usr/bin/env python3
import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

if len(sys.argv) < 3:
    sys.exit("Usage: python3 generate_single_plots.py <csv> <out_dir> [data_rate]")

csv_file = sys.argv[1]
out_dir = sys.argv[2]
data_rate = sys.argv[3] if len(sys.argv) >= 4 else "Unknown"

os.makedirs(out_dir, exist_ok=True)

df = pd.read_csv(csv_file)

bands = df['freq_band'].unique()
protocols = df['protocol'].unique()
x = np.arange(len(bands))
width = 0.35

def add_bar_labels(ax, bars):
    """Add exact value labels on top of bars"""
    for bar in bars:
        height = bar.get_height()
        ax.annotate(f'{height:.2f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),  # 3 points vertical offset
                    textcoords="offset points",
                    ha='center', va='bottom', fontsize=8)

# ========== THROUGHPUT PLOT ==========
fig, ax = plt.subplots(figsize=(10, 6))
for i, proto in enumerate(protocols):
    vals = []
    for b in bands:
        row = df[(df['freq_band'] == b) & (df['protocol'] == proto)]
        if not row.empty:
            vals.append(float(row['throughput_mbps'].values[0]))
        else:
            vals.append(0.0)
    bars = ax.bar(x + i * width, vals, width=width, label=proto)
    add_bar_labels(ax, bars)

ax.set_xticks(x + width * (len(protocols) - 1) / 2)
ax.set_xticklabels(bands)
ax.set_ylabel('Throughput (Mbps)')
ax.set_title(f'WiFi 7 Single Link Throughput Comparison (802.11be) - Data Rate: {data_rate}')
ax.legend()
plt.tight_layout()
plt.savefig(os.path.join(out_dir, 'single_link_throughput.png'))
print('Throughput Plot generated:', os.path.join(out_dir, 'single_link_throughput.png'))
plt.close()

# ========== DELAY PLOT ==========
if 'delay_ms' in df.columns:
    fig, ax = plt.subplots(figsize=(10, 6))
    for i, proto in enumerate(protocols):
        vals = []
        for b in bands:
            row = df[(df['freq_band'] == b) & (df['protocol'] == proto)]
            if not row.empty:
                vals.append(float(row['delay_ms'].values[0]))
            else:
                vals.append(0.0)
        bars = ax.bar(x + i * width, vals, width=width, label=proto)
        add_bar_labels(ax, bars)

    ax.set_xticks(x + width * (len(protocols) - 1) / 2)
    ax.set_xticklabels(bands)
    ax.set_ylabel('Delay (ms)')
    ax.set_title(f'WiFi 7 Single Link Delay Comparison (802.11be) - Data Rate: {data_rate}')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'single_link_delay.png'))
    print('Delay Plot generated:', os.path.join(out_dir, 'single_link_delay.png'))
    plt.close()

# ========== JITTER PLOT ==========
if 'jitter_ms' in df.columns:
    fig, ax = plt.subplots(figsize=(10, 6))
    for i, proto in enumerate(protocols):
        vals = []
        for b in bands:
            row = df[(df['freq_band'] == b) & (df['protocol'] == proto)]
            if not row.empty:
                vals.append(float(row['jitter_ms'].values[0]))
            else:
                vals.append(0.0)
        bars = ax.bar(x + i * width, vals, width=width, label=proto)
        add_bar_labels(ax, bars)

    ax.set_xticks(x + width * (len(protocols) - 1) / 2)
    ax.set_xticklabels(bands)
    ax.set_ylabel('Jitter (ms)')
    ax.set_title(f'WiFi 7 Single Link Jitter Comparison (802.11be) - Data Rate: {data_rate}')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'single_link_jitter.png'))
    print('Jitter Plot generated:', os.path.join(out_dir, 'single_link_jitter.png'))
    plt.close()

# ========== PACKET LOSS PLOT ==========
if 'loss_rate_pct' in df.columns:
    fig, ax = plt.subplots(figsize=(10, 6))
    for i, proto in enumerate(protocols):
        vals = []
        for b in bands:
            row = df[(df['freq_band'] == b) & (df['protocol'] == proto)]
            if not row.empty:
                vals.append(float(row['loss_rate_pct'].values[0]))
            else:
                vals.append(0.0)
        bars = ax.bar(x + i * width, vals, width=width, label=proto)
        add_bar_labels(ax, bars)

    ax.set_xticks(x + width * (len(protocols) - 1) / 2)
    ax.set_xticklabels(bands)
    ax.set_ylabel('Packet Loss Rate (%)')
    ax.set_title(f'WiFi 7 Single Link Packet Loss Comparison (802.11be) - Data Rate: {data_rate}')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'single_link_packet_loss.png'))
    print('Packet Loss Plot generated:', os.path.join(out_dir, 'single_link_packet_loss.png'))
    plt.close()

print('All Single Link plots generated successfully!')
