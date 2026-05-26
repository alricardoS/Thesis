#!/bin/bash
# run_multi_sta_mlo_experiments.sh
# Script para correr APENAS experimentos MLO com 4 STAs
# Gera outputs e gráficos
set -e

BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="$BASE_DIR/results_multi_sta_mlo"
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

# Compilar o programa MLO multi-sta
echo "=============================================="
echo "Compiling MLO Multi-STA ns-3 program..."
echo "=============================================="
./ns3 build scratch/wifi7-mlo-multi-sta

for DATA_RATE in "${DATA_RATES[@]}"; do
    TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
    
    # Criar diretórios específicos para este data rate
    OUTPUTS_DIR="$RESULTS_DIR/outputs_${DATA_RATE}"
    PLOTS_DIR="$RESULTS_DIR/plots_${DATA_RATE}"
    mkdir -p "$OUTPUTS_DIR"
    mkdir -p "$PLOTS_DIR"
    
    echo ""
    echo "=============================================="
    echo "Running MLO Multi-STA experiments for Data Rate: $DATA_RATE (per STA)"
    echo "Total potential throughput: 4 x $DATA_RATE"
    echo "=============================================="
    
    CSV_MLO="$OUTPUTS_DIR/single_multi_sta_results_${TIMESTAMP}.csv"
    echo "freq_band,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_MLO"

    CSV_MLO_LOSS="$OUTPUTS_DIR/single_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,total_rx_packets,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,total_granular_pct_of_total_tx,e2e_lost_pct_of_total_tx,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_MLO_LOSS"

    CSV_MLO_PHY_REASON="$OUTPUTS_DIR/single_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,reason,count,pct_of_phy_rx,pct_of_total_granular,pct_of_total_tx_packets" > "$CSV_MLO_PHY_REASON"

    CSV_MLO_PHY_PKT_TYPE="$OUTPUTS_DIR/single_multi_sta_phy_rx_pkt_type_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,reason,mac_type,count,pct_of_reason,pct_of_phy_rx,pct_of_total_tx_packets" > "$CSV_MLO_PHY_PKT_TYPE"

    CSV_MLO_PHY_PKT_SIZE="$OUTPUTS_DIR/single_multi_sta_phy_rx_pkt_size_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,packet_size,count,pct_of_phy_rx,pct_of_total_tx_packets" > "$CSV_MLO_PHY_PKT_SIZE"

    CSV_MLO_TC_REASON="$OUTPUTS_DIR/single_multi_sta_tc_drop_reasons_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,stage,reason,count,pct_of_tc_stage,pct_of_total_granular,pct_of_total_tx_packets" > "$CSV_MLO_TC_REASON"

    CSV_MLO_LINK_USAGE="$OUTPUTS_DIR/mlo_multi_sta_link_activity_${TIMESTAMP}.csv"
    echo "pair,protocol,link_id,tx_time_s,duty_pct,overlap_time_s,overlap_pct,mu_tx_count,su_tx_count" > "$CSV_MLO_LINK_USAGE"

    CSV_MLO_RU_USAGE="$OUTPUTS_DIR/mlo_multi_sta_ru_allocation_${TIMESTAMP}.csv"
    echo "pair,protocol,link_id,ru_type,count" > "$CSV_MLO_RU_USAGE"
    
    # Pares de frequências MLO
    declare -a PAIRS=("2 5 2.4+5" "2 6 2.4+6" "5 6 5+6")
    declare -a PROTOS=("UDP" "TCP")
    
    for pair in "${PAIRS[@]}"; do
        read -r F1 F2 NAME <<< "$pair"
        for proto in "${PROTOS[@]}"; do
            echo "Running MLO Multi-STA: $NAME ($proto) @ $DATA_RATE per STA..."
            OUTFILE="$OUTPUTS_DIR/mlo_multi_sta_${F1}_${F2}_${proto}_${TIMESTAMP}.txt"
            ./ns3 run "scratch/wifi7-mlo-multi-sta --freq1=$F1 --freq2=$F2 --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --staticSetup=true --enablePcaps=false" 2>&1 | tee "$OUTFILE"

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

            echo "$NAME,$proto,$TP,$DELAY,$JITTER,$LOSS,$AVGTP" >> "$CSV_MLO"
            echo "  -> Total TP: $TP Mbps, Delay: $DELAY ms, Jitter: $JITTER ms, Loss: $LOSS%, TP/STA: $AVGTP Mbps"

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

            TOTAL_GRAN_PCT_TX=$(awk -v n="$TOTAL_GRAN" -v d="$TOTAL_TX" 'BEGIN { if (d>0) printf "%.6f", n*100.0/d; else printf "0" }')
            E2E_LOST_PCT_TX=$(awk -v n="$E2E_LOST" -v d="$TOTAL_TX" 'BEGIN { if (d>0) printf "%.6f", n*100.0/d; else printf "0" }')
            echo "$NAME,$proto,$TOTAL_TX,$TOTAL_RX,$PHY_TX,$PHY_RX,$MAC_TX,$MAC_RX,$WIFI_Q,$TC_BEFORE,$TC_AFTER,$TC_DROP,$TOTAL_GRAN,$E2E_LOST,$TOTAL_GRAN_PCT_TX,$E2E_LOST_PCT_TX,$PHY_PCT,$MAC_PCT,$WIFI_Q_PCT,$TC_PCT,$UNACC_PCT" >> "$CSV_MLO_LOSS"

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

                REASON_PCT_TX=$(awk -v n="$REASON_COUNT" -v d="$TOTAL_TX" 'BEGIN { if (d>0) printf "%.6f", n*100.0/d; else printf "0" }')
                echo "$NAME,$proto,$TOTAL_TX,$REASON,$REASON_COUNT,$REASON_PCT_PHY,$REASON_PCT_TOTAL,$REASON_PCT_TX" >> "$CSV_MLO_PHY_REASON"
            done < <(grep 'PHY_RX_DROP_REASON:' "$OUTFILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Reason=/){sub(/^Reason=/,"",$i); print $i; break}}}')
                MACTYPE=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^MacType=/){sub(/^MacType=/,"",$i); print $i; break}}}')
                COUNT=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Count=/){sub(/^Count=/,"",$i); print $i; break}}}')
                PCT_REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctReason=/){sub(/^PctReason=/,"",$i); print $i; break}}}')
                PCT_PHY=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctPhyRx=/){sub(/^PctPhyRx=/,"",$i); print $i; break}}}')
                [ -n "$REASON" ] || continue
                [ -n "$MACTYPE" ] || MACTYPE="UNKNOWN"
                PCT_TX=$(awk -v n="${COUNT:-0}" -v d="$TOTAL_TX" 'BEGIN { if (d>0) printf "%.6f", n*100.0/d; else printf "0" }')
                echo "$NAME,$proto,$TOTAL_TX,$REASON,$MACTYPE,${COUNT:-0},${PCT_REASON:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_MLO_PHY_PKT_TYPE"
            done < <(grep 'PHY_RX_DROP_PKT_TYPE:' "$OUTFILE" || true)

            while IFS= read -r line; do
                PKT_SIZE=$(echo "$line" | grep -oE 'PacketSize=[0-9]+' | sed 's/PacketSize=//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                [ -n "$PKT_SIZE" ] || continue
                PCT_TX=$(awk -v n="${COUNT:-0}" -v d="$TOTAL_TX" 'BEGIN { if (d>0) printf "%.6f", n*100.0/d; else printf "0" }')
                echo "$NAME,$proto,$TOTAL_TX,${PKT_SIZE:-0},${COUNT:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_MLO_PHY_PKT_SIZE"
            done < <(grep 'PHY_RX_DROP_PKT_SIZE:' "$OUTFILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_STAGE=$(echo "$line" | grep -oE 'PctTcBefore=-?[0-9]+\.?[0-9]*' | sed 's/PctTcBefore=//')
                PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                [ -n "$REASON" ] || continue
                PCT_TX=$(awk -v n="${COUNT:-0}" -v d="$TOTAL_TX" 'BEGIN { if (d>0) printf "%.6f", n*100.0/d; else printf "0" }')
                echo "$NAME,$proto,$TOTAL_TX,before_enqueue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_MLO_TC_REASON"
            done < <(grep 'TC_DROP_BEFORE_REASON:' "$OUTFILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_STAGE=$(echo "$line" | grep -oE 'PctTcAfter=-?[0-9]+\.?[0-9]*' | sed 's/PctTcAfter=//')
                PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                [ -n "$REASON" ] || continue
                PCT_TX=$(awk -v n="${COUNT:-0}" -v d="$TOTAL_TX" 'BEGIN { if (d>0) printf "%.6f", n*100.0/d; else printf "0" }')
                echo "$NAME,$proto,$TOTAL_TX,after_dequeue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_MLO_TC_REASON"
            done < <(grep 'TC_DROP_AFTER_REASON:' "$OUTFILE" || true)

            while IFS= read -r usageLine; do
                [ -z "$usageLine" ] && continue
                LINK_ID=$(echo "$usageLine" | grep -oE 'Link=[0-9]+' | sed 's/Link=//')
                TX_TIME=$(echo "$usageLine" | grep -oE 'TxTime_s=[0-9]+\.?[0-9]*' | sed 's/TxTime_s=//')
                DUTY_PCT=$(echo "$usageLine" | grep -oE 'Duty_pct=[0-9]+\.?[0-9]*' | sed 's/Duty_pct=//')
                OVERLAP_TIME=$(echo "$usageLine" | grep -oE 'OverlapTime_s=[0-9]+\.?[0-9]*' | sed 's/OverlapTime_s=//')
                OVERLAP_PCT=$(echo "$usageLine" | grep -oE 'Overlap_pct=[0-9]+\.?[0-9]*' | sed 's/Overlap_pct=//')
                MU_TX=$(echo "$usageLine" | grep -oE 'MuTxCount=[0-9]+' | sed 's/MuTxCount=//')
                SU_TX=$(echo "$usageLine" | grep -oE 'SuTxCount=[0-9]+' | sed 's/SuTxCount=//')

                [ -z "$LINK_ID" ] && LINK_ID=0
                [ -z "$TX_TIME" ] && TX_TIME=0
                [ -z "$DUTY_PCT" ] && DUTY_PCT=0
                [ -z "$OVERLAP_TIME" ] && OVERLAP_TIME=0
                [ -z "$OVERLAP_PCT" ] && OVERLAP_PCT=0
                [ -z "$MU_TX" ] && MU_TX=0
                [ -z "$SU_TX" ] && SU_TX=0

                echo "$NAME,$proto,$LINK_ID,$TX_TIME,$DUTY_PCT,$OVERLAP_TIME,$OVERLAP_PCT,$MU_TX,$SU_TX" >> "$CSV_MLO_LINK_USAGE"
            done < <(grep 'MLO_LINK_ACTIVITY:' "$OUTFILE" || true)

            while IFS= read -r ruLine; do
                [ -z "$ruLine" ] && continue
                LINK_ID=$(echo "$ruLine" | grep -oE 'Link=[0-9]+' | sed 's/Link=//')
                RU_TYPE=$(echo "$ruLine" | grep -oE 'RuType=[^ ]+' | sed 's/RuType=//')
                RU_COUNT=$(echo "$ruLine" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')

                [ -z "$LINK_ID" ] && LINK_ID=0
                [ -z "$RU_TYPE" ] && RU_TYPE=UNKNOWN
                [ -z "$RU_COUNT" ] && RU_COUNT=0

                echo "$NAME,$proto,$LINK_ID,$RU_TYPE,$RU_COUNT" >> "$CSV_MLO_RU_USAGE"
            done < <(grep 'MLO_RU_ALLOCATION:' "$OUTFILE" || true)
        done
    done
    
    echo "MLO Multi-STA experiments finished for $DATA_RATE. Results: $CSV_MLO"
    echo "MLO Loss Breakdown: $CSV_MLO_LOSS"
    echo "MLO PHY RX reason breakdown: $CSV_MLO_PHY_REASON"
    
    # ========== GENERATE PLOTS ==========
    if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
        echo ""
        echo "--- Generating plots for $DATA_RATE ---"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_plots.py" "$CSV_MLO" "$PLOTS_DIR" "$DATA_RATE-MLO"
        "$PYTHON_BIN" "$BASE_DIR/generate_single_link_loss_breakdown_plots.py" "$CSV_MLO_LOSS" "$PLOTS_DIR" "$DATA_RATE-MLO" "$CSV_MLO_PHY_REASON"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_phy_drop_reason_table.py" "$CSV_MLO_PHY_REASON" "$PLOTS_DIR" "$DATA_RATE-MLO"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tc_drop_reason_table.py" "$CSV_MLO_TC_REASON" "$OUTPUTS_DIR/__mlo_missing__.csv" "$PLOTS_DIR" "$DATA_RATE-MLO"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_link_ru_plots.py" "$CSV_MLO_LINK_USAGE" "$CSV_MLO_RU_USAGE" "$PLOTS_DIR" "$DATA_RATE" "$CSV_MLO_LINK_TRAFFIC"
        echo "Plots generated in: $PLOTS_DIR"
    fi
    
    # ========== SUMMARY ==========
    echo ""
    echo "=============================================="
    echo "Summary for Data Rate: $DATA_RATE (per STA)"
    echo "=============================================="
    echo ""
    echo "MLO Results:"
    cat "$CSV_MLO"
    
done

if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
    echo ""
    echo "=============================================="
    echo "Generating comparison plots across all data rates..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_comparison_plots.py"
fi

echo ""
echo "=============================================="
echo "All MLO Multi-STA experiments completed!"
echo "Results saved in: $RESULTS_DIR"
echo "Comparison plots in: $RESULTS_DIR/comparison_plots_mlo/"
echo "=============================================="
