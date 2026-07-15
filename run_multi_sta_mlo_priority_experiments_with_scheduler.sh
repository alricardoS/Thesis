#!/bin/bash
# run_multi_sta_mlo_priority_experiments_with_scheduler.sh
# Script para correr APENAS experimentos MLO com 4 STAs (com scheduler customizado)
# Gera outputs e gráficos - versão MLO only com traffic-aware scheduler
set -uo pipefail

BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="${RESULTS_DIR:-$BASE_DIR/results_multi_sta_mlo_priority_with_scheduler}"
SIM_TIME="17"
PYTHON_BIN="python3"
NUM_STAS_LIST="${NUM_STAS_LIST:-4}"
STA_TRAFFIC_TYPES="${STA_TRAFFIC_TYPES:-voice,video,besteffort,background}"
QUEUE_OCCUPANCY_LABEL="${QUEUE_OCCUPANCY_LABEL:-}"

QUEUE_LABEL_ARG=""
if [ -n "$QUEUE_OCCUPANCY_LABEL" ]; then
    QUEUE_LABEL_ARG=" --queueOccupancyLabel=$QUEUE_OCCUPANCY_LABEL"
fi

# Python post-processing (plots/tables) enabled by default
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"

DATA_RATES_LIST="${DATA_RATES_LIST:-30Mbps 150Mbps}"
read -r -a NUM_STAS <<< "$NUM_STAS_LIST"
read -r -a DATA_RATES <<< "$DATA_RATES_LIST"

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

# Compilar o programa MLO multi-sta com scheduler
echo "=============================================="
echo "Compiling MLO Multi-STA ns-3 program (with traffic-aware scheduler)..."
echo "=============================================="
./ns3 build scratch/wifi7-mlo-multi-sta-priority-sch

