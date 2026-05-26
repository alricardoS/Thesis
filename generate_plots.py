#!/usr/bin/env python3
"""
generate_plots.py
Script para gerar gráficos comparativos dos resultados das experiências WiFi 7.

Gera:
- Gráfico de barras comparando throughput entre pares de frequências
- Gráfico de barras comparando delay
- Gráfico de barras comparando jitter
- Gráfico combinado com todas as métricas

Uso:
    python3 generate_plots.py <csv_file> <output_dir> <timestamp>
    
Ou:
    python3 generate_plots.py  (usa ficheiro CSV mais recente)
"""

import sys
import os
import glob
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# Configurar estilo dos gráficos
plt.style.use('seaborn-v0_8-whitegrid')
plt.rcParams['figure.figsize'] = (10, 6)
plt.rcParams['font.size'] = 12
plt.rcParams['axes.titlesize'] = 14
plt.rcParams['axes.labelsize'] = 12

# Cores para cada par de frequências
COLORS = {
    '2.4GHz+5GHz': '#3498db',   # Azul
    '2.4GHz+6GHz': '#2ecc71',   # Verde
    '5GHz+6GHz': '#e74c3c'      # Vermelho
}

def load_data(csv_file):
    """Carregar dados do ficheiro CSV."""
    df = pd.read_csv(csv_file)
    return df

