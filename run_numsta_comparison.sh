#!/bin/bash
# run_numsta_comparison.sh
# Runner "estilo run_multi_sta_experiments.sh" para variação de número de STAs
# Testa SLO e MLO, com e sem OFDMA, guardando tudo numa pasta separada.
set -e

BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="${RESULTS_DIR:-$BASE_DIR/results_multi_sta_scaling}"
SIM_TIME="12"
PYTHON_BIN="python3"
BUILD_LOCK_FILE="$BASE_DIR/.ns3_build.lock"
FINALIZE_DONE="false"

# Python post-processing (plots/tables) enabled by default
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"

# Configuração principal
declare -a DATA_RATES=("150Mbps")
NUM_STAS_LIST="${NUM_STAS_LIST:-2 4 8 16}"
read -r -a NUM_STAS <<< "$NUM_STAS_LIST"
declare -a OFDMA_MODES=("false" "true")

# Protocolos em teste (TCP opcional)
declare -a PROTOS=("UDP")
# declare -a PROTOS=("UDP" "TCP")

declare -a FREQS=("2" "5" "6")
declare -a PAIRS=("2 5 2.4+5" "2 6 2.4+6" "5 6 5+6")

cd "$BASE_DIR"
mkdir -p "$RESULTS_DIR"

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

