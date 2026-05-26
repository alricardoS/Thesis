#!/usr/bin/env python3
"""
generate_multi_sta_individual_plots.py
Gera gráficos individuais por STA para testes multi-STA
Mostra métricas (throughput, delay, jitter, packet loss) para cada STA separadamente
"""
import os
import glob
import re
import sys
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

BASE_DIR = "/home/ricardosantos/ns-3.47"
RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE_DIR, "results_multi_sta")
OUTPUT_DIR = os.path.join(RESULTS_DIR, "individual_sta_plots")

os.makedirs(OUTPUT_DIR, exist_ok=True)

# Frequency bands and pairs
FREQ_BANDS = ["2", "5", "6"]
FREQ_BAND_LABELS = {"2": "2.4GHz", "5": "5GHz", "6": "6GHz"}
FREQ_PAIRS = [("2", "5"), ("2", "6"), ("5", "6")]
FREQ_PAIR_LABELS = {("2", "5"): "2.4+5", ("2", "6"): "2.4+6", ("5", "6"): "5+6"}

def list_output_dirs():
    return sorted(glob.glob(os.path.join(RESULTS_DIR, "outputs_*")))

# Colors for STAs
STA_COLORS = ['#E74C3C', '#3498DB', '#2ECC71', '#F39C12']  # Red, Blue, Green, Orange
STA_HATCHES = ['//', 'xx', '..', '\\\\']

def extract_sta_data(output_file):
    """Extract per-STA data from an output file"""
    sta_data = []
    
    if not os.path.exists(output_file):
        return sta_data
    
    with open(output_file, 'r') as f:
        content = f.read()
    
    # Pattern for individual STA lines
    pattern = r'FLOW_SUMMARY_STA(\d+):.*?Throughput_Mbps=([0-9.]+).*?Delay_ms=([0-9.]+).*?Jitter_ms=([0-9.]+).*?LossRate_pct=([0-9.]+)'
    
    matches = re.findall(pattern, content)
    for match in matches:
        sta_id = int(match[0])
        sta_data.append({
            'sta_id': sta_id,
            'throughput_mbps': float(match[1]),
            'delay_ms': float(match[2]),
            'jitter_ms': float(match[3]),
            'loss_rate_pct': float(match[4])
        })
    
    return sorted(sta_data, key=lambda x: x['sta_id'])

def find_output_file(outputs_dir, pattern):
    """Find the most recent output file matching the pattern"""
    files = glob.glob(os.path.join(outputs_dir, pattern))
    if not files:
        return None
    return max(files, key=os.path.getmtime)

def add_bar_labels(ax, bars, fmt='.2f'):
    """Add exact value labels on top of bars"""
    for bar in bars:
        height = bar.get_height()
        if height > 0:
            ax.annotate(f'{height:{fmt}}',
                        xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3),
                        textcoords="offset points",
                        ha='center', va='bottom', fontsize=8, fontweight='bold')

def load_all_single_link_data():
    """Load all single link per-STA data"""
    all_data = []
    
    for outputs_dir in list_output_dirs():
        scenario = os.path.basename(outputs_dir).replace("outputs_", "")

        for freq in FREQ_BANDS:
            pattern = f"single_multi_sta_{freq}_*_*.txt"
            for output_file in glob.glob(os.path.join(outputs_dir, pattern)):
                match = re.search(rf"single_multi_sta_{freq}_([A-Za-z0-9]+)_", os.path.basename(output_file))
                if not match:
                    continue
                proto = match.group(1)

                sta_data = extract_sta_data(output_file)
                freq_label = FREQ_BAND_LABELS[freq]

                for sta in sta_data:
                    all_data.append({
                        'scenario': scenario,
                        'freq_band': freq_label,
                        'protocol': proto,
                        'sta_id': sta['sta_id'],
                        'sta_label': f"STA{sta['sta_id']}",
                        'throughput_mbps': sta['throughput_mbps'],
                        'delay_ms': sta['delay_ms'],
                        'jitter_ms': sta['jitter_ms'],
                        'loss_rate_pct': sta['loss_rate_pct']
                    })
    
    return pd.DataFrame(all_data) if all_data else None

