#!/bin/bash
# run_multi_sta_single_experiments.sh
# Script para correr APENAS experimentos Single Link (SLO) com 4 STAs
# Gera outputs e gráficos
set -e

BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="$BASE_DIR/results_multi_sta_single"
SIM_TIME="12"
PYTHON_BIN="python3"

# Python post-processing (plots) enabled by default
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"

# Data rates a testar (iguais aos testes single-user)
declare -a DATA_RATES=("150Mbps" "450Mbps" "1200Mbps" "2000Mbps")

cd "$BASE_DIR"

if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
    for CANDIDATE in "$BASE_DIR/.venv/bin/python3" "/usr/bin/python3" "python3"; do
        if [ "$CANDIDATE" = "python3" ] || [ -x "$CANDIDATE" ]; then
            if "$CANDIDATE" -c "import pandas, matplotlib, numpy" >/dev/null 2>&1; then
                PYTHON_BIN="$CANDIDATE"
                break
            fi
        fi
    done

    if ! "$PYTHON_BIN" -c "import pandas, matplotlib, numpy" >/dev/null 2>&1; then
        echo "[WARN] Python dependencies (pandas/matplotlib/numpy) not available in .venv or system python."
        echo "[WARN] Skipping Python report generation. Raw CSV/TXT outputs will still be generated."
        GENERATE_PYTHON_REPORTS="false"
    else
        echo "[INFO] Using Python interpreter for reports: $PYTHON_BIN"
    fi
fi

# Compilar o programa single-link multi-sta
echo "=============================================="
echo "Compiling Single Link Multi-STA ns-3 program..."
echo "=============================================="
./ns3 build scratch/wifi7-single-link-multi-sta