generate_scenario_plots()
{
    local scenario_tag="$1"
    local outputs_dir="$2"
    local plots_dir="$3"

    local csv_single csv_mlo csv_single_loss csv_mlo_loss csv_single_phy csv_mlo_phy csv_mlo_link_usage csv_mlo_ru_usage csv_mlo_ctrl_link csv_single_tc csv_mlo_tc

    csv_single=$(ls -t "$outputs_dir"/single_multi_sta_results_*.csv 2>/dev/null | head -n1 || true)
    csv_mlo=$(ls -t "$outputs_dir"/mlo_multi_sta_results_*.csv 2>/dev/null | head -n1 || true)
    csv_single_loss=$(ls -t "$outputs_dir"/single_multi_sta_loss_breakdown_*.csv 2>/dev/null | head -n1 || true)
    csv_mlo_loss=$(ls -t "$outputs_dir"/mlo_multi_sta_loss_breakdown_*.csv 2>/dev/null | head -n1 || true)
    csv_single_phy=$(ls -t "$outputs_dir"/single_multi_sta_phy_rx_reasons_*.csv 2>/dev/null | head -n1 || true)
    csv_mlo_phy=$(ls -t "$outputs_dir"/mlo_multi_sta_phy_rx_reasons_*.csv 2>/dev/null | head -n1 || true)
    csv_mlo_link_usage=$(ls -t "$outputs_dir"/mlo_multi_sta_link_activity_*.csv 2>/dev/null | head -n1 || true)
    csv_mlo_ru_usage=$(ls -t "$outputs_dir"/mlo_multi_sta_ru_allocation_*.csv 2>/dev/null | head -n1 || true)
    csv_mlo_ctrl_link=$(ls -t "$outputs_dir"/mlo_multi_sta_control_link_summary_*.csv 2>/dev/null | head -n1 || true)
    csv_single_tc=$(ls -t "$outputs_dir"/single_multi_sta_tc_drop_reasons_*.csv 2>/dev/null | head -n1 || true)
    csv_mlo_tc=$(ls -t "$outputs_dir"/mlo_multi_sta_tc_drop_reasons_*.csv 2>/dev/null | head -n1 || true)

    if [ -z "$csv_single" ] || [ -z "$csv_mlo" ] || [ -z "$csv_single_loss" ] || [ -z "$csv_mlo_loss" ] || [ -z "$csv_single_phy" ] || [ -z "$csv_mlo_phy" ]; then
        echo "[WARN] Missing CSV inputs for scenario $scenario_tag. Skipping per-scenario plot generation."
        return 1
    fi

    mkdir -p "$plots_dir"

    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_plots.py" "$csv_single" "$plots_dir" "$scenario_tag"
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_plots.py" "$csv_mlo" "$plots_dir" "$scenario_tag"
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_loss_breakdown_plots.py" "$csv_single_loss" "$csv_mlo_loss" "$plots_dir" "$scenario_tag" "$csv_single_phy" "$csv_mlo_phy"

    if [ -n "$csv_mlo_link_usage" ] && [ -n "$csv_mlo_ru_usage" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_link_ru_plots.py" "$csv_mlo_link_usage" "$csv_mlo_ru_usage" "$plots_dir" "$scenario_tag"
    fi

    if [ -n "$csv_mlo_ctrl_link" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_control_link_table.py" "$csv_mlo_ctrl_link" "$plots_dir" "$scenario_tag"
    fi

    if [ -n "$csv_single_tc" ] || [ -n "$csv_mlo_tc" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tc_drop_reason_table.py" "$csv_single_tc" "$csv_mlo_tc" "$plots_dir" "$scenario_tag"
    fi
}

reconcile_missing_scenario_plots()
{
    [ "$GENERATE_PYTHON_REPORTS" = "true" ] || return 0

    echo ""
    echo "=============================================="
    echo "Reconciling per-scenario plot folders..."
    echo "=============================================="

    local outputs_dir scenario_tag plots_dir file_count
    for outputs_dir in "$RESULTS_DIR"/outputs_*; do
        [ -d "$outputs_dir" ] || continue
        scenario_tag="${outputs_dir##*/outputs_}"
        plots_dir="$RESULTS_DIR/plots_${scenario_tag}"
        mkdir -p "$plots_dir"

        file_count=$(find "$plots_dir" -maxdepth 1 -type f | wc -l)
        if [ "$file_count" -eq 0 ]; then
            echo "[INFO] Plot folder empty for $scenario_tag. Regenerating from CSV outputs..."
            if ! generate_scenario_plots "$scenario_tag" "$outputs_dir" "$plots_dir"; then
                echo "[WARN] Could not regenerate plots for $scenario_tag"
            fi
        fi
    done
}

build_ns3_with_lock()
{
    echo "=============================================="
    echo "Compiling Multi-STA scaling ns-3 programs..."
    echo "=============================================="

    if command -v flock >/dev/null 2>&1; then
        exec 9>"$BUILD_LOCK_FILE"
        echo "[INFO] Waiting for build lock: $BUILD_LOCK_FILE"
        flock 9
        ./ns3 build scratch/wifi7-single-link-multi-sta
        ./ns3 build scratch/wifi7-mlo-multi-sta
        flock -u 9
        exec 9>&-
    else
        ./ns3 build scratch/wifi7-single-link-multi-sta
        ./ns3 build scratch/wifi7-mlo-multi-sta
    fi
}

finalize_reports()
{
    [ "$GENERATE_PYTHON_REPORTS" = "true" ] || return 0
    [ "$FINALIZE_DONE" = "true" ] && return 0
    FINALIZE_DONE="true"

    set +e

    reconcile_missing_scenario_plots

    echo ""
    echo "=============================================="
    echo "Generating comparison plots across all scenarios..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_comparison_plots.py" "$RESULTS_DIR"
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_comparison_plots.py" "$RESULTS_DIR"

    echo ""
    echo "=============================================="
    echo "Generating per-STA throughput tables..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tables.py" "$RESULTS_DIR"

    echo ""
    echo "=============================================="
    echo "Generating individual per-STA plots..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_individual_plots.py" "$RESULTS_DIR"

    set -e
}

trap finalize_reports EXIT

build_ns3_with_lock

for OFDMA_ENABLED in "${OFDMA_MODES[@]}"; do
    if [ "$OFDMA_ENABLED" == "true" ]; then
        OFDMA_SUFFIX="_OFDMA"
        OFDMA_LABEL="with OFDMA"
    else
        OFDMA_SUFFIX=""
        OFDMA_LABEL="without OFDMA"
    fi

    for NSTAS in "${NUM_STAS[@]}"; do
        for DATA_RATE in "${DATA_RATES[@]}"; do
            TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
            SCENARIO_TAG="${DATA_RATE}_${NSTAS}stas${OFDMA_SUFFIX}"

            OUTPUTS_DIR="$RESULTS_DIR/outputs_${SCENARIO_TAG}"
            PLOTS_DIR="$RESULTS_DIR/plots_${SCENARIO_TAG}"
            mkdir -p "$OUTPUTS_DIR" "$PLOTS_DIR"

            echo ""
            echo "=============================================="
            echo "Running Multi-STA scaling experiments"
            echo "Data Rate: $DATA_RATE (per STA) | Num STAs: $NSTAS | $OFDMA_LABEL"
            echo "Total offered load: $NSTAS x $DATA_RATE"
            echo "=============================================="

            # ========== SINGLE LINK (SLO) ==========
            echo ""
            echo "--- Single Link Multi-STA Experiments ---"

            CSV_SINGLE="$OUTPUTS_DIR/single_multi_sta_results_${TIMESTAMP}.csv"
            echo "freq_band,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_SINGLE"

            CSV_SINGLE_LOSS="$OUTPUTS_DIR/single_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
            echo "freq_band,protocol,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_SINGLE_LOSS"

            CSV_SINGLE_PHY_REASON="$OUTPUTS_DIR/single_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
            echo "freq_band,protocol,reason,count,pct_of_phy_rx,pct_of_total_granular" > "$CSV_SINGLE_PHY_REASON"

            CSV_SINGLE_TC_REASON="$OUTPUTS_DIR/single_multi_sta_tc_drop_reasons_${TIMESTAMP}.csv"
            echo "freq_band,protocol,stage,reason,count,pct_of_tc_stage,pct_of_total_granular" > "$CSV_SINGLE_TC_REASON"

            for freq in "${FREQS[@]}"; do
                for proto in "${PROTOS[@]}"; do
                    if [ "$freq" == "2" ]; then LABEL="2.4GHz"; fi
                    if [ "$freq" == "5" ]; then LABEL="5GHz"; fi
                    if [ "$freq" == "6" ]; then LABEL="6GHz"; fi

                    echo "Running Single Link: $LABEL ($proto) @ $DATA_RATE per STA | STAs=$NSTAS | $OFDMA_LABEL"
                    OUTFILE="$OUTPUTS_DIR/single_multi_sta_${freq}_${proto}_${TIMESTAMP}.txt"
                    ./ns3 run "scratch/wifi7-single-link-multi-sta --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --numStas=$NSTAS --simTime=$SIM_TIME --staticSetup=true --enableOfdma=$OFDMA_ENABLED" 2>&1 | tee "$OUTFILE"

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

                    while IFS= read -r tcLine; do
                        [ -z "$tcLine" ] && continue
                        STAGE="BEFORE"
                        REASON=$(echo "$tcLine" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=//')
                        REASON_COUNT=$(echo "$tcLine" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        REASON_PCT_STAGE=$(echo "$tcLine" | grep -oE 'PctTcBefore=[0-9]+\.?[0-9]*' | sed 's/PctTcBefore=//')
                        REASON_PCT_TOTAL=$(echo "$tcLine" | grep -oE 'PctTotalDrops=[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')

                        [ -z "$REASON" ] && REASON="UNKNOWN"
                        [ -z "$REASON_COUNT" ] && REASON_COUNT=0
                        [ -z "$REASON_PCT_STAGE" ] && REASON_PCT_STAGE=0
                        [ -z "$REASON_PCT_TOTAL" ] && REASON_PCT_TOTAL=0

                        echo "$LABEL,$proto,$STAGE,$REASON,$REASON_COUNT,$REASON_PCT_STAGE,$REASON_PCT_TOTAL" >> "$CSV_SINGLE_TC_REASON"
                    done < <(grep 'TC_DROP_BEFORE_REASON:' "$OUTFILE" || true)

                    while IFS= read -r tcLine; do
                        [ -z "$tcLine" ] && continue
                        STAGE="AFTER"
                        REASON=$(echo "$tcLine" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=//')
                        REASON_COUNT=$(echo "$tcLine" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        REASON_PCT_STAGE=$(echo "$tcLine" | grep -oE 'PctTcAfter=[0-9]+\.?[0-9]*' | sed 's/PctTcAfter=//')
                        REASON_PCT_TOTAL=$(echo "$tcLine" | grep -oE 'PctTotalDrops=[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')

                        [ -z "$REASON" ] && REASON="UNKNOWN"
                        [ -z "$REASON_COUNT" ] && REASON_COUNT=0
                        [ -z "$REASON_PCT_STAGE" ] && REASON_PCT_STAGE=0
                        [ -z "$REASON_PCT_TOTAL" ] && REASON_PCT_TOTAL=0

                        echo "$LABEL,$proto,$STAGE,$REASON,$REASON_COUNT,$REASON_PCT_STAGE,$REASON_PCT_TOTAL" >> "$CSV_SINGLE_TC_REASON"
                    done < <(grep 'TC_DROP_AFTER_REASON:' "$OUTFILE" || true)
                done
            done

            echo "Single Link finished. Results: $CSV_SINGLE"
            echo "Single Link Packet Loss Breakdown: $CSV_SINGLE_LOSS"

            # ========== MLO ==========
            echo ""
            echo "--- MLO Multi-STA Experiments ---"

            CSV_MLO="$OUTPUTS_DIR/mlo_multi_sta_results_${TIMESTAMP}.csv"
            echo "pair,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_MLO"

            CSV_MLO_LOSS="$OUTPUTS_DIR/mlo_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
            echo "pair,protocol,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_MLO_LOSS"

            CSV_MLO_PHY_REASON="$OUTPUTS_DIR/mlo_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
            echo "pair,protocol,reason,count,pct_of_phy_rx,pct_of_total_granular" > "$CSV_MLO_PHY_REASON"

            CSV_MLO_TC_REASON="$OUTPUTS_DIR/mlo_multi_sta_tc_drop_reasons_${TIMESTAMP}.csv"
            echo "pair,protocol,stage,reason,count,pct_of_tc_stage,pct_of_total_granular" > "$CSV_MLO_TC_REASON"

            CSV_MLO_LINK_USAGE="$OUTPUTS_DIR/mlo_multi_sta_link_activity_${TIMESTAMP}.csv"
            echo "pair,protocol,link_id,tx_time_s,duty_pct,overlap_time_s,overlap_pct,mu_tx_count,su_tx_count" > "$CSV_MLO_LINK_USAGE"

            CSV_MLO_RU_USAGE="$OUTPUTS_DIR/mlo_multi_sta_ru_allocation_${TIMESTAMP}.csv"
            echo "pair,protocol,link_id,ru_type,count" > "$CSV_MLO_RU_USAGE"

            CSV_MLO_CTRL_LINK="$OUTPUTS_DIR/mlo_multi_sta_control_link_summary_${TIMESTAMP}.csv"
            echo "pair,protocol,response_type,matched,same_link,cross_link,unmatched,same_link_pct" > "$CSV_MLO_CTRL_LINK"

            for pair in "${PAIRS[@]}"; do
                read -r F1 F2 NAME <<< "$pair"
                for proto in "${PROTOS[@]}"; do
                    echo "Running MLO: $NAME ($proto) @ $DATA_RATE per STA | STAs=$NSTAS | $OFDMA_LABEL"
                    OUTFILE="$OUTPUTS_DIR/mlo_multi_sta_${F1}_${F2}_${proto}_${TIMESTAMP}.txt"
                    ./ns3 run "scratch/wifi7-mlo-multi-sta --freq1=$F1 --freq2=$F2 --protocol=$proto --dataRate=$DATA_RATE --numStas=$NSTAS --simTime=$SIM_TIME --staticSetup=true --enablePcaps=false --enableOfdma=$OFDMA_ENABLED" 2>&1 | tee "$OUTFILE"

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

                    while IFS= read -r tcLine; do
                        [ -z "$tcLine" ] && continue
                        STAGE="BEFORE"
                        REASON=$(echo "$tcLine" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=//')
                        REASON_COUNT=$(echo "$tcLine" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        REASON_PCT_STAGE=$(echo "$tcLine" | grep -oE 'PctTcBefore=[0-9]+\.?[0-9]*' | sed 's/PctTcBefore=//')
                        REASON_PCT_TOTAL=$(echo "$tcLine" | grep -oE 'PctTotalDrops=[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')

                        [ -z "$REASON" ] && REASON="UNKNOWN"
                        [ -z "$REASON_COUNT" ] && REASON_COUNT=0
                        [ -z "$REASON_PCT_STAGE" ] && REASON_PCT_STAGE=0
                        [ -z "$REASON_PCT_TOTAL" ] && REASON_PCT_TOTAL=0

                        echo "$NAME,$proto,$STAGE,$REASON,$REASON_COUNT,$REASON_PCT_STAGE,$REASON_PCT_TOTAL" >> "$CSV_MLO_TC_REASON"
                    done < <(grep 'TC_DROP_BEFORE_REASON:' "$OUTFILE" || true)

                    while IFS= read -r tcLine; do
                        [ -z "$tcLine" ] && continue
                        STAGE="AFTER"
                        REASON=$(echo "$tcLine" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=//')
                        REASON_COUNT=$(echo "$tcLine" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        REASON_PCT_STAGE=$(echo "$tcLine" | grep -oE 'PctTcAfter=[0-9]+\.?[0-9]*' | sed 's/PctTcAfter=//')
                        REASON_PCT_TOTAL=$(echo "$tcLine" | grep -oE 'PctTotalDrops=[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')

                        [ -z "$REASON" ] && REASON="UNKNOWN"
                        [ -z "$REASON_COUNT" ] && REASON_COUNT=0
                        [ -z "$REASON_PCT_STAGE" ] && REASON_PCT_STAGE=0
                        [ -z "$REASON_PCT_TOTAL" ] && REASON_PCT_TOTAL=0

                        echo "$NAME,$proto,$STAGE,$REASON,$REASON_COUNT,$REASON_PCT_STAGE,$REASON_PCT_TOTAL" >> "$CSV_MLO_TC_REASON"
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

                    while IFS= read -r ctrlLine; do
                        [ -z "$ctrlLine" ] && continue
                        RESP_TYPE=$(echo "$ctrlLine" | grep -oE 'ResponseType=[^ ]+' | sed 's/ResponseType=//')
                        MATCHED=$(echo "$ctrlLine" | grep -oE 'Matched=[0-9]+' | sed 's/Matched=//')
                        SAME=$(echo "$ctrlLine" | grep -oE 'SameLink=[0-9]+' | sed 's/SameLink=//')
                        CROSS=$(echo "$ctrlLine" | grep -oE 'CrossLink=[0-9]+' | sed 's/CrossLink=//')
                        UNMATCHED=$(echo "$ctrlLine" | grep -oE 'Unmatched=[0-9]+' | sed 's/Unmatched=//')
                        SAME_PCT=$(echo "$ctrlLine" | grep -oE 'SameLink_pct=[0-9]+\.?[0-9]*' | sed 's/SameLink_pct=//')

                        [ -z "$RESP_TYPE" ] && RESP_TYPE=UNKNOWN
                        [ -z "$MATCHED" ] && MATCHED=0
                        [ -z "$SAME" ] && SAME=0
                        [ -z "$CROSS" ] && CROSS=0
                        [ -z "$UNMATCHED" ] && UNMATCHED=0
                        [ -z "$SAME_PCT" ] && SAME_PCT=0

                        echo "$NAME,$proto,$RESP_TYPE,$MATCHED,$SAME,$CROSS,$UNMATCHED,$SAME_PCT" >> "$CSV_MLO_CTRL_LINK"
                    done < <(grep 'CONTROL_LINK_SUMMARY:' "$OUTFILE" || true)

                    while IFS= read -r totalLine; do
                        [ -z "$totalLine" ] && continue
                        MATCHED=$(echo "$totalLine" | grep -oE 'Matched=[0-9]+' | sed 's/Matched=//')
                        SAME=$(echo "$totalLine" | grep -oE 'SameLink=[0-9]+' | sed 's/SameLink=//')
                        CROSS=$(echo "$totalLine" | grep -oE 'CrossLink=[0-9]+' | sed 's/CrossLink=//')
                        UNMATCHED=$(echo "$totalLine" | grep -oE 'Unmatched=[0-9]+' | sed 's/Unmatched=//')
                        SAME_PCT=$(echo "$totalLine" | grep -oE 'SameLink_pct=[0-9]+\.?[0-9]*' | sed 's/SameLink_pct=//')

                        [ -z "$MATCHED" ] && MATCHED=0
                        [ -z "$SAME" ] && SAME=0
                        [ -z "$CROSS" ] && CROSS=0
                        [ -z "$UNMATCHED" ] && UNMATCHED=0
                        [ -z "$SAME_PCT" ] && SAME_PCT=0

                        echo "$NAME,$proto,TOTAL,$MATCHED,$SAME,$CROSS,$UNMATCHED,$SAME_PCT" >> "$CSV_MLO_CTRL_LINK"
                    done < <(grep 'CONTROL_LINK_SUMMARY_TOTAL:' "$OUTFILE" || true)
                done
            done

            echo "MLO finished. Results: $CSV_MLO"
            echo "MLO Packet Loss Breakdown: $CSV_MLO_LOSS"

            # ========== PLOTS ==========
            if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
                echo ""
                echo "--- Generating plots for scenario: $SCENARIO_TAG ---"
                generate_scenario_plots "$SCENARIO_TAG" "$OUTPUTS_DIR" "$PLOTS_DIR"

                echo "Plots generated in: $PLOTS_DIR"
            fi

            # ========== SUMMARY ==========
            echo ""
            echo "=============================================="
            echo "Summary for scenario: $SCENARIO_TAG"
            echo "=============================================="
            echo ""
            echo "Single Link Results:"
            cat "$CSV_SINGLE"
            echo ""
            echo "MLO Results:"
            cat "$CSV_MLO"
        done
    done
done

finalize_reports

echo ""
echo "=============================================="
echo "All Multi-STA scaling experiments completed!"
echo "Results saved in: $RESULTS_DIR"
echo "=============================================="