def load_all_mlo_data():
    """Load all MLO per-STA data"""
    all_data = []
    
    for outputs_dir in list_output_dirs():
        scenario = os.path.basename(outputs_dir).replace("outputs_", "")

        for f1, f2 in FREQ_PAIRS:
            pattern = f"mlo_multi_sta_{f1}_{f2}_*_*.txt"
            for output_file in glob.glob(os.path.join(outputs_dir, pattern)):
                match = re.search(rf"mlo_multi_sta_{f1}_{f2}_([A-Za-z0-9]+)_", os.path.basename(output_file))
                if not match:
                    continue
                proto = match.group(1)

                sta_data = extract_sta_data(output_file)
                pair_label = FREQ_PAIR_LABELS[(f1, f2)]

                for sta in sta_data:
                    all_data.append({
                        'scenario': scenario,
                        'freq_pair': pair_label,
                        'protocol': proto,
                        'sta_id': sta['sta_id'],
                        'sta_label': f"STA{sta['sta_id']}",
                        'throughput_mbps': sta['throughput_mbps'],
                        'delay_ms': sta['delay_ms'],
                        'jitter_ms': sta['jitter_ms'],
                        'loss_rate_pct': sta['loss_rate_pct']
                    })
    
    return pd.DataFrame(all_data) if all_data else None

def create_individual_sta_plot(df, metric, ylabel, title_prefix, filename_prefix, 
                                group_col, is_mlo=False):
    """Create plots showing individual STA metrics side by side"""

    protocols = sorted(df['protocol'].dropna().unique())
    scenarios = sorted(df['scenario'].dropna().unique())

    for proto in protocols:
        df_proto = df[df['protocol'] == proto]
        if df_proto.empty:
            continue
        
        groups = df_proto[group_col].unique()
        sta_ids = sorted(df_proto['sta_id'].unique())
        num_stas = len(sta_ids)
        
        # Create plot for each scenario
        for scenario in scenarios:
            df_scenario = df_proto[df_proto['scenario'] == scenario]
            if df_scenario.empty:
                continue
            
            fig, ax = plt.subplots(figsize=(12, 7))
            
            x = np.arange(len(groups))
            width = 0.35 / num_stas  # Adjust width based on number of STAs
            total_width = width * num_stas
            
            for i, sta_id in enumerate(sta_ids):
                vals = []
                for g in groups:
                    row = df_scenario[(df_scenario[group_col] == g) & (df_scenario['sta_id'] == sta_id)]
                    if not row.empty:
                        vals.append(float(row[metric].values[0]))
                    else:
                        vals.append(0.0)
                
                offset = (i - (num_stas - 1) / 2) * width
                bars = ax.bar(x + offset, vals, width=width * 0.9, 
                             label=f'STA{sta_id}', 
                             color=STA_COLORS[i % len(STA_COLORS)],
                             hatch=STA_HATCHES[i % len(STA_HATCHES)],
                             edgecolor='black', linewidth=0.5)
                
                fmt = '.0f' if 'throughput' in metric else '.2f'
                add_bar_labels(ax, bars, fmt)
            
            ax.set_xticks(x)
            ax.set_xticklabels(groups, fontsize=12, fontweight='bold')
            ax.set_xlabel('Frequency Band' if not is_mlo else 'Frequency Pair', 
                         fontsize=12, fontweight='bold')
            ax.set_ylabel(ylabel, fontsize=12, fontweight='bold')
            ax.set_title(f'{title_prefix} - {proto} @ {scenario}\n(Individual STA Metrics)', 
                        fontsize=14, fontweight='bold')
            ax.legend(title='Station', fontsize=10)
            ax.grid(axis='y', alpha=0.3)
            
            plt.tight_layout()
            
            prefix = "mlo" if is_mlo else "single"
            output_file = os.path.join(OUTPUT_DIR, 
                f'{prefix}_individual_{filename_prefix}_{proto.lower()}_{scenario}.png')
            plt.savefig(output_file, dpi=150)
            print(f'Generated: {output_file}')
            plt.close()