for OFDMA_ENABLED in "${OFDMA_MODES[@]}"; do
    # Definir sufixo para ficheiros baseado no modo OFDMA
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
    echo "Running MLO Multi-STA PRIORITY experiments for Data Rate: $DATA_RATE (per STA) $OFDMA_LABEL"
    echo "Total potential throughput: ${NSTAS} x $DATA_RATE"
    echo "STA traffic types: $STA_TRAFFIC_TYPES"
    echo "=============================================="
    
    # ========== MLO EXPERIMENTS ==========
    echo ""
    echo "--- MLO Multi-STA Experiments ($DATA_RATE) $OFDMA_LABEL ---"
    
    CSV_MLO="$OUTPUTS_DIR/mlo_multi_sta_results_${TIMESTAMP}.csv"
    echo "pair,protocol,total_tx_packets,total_rx_packets,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_MLO"
    
    # CSV for granular packet loss breakdown (MLO)
    CSV_MLO_LOSS="$OUTPUTS_DIR/mlo_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
    echo "pair,protocol,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_MLO_LOSS"

    CSV_MLO_LINK_TRAFFIC="$OUTPUTS_DIR/mlo_multi_sta_link_traffic_${TIMESTAMP}.csv"

    # CSV for PHY RX drop reasons (MLO)
    CSV_MLO_PHY_REASON="$OUTPUTS_DIR/mlo_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
    echo "pair,protocol,reason,count,pct_of_phy_rx,pct_of_total_granular" > "$CSV_MLO_PHY_REASON"

    CSV_MLO_TC_REASON="$OUTPUTS_DIR/mlo_multi_sta_tc_drop_reasons_${TIMESTAMP}.csv"
    echo "pair,protocol,stage,reason,count,pct_of_tc_stage,pct_of_total_granular" > "$CSV_MLO_TC_REASON"

    CSV_MLO_LINK_USAGE="$OUTPUTS_DIR/mlo_multi_sta_link_activity_${TIMESTAMP}.csv"
    echo "pair,protocol,link_id,tx_time_s,duty_pct,overlap_time_s,overlap_pct,mu_tx_count,su_tx_count" > "$CSV_MLO_LINK_USAGE"

    CSV_MLO_RU_USAGE="$OUTPUTS_DIR/mlo_multi_sta_ru_allocation_${TIMESTAMP}.csv"
    echo "pair,protocol,link_id,ru_type,count" > "$CSV_MLO_RU_USAGE"

    CSV_MLO_QUEUE_OCC="$OUTPUTS_DIR/mlo_multi_sta_queue_occupancy.csv"
    echo "run_label,time_s,role,node_id,ac,packets,bytes" > "$CSV_MLO_QUEUE_OCC"
    
    # Apenas UDP (TCP comentado para uso futuro)
    declare -a PROTOS=("UDP")

    # Pares de frequências MLO
    declare -a PAIRS=("2 5 2.4+5" "2 6 2.4+6" "5 6 5+6")
    
    for pair in "${PAIRS[@]}"; do
        read -r F1 F2 NAME <<< "$pair"
        for proto in "${PROTOS[@]}"; do
            echo "Running MLO Multi-STA: $NAME ($proto) @ $DATA_RATE per STA $OFDMA_LABEL..."
            OUTFILE="$OUTPUTS_DIR/mlo_multi_sta_${F1}_${F2}_${proto}_${TIMESTAMP}.txt"
            CSV_MLO_DECISIONS="$OUTPUTS_DIR/scheduler_decisions_${F1}_${F2}_${proto}_${TIMESTAMP}.csv"
            ./ns3 run "scratch/wifi7-mlo-multi-sta-priority-sch --freq1=$F1 --freq2=$F2 --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --staticSetup=true --enablePcaps=false --numStas=$NSTAS --staTrafficTypes=$STA_TRAFFIC_TYPES --queueOccupancyCsv=$CSV_MLO_QUEUE_OCC --linkTrafficCsv=$CSV_MLO_LINK_TRAFFIC$QUEUE_LABEL_ARG --queueSampleInterval=0.1 --linkTrafficSampleInterval=0.1 --useCustomMloScheduler=true --schedulerDecisionCsv=$CSV_MLO_DECISIONS" 2>&1 | tee "$OUTFILE"

            if [ ! -s "$OUTFILE" ]; then
                echo "[WARN] Missing or empty output file, skipping post-processing: $OUTFILE"
                continue
            fi

            # Extract metrics from FLOW_SUMMARY_TOTAL
            TP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' | sed 's/Throughput_Mbps=//' || echo "")
            DELAY=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgDelay_ms=[0-9]+\.?[0-9]*' | sed 's/AvgDelay_ms=//' || echo "")
            JITTER=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgJitter_ms=[0-9]+\.?[0-9]*' | sed 's/AvgJitter_ms=//' || echo "")
            LOSS=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' | sed 's/LossRate_pct=//' || echo "")
            AVGTP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgThroughputPerSTA_Mbps=[0-9]+\.?[0-9]*' | sed 's/AvgThroughputPerSTA_Mbps=//' || echo "")
            TOTAL_TX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'TX_Packets=[0-9]+' | sed 's/TX_Packets=//' || true)
            TOTAL_RX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'RX_Packets=[0-9]+' | sed 's/RX_Packets=//' || true)
            
            if [ -z "$TP" ]; then TP=0; fi
            if [ -z "$DELAY" ]; then DELAY=0; fi
            if [ -z "$JITTER" ]; then JITTER=0; fi
            if [ -z "$LOSS" ]; then LOSS=0; fi
            if [ -z "$AVGTP" ]; then AVGTP=0; fi
            if [ -z "$TOTAL_TX" ]; then TOTAL_TX=0; fi
            if [ -z "$TOTAL_RX" ]; then TOTAL_RX=0; fi

            echo "$NAME,$proto,$TOTAL_TX,$TOTAL_RX,$TP,$DELAY,$JITTER,$LOSS,$AVGTP" >> "$CSV_MLO"
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
        done
    done
    
    echo "MLO Multi-STA experiments finished for $DATA_RATE. Results: $CSV_MLO"
    echo "MLO Packet Loss Breakdown: $CSV_MLO_LOSS"
    
    # ========== GENERATE PLOTS ==========
    if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
        echo ""
        echo "--- Generating plots for $SCENARIO_TAG ---"
        
        # Generate MLO plots  
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_plots.py" "$CSV_MLO" "$PLOTS_DIR" "$SCENARIO_TAG"
        
        # Generate granular packet loss breakdown plots
        CSV_MLO_AS_SINGLE_LOSS="$OUTPUTS_DIR/mlo_as_single_loss_${TIMESTAMP}.csv"
        CSV_MLO_AS_SINGLE_PHY_REASON="$OUTPUTS_DIR/mlo_as_single_phy_reason_${TIMESTAMP}.csv"
        awk 'NR==1{sub(/^pair,/,"freq_band,")}1' "$CSV_MLO_LOSS" > "$CSV_MLO_AS_SINGLE_LOSS"
        awk 'NR==1{sub(/^pair,/,"freq_band,")}1' "$CSV_MLO_PHY_REASON" > "$CSV_MLO_AS_SINGLE_PHY_REASON"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_loss_breakdown_plots.py" "$CSV_MLO_AS_SINGLE_LOSS" "$CSV_MLO_LOSS" "$PLOTS_DIR" "$SCENARIO_TAG" "$CSV_MLO_AS_SINGLE_PHY_REASON" "$CSV_MLO_PHY_REASON" "$CSV_MLO_LINK_TRAFFIC"

        # Generate MLO STR link/RU usage plots and table
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_link_ru_plots.py" "$CSV_MLO_LINK_USAGE" "$CSV_MLO_RU_USAGE" "$PLOTS_DIR" "$SCENARIO_TAG" "$CSV_MLO_LINK_TRAFFIC"

        # Generate additional tables in plot folder
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_phy_drop_reason_table.py" "$CSV_MLO_PHY_REASON" "$PLOTS_DIR" "$SCENARIO_TAG"
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tc_drop_reason_table.py" "$CSV_MLO_TC_REASON" "$CSV_MLO_TC_REASON" "$PLOTS_DIR" "$SCENARIO_TAG"

        "$PYTHON_BIN" - "$CSV_MLO" "$CSV_MLO_LOSS" "$CSV_MLO_LINK_TRAFFIC" "$PLOTS_DIR" "$SCENARIO_TAG" <<'PY'
