#!/usr/bin/env python3
"""
Generate tables showing per-STA throughput for multi-STA experiments.
Creates both console output tables and CSV files.
"""
import os
import glob
import re
import sys
import pandas as pd

BASE_DIR = "/home/ricardosantos/ns-3.47"
RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BASE_DIR, "results_multi_sta")
OUTPUT_DIR = os.path.join(RESULTS_DIR, "tables")

os.makedirs(OUTPUT_DIR, exist_ok=True)

# Frequency bands and pairs
FREQ_BANDS = ["2", "5", "6"]
FREQ_PAIRS = [("2", "5"), ("2", "6"), ("5", "6")]

def list_output_dirs():
    return sorted(glob.glob(os.path.join(RESULTS_DIR, "outputs_*")))

def extract_sta_data(output_file):
    """Extract per-STA data from an output file"""
    sta_data = []
    
    if not os.path.exists(output_file):
        return sta_data
    
    with open(output_file, 'r') as f:
        content = f.read()
    
    # Pattern for individual STA lines
    # FLOW_SUMMARY_STA0: freq=5 proto=UDP Throughput_Mbps=152.884 Delay_ms=1.93628 ...
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

def print_dataframe_as_table(df, title=""):
    """Print a DataFrame as a formatted table without tabulate"""
    if title:
        print(f"\n{title}")
        print("-" * len(title))
    
    # Get column widths
    col_widths = {}
    for col in df.columns:
        max_len = max(len(str(col)), df[col].astype(str).str.len().max())
        col_widths[col] = max(max_len, 8)
    
    # Print header
    header = " | ".join(f"{col:>{col_widths[col]}}" for col in df.columns)
    print(header)
    print("-" * len(header))
    
    # Print rows
    for _, row in df.iterrows():
        row_str = " | ".join(f"{str(val):>{col_widths[col]}}" for col, val in row.items())
        print(row_str)
    print()

def generate_single_link_tables():
    """Generate tables for single link multi-STA experiments"""
    print("\n" + "="*80)
    print("SINGLE LINK - PER-STA THROUGHPUT TABLES")
    print("="*80)
    
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
                freq_label = "2.4GHz" if freq == "2" else f"{freq}GHz"

                for sta in sta_data:
                    all_data.append({
                        'scenario': scenario,
                        'freq_band': freq_label,
                        'protocol': proto,
                        'sta_id': f"STA{sta['sta_id']}",
                        'throughput_mbps': sta['throughput_mbps'],
                        'delay_ms': sta['delay_ms'],
                        'jitter_ms': sta['jitter_ms'],
                        'loss_rate_pct': sta['loss_rate_pct']
                    })
    
    if not all_data:
        print("No data found!")
        return
    
    df = pd.DataFrame(all_data)
    
    protocols = sorted(df['protocol'].dropna().unique())

    # Generate pivot tables for each available protocol
    for proto in protocols:
        print(f"\n{'='*60}")
        print(f"SINGLE LINK - {proto} - Throughput (Mbps) per STA")
        print("="*60)
        
        df_proto = df[df['protocol'] == proto]
        if df_proto.empty:
            continue
        
        # Create pivot table: rows = (scenario, freq_band), columns = sta_id
        pivot = df_proto.pivot_table(
            values='throughput_mbps',
            index=['scenario', 'freq_band'],
            columns='sta_id',
            aggfunc='first'
        )
        
        # Add total column
        pivot['Total'] = pivot.sum(axis=1)
        
        # Reset index for better display
        pivot = pivot.reset_index()
        
        # Round values for display
        for col in pivot.columns:
            if col not in ['scenario', 'freq_band']:
                pivot[col] = pd.to_numeric(pivot[col], errors='coerce').round(2)
        
        print_dataframe_as_table(pivot)
        
        # Save to CSV
        csv_file = os.path.join(OUTPUT_DIR, f'single_link_per_sta_throughput_{proto.lower()}.csv')
        pivot.to_csv(csv_file, index=False)
        print(f"\nSaved to: {csv_file}")
    
    # Generate detailed table with all metrics
    print(f"\n{'='*60}")
    print("SINGLE LINK - Detailed Per-STA Metrics")
    print("="*60)
    
    df_detailed = df.copy()
    df_detailed = df_detailed.round({'throughput_mbps': 2, 'delay_ms': 3, 'jitter_ms': 4, 'loss_rate_pct': 2})
    
    csv_file = os.path.join(OUTPUT_DIR, 'single_link_per_sta_detailed.csv')
    df_detailed.to_csv(csv_file, index=False)
    print(f"Detailed data saved to: {csv_file}")

