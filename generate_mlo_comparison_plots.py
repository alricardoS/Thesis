#!/usr/bin/env python3
"""
Generate comparison plots for MLO experiments across multiple data rates.
Each plot shows 4 groups (data rates) with 3 bars each (frequency pairs).
UDP and TCP are shown in separate plots.
"""
import sys
import os
import glob
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

BASE_DIR = "/home/ricardosantos/ns-3.47"
RESULTS_DIR = os.path.join(BASE_DIR, "results")
OUTPUT_DIR = os.path.join(RESULTS_DIR, "comparison_plots_mlo")

os.makedirs(OUTPUT_DIR, exist_ok=True)

# Data rates to look for
DATA_RATES = ["150Mbps", "450Mbps", "1200Mbps", "2000Mbps"]

# Frequency pairs for MLO
FREQ_PAIRS = ["2.4+5", "2.4+6", "5+6"]

# Colors for each frequency pair
COLORS = {
    "2.4+5": "#E74C3C",   # Red
    "2.4+6": "#3498DB",   # Blue
    "5+6": "#2ECC71"      # Green
}

# Hatch patterns for black and white printing
HATCHES = {
    "2.4+5": "//",   # Diagonal lines
    "2.4+6": "xx",   # Cross-hatch
    "5+6": ".."      # Dots
}

def load_all_data():
    """Load data from all data rate CSV files"""
    all_data = []
    
    for dr in DATA_RATES:
        outputs_dir = os.path.join(RESULTS_DIR, f"outputs_{dr}")
        if not os.path.exists(outputs_dir):
            print(f"Warning: Directory not found: {outputs_dir}")
            continue
        
        # Find the most recent mlo_results CSV
        csv_files = glob.glob(os.path.join(outputs_dir, "mlo_results_*.csv"))
        if not csv_files:
            print(f"Warning: No mlo_results CSV found in {outputs_dir}")
            continue
        
        # Use the most recent one
        csv_file = max(csv_files, key=os.path.getmtime)
        print(f"Loading: {csv_file}")
        
        df = pd.read_csv(csv_file)
        df['data_rate'] = dr
        all_data.append(df)
    
    if not all_data:
        sys.exit("Error: No data files found!")
    
    return pd.concat(all_data, ignore_index=True)

def add_bar_labels(ax, bars, fmt='.1f'):
    """Add exact value labels on top of bars"""
    for bar in bars:
        height = bar.get_height()
        if height > 0:
            ax.annotate(f'{height:{fmt}}',
                        xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3),
                        textcoords="offset points",
                        ha='center', va='bottom', fontsize=15, fontweight='bold', rotation=0)

def create_comparison_plot(df, metric, ylabel, title_suffix, filename_suffix):
    """Create comparison plots for a given metric, one for each protocol"""
    protocols = ["UDP", "TCP"]
    
    for proto in protocols:
        fig, ax = plt.subplots(figsize=(12, 7))
        
        x = np.arange(len(DATA_RATES))
        width = 0.25  # Width of each bar
        
        for i, pair in enumerate(FREQ_PAIRS):
            vals = []
            for dr in DATA_RATES:
                row = df[(df['data_rate'] == dr) & 
                        (df['pair'] == pair) & 
                        (df['protocol'] == proto)]
                if not row.empty:
                    vals.append(float(row[metric].values[0]))
                else:
                    vals.append(0.0)
            
            offset = (i - 1) * width  # Center the group of bars
            bars = ax.bar(x + offset, vals, width=width, label=pair, 
                         color=COLORS[pair], hatch=HATCHES[pair], edgecolor='black', linewidth=0.5)
            # Use appropriate format: no decimals for throughput, 2 decimals for jitter/delay/loss
            if 'throughput' in filename_suffix:
                add_bar_labels(ax, bars, fmt='.0f')
            elif 'jitter' in filename_suffix or 'delay' in filename_suffix or 'packet_loss' in filename_suffix:
                add_bar_labels(ax, bars, fmt='.2f')
            else:
                add_bar_labels(ax, bars, fmt='.1f')
        
        ax.set_xticks(x)
        ax.set_xticklabels(DATA_RATES, fontsize=16, fontweight='bold')
        ax.set_xlabel('Data Rate', fontsize=16, fontweight='bold')
        ax.set_ylabel(ylabel, fontsize=16, fontweight='bold')
        ax.tick_params(axis='y', labelsize=16)
        plt.setp(ax.get_yticklabels(), fontweight='bold')
        ax.legend(title='Frequency Pair', fontsize=17, title_fontsize=16)
        ax.grid(axis='y', alpha=0.3)
        
        plt.tight_layout()
        output_file = os.path.join(OUTPUT_DIR, f'mlo_comparison_{filename_suffix}_{proto.lower()}.png')
        plt.savefig(output_file, dpi=150)
        print(f'Generated: {output_file}')
        plt.close()

def main():
    print("=" * 50)
    print("Generating MLO Comparison Plots")
    print("=" * 50)
    
    df = load_all_data()
    print(f"\nLoaded {len(df)} rows of data")
    print(f"Data rates found: {df['data_rate'].unique()}")
    print(f"Frequency pairs found: {df['pair'].unique()}")
    print(f"Protocols found: {df['protocol'].unique()}")
    print()
    
    # Generate Throughput plots
    create_comparison_plot(df, 'throughput_mbps', 'Throughput (Mbps)', 
                          'Throughput Comparison', 'throughput')
    
    # Generate Delay plots
    if 'delay_ms' in df.columns:
        create_comparison_plot(df, 'delay_ms', 'Delay (ms)', 
                              'Delay Comparison', 'delay')
    
    # Generate Jitter plots
    if 'jitter_ms' in df.columns:
        create_comparison_plot(df, 'jitter_ms', 'Jitter (ms)', 
                              'Jitter Comparison', 'jitter')
    
    # Generate Packet Loss plots
    if 'loss_rate_pct' in df.columns:
        create_comparison_plot(df, 'loss_rate_pct', 'Packet Loss Rate (%)', 
                              'Packet Loss Comparison', 'packet_loss')
    
    print()
    print("=" * 50)
    print(f"All plots saved to: {OUTPUT_DIR}")
    print("=" * 50)

if __name__ == "__main__":
    main()