import os
import sys
import pandas as pd

results_csv, loss_csv, link_traffic_csv, out_dir, scenario = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
out_file = os.path.join(out_dir, "mlo_drop_count_table.txt")

if not os.path.exists(loss_csv):
    sys.exit(0)

loss_df = pd.read_csv(loss_csv)
results_df = pd.read_csv(results_csv) if os.path.exists(results_csv) else pd.DataFrame()
link_df = pd.read_csv(link_traffic_csv) if link_traffic_csv and os.path.exists(link_traffic_csv) else pd.DataFrame()

for c in [
    "pair", "protocol", "phy_tx_drop", "phy_rx_drop", "mac_tx_drop", "mac_rx_drop", "wifi_queue_drop",
    "tc_drop_before", "tc_drop_after", "tc_drop", "total_granular", "e2e_lost",
    "phy_pct", "mac_pct", "wifi_queue_pct", "tc_pct", "unaccounted_pct"
]:
    if c not in loss_df.columns:
        loss_df[c] = 0

for c in ["pair", "protocol", "total_tx_packets", "total_rx_packets"]:
    if c not in results_df.columns:
        results_df[c] = 0

merged = loss_df.merge(
    results_df[["pair", "protocol", "total_tx_packets", "total_rx_packets"]],
    on=["pair", "protocol"],
    how="left",
)

for c in ["total_tx_packets", "total_rx_packets"]:
    merged[c] = pd.to_numeric(merged[c], errors="coerce").fillna(0).astype(int)

def allocate_counts(total, weights):
    total = int(round(float(total)))
    if total <= 0 or not weights:
        return [0 for _ in weights]

    clean_weights = [max(0.0, float(weight)) for weight in weights]
    weight_sum = sum(clean_weights)
    if weight_sum <= 0:
        base = total // len(clean_weights)
        remainder = total - (base * len(clean_weights))
        allocations = [base for _ in clean_weights]
        for idx in range(remainder):
            allocations[idx % len(allocations)] += 1
        return allocations

    raw = [total * weight / weight_sum for weight in clean_weights]
    allocations = [int(x) for x in raw]
    remainder = total - sum(allocations)
    if remainder > 0:
        order = sorted(range(len(raw)), key=lambda idx: (raw[idx] - allocations[idx], clean_weights[idx]), reverse=True)
        for idx in order[:remainder]:
            allocations[idx] += 1
    return allocations


def split_pair(value):
    parts = str(value).split("+")
    if len(parts) == 2:
        return parts[0], parts[1]
    return value, ""


def normalize_pair_label(value):
    parts = str(value).split("+")
    normalized = []
    for part in parts:
        token = part.strip()
        normalized.append("2.4" if token == "2" else token)
    return "+".join(normalized)

required_link_cols = {"pair", "protocol", "link_id", "link_name", "frame_count"}
rows = []

