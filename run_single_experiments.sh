#!/bin/bash
set -e
BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="$BASE_DIR/results"
OUTPUTS_DIR="$RESULTS_DIR/outputs"
PLOTS_DIR="$RESULTS_DIR/plots"
mkdir -p "$OUTPUTS_DIR"
mkdir -p "$PLOTS_DIR"

DATA_RATE="2000Mbps" 
SIM_TIME="12"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

cd "$BASE_DIR"
./ns3 build scratch/wifi7-single-link-test

CSV_FILE="$OUTPUTS_DIR/single_results_${TIMESTAMP}.csv"
echo "freq_band,protocol,throughput_mbps,delay_ms,jitter_ms" > "$CSV_FILE"

declare -a FREQS=("2" "5" "6")
declare -a PROTOS=("UDP" "TCP")

for freq in "${FREQS[@]}"; do
    for proto in "${PROTOS[@]}"; do
        if [ "$freq" == "2" ]; then LABEL="2.4GHz"; fi
        if [ "$freq" == "5" ]; then LABEL="5GHz"; fi
        if [ "$freq" == "6" ]; then LABEL="6GHz"; fi

        echo "Running Single Link: $LABEL ($proto)..."
        OUTFILE="$OUTPUTS_DIR/single_${freq}_${proto}_${TIMESTAMP}.txt"
        ./ns3 run "scratch/wifi7-single-link-test --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME" 2>&1 | tee "$OUTFILE"
        
        TP=$(grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Throughput_Mbps=//' | head -1)
        if [ -z "$TP" ]; then TP=0; fi
        
        # Extract average delay and jitter from TIME_STATS lines (exclude 0 values)
        DELAY=$(grep -oE 'Delay: [0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Delay: //' | awk '$1 > 0 {sum+=$1; count++} END {if(count>0) print sum/count; else print 0}')
        JITTER=$(grep -oE 'Jitter: [0-9]+\.?[0-9]*' "$OUTFILE" | sed 's/Jitter: //' | awk '$1 > 0 {sum+=$1; count++} END {if(count>0) print sum/count; else print 0}')
        
        if [ -z "$DELAY" ]; then DELAY=0; fi
        if [ -z "$JITTER" ]; then JITTER=0; fi
        
        echo "$LABEL,$proto,$TP,$DELAY,$JITTER" >> "$CSV_FILE"
    done
done

python3 "$BASE_DIR/generate_single_plots.py" "$CSV_FILE" "$PLOTS_DIR" "$DATA_RATE"

echo "Single-link experiments finished. Results: $CSV_FILE"