def plot_throughput(df, output_dir, timestamp, data_rate="Unknown"):
    """Gerar gráfico de throughput."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    pairs = df['pair'].tolist()
    throughputs = df['throughput_mbps'].tolist()
    colors = [COLORS.get(p, '#95a5a6') for p in pairs]
    
    bars = ax.bar(pairs, throughputs, color=colors, edgecolor='black', linewidth=1.2)
    
    # Adicionar valores em cima das barras
    for bar, val in zip(bars, throughputs):
        height = bar.get_height()
        ax.annotate(f'{val:.0f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    ax.set_xlabel('Frequency Pair')
    ax.set_ylabel('Throughput (Mbps)')
    ax.set_title(f'WiFi 7 MLO Throughput Comparison by Frequency Pair - Data Rate: {data_rate}')
    ax.set_ylim(0, max(throughputs) * 1.15)
    
    plt.tight_layout()
    filepath = os.path.join(output_dir, f'throughput_{timestamp}.png')
    plt.savefig(filepath, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {filepath}")
    return filepath

def plot_delay(df, output_dir, timestamp, data_rate="Unknown"):
    """Gerar gráfico de delay."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    pairs = df['pair'].tolist()
    delays = df['delay_ms'].tolist()
    colors = [COLORS.get(p, '#95a5a6') for p in pairs]
    
    bars = ax.bar(pairs, delays, color=colors, edgecolor='black', linewidth=1.2)
    
    for bar, val in zip(bars, delays):
        height = bar.get_height()
        ax.annotate(f'{val:.2f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    ax.set_xlabel('Frequency Pair')
    ax.set_ylabel('Delay (ms)')
    ax.set_title(f'WiFi 7 MLO Delay Comparison by Frequency Pair - Data Rate: {data_rate}')
    ax.set_ylim(0, max(delays) * 1.3 if max(delays) > 0 else 1)
    
    plt.tight_layout()
    filepath = os.path.join(output_dir, f'delay_{timestamp}.png')
    plt.savefig(filepath, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {filepath}")
    return filepath

def plot_jitter(df, output_dir, timestamp, data_rate="Unknown"):
    """Gerar gráfico de jitter."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    pairs = df['pair'].tolist()
    jitters = df['jitter_ms'].tolist()
    colors = [COLORS.get(p, '#95a5a6') for p in pairs]
    
    bars = ax.bar(pairs, jitters, color=colors, edgecolor='black', linewidth=1.2)
    
    for bar, val in zip(bars, jitters):
        height = bar.get_height()
        ax.annotate(f'{val:.3f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha='center', va='bottom', fontsize=11, fontweight='bold')
    
    ax.set_xlabel('Frequency Pair')
    ax.set_ylabel('Jitter (ms)')
    ax.set_title(f'WiFi 7 MLO Jitter Comparison by Frequency Pair - Data Rate: {data_rate}')
    ax.set_ylim(0, max(jitters) * 1.3 if max(jitters) > 0 else 0.1)
    
    plt.tight_layout()
    filepath = os.path.join(output_dir, f'jitter_{timestamp}.png')
    plt.savefig(filepath, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {filepath}")
    return filepath

def plot_combined(df, output_dir, timestamp, data_rate="Unknown"):
    """Gerar gráfico combinado com todas as métricas."""
    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    
    pairs = df['pair'].tolist()
    colors = [COLORS.get(p, '#95a5a6') for p in pairs]
    
    # Throughput
    ax1 = axes[0]
    throughputs = df['throughput_mbps'].tolist()
    bars1 = ax1.bar(pairs, throughputs, color=colors, edgecolor='black', linewidth=1.2)
    for bar, val in zip(bars1, throughputs):
        height = bar.get_height()
        ax1.annotate(f'{val:.0f}',
                     xy=(bar.get_x() + bar.get_width() / 2, height),
                     xytext=(0, 3), textcoords="offset points",
                     ha='center', va='bottom', fontsize=10, fontweight='bold')
    ax1.set_ylabel('Throughput (Mbps)')
    ax1.set_title('Throughput')
    ax1.set_ylim(0, max(throughputs) * 1.15)
    ax1.tick_params(axis='x', rotation=15)
    
    # Delay
    ax2 = axes[1]
    delays = df['delay_ms'].tolist()
    bars2 = ax2.bar(pairs, delays, color=colors, edgecolor='black', linewidth=1.2)
    for bar, val in zip(bars2, delays):
        height = bar.get_height()
        ax2.annotate(f'{val:.2f}',
                     xy=(bar.get_x() + bar.get_width() / 2, height),
                     xytext=(0, 3), textcoords="offset points",
                     ha='center', va='bottom', fontsize=10, fontweight='bold')
    ax2.set_ylabel('Delay (ms)')
    ax2.set_title('Delay')
    ax2.set_ylim(0, max(delays) * 1.3 if max(delays) > 0 else 1)
    ax2.tick_params(axis='x', rotation=15)
    
    # Jitter
    ax3 = axes[2]
    jitters = df['jitter_ms'].tolist()
    bars3 = ax3.bar(pairs, jitters, color=colors, edgecolor='black', linewidth=1.2)
    for bar, val in zip(bars3, jitters):
        height = bar.get_height()
        ax3.annotate(f'{val:.3f}',
                     xy=(bar.get_x() + bar.get_width() / 2, height),
                     xytext=(0, 3), textcoords="offset points",
                     ha='center', va='bottom', fontsize=10, fontweight='bold')
    ax3.set_ylabel('Jitter (ms)')
    ax3.set_title('Jitter')
    ax3.set_ylim(0, max(jitters) * 1.3 if max(jitters) > 0 else 0.1)
    ax3.tick_params(axis='x', rotation=15)
    
    fig.suptitle(f'WiFi 7 MLO Performance Comparison by Frequency Pair - Data Rate: {data_rate}', fontsize=14, fontweight='bold')
    plt.tight_layout()
    
    filepath = os.path.join(output_dir, f'combined_{timestamp}.png')
    plt.savefig(filepath, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {filepath}")
    return filepath

def plot_time_series(output_dir, timestamp, data_rate="Unknown"):
    """
    Gerar gráfico de throughput ao longo do tempo.
    Lê os ficheiros de output individuais e extrai TIME_STATS.
    """
    base_dir = os.path.dirname(output_dir)
    outputs_dir = os.path.join(base_dir, 'outputs')
    
    # Procurar ficheiros de output com o mesmo timestamp
    pattern = os.path.join(outputs_dir, f'freq*_{timestamp}.txt')
    files = glob.glob(pattern)
    
    if not files:
        print("  No time series data found.")
        return None
    
    fig, ax = plt.subplots(figsize=(12, 6))
    
    for filepath in sorted(files):
        # Extrair nome do par de frequências do nome do ficheiro
        basename = os.path.basename(filepath)
        # freq2_5_timestamp.txt -> 2_5
        parts = basename.replace(f'_{timestamp}.txt', '').replace('freq', '').split('_')
        if len(parts) >= 2:
            freq1, freq2 = parts[0], parts[1]
            if freq1 == '2':
                freq1_label = '2.4'
            else:
                freq1_label = freq1
            if freq2 == '2':
                freq2_label = '2.4'
            else:
                freq2_label = freq2
            pair_name = f'{freq1_label}GHz+{freq2_label}GHz'
        else:
            pair_name = basename
        
        # Ler TIME_STATS do ficheiro
        times = []
        throughputs = []
        with open(filepath, 'r') as f:
            for line in f:
                if 'TIME_STATS:' in line:
                    parts = line.strip().split()
                    # TIME_STATS: time throughput delay jitter
                    if len(parts) >= 3:
                        try:
                            t = float(parts[1])
                            tp = float(parts[2])
                            times.append(t)
                            throughputs.append(tp)
                        except ValueError:
                            continue
        
        if times and throughputs:
            color = COLORS.get(pair_name, '#95a5a6')
            ax.plot(times, throughputs, marker='o', label=pair_name, color=color, linewidth=2, markersize=6)
    
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Throughput (Mbps)')
    ax.set_title(f'WiFi 7 MLO Throughput Over Time by Frequency Pair - Data Rate: {data_rate}')
    ax.legend(loc='best')
    ax.grid(True, alpha=0.3)
    
    plt.tight_layout()
    filepath = os.path.join(output_dir, f'throughput_timeseries_{timestamp}.png')
    plt.savefig(filepath, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"  Saved: {filepath}")
    return filepath

def main():
    # Determinar argumentos
    data_rate = "Unknown"
    if len(sys.argv) >= 5:
        csv_file = sys.argv[1]
        output_dir = sys.argv[2]
        timestamp = sys.argv[3]
        data_rate = sys.argv[4]
    elif len(sys.argv) >= 4:
        csv_file = sys.argv[1]
        output_dir = sys.argv[2]
        timestamp = sys.argv[3]
    elif len(sys.argv) == 2:
        csv_file = sys.argv[1]
        output_dir = '/home/ricardosantos/ns-3.47/results/plots'
        timestamp = os.path.basename(csv_file).replace('results_', '').replace('.csv', '')
    else:
        # Procurar ficheiro CSV mais recente
        base_dir = '/home/ricardosantos/ns-3.47/results/outputs'
        csv_files = glob.glob(os.path.join(base_dir, 'results_*.csv'))
        if not csv_files:
            print("Error: No CSV file found. Please provide a CSV file path.")
            print("Usage: python3 generate_plots.py <csv_file> [output_dir] [timestamp] [data_rate]")
            sys.exit(1)
        csv_file = max(csv_files, key=os.path.getctime)
        output_dir = '/home/ricardosantos/ns-3.47/results/plots'
        timestamp = os.path.basename(csv_file).replace('results_', '').replace('.csv', '')
    
    print(f"Loading data from: {csv_file}")
    print(f"Output directory: {output_dir}")
    print(f"Timestamp: {timestamp}")
    print(f"Data Rate: {data_rate}")
    print()
    
    # Criar diretório de output se não existir
    os.makedirs(output_dir, exist_ok=True)
    
    # Carregar dados
    df = load_data(csv_file)
    print(f"Loaded {len(df)} frequency pair results")
    print()
    print("Data:")
    print(df.to_string(index=False))
    print()
    
    # Gerar gráficos
    print("Generating plots...")
    plot_throughput(df, output_dir, timestamp, data_rate)
    plot_delay(df, output_dir, timestamp, data_rate)
    plot_jitter(df, output_dir, timestamp, data_rate)
    plot_combined(df, output_dir, timestamp, data_rate)
    plot_time_series(output_dir, timestamp, data_rate)
    
    print()
    print("All plots generated successfully!")
    print(f"Check: {output_dir}")

if __name__ == '__main__':
    main()