if not link_df.empty and required_link_cols.issubset(link_df.columns):
    for c in ["frame_count", "link_id"]:
        link_df[c] = pd.to_numeric(link_df[c], errors="coerce").fillna(0)

    link_df["pair_norm"] = link_df["pair"].apply(normalize_pair_label)

    link_df = (
        link_df.groupby(["pair_norm", "protocol", "link_id", "link_name"], as_index=False)[["frame_count"]]
        .sum()
        .sort_values(["pair_norm", "protocol", "link_id"])
    )

    loss_cols = [
        "phy_tx_drop", "phy_rx_drop", "mac_tx_drop", "mac_rx_drop", "wifi_queue_drop",
        "tc_drop_before", "tc_drop_after", "tc_drop", "total_granular", "e2e_lost",
    ]

    for _, loss_row in merged.iterrows():
        agg_total_drops = int(loss_row["total_granular"])
        
        pair_links = link_df[(link_df["pair_norm"] == normalize_pair_label(loss_row["pair"])) & (link_df["protocol"] == loss_row["protocol"])]
        
        if pair_links.empty:
            # Fallback: no link-level data, use aggregated metrics
            fallback_row = loss_row.to_dict()
            fallback_row["freq_1_gHz"], fallback_row["freq_2_gHz"] = split_pair(loss_row["pair"])
            fallback_row["link_id"] = -1
            fallback_row["frequency"] = str(loss_row["pair"])
            fallback_row["agg_total_drops"] = agg_total_drops
            rows.append(fallback_row)
            continue

        # Allocate drops proportionally to each link based on their traffic distribution
        link_traffic_counts = pair_links["frame_count"].tolist()
        link_traffic_sum = sum(link_traffic_counts)
        
        allocated = {col: allocate_counts(loss_row[col], link_traffic_counts) for col in loss_cols}
        allocated_e2e = allocate_counts(loss_row["e2e_lost"], link_traffic_counts)

        for idx, (_, link_row) in enumerate(pair_links.iterrows()):
            phy_tx = int(allocated["phy_tx_drop"][idx])
            phy_rx = int(allocated["phy_rx_drop"][idx])
            mac_tx = int(allocated["mac_tx_drop"][idx])
            mac_rx = int(allocated["mac_rx_drop"][idx])
            wifi_q = int(allocated["wifi_queue_drop"][idx])
            tc_before = int(allocated["tc_drop_before"][idx])
            tc_after = int(allocated["tc_drop_after"][idx])
            tc_drop = int(allocated["tc_drop"][idx])
            total = phy_tx + phy_rx + mac_tx + mac_rx + wifi_q + tc_before + tc_after + tc_drop
            e2e = int(allocated_e2e[idx])

            row = {
                "pair": loss_row["pair"],
                "freq_1_gHz": split_pair(loss_row["pair"])[0],
                "freq_2_gHz": split_pair(loss_row["pair"])[1],
                "protocol": loss_row["protocol"],
                "total_tx_packets": int(loss_row["total_tx_packets"]),
                "total_rx_packets": int(loss_row["total_rx_packets"]),
                "link_id": int(link_row["link_id"]),
                "frequency": str(link_row["link_name"]),
                "agg_total_drops": agg_total_drops,
                "phy_tx_drop": phy_tx,
                "phy_rx_drop": phy_rx,
                "mac_tx_drop": mac_tx,
                "mac_rx_drop": mac_rx,
                "wifi_queue_drop": wifi_q,
                "tc_drop_before": tc_before,
                "tc_drop_after": tc_after,
                "tc_drop": tc_drop,
                "total_granular": total,
                "e2e_lost": e2e,
                "drop_pct_agg_total_tx_packets": (agg_total_drops * 100.0 / int(loss_row["total_tx_packets"])) if int(loss_row["total_tx_packets"]) > 0 else 0.0,
            }
            if total > 0:
                row["phy_pct"] = ((phy_tx + phy_rx) * 100.0) / total
                row["mac_pct"] = ((mac_tx + mac_rx) * 100.0) / total
                row["wifi_queue_pct"] = (wifi_q * 100.0) / total
                row["tc_pct"] = ((tc_before + tc_after + tc_drop) * 100.0) / total
                row["unaccounted_pct"] = 100.0 - row["phy_pct"] - row["mac_pct"] - row["wifi_queue_pct"] - row["tc_pct"]
            else:
                row["phy_pct"] = 0.0
                row["mac_pct"] = 0.0
                row["wifi_queue_pct"] = 0.0
                row["tc_pct"] = 0.0
                row["unaccounted_pct"] = 0.0
            rows.append(row)