for DATA_RATE in "${DATA_RATES[@]}"; do
    TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
    
    # Criar diretórios específicos para este data rate
    OUTPUTS_DIR="$RESULTS_DIR/outputs_${DATA_RATE}"
    PLOTS_DIR="$RESULTS_DIR/plots_${DATA_RATE}"
    mkdir -p "$OUTPUTS_DIR"
    mkdir -p "$PLOTS_DIR"
    
    echo ""
    echo "=============================================="
    echo "Running Single Link Multi-STA experiments for Data Rate: $DATA_RATE (per STA)"
    echo "Total potential throughput: 4 x $DATA_RATE"
    echo "=============================================="
    
    CSV_SINGLE="$OUTPUTS_DIR/single_multi_sta_results_${TIMESTAMP}.csv"
    echo "freq_band,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_SINGLE"

    CSV_SINGLE_LOSS="$OUTPUTS_DIR/single_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,total_rx_packets,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,total_granular_pct_of_total_tx,e2e_lost_pct_of_total_tx,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_SINGLE_LOSS"

    CSV_SINGLE_PHY_REASON="$OUTPUTS_DIR/single_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,reason,count,pct_of_phy_rx,pct_of_total_granular,pct_of_total_tx_packets" > "$CSV_SINGLE_PHY_REASON"

    CSV_SINGLE_PHY_PKT_TYPE="$OUTPUTS_DIR/single_multi_sta_phy_rx_pkt_type_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,reason,mac_type,count,pct_of_reason,pct_of_phy_rx,pct_of_total_tx_packets" > "$CSV_SINGLE_PHY_PKT_TYPE"

    CSV_SINGLE_PHY_PKT_SIZE="$OUTPUTS_DIR/single_multi_sta_phy_rx_pkt_size_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,packet_size,count,pct_of_phy_rx,pct_of_total_tx_packets" > "$CSV_SINGLE_PHY_PKT_SIZE"

    CSV_SINGLE_TC_REASON="$OUTPUTS_DIR/single_multi_sta_tc_drop_reasons_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,stage,reason,count,pct_of_tc_stage,pct_of_total_granular,pct_of_total_tx_packets" > "$CSV_SINGLE_TC_REASON"
    
    declare -a FREQS=("2" "5" "6")
    declare -a PROTOS=("UDP" "TCP")
    
    for freq in "${FREQS[@]}"; do
        for proto in "${PROTOS[@]}"; do
            if [ "$freq" == "2" ]; then LABEL="2.4GHz"; fi
            if [ "$freq" == "5" ]; then LABEL="5GHz"; fi
            if [ "$freq" == "6" ]; then LABEL="6GHz"; fi

            echo "Running Single Link Multi-STA: $LABEL ($proto) @ $DATA_RATE per STA..."
            OUTFILE="$OUTPUTS_DIR/single_multi_sta_${freq}_${proto}_${TIMESTAMP}.txt"
            ./ns3 run "scratch/wifi7-single-link-multi-sta --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --staticSetup=true" 2>&1 | tee "$OUTFILE"
            
            # Extract metrics from FLOW_SUMMARY_TOTAL
            TP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' | sed 's/Throughput_Mbps=//')
            DELAY=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgDelay_ms=[0-9]+\.?[0-9]*' | sed 's/AvgDelay_ms=//')
            JITTER=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgJitter_ms=[0-9]+\.?[0-9]*' | sed 's/AvgJitter_ms=//')
            LOSS=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' | sed 's/LossRate_pct=//')
            AVGTP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgThroughputPerSTA_Mbps=[0-9]+\.?[0-9]*' | sed 's/AvgThroughputPerSTA_Mbps=//')
            TOTAL_TX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'TX_Packets=[0-9]+' | sed 's/TX_Packets=//' || true)
            TOTAL_RX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'RX_Packets=[0-9]+' | sed 's/RX_Packets=//' || true)
            
            if [ -z "$TP" ]; then TP=0; fi
            if [ -z "$DELAY" ]; then DELAY=0; fi
            if [ -z "$JITTER" ]; then JITTER=0; fi
            if [ -z "$LOSS" ]; then LOSS=0; fi
            if [ -z "$AVGTP" ]; then AVGTP=0; fi
            if [ -z "$TOTAL_TX" ]; then TOTAL_TX=0; fi
            if [ -z "$TOTAL_RX" ]; then TOTAL_RX=0; fi
            
            echo "$LABEL,$proto,$TP,$DELAY,$JITTER,$LOSS,$AVGTP" >> "$CSV_SINGLE"
            echo "  -> Total TP: $TP Mbps, Delay: $DELAY ms, Jitter: $JITTER ms, Loss: $LOSS%, TP/STA: $AVGTP Mbps"

            # Extract granular packet loss breakdown
            PHY_TX=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'PhyTxDrop=[0-9]+' | sed 's/PhyTxDrop=//' || echo "0")
            PHY_RX=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'PhyRxDrop=[0-9]+' | sed 's/PhyRxDrop=//' || echo "0")
            MAC_TX=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'MacTxDrop=[0-9]+' | sed 's/MacTxDrop=//' || echo "0")
            MAC_RX=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'MacRxDrop=[0-9]+' | sed 's/MacRxDrop=//' || echo "0")
            WIFI_Q=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'WifiQueueDrop=[0-9]+' | sed 's/WifiQueueDrop=//' || echo "0")
            TC_BEFORE=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'TcDropBeforeEnqueue=[0-9]+' | sed 's/TcDropBeforeEnqueue=//' || echo "0")
            TC_AFTER=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'TcDropAfterDequeue=[0-9]+' | sed 's/TcDropAfterDequeue=//' || echo "0")
            TC_DROP=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'TcDrop=[0-9]+' | sed 's/TcDrop=//' || echo "0")
            TOTAL_GRAN=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'TotalGranularDrops=[0-9]+' | sed 's/TotalGranularDrops=//' || echo "0")
            E2E_LOST=$(grep 'PACKET_LOSS_BREAKDOWN' "$OUTFILE" | grep -oE 'E2E_LostPackets=[0-9]+' | sed 's/E2E_LostPackets=//' || echo "0")

            PHY_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'PHY_pct=-?[0-9]+\.?[0-9]*' | sed 's/PHY_pct=//' || echo "0")
            MAC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'MAC_pct=-?[0-9]+\.?[0-9]*' | sed 's/MAC_pct=//' || echo "0")
            WIFI_Q_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'WifiQueue_pct=-?[0-9]+\.?[0-9]*' | sed 's/WifiQueue_pct=//' || echo "0")
            TC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'TC_pct=-?[0-9]+\.?[0-9]*' | sed 's/TC_pct=//' || echo "0")
            UNACC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'Unaccounted_pct=-?[0-9]+\.?[0-9]*' | sed 's/Unaccounted_pct=//' || echo "0")

            [ -z "$PHY_TX" ] && PHY_TX=0
            [ -z "$PHY_RX" ] && PHY_RX=0
            [ -z "$MAC_TX" ] && MAC_TX=0
            [ -z "$MAC_RX" ] && MAC_RX=0
            [ -z "$WIFI_Q" ] && WIFI_Q=0
            [ -z "$TC_BEFORE" ] && TC_BEFORE=0
            [ -z "$TC_AFTER" ] && TC_AFTER=0
            [ -z "$TC_DROP" ] && TC_DROP=0
            [ -z "$TOTAL_GRAN" ] && TOTAL_GRAN=0
            [ -z "$E2E_LOST" ] && E2E_LOST=0
            [ -z "$PHY_PCT" ] && PHY_PCT=0
            [ -z "$MAC_PCT" ] && MAC_PCT=0
            [ -z "$WIFI_Q_PCT" ] && WIFI_Q_PCT=0
            [ -z "$TC_PCT" ] && TC_PCT=0
            [ -z "$UNACC_PCT" ] && UNACC_PCT=0

            TOTAL_GRAN_PCT_TX=$(pct_of_total "$TOTAL_GRAN" "$TOTAL_TX")
            E2E_LOST_PCT_TX=$(pct_of_total "$E2E_LOST" "$TOTAL_TX")
            echo "$LABEL,$proto,$TOTAL_TX,$TOTAL_RX,$PHY_TX,$PHY_RX,$MAC_TX,$MAC_RX,$WIFI_Q,$TC_BEFORE,$TC_AFTER,$TC_DROP,$TOTAL_GRAN,$E2E_LOST,$TOTAL_GRAN_PCT_TX,$E2E_LOST_PCT_TX,$PHY_PCT,$MAC_PCT,$WIFI_Q_PCT,$TC_PCT,$UNACC_PCT" >> "$CSV_SINGLE_LOSS"

            while IFS= read -r reasonLine; do
                [ -z "$reasonLine" ] && continue
                REASON=$(echo "$reasonLine" | grep -oE 'Reason=[^ ]+' | sed 's/Reason=//')
                REASON_COUNT=$(echo "$reasonLine" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                [ -z "$REASON" ] && REASON="UNKNOWN"
                [ -z "$REASON_COUNT" ] && REASON_COUNT=0

                if [ "$PHY_RX" -gt 0 ]; then
                    REASON_PCT_PHY=$(awk -v c="$REASON_COUNT" -v t="$PHY_RX" 'BEGIN{printf "%.4f", (100.0*c)/t}')
                else
                    REASON_PCT_PHY="0"
                fi

                if [ "$TOTAL_GRAN" -gt 0 ]; then
                    REASON_PCT_TOTAL=$(awk -v c="$REASON_COUNT" -v t="$TOTAL_GRAN" 'BEGIN{printf "%.4f", (100.0*c)/t}')
                else
                    REASON_PCT_TOTAL="0"
                fi

                REASON_PCT_TX=$(pct_of_total "$REASON_COUNT" "$TOTAL_TX")
                echo "$LABEL,$proto,$TOTAL_TX,$REASON,$REASON_COUNT,$REASON_PCT_PHY,$REASON_PCT_TOTAL,$REASON_PCT_TX" >> "$CSV_SINGLE_PHY_REASON"
            done < <(grep 'PHY_RX_DROP_REASON:' "$OUTFILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Reason=/){sub(/^Reason=/,"",$i); print $i; break}}}')
                MACTYPE=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^MacType=/){sub(/^MacType=/,"",$i); print $i; break}}}')
                COUNT=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Count=/){sub(/^Count=/,"",$i); print $i; break}}}')
                PCT_REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctReason=/){sub(/^PctReason=/,"",$i); print $i; break}}}')
                PCT_PHY=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctPhyRx=/){sub(/^PctPhyRx=/,"",$i); print $i; break}}}')
                [ -n "$REASON" ] || continue
                [ -n "$MACTYPE" ] || MACTYPE="UNKNOWN"
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                echo "$LABEL,$proto,$TOTAL_TX,$REASON,$MACTYPE,${COUNT:-0},${PCT_REASON:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_PKT_TYPE"
            done < <(grep 'PHY_RX_DROP_PKT_TYPE:' "$OUTFILE" || true)

            while IFS= read -r line; do
                PKT_SIZE=$(echo "$line" | grep -oE 'PacketSize=[0-9]+' | sed 's/PacketSize=//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                [ -n "$PKT_SIZE" ] || continue
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                echo "$LABEL,$proto,$TOTAL_TX,${PKT_SIZE:-0},${COUNT:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_PKT_SIZE"
            done < <(grep 'PHY_RX_DROP_PKT_SIZE:' "$OUTFILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_STAGE=$(echo "$line" | grep -oE 'PctTcBefore=-?[0-9]+\.?[0-9]*' | sed 's/PctTcBefore=//')
                PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                [ -n "$REASON" ] || continue
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                echo "$LABEL,$proto,$TOTAL_TX,before_enqueue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_TC_REASON"
            done < <(grep 'TC_DROP_BEFORE_REASON:' "$OUTFILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_STAGE=$(echo "$line" | grep -oE 'PctTcAfter=-?[0-9]+\.?[0-9]*' | sed 's/PctTcAfter=//')
                PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                [ -n "$REASON" ] || continue
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                echo "$LABEL,$proto,$TOTAL_TX,after_dequeue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_TC_REASON"
            done < <(grep 'TC_DROP_AFTER_REASON:' "$OUTFILE" || true)
        done
    done
    
    echo "Single Link Multi-STA experiments finished for $DATA_RATE. Results: $CSV_SINGLE"
    echo "Single Link Loss Breakdown: $CSV_SINGLE_LOSS"
    echo "Single Link PHY RX reason breakdown: $CSV_SINGLE_PHY_REASON"
    
    # ========== GENERATE PLOTS ==========
    if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
        echo ""
        echo "--- Generating plots for $DATA_RATE ---"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_plots.py" "$CSV_SINGLE" "$PLOTS_DIR" "$DATA_RATE"
        "$PYTHON_BIN" "$BASE_DIR/generate_single_link_loss_breakdown_plots.py" "$CSV_SINGLE_LOSS" "$PLOTS_DIR" "$DATA_RATE" "$CSV_SINGLE_PHY_REASON"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_phy_drop_reason_table.py" "$CSV_SINGLE_PHY_REASON" "$PLOTS_DIR" "$DATA_RATE"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tc_drop_reason_table.py" "$CSV_SINGLE_TC_REASON" "$OUTPUTS_DIR/__mlo_missing__.csv" "$PLOTS_DIR" "$DATA_RATE"

        "$PYTHON_BIN" - "$CSV_SINGLE_LOSS" "$PLOTS_DIR" "$DATA_RATE" <<'PY'
import os
import sys
import pandas as pd

loss_csv, out_dir, scenario = sys.argv[1], sys.argv[2], sys.argv[3]
out_file = os.path.join(out_dir, "single_drop_count_table.txt")

if os.path.exists(loss_csv):
    df = pd.read_csv(loss_csv)
    with open(out_file, "w") as f:
        f.write(df.to_string(index=False))
    print(f"Drop count table generated: {out_file}")
PY

        # This runner has no MLO links; generate explicit N/A artifact for consistency.
        {
            echo "=============================================================="
            echo "MLO Link/RU Usage Summary - $DATA_RATE"
            echo "=============================================================="
            echo "Not applicable: this test runner executes only Single-Link (SLO) scenarios."
            echo "No MLO links and no RU split across links are present in this run."
        } > "$PLOTS_DIR/mlo_link_ru_usage_table.txt"

        echo "Plots generated in: $PLOTS_DIR"
    fi
    
    # ========== SUMMARY ==========
    echo ""
    echo "=============================================="
    echo "Summary for Data Rate: $DATA_RATE (per STA)"
    echo "=============================================="
    echo ""
    echo "Single Link Results:"
    cat "$CSV_SINGLE"
    
done

if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
    echo ""
    echo "=============================================="
    echo "Generating comparison plots across all data rates..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_comparison_plots.py"
fi

echo ""
echo "=============================================="
echo "All Single Link Multi-STA experiments completed!"
echo "Results saved in: $RESULTS_DIR"
echo "Comparison plots in: $RESULTS_DIR/comparison_plots_single/"
echo "=============================================="
