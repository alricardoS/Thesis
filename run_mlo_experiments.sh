#!/bin/bash
# run_mlo_experiments.sh
set -e
BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="$BASE_DIR/results"
OUTPUTS_DIR="$RESULTS_DIR/outputs"
PLOTS_DIR="$RESULTS_DIR/plots"
mkdir -p "$OUTPUTS_DIR"
mkdir -p "$PLOTS_DIR"

DATA_RATE="6000Mbps" # Carga alta para testar saturação
SIM_TIME="12"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

# Compilar (build the new file with protocol support)
cd "$BASE_DIR"
./ns3 build scratch/wifi7-freq-pair-test-proto

CSV_FILE="$OUTPUTS_DIR/mlo_results_${TIMESTAMP}.csv"
echo "pair,protocol,throughput_mbps,delay_ms,jitter_ms" > "$CSV_FILE"

# Pares de frequências
declare -a PAIRS=("2 5 2.4+5" "2 6 2.4+6" "5 6 5+6")
# Protocolos
declare -a PROTOS=("UDP" "TCP")

for pair in "${PAIRS[@]}"; do
    read -r F1 F2 NAME <<< "$pair"
    for proto in "${PROTOS[@]}"; do
        echo "Running MLO: $NAME ($proto)..."
        OUTFILE="$OUTPUTS_DIR/mlo_${F1}_${F2}_${proto}_${TIMESTAMP}.txt"
        ./ns3 run "scratch/wifi7-freq-pair-test-proto --freq1=$F1 --freq2=$F2 --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --enablePcaps=false" 2>&1 | tee "$OUTFILE"

        # Extract throughput from FLOW_SUMMARY
        TP=$(grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Throughput_Mbps=//' | head -1)
        if [ -z "$TP" ]; then TP=0; fi

        # Extract average delay and jitter from TIME_STATS lines (exclude 0 values)
        DELAY=$(grep -oE 'Delay: [0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Delay: //' | awk '$1 > 0 {sum+=$1; count++} END {if(count>0) print sum/count; else print 0}')
        JITTER=$(grep -oE 'Jitter: [0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Jitter: //' | awk '$1 > 0 {sum+=$1; count++} END {if(count>0) print sum/count; else print 0}')
        
        if [ -z "$DELAY" ]; then DELAY=0; fi
        if [ -z "$JITTER" ]; then JITTER=0; fi

        echo "$NAME,$proto,$TP,$DELAY,$JITTER" >> "$CSV_FILE"
    done
done

python3 "$BASE_DIR/generate_mlo_plots.py" "$CSV_FILE" "$PLOTS_DIR"

echo "MLO experiments finished. Results: $CSV_FILE"
