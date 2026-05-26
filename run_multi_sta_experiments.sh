#!/bin/bash
# run_multi_sta_experiments.sh
# Script para correr experimentos Multi-STA (2 STAs) tanto para SLO quanto MLO
# Testa diferentes data rates com múltiplos utilizadores
# Suporta testes com e sem OFDMA
set -e

BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="$BASE_DIR/results_multi_sta"
SIM_TIME="12"
PYTHON_BIN="python3"

# Python post-processing (plots/tables) enabled by default
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"

# Data rates a testar
declare -a DATA_RATES=("150Mbps" "1200Mbps")

# OFDMA modes: false (sem OFDMA) e true (com OFDMA)
declare -a OFDMA_MODES=("false") # Para comparação futura, pode-se adicionar "true" para testar com OFDMA

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

# Compilar os dois programas multi-sta
echo "=============================================="
echo "Compiling Multi-STA ns-3 programs..."
echo "=============================================="
./ns3 build scratch/wifi7-single-link-multi-sta
./ns3 build scratch/wifi7-mlo-multi-sta

for OFDMA_ENABLED in "${OFDMA_MODES[@]}"; do
    # Definir sufixo para ficheiros baseado no modo OFDMA
    if [ "$OFDMA_ENABLED" == "true" ]; then
        OFDMA_SUFFIX="_OFDMA"
        OFDMA_LABEL="with OFDMA"
    else
        OFDMA_SUFFIX=""
        OFDMA_LABEL="without OFDMA"
    fi

