#!/usr/bin/env python3
"""
generate_multi_sta_mlo_plots.py
Gera gráficos para testes MLO com múltiplas STAs
Estilo igual aos scripts originais + gráficos per-STA
"""
import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

if len(sys.argv) < 3:
    sys.exit("Usage: python3 generate_multi_sta_mlo_plots.py <csv> <out_dir> [data_rate]")

csv_file = sys.argv[1]
out_dir = sys.argv[2]
data_rate = sys.argv[3] if len(sys.argv) >= 4 else "Unknown"

os.makedirs(out_dir, exist_ok=True)

df = pd.read_csv(csv_file)

pairs = df['pair'].unique()
protocols = df['protocol'].unique()
x = np.arange(len(pairs))
width = 0.35

def add_bar_labels(ax, bars):
    """Add exact value labels on top of bars"""
    for bar in bars:
        height = bar.get_height()
        ax.annotate(f'{height:.2f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha='center', va='bottom', fontsize=8)

# ========== THROUGHPUT PLOT (estilo original) ==========
fig, ax = plt.subplots(figsize=(10, 6))
for i, proto in enumerate(protocols):
    vals = []
    for p in pairs:
        row = df[(df['pair'] == p) & (df['protocol'] == proto)]
        if not row.empty:
            vals.append(float(row['throughput_mbps'].values[0]))
        else:
            vals.append(0.0)
    bars = ax.bar(x + i * width, vals, width=width, label=proto)
    add_bar_labels(ax, bars)

ax.set_xticks(x + width * (len(protocols) - 1) / 2)
ax.set_xticklabels(pairs)
ax.set_ylabel('Throughput (Mbps)')
ax.set_title(f'WiFi 7 MLO Throughput Comparison: UDP vs TCP - Data Rate: {data_rate}')
ax.legend()
plt.tight_layout()
plt.savefig(os.path.join(out_dir, 'mlo_throughput.png'))
print('Throughput Plot generated:', os.path.join(out_dir, 'mlo_throughput.png'))
plt.close()

# ========== DELAY PLOT (estilo original) ==========
if 'delay_ms' in df.columns:
    fig, ax = plt.subplots(figsize=(10, 6))
    for i, proto in enumerate(protocols):
        vals = []
        for p in pairs:
            row = df[(df['pair'] == p) & (df['protocol'] == proto)]
            if not row.empty:
                vals.append(float(row['delay_ms'].values[0]))
            else:
                vals.append(0.0)
        bars = ax.bar(x + i * width, vals, width=width, label=proto)
        add_bar_labels(ax, bars)

    ax.set_xticks(x + width * (len(protocols) - 1) / 2)
    ax.set_xticklabels(pairs)
    ax.set_ylabel('Delay (ms)')
    ax.set_title(f'WiFi 7 MLO Delay Comparison: UDP vs TCP - Data Rate: {data_rate}')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'mlo_delay.png'))
    print('Delay Plot generated:', os.path.join(out_dir, 'mlo_delay.png'))
    plt.close()

# ========== JITTER PLOT (estilo original) ==========
if 'jitter_ms' in df.columns:
    fig, ax = plt.subplots(figsize=(10, 6))
    for i, proto in enumerate(protocols):
        vals = []
        for p in pairs:
            row = df[(df['pair'] == p) & (df['protocol'] == proto)]
            if not row.empty:
                vals.append(float(row['jitter_ms'].values[0]))
            else:
                vals.append(0.0)
        bars = ax.bar(x + i * width, vals, width=width, label=proto)
        add_bar_labels(ax, bars)

    ax.set_xticks(x + width * (len(protocols) - 1) / 2)
    ax.set_xticklabels(pairs)
    ax.set_ylabel('Jitter (ms)')
    ax.set_title(f'WiFi 7 MLO Jitter Comparison: UDP vs TCP - Data Rate: {data_rate}')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'mlo_jitter.png'))
    print('Jitter Plot generated:', os.path.join(out_dir, 'mlo_jitter.png'))
    plt.close()

# ========== PACKET LOSS RATE PLOT ==========
if 'loss_rate_pct' in df.columns:
    fig, ax = plt.subplots(figsize=(10, 6))
    for i, proto in enumerate(protocols):
        vals = []
        for p in pairs:
            row = df[(df['pair'] == p) & (df['protocol'] == proto)]
            if not row.empty:
                vals.append(float(row['loss_rate_pct'].values[0]))
            else:
                vals.append(0.0)
        bars = ax.bar(x + i * width, vals, width=width, label=proto)
        add_bar_labels(ax, bars)

    ax.set_xticks(x + width * (len(protocols) - 1) / 2)
    ax.set_xticklabels(pairs)
    ax.set_ylabel('Packet Loss Rate (%)')
    ax.set_title(f'WiFi 7 MLO Packet Loss Rate: UDP vs TCP - Data Rate: {data_rate}')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'mlo_packet_loss.png'))
    print('Packet Loss Plot generated:', os.path.join(out_dir, 'mlo_packet_loss.png'))
    plt.close()

# ========== AVERAGE THROUGHPUT PER STA PLOT ==========
if 'avg_throughput_per_sta_mbps' in df.columns:
    fig, ax = plt.subplots(figsize=(10, 6))
    for i, proto in enumerate(protocols):
        vals = []
        for p in pairs:
            row = df[(df['pair'] == p) & (df['protocol'] == proto)]
            if not row.empty:
                vals.append(float(row['avg_throughput_per_sta_mbps'].values[0]))
            else:
                vals.append(0.0)
        bars = ax.bar(x + i * width, vals, width=width, label=proto)
        add_bar_labels(ax, bars)

    ax.set_xticks(x + width * (len(protocols) - 1) / 2)
    ax.set_xticklabels(pairs)
    ax.set_ylabel('Avg Throughput per STA (Mbps)')
    ax.set_title(f'WiFi 7 MLO Avg Throughput per STA: UDP vs TCP - Data Rate: {data_rate}')
    ax.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(out_dir, 'mlo_avg_throughput_per_sta.png'))
    print('Avg Throughput per STA Plot generated:', os.path.join(out_dir, 'mlo_avg_throughput_per_sta.png'))
    plt.close()

print('All MLO plots generated successfully!')
