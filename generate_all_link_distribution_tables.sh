#!/bin/bash
# Generate link distribution tables for all scenarios in priority suite

SUITE_DIR="/home/ricardosantos/ns-3.47/results_multi_sta_priority_suite"

if [[ ! -d "$SUITE_DIR" ]]; then
    echo "Error: Suite directory not found: $SUITE_DIR"
    exit 1
fi

echo "Generating link traffic and frame type distribution tables..."
echo ""

count=0
for scenario_dir in "$SUITE_DIR"/*/; do
    scenario_name=$(basename "$scenario_dir")
    
    for outputs_dir in "$scenario_dir"/outputs_*; do
        if [[ ! -d "$outputs_dir" ]]; then
            continue
        fi
        
        # Extract data rate from folder name (e.g., outputs_30Mbps_4stas)
        outputs_basename=$(basename "$outputs_dir")
        data_rate=$(echo "$outputs_basename" | sed 's/outputs_//' | sed 's/_4stas.*//')
        
        # Determine plots directory
        plots_dir="$scenario_dir/plots_${data_rate}_4stas"
        mkdir -p "$plots_dir"
        
        # Generate traffic distribution table
        traffic_table="$plots_dir/link_traffic_distribution_table.txt"
        python3 /home/ricardosantos/ns-3.47/generate_link_traffic_distribution_table.py \
            "$outputs_dir" "$scenario_name" "$traffic_table" 2>/dev/null
        
        if [[ -f "$traffic_table" ]]; then
            echo "✓ $scenario_name / ${data_rate}: Traffic distribution"
            ((count++))
        fi
        
        # Generate frame types distribution table
        frame_table="$plots_dir/link_frame_types_distribution_table.txt"
        python3 /home/ricardosantos/ns-3.47/generate_link_frame_types_distribution_table.py \
            "$outputs_dir" "$scenario_name" "$frame_table" 2>/dev/null
        
        if [[ -f "$frame_table" ]]; then
            echo "✓ $scenario_name / ${data_rate}: Frame types distribution"
            ((count++))
        fi
    done
done

echo ""
echo "Done. Generated $count tables."
