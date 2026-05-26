#!/bin/bash

cd /home/ricardosantos/ns-3.47

for plots_dir in results_single_link_scaling/plots_*; do
  scenario=$(basename "$plots_dir")
  outputs_dir="results_single_link_scaling/outputs_$scenario"
  
  if [ ! -d "$outputs_dir" ]; then
    continue
  fi
  
  loss_csv=$(ls -t "$outputs_dir"/single_multi_sta_loss_breakdown_*.csv 2>/dev/null | head -n1)
  phy_csv=$(ls -t "$outputs_dir"/single_multi_sta_phy_rx_reasons_*.csv 2>/dev/null | head -n1)
  
  if [ -z "$loss_csv" ]; then
    continue
  fi
  
  echo "Gerando para: $scenario"
  python3 generate_single_link_loss_breakdown_plots.py "$loss_csv" "$plots_dir" "$scenario" "$phy_csv"
done