for DATA_RATE in "${DATA_RATES[@]}"; do
    TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
    
    # Criar diretórios específicos para este data rate e modo OFDMA
    OUTPUTS_DIR="$RESULTS_DIR/outputs_${DATA_RATE}${OFDMA_SUFFIX}"
    PLOTS_DIR="$RESULTS_DIR/plots_${DATA_RATE}${OFDMA_SUFFIX}"
    mkdir -p "$OUTPUTS_DIR"
    mkdir -p "$PLOTS_DIR"
    
    echo ""
    echo "=============================================="
    echo "Running Multi-STA experiments for Data Rate: $DATA_RATE (per STA) $OFDMA_LABEL"
    echo "Total potential throughput: 2 x $DATA_RATE"
    echo "=============================================="
    
    # ========== SINGLE LINK EXPERIMENTS (SLO) ==========
    echo ""
    echo "--- Single Link Multi-STA Experiments ($DATA_RATE) $OFDMA_LABEL ---"
    
    CSV_SINGLE="$OUTPUTS_DIR/single_multi_sta_results_${TIMESTAMP}.csv"
    echo "freq_band,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_SINGLE"
    
    # CSV for granular packet loss breakdown
    CSV_SINGLE_LOSS="$OUTPUTS_DIR/single_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
    echo "freq_band,protocol,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_SINGLE_LOSS"

    # CSV for PHY RX drop reasons (Single Link)
    CSV_SINGLE_PHY_REASON="$OUTPUTS_DIR/single_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
    echo "freq_band,protocol,reason,count,pct_of_phy_rx,pct_of_total_granular" > "$CSV_SINGLE_PHY_REASON"
    
    declare -a FREQS=("2" "5" "6")
    # Apenas UDP (TCP comentado para uso futuro)
    declare -a PROTOS=("UDP")
    # declare -a PROTOS=("UDP" "TCP")

    for freq in "${FREQS[@]}"; do
        for proto in "${PROTOS[@]}"; do
            if [ "$freq" == "2" ]; then LABEL="2.4GHz"; fi
            if [ "$freq" == "5" ]; then LABEL="5GHz"; fi
            if [ "$freq" == "6" ]; then LABEL="6GHz"; fi

            echo "Running Single Link Multi-STA: $LABEL ($proto) @ $DATA_RATE per STA $OFDMA_LABEL..."
            OUTFILE="$OUTPUTS_DIR/single_multi_sta_${freq}_${proto}_${TIMESTAMP}.txt"
            ./ns3 run "scratch/wifi7-single-link-multi-sta --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --staticSetup=true" 2>&1 | tee "$OUTFILE"
            
            # Extract metrics from FLOW_SUMMARY_TOTAL
            TP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' | sed 's/Throughput_Mbps=//')
            DELAY=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgDelay_ms=[0-9]+\.?[0-9]*' | sed 's/AvgDelay_ms=//')
            JITTER=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgJitter_ms=[0-9]+\.?[0-9]*' | sed 's/AvgJitter_ms=//')
            LOSS=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' | sed 's/LossRate_pct=//')
            AVGTP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgThroughputPerSTA_Mbps=[0-9]+\.?[0-9]*' | sed 's/AvgThroughputPerSTA_Mbps=//')
            
            if [ -z "$TP" ]; then TP=0; fi
            if [ -z "$DELAY" ]; then DELAY=0; fi
            if [ -z "$JITTER" ]; then JITTER=0; fi
            if [ -z "$LOSS" ]; then LOSS=0; fi
            if [ -z "$AVGTP" ]; then AVGTP=0; fi
            
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
            
            # Extract percentages
            PHY_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'PHY_pct=[0-9]+\.?[0-9]*' | sed 's/PHY_pct=//' || echo "0")
            MAC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'MAC_pct=[0-9]+\.?[0-9]*' | sed 's/MAC_pct=//' || echo "0")
            WIFI_Q_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'WifiQueue_pct=[0-9]+\.?[0-9]*' | sed 's/WifiQueue_pct=//' || echo "0")
            TC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'TC_pct=[0-9]+\.?[0-9]*' | sed 's/TC_pct=//' || echo "0")
            UNACC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'Unaccounted_pct=[0-9]+\.?[0-9]*' | sed 's/Unaccounted_pct=//' || echo "0")
            
            if [ -z "$PHY_TX" ]; then PHY_TX=0; fi
            if [ -z "$PHY_RX" ]; then PHY_RX=0; fi
            if [ -z "$MAC_TX" ]; then MAC_TX=0; fi
            if [ -z "$MAC_RX" ]; then MAC_RX=0; fi
            if [ -z "$WIFI_Q" ]; then WIFI_Q=0; fi
            if [ -z "$TC_BEFORE" ]; then TC_BEFORE=0; fi
            if [ -z "$TC_AFTER" ]; then TC_AFTER=0; fi
            if [ -z "$TC_DROP" ]; then TC_DROP=0; fi
            if [ -z "$TOTAL_GRAN" ]; then TOTAL_GRAN=0; fi
            if [ -z "$E2E_LOST" ]; then E2E_LOST=0; fi
            if [ -z "$PHY_PCT" ]; then PHY_PCT=0; fi
            if [ -z "$MAC_PCT" ]; then MAC_PCT=0; fi
            if [ -z "$WIFI_Q_PCT" ]; then WIFI_Q_PCT=0; fi
            if [ -z "$TC_PCT" ]; then TC_PCT=0; fi
            if [ -z "$UNACC_PCT" ]; then UNACC_PCT=0; fi
            
            echo "$LABEL,$proto,$PHY_TX,$PHY_RX,$MAC_TX,$MAC_RX,$WIFI_Q,$TC_BEFORE,$TC_AFTER,$TC_DROP,$TOTAL_GRAN,$E2E_LOST,$PHY_PCT,$MAC_PCT,$WIFI_Q_PCT,$TC_PCT,$UNACC_PCT" >> "$CSV_SINGLE_LOSS"
            echo "  -> Loss Breakdown: PHY=$PHY_PCT% MAC=$MAC_PCT% WiFiQueue=$WIFI_Q_PCT% TC=$TC_PCT% Unaccounted=$UNACC_PCT%"

            # Extract PHY RX drop reasons
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

                echo "$LABEL,$proto,$REASON,$REASON_COUNT,$REASON_PCT_PHY,$REASON_PCT_TOTAL" >> "$CSV_SINGLE_PHY_REASON"
            done < <(grep 'PHY_RX_DROP_REASON:' "$OUTFILE" || true)
        done
    done
    
    echo "Single Link Multi-STA experiments finished for $DATA_RATE. Results: $CSV_SINGLE"
    echo "Single Link Packet Loss Breakdown: $CSV_SINGLE_LOSS"
    
    # ========== MLO EXPERIMENTS ==========
    echo ""
    echo "--- MLO Multi-STA Experiments ($DATA_RATE) $OFDMA_LABEL ---"
    
    CSV_MLO="$OUTPUTS_DIR/mlo_multi_sta_results_${TIMESTAMP}.csv"
    echo "pair,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_MLO"
    
    # CSV for granular packet loss breakdown (MLO)
    CSV_MLO_LOSS="$OUTPUTS_DIR/mlo_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
    echo "pair,protocol,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_MLO_LOSS"

    # CSV for PHY RX drop reasons (MLO)
    CSV_MLO_PHY_REASON="$OUTPUTS_DIR/mlo_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
    echo "pair,protocol,reason,count,pct_of_phy_rx,pct_of_total_granular" > "$CSV_MLO_PHY_REASON"

    CSV_MLO_LINK_USAGE="$OUTPUTS_DIR/mlo_multi_sta_link_activity_${TIMESTAMP}.csv"
    echo "pair,protocol,link_id,tx_time_s,duty_pct,overlap_time_s,overlap_pct,mu_tx_count,su_tx_count" > "$CSV_MLO_LINK_USAGE"

    CSV_MLO_RU_USAGE="$OUTPUTS_DIR/mlo_multi_sta_ru_allocation_${TIMESTAMP}.csv"
    echo "pair,protocol,link_id,ru_type,count" > "$CSV_MLO_RU_USAGE"
    
    # Pares de frequências MLO
    declare -a PAIRS=("2 5 2.4+5" "2 6 2.4+6" "5 6 5+6")
    
    for pair in "${PAIRS[@]}"; do
        read -r F1 F2 NAME <<< "$pair"
        for proto in "${PROTOS[@]}"; do
            echo "Running MLO Multi-STA: $NAME ($proto) @ $DATA_RATE per STA $OFDMA_LABEL..."
            OUTFILE="$OUTPUTS_DIR/mlo_multi_sta_${F1}_${F2}_${proto}_${TIMESTAMP}.txt"
            ./ns3 run "scratch/wifi7-mlo-multi-sta --freq1=$F1 --freq2=$F2 --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --staticSetup=true --enablePcaps=false" 2>&1 | tee "$OUTFILE"

            # Extract metrics from FLOW_SUMMARY_TOTAL
            TP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' | sed 's/Throughput_Mbps=//')
            DELAY=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgDelay_ms=[0-9]+\.?[0-9]*' | sed 's/AvgDelay_ms=//')
            JITTER=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgJitter_ms=[0-9]+\.?[0-9]*' | sed 's/AvgJitter_ms=//')
            LOSS=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' | sed 's/LossRate_pct=//')
            AVGTP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgThroughputPerSTA_Mbps=[0-9]+\.?[0-9]*' | sed 's/AvgThroughputPerSTA_Mbps=//')
            
            if [ -z "$TP" ]; then TP=0; fi
            if [ -z "$DELAY" ]; then DELAY=0; fi
            if [ -z "$JITTER" ]; then JITTER=0; fi
            if [ -z "$LOSS" ]; then LOSS=0; fi
            if [ -z "$AVGTP" ]; then AVGTP=0; fi

            echo "$NAME,$proto,$TP,$DELAY,$JITTER,$LOSS,$AVGTP" >> "$CSV_MLO"
            echo "  -> Total TP: $TP Mbps, Delay: $DELAY ms, Jitter: $JITTER ms, Loss: $LOSS%, TP/STA: $AVGTP Mbps"
            
            # Extract granular packet loss breakdown (MLO)
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
            
            # Extract percentages
            PHY_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'PHY_pct=[0-9]+\.?[0-9]*' | sed 's/PHY_pct=//' || echo "0")
            MAC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'MAC_pct=[0-9]+\.?[0-9]*' | sed 's/MAC_pct=//' || echo "0")
            WIFI_Q_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'WifiQueue_pct=[0-9]+\.?[0-9]*' | sed 's/WifiQueue_pct=//' || echo "0")
            TC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'TC_pct=[0-9]+\.?[0-9]*' | sed 's/TC_pct=//' || echo "0")
            UNACC_PCT=$(grep 'LOSS_ATTRIBUTION' "$OUTFILE" | grep -oE 'Unaccounted_pct=[0-9]+\.?[0-9]*' | sed 's/Unaccounted_pct=//' || echo "0")
            
            if [ -z "$PHY_TX" ]; then PHY_TX=0; fi
            if [ -z "$PHY_RX" ]; then PHY_RX=0; fi
            if [ -z "$MAC_TX" ]; then MAC_TX=0; fi
            if [ -z "$MAC_RX" ]; then MAC_RX=0; fi
            if [ -z "$WIFI_Q" ]; then WIFI_Q=0; fi
            if [ -z "$TC_BEFORE" ]; then TC_BEFORE=0; fi
            if [ -z "$TC_AFTER" ]; then TC_AFTER=0; fi
            if [ -z "$TC_DROP" ]; then TC_DROP=0; fi
            if [ -z "$TOTAL_GRAN" ]; then TOTAL_GRAN=0; fi
            if [ -z "$E2E_LOST" ]; then E2E_LOST=0; fi
            if [ -z "$PHY_PCT" ]; then PHY_PCT=0; fi
            if [ -z "$MAC_PCT" ]; then MAC_PCT=0; fi
            if [ -z "$WIFI_Q_PCT" ]; then WIFI_Q_PCT=0; fi
            if [ -z "$TC_PCT" ]; then TC_PCT=0; fi
            if [ -z "$UNACC_PCT" ]; then UNACC_PCT=0; fi
            
            echo "$NAME,$proto,$PHY_TX,$PHY_RX,$MAC_TX,$MAC_RX,$WIFI_Q,$TC_BEFORE,$TC_AFTER,$TC_DROP,$TOTAL_GRAN,$E2E_LOST,$PHY_PCT,$MAC_PCT,$WIFI_Q_PCT,$TC_PCT,$UNACC_PCT" >> "$CSV_MLO_LOSS"
            echo "  -> Loss Breakdown: PHY=$PHY_PCT% MAC=$MAC_PCT% WiFiQueue=$WIFI_Q_PCT% TC=$TC_PCT% Unaccounted=$UNACC_PCT%"

            # Extract PHY RX drop reasons
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

                echo "$NAME,$proto,$REASON,$REASON_COUNT,$REASON_PCT_PHY,$REASON_PCT_TOTAL" >> "$CSV_MLO_PHY_REASON"
            done < <(grep 'PHY_RX_DROP_REASON:' "$OUTFILE" || true)

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
    echo "MLO Packet Loss Breakdown: $CSV_MLO_LOSS"
    
    # ========== GENERATE PLOTS ==========
    if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
        echo ""
        echo "--- Generating plots for $DATA_RATE $OFDMA_LABEL ---"
        
        # Generate Single Link plots
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_plots.py" "$CSV_SINGLE" "$PLOTS_DIR" "$DATA_RATE$OFDMA_SUFFIX"
        
        # Generate MLO plots  
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_plots.py" "$CSV_MLO" "$PLOTS_DIR" "$DATA_RATE$OFDMA_SUFFIX"
        
        # Generate granular packet loss breakdown plots
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_loss_breakdown_plots.py" "$CSV_SINGLE_LOSS" "$CSV_MLO_LOSS" "$PLOTS_DIR" "$DATA_RATE$OFDMA_SUFFIX" "$CSV_SINGLE_PHY_REASON" "$CSV_MLO_PHY_REASON"

        # Generate MLO STR link/RU usage plots and table
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_link_ru_plots.py" "$CSV_MLO_LINK_USAGE" "$CSV_MLO_RU_USAGE" "$PLOTS_DIR" "$DATA_RATE$OFDMA_SUFFIX"
        
        echo "Plots generated in: $PLOTS_DIR"
    fi
    
    # ========== SUMMARY ==========
    echo ""
    echo "=============================================="
    echo "Summary for Data Rate: $DATA_RATE (per STA) $OFDMA_LABEL"
    echo "=============================================="
    echo ""
    echo "Single Link Results:"
    cat "$CSV_SINGLE"
    echo ""
    echo "MLO Results:"
    cat "$CSV_MLO"
    
done
done  # End of OFDMA loop

if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
    echo ""
    echo "=============================================="
    echo "Generating comparison plots across all data rates..."
    echo "=============================================="

    # Generate comparison plots that show all data rates together
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_comparison_plots.py"
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_comparison_plots.py"

    # Generate per-STA throughput tables
    echo ""
    echo "=============================================="
    echo "Generating per-STA throughput tables..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tables.py"

    # Generate individual per-STA plots
    echo ""
    echo "=============================================="
    echo "Generating individual per-STA plots..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_individual_plots.py"
fi

echo ""
echo "=============================================="
echo "All Multi-STA experiments completed!"
echo "Results saved in: $RESULTS_DIR"
echo "Directories created:"
echo "  - outputs_150Mbps/ (sem OFDMA)"
echo "  - outputs_1200Mbps/ (sem OFDMA)"
echo "  - outputs_150Mbps_OFDMA/ (com OFDMA)"
echo "  - outputs_1200Mbps_OFDMA/ (com OFDMA)"
echo "  - plots_150Mbps/, plots_1200Mbps/ (sem OFDMA)"
echo "  - plots_150Mbps_OFDMA/, plots_1200Mbps_OFDMA/ (com OFDMA)"
echo "=============================================="
