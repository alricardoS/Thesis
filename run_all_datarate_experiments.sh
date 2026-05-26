#!/bin/bash
# run_all_datarate_experiments.sh
# Script agregado para correr Single Link e MLO experiments para múltiplos data rates
set -e

BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="$BASE_DIR/results"
SIM_TIME="12"

# Data rates a testar
declare -a DATA_RATES=("150Mbps" "450Mbps" "1200Mbps" "2000Mbps")

cd "$BASE_DIR"

# Compilar os dois programas
echo "=============================================="
echo "Compiling ns-3 programs..."
echo "=============================================="
./ns3 build scratch/wifi7-single-link-test
./ns3 build scratch/wifi7-freq-pair-test-proto

for DATA_RATE in "${DATA_RATES[@]}"; do
    TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
    
    # Criar diretórios específicos para este data rate
    OUTPUTS_DIR="$RESULTS_DIR/outputs_${DATA_RATE}"
    PLOTS_DIR="$RESULTS_DIR/plots_${DATA_RATE}"
    mkdir -p "$OUTPUTS_DIR"
    mkdir -p "$PLOTS_DIR"
    
    echo ""
    echo "=============================================="
    echo "Running experiments for Data Rate: $DATA_RATE"
    echo "=============================================="
    
    # ========== SINGLE LINK EXPERIMENTS ==========
    echo ""
    echo "--- Single Link Experiments ($DATA_RATE) ---"
    
    CSV_SINGLE="$OUTPUTS_DIR/single_results_${TIMESTAMP}.csv"
    echo "freq_band,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct" > "$CSV_SINGLE"
    
    declare -a FREQS=("2" "5" "6")
    declare -a PROTOS=("UDP" "TCP")
    
    for freq in "${FREQS[@]}"; do
        for proto in "${PROTOS[@]}"; do
            if [ "$freq" == "2" ]; then LABEL="2.4GHz"; fi
            if [ "$freq" == "5" ]; then LABEL="5GHz"; fi
            if [ "$freq" == "6" ]; then LABEL="6GHz"; fi

            echo "Running Single Link: $LABEL ($proto) @ $DATA_RATE..."
            OUTFILE="$OUTPUTS_DIR/single_${freq}_${proto}_${TIMESTAMP}.txt"
            ./ns3 run "scratch/wifi7-single-link-test --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME" 2>&1 | tee "$OUTFILE"
            
            # Extract metrics from FLOW_SUMMARY
            TP=$(grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Throughput_Mbps=//' | head -1)
            DELAY=$(grep -oE 'Delay_ms=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Delay_ms=//' | head -1)
            JITTER=$(grep -oE 'Jitter_ms=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Jitter_ms=//' | head -1)
            LOSS=$(grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/LossRate_pct=//' | head -1)
            
            if [ -z "$TP" ]; then TP=0; fi
            if [ -z "$DELAY" ]; then DELAY=0; fi
            if [ -z "$JITTER" ]; then JITTER=0; fi
            if [ -z "$LOSS" ]; then LOSS=0; fi
            
            echo "$LABEL,$proto,$TP,$DELAY,$JITTER,$LOSS" >> "$CSV_SINGLE"
        done
    done
    
    echo "Generating Single Link plots for $DATA_RATE..."
    python3 "$BASE_DIR/generate_single_plots.py" "$CSV_SINGLE" "$PLOTS_DIR" "$DATA_RATE"
    echo "Single Link experiments finished for $DATA_RATE. Results: $CSV_SINGLE"
    
    # ========== MLO EXPERIMENTS ==========
    echo ""
    echo "--- MLO Experiments ($DATA_RATE) ---"
    
    CSV_MLO="$OUTPUTS_DIR/mlo_results_${TIMESTAMP}.csv"
    echo "pair,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct" > "$CSV_MLO"
    
    # Pares de frequências
    declare -a PAIRS=("2 5 2.4+5" "2 6 2.4+6" "5 6 5+6")
    
    for pair in "${PAIRS[@]}"; do
        read -r F1 F2 NAME <<< "$pair"
        for proto in "${PROTOS[@]}"; do
            echo "Running MLO: $NAME ($proto) @ $DATA_RATE..."
            OUTFILE="$OUTPUTS_DIR/mlo_${F1}_${F2}_${proto}_${TIMESTAMP}.txt"
            ./ns3 run "scratch/wifi7-freq-pair-test-proto --freq1=$F1 --freq2=$F2 --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --enablePcaps=false" 2>&1 | tee "$OUTFILE"

            # Extract metrics from FLOW_SUMMARY
            TP=$(grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Throughput_Mbps=//' | head -1)
            DELAY=$(grep -oE 'Delay_ms=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Delay_ms=//' | head -1)
            JITTER=$(grep -oE 'Jitter_ms=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Jitter_ms=//' | head -1)
            LOSS=$(grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/LossRate_pct=//' | head -1)
            
            if [ -z "$TP" ]; then TP=0; fi
            if [ -z "$DELAY" ]; then DELAY=0; fi
            if [ -z "$JITTER" ]; then JITTER=0; fi
            if [ -z "$LOSS" ]; then LOSS=0; fi

            echo "$NAME,$proto,$TP,$DELAY,$JITTER,$LOSS" >> "$CSV_MLO"
        done
    done
    
    echo "Generating MLO plots for $DATA_RATE..."
    python3 "$BASE_DIR/generate_mlo_plots.py" "$CSV_MLO" "$PLOTS_DIR" "$DATA_RATE"
    echo "MLO experiments finished for $DATA_RATE. Results: $CSV_MLO"
    
done

echo ""
echo "=============================================="
echo "ALL EXPERIMENTS COMPLETED!"
echo "=============================================="
echo ""
echo "Results organized by data rate:"
for DATA_RATE in "${DATA_RATES[@]}"; do
    echo "  - $DATA_RATE:"
    echo "      Outputs: $RESULTS_DIR/outputs_${DATA_RATE}/"
    echo "      Plots:   $RESULTS_DIR/plots_${DATA_RATE}/"
done

echo ""
echo "=============================================="
echo "Generating Comparison Plots..."
echo "=============================================="
python3 "$BASE_DIR/generate_single_comparison_plots.py"
python3 "$BASE_DIR/generate_mlo_comparison_plots.py"
echo "Comparison plots generated in:"
echo "  - $RESULTS_DIR/comparison_plots_single/"
echo "  - $RESULTS_DIR/comparison_plots_mlo/"