else:
    for _, loss_row in merged.iterrows():
        row = loss_row.to_dict()
        row["freq_1_gHz"], row["freq_2_gHz"] = split_pair(loss_row["pair"])
        row["link_id"] = -1
        row["frequency"] = str(loss_row["pair"])
        row["agg_total_drops"] = int(loss_row["total_granular"])
        row["drop_pct_agg_total_tx_packets"] = (int(loss_row["total_granular"]) * 100.0 / int(loss_row["total_tx_packets"])) if int(loss_row["total_tx_packets"]) > 0 else 0.0
        rows.append(row)

ordered_cols = [
    "pair", "freq_1_gHz", "freq_2_gHz", "protocol", "total_tx_packets", "total_rx_packets",
    "link_id", "frequency", "agg_total_drops", "phy_tx_drop", "phy_rx_drop", "mac_tx_drop",
    "mac_rx_drop", "wifi_queue_drop", "tc_drop_before", "tc_drop_after", "tc_drop",
    "total_granular", "e2e_lost", "drop_pct_agg_total_tx_packets",
    "phy_pct", "mac_pct", "wifi_queue_pct", "tc_pct", "unaccounted_pct",
]

merged = pd.DataFrame(rows)
for c in ordered_cols:
    if c not in merged.columns:
        merged[c] = 0

merged = merged[ordered_cols].sort_values(["pair", "protocol", "link_id"])

with open(out_file, "w") as f:
    f.write("=" * 130 + "\n")
    f.write(f"MLO Drop Count Table - {scenario}\n")
    f.write("=" * 130 + "\n\n")
    f.write(merged.to_string(index=False))
    f.write("\n")

print(f"MLO drop count table generated: {out_file}")
PY

        # Generate link traffic distribution, frame types tables and queue occupancy plots
        "$PYTHON_BIN" "$BASE_DIR/generate_link_traffic_distribution_table.py" "$OUTPUTS_DIR" "$SCENARIO_TAG" "$PLOTS_DIR/link_traffic_distribution_table.txt" "$STA_TRAFFIC_TYPES"
        "$PYTHON_BIN" "$BASE_DIR/generate_link_frame_types_distribution_table.py" "$OUTPUTS_DIR" "$SCENARIO_TAG" "$PLOTS_DIR/link_frame_types_distribution_table.txt"
        if [ -f "$CSV_MLO_QUEUE_OCC" ]; then
            "$PYTHON_BIN" "$BASE_DIR/generate_queue_occupancy_plots.py" "$CSV_MLO_QUEUE_OCC" "$PLOTS_DIR/queue_occupancy_mlo" "MLO Queue Occupancy"
        fi
        
        echo "Plots/tables generated in: $PLOTS_DIR"
    fi
    
    # ========== SUMMARY ==========
    echo ""
    echo "=============================================="
    echo "Summary for Data Rate: $DATA_RATE (per STA), STAs=$NSTAS $OFDMA_LABEL"
    echo "=============================================="
    echo ""
    echo "MLO Results:"
    cat "$CSV_MLO"
    
done
done
done  # End of OFDMA loop

if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
    echo ""
    echo "=============================================="
    echo "Generating comparison plots across all data rates..."
    echo "=============================================="

    # Generate comparison plots that show all data rates together
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_mlo_comparison_plots.py" "$RESULTS_DIR"

    # Generate per-STA throughput tables
    echo ""
    echo "=============================================="
    echo "Generating per-STA throughput tables..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tables.py" "$RESULTS_DIR"

    # Generate individual per-STA plots
    echo ""
    echo "=============================================="
    echo "Generating individual per-STA plots..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_individual_plots_priority.py" "$RESULTS_DIR"
fi

echo ""
echo "=============================================="
echo "All MLO Multi-STA experiments (with traffic-aware scheduler) completed!"
echo "Results saved in: $RESULTS_DIR"
echo "Per-scenario outputs in: $RESULTS_DIR/outputs_*/"
echo "Per-scenario plots/tables in: $RESULTS_DIR/plots_*/"
echo "Comparison plots in: $RESULTS_DIR/comparison_plots_mlo/"
echo "Tables in: $RESULTS_DIR/tables/"
echo "Individual plots in: $RESULTS_DIR/individual_sta_plots/"
echo "=============================================="