def create_sta_comparison_across_datarates(df, metric, ylabel, title_prefix, 
                                            filename_prefix, group_col, is_mlo=False):
    """Create plots comparing each STA across all data rates"""
    
    groups = df[group_col].unique()
    sta_ids = sorted(df['sta_id'].unique())
    
    protocols = sorted(df['protocol'].dropna().unique())
    scenarios = sorted(df['scenario'].dropna().unique())

    for proto in protocols:
        df_proto = df[df['protocol'] == proto]
        if df_proto.empty:
            continue
        
        for group in groups:
            df_group = df_proto[df_proto[group_col] == group]
            if df_group.empty:
                continue
            
            fig, ax = plt.subplots(figsize=(12, 7))
            
            x = np.arange(len(scenarios))
            num_stas = len(sta_ids)
            width = 0.6 / num_stas
            
            for i, sta_id in enumerate(sta_ids):
                vals = []
                for scenario in scenarios:
                    row = df_group[(df_group['scenario'] == scenario) & (df_group['sta_id'] == sta_id)]
                    if not row.empty:
                        vals.append(float(row[metric].values[0]))
                    else:
                        vals.append(0.0)
                
                offset = (i - (num_stas - 1) / 2) * width
                bars = ax.bar(x + offset, vals, width=width * 0.9,
                             label=f'STA{sta_id}',
                             color=STA_COLORS[i % len(STA_COLORS)],
                             hatch=STA_HATCHES[i % len(STA_HATCHES)],
                             edgecolor='black', linewidth=0.5)
                
                fmt = '.0f' if 'throughput' in metric else '.2f'
                add_bar_labels(ax, bars, fmt)
            
            ax.set_xticks(x)
            ax.set_xticklabels(scenarios, fontsize=12, fontweight='bold', rotation=20, ha='right')
            ax.set_xlabel('Scenario', fontsize=12, fontweight='bold')
            ax.set_ylabel(ylabel, fontsize=12, fontweight='bold')
            ax.set_title(f'{title_prefix} - {proto} @ {group}\n(Per-STA Comparison Across Scenarios)', 
                        fontsize=14, fontweight='bold')
            ax.legend(title='Station', fontsize=10)
            ax.grid(axis='y', alpha=0.3)
            
            plt.tight_layout()
            
            prefix = "mlo" if is_mlo else "single"
            group_safe = group.replace('+', '_').replace('.', '_')
            output_file = os.path.join(OUTPUT_DIR, 
                f'{prefix}_sta_comparison_{filename_prefix}_{proto.lower()}_{group_safe}.png')
            plt.savefig(output_file, dpi=150)
            print(f'Generated: {output_file}')
            plt.close()

def main():
    print("=" * 80)
    print("GENERATING INDIVIDUAL PER-STA PLOTS")
    print("=" * 80)
    
    # ========== SINGLE LINK ==========
    print("\n--- Single Link Individual STA Plots ---")
    df_single = load_all_single_link_data()
    
    if df_single is not None and not df_single.empty:
        print(f"Loaded {len(df_single)} rows of single link data")
        print(f"STAs found: {sorted(df_single['sta_id'].unique())}")
        
        # Individual STA plots per data rate
        create_individual_sta_plot(df_single, 'throughput_mbps', 'Throughput (Mbps)',
                                   'Single Link Throughput', 'throughput', 'freq_band')
        create_individual_sta_plot(df_single, 'delay_ms', 'Delay (ms)',
                                   'Single Link Delay', 'delay', 'freq_band')
        create_individual_sta_plot(df_single, 'jitter_ms', 'Jitter (ms)',
                                   'Single Link Jitter', 'jitter', 'freq_band')
        create_individual_sta_plot(df_single, 'loss_rate_pct', 'Packet Loss (%)',
                                   'Single Link Packet Loss', 'loss', 'freq_band')
        
        # STA comparison across data rates
        create_sta_comparison_across_datarates(df_single, 'throughput_mbps', 'Throughput (Mbps)',
                                               'Single Link Throughput', 'throughput', 'freq_band')
        create_sta_comparison_across_datarates(df_single, 'delay_ms', 'Delay (ms)',
                                               'Single Link Delay', 'delay', 'freq_band')
    else:
        print("No single link data found!")
    
    # ========== MLO ==========
    print("\n--- MLO Individual STA Plots ---")
    df_mlo = load_all_mlo_data()
    
    if df_mlo is not None and not df_mlo.empty:
        print(f"Loaded {len(df_mlo)} rows of MLO data")
        print(f"STAs found: {sorted(df_mlo['sta_id'].unique())}")
        
        # Individual STA plots per data rate
        create_individual_sta_plot(df_mlo, 'throughput_mbps', 'Throughput (Mbps)',
                                   'MLO Throughput', 'throughput', 'freq_pair', is_mlo=True)
        create_individual_sta_plot(df_mlo, 'delay_ms', 'Delay (ms)',
                                   'MLO Delay', 'delay', 'freq_pair', is_mlo=True)
        create_individual_sta_plot(df_mlo, 'jitter_ms', 'Jitter (ms)',
                                   'MLO Jitter', 'jitter', 'freq_pair', is_mlo=True)
        create_individual_sta_plot(df_mlo, 'loss_rate_pct', 'Packet Loss (%)',
                                   'MLO Packet Loss', 'loss', 'freq_pair', is_mlo=True)
        
        # STA comparison across data rates
        create_sta_comparison_across_datarates(df_mlo, 'throughput_mbps', 'Throughput (Mbps)',
                                               'MLO Throughput', 'throughput', 'freq_pair', is_mlo=True)
        create_sta_comparison_across_datarates(df_mlo, 'delay_ms', 'Delay (ms)',
                                               'MLO Delay', 'delay', 'freq_pair', is_mlo=True)
    else:
        print("No MLO data found!")
    
    print("\n" + "=" * 80)
    print(f"All individual STA plots saved to: {OUTPUT_DIR}")
    print("=" * 80)

if __name__ == "__main__":
    main()
