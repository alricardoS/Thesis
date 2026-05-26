#!/usr/bin/env python3
"""
Generate loss breakdown plots for all single-link scenarios
"""
import os
import glob
import subprocess
import sys

BASE_DIR = "/home/ricardosantos/ns-3.47"
RESULTS_DIR = os.path.join(BASE_DIR, "results_single_link_scaling")

os.chdir(BASE_DIR)

# Find all plot directories
plot_dirs = sorted(glob.glob(os.path.join(RESULTS_DIR, "plots_*")))
print(f"Found {len(plot_dirs)} plot directories")

for plot_dir in plot_dirs:
    basename = os.path.basename(plot_dir)
    scenario = basename.replace("plots_", "")
    outputs_dir = os.path.join(RESULTS_DIR, f"outputs_{scenario}")
    
    if not os.path.isdir(outputs_dir):
        print(f"SKIP: Outputs dir not found for {scenario}")
        continue
    
    # Find latest CSV files
    loss_pattern = os.path.join(outputs_dir, "single_multi_sta_loss_breakdown_*.csv")
    phy_pattern = os.path.join(outputs_dir, "single_multi_sta_phy_rx_reasons_*.csv")
    
    loss_csvs = sorted(glob.glob(loss_pattern))
    phy_csvs = sorted(glob.glob(phy_pattern))
    
    if not loss_csvs:
        print(f"SKIP: No loss CSV for {scenario}")
        continue
    
    loss_csv = loss_csvs[-1]  # Latest
    phy_csv = phy_csvs[-1] if phy_csvs else None
    
    print(f"Generating for: {scenario}")
    
    cmd = [
        sys.executable,
        "generate_single_link_loss_breakdown_plots.py",
        loss_csv,
        plot_dir,
        scenario,
    ]
    
    if phy_csv:
        cmd.append(phy_csv)
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode == 0:
            print(f"  ✓ Success")
        else:
            print(f"  ✗ Error: {result.stderr}")
    except subprocess.TimeoutExpired:
        print(f"  ✗ Timeout")
    except Exception as e:
        print(f"  ✗ Exception: {e}")

print("\nAll scenarios processed!")