def generate_mlo_tables():
    """Generate tables for MLO multi-STA experiments"""
    print("\n" + "="*80)
    print("MLO - PER-STA THROUGHPUT TABLES")
    print("="*80)
    
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
                f1_label = "2.4" if f1 == "2" else f1
                f2_label = "2.4" if f2 == "2" else f2
                pair_label = f"{f1_label}+{f2_label}"

                for sta in sta_data:
                    all_data.append({
                        'scenario': scenario,
                        'freq_pair': pair_label,
                        'protocol': proto,
                        'sta_id': f"STA{sta['sta_id']}",
                        'throughput_mbps': sta['throughput_mbps'],
                        'delay_ms': sta['delay_ms'],
                        'jitter_ms': sta['jitter_ms'],
                        'loss_rate_pct': sta['loss_rate_pct']
                    })
    
    if not all_data:
        print("No data found!")
        return
    
    df = pd.DataFrame(all_data)
    
    protocols = sorted(df['protocol'].dropna().unique())

    # Generate pivot tables for each available protocol
    for proto in protocols:
        print(f"\n{'='*60}")
        print(f"MLO - {proto} - Throughput (Mbps) per STA")
        print("="*60)
        
        df_proto = df[df['protocol'] == proto]
        if df_proto.empty:
            continue
        
        # Create pivot table: rows = (scenario, freq_pair), columns = sta_id
        pivot = df_proto.pivot_table(
            values='throughput_mbps',
            index=['scenario', 'freq_pair'],
            columns='sta_id',
            aggfunc='first'
        )
        
        # Add total column
        pivot['Total'] = pivot.sum(axis=1)
        
        # Reset index for better display
        pivot = pivot.reset_index()
        
        # Round values for display
        for col in pivot.columns:
            if col not in ['scenario', 'freq_pair']:
                pivot[col] = pd.to_numeric(pivot[col], errors='coerce').round(2)
        
        print_dataframe_as_table(pivot)
        
        # Save to CSV
        csv_file = os.path.join(OUTPUT_DIR, f'mlo_per_sta_throughput_{proto.lower()}.csv')
        pivot.to_csv(csv_file, index=False)
        print(f"\nSaved to: {csv_file}")
    
    # Generate detailed table with all metrics
    print(f"\n{'='*60}")
    print("MLO - Detailed Per-STA Metrics")
    print("="*60)
    
    df_detailed = df.copy()
    df_detailed = df_detailed.round({'throughput_mbps': 2, 'delay_ms': 3, 'jitter_ms': 4, 'loss_rate_pct': 2})
    
    csv_file = os.path.join(OUTPUT_DIR, 'mlo_per_sta_detailed.csv')
    df_detailed.to_csv(csv_file, index=False)
    print(f"Detailed data saved to: {csv_file}")

def generate_summary_table():
    """Generate a summary table comparing total throughput vs sum of per-STA"""
    print("\n" + "="*80)
    print("SUMMARY: TOTAL THROUGHPUT VS SUM OF PER-STA THROUGHPUT")
    print("="*80)
    
    summary_data = []
    
    for outputs_dir in list_output_dirs():
        scenario = os.path.basename(outputs_dir).replace("outputs_", "")

        # Single Link
        for freq in FREQ_BANDS:
            pattern = f"single_multi_sta_{freq}_*_*.txt"
            for output_file in glob.glob(os.path.join(outputs_dir, pattern)):
                match = re.search(rf"single_multi_sta_{freq}_([A-Za-z0-9]+)_", os.path.basename(output_file))
                if not match:
                    continue
                proto = match.group(1)

                sta_data = extract_sta_data(output_file)
                if sta_data:
                    sum_sta = sum(s['throughput_mbps'] for s in sta_data)
                    freq_label = "2.4GHz" if freq == "2" else f"{freq}GHz"

                    summary_data.append({
                        'type': 'Single Link',
                        'scenario': scenario,
                        'config': freq_label,
                        'protocol': proto,
                        'num_stas': len(sta_data),
                        'sum_sta_throughput': sum_sta,
                        'avg_sta_throughput': sum_sta / len(sta_data)
                    })
        
        # MLO
        for f1, f2 in FREQ_PAIRS:
            pattern = f"mlo_multi_sta_{f1}_{f2}_*_*.txt"
            for output_file in glob.glob(os.path.join(outputs_dir, pattern)):
                match = re.search(rf"mlo_multi_sta_{f1}_{f2}_([A-Za-z0-9]+)_", os.path.basename(output_file))
                if not match:
                    continue
                proto = match.group(1)

                sta_data = extract_sta_data(output_file)
                if sta_data:
                    sum_sta = sum(s['throughput_mbps'] for s in sta_data)
                    f1_label = "2.4" if f1 == "2" else f1
                    f2_label = "2.4" if f2 == "2" else f2
                    pair_label = f"{f1_label}+{f2_label}"

                    summary_data.append({
                        'type': 'MLO',
                        'scenario': scenario,
                        'config': pair_label,
                        'protocol': proto,
                        'num_stas': len(sta_data),
                        'sum_sta_throughput': sum_sta,
                        'avg_sta_throughput': sum_sta / len(sta_data)
                    })
    
    if summary_data:
        df_summary = pd.DataFrame(summary_data)
        df_summary = df_summary.round({'sum_sta_throughput': 2, 'avg_sta_throughput': 2})
        
        print_dataframe_as_table(df_summary)
        
        csv_file = os.path.join(OUTPUT_DIR, 'summary_per_sta_throughput.csv')
        df_summary.to_csv(csv_file, index=False)
        print(f"\nSaved to: {csv_file}")

def main():
    print("="*80)
    print("MULTI-STA PER-STA THROUGHPUT TABLE GENERATOR")
    print("="*80)
    
    generate_single_link_tables()
    generate_mlo_tables()
    generate_summary_table()
    
    print("\n" + "="*80)
    print(f"All tables saved to: {OUTPUT_DIR}")
    print("="*80)

if __name__ == "__main__":
    main()
