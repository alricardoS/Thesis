#!/bin/bash
# run_single_link_2ul_2dl.sh
#
# Runner dedicado para o novo teste single-link com:
#   - 2 STAs uplink -> AP
#   - 2 STAs downlink <- AP
#   - 300 Mbps por ligação
#
# Inclui parsing de logs e geração de tabelas/gráficos.

set -euo pipefail

BASE_DIR="${BASE_DIR:-/home/ricardosantos/ns-3.47}"
RESULTS_DIR="${RESULTS_DIR:-$BASE_DIR/results_single_link_2ul_2dl}"
SIM_TIME="${SIM_TIME:-12}"
STATIC_SETUP="${STATIC_SETUP:-false}"
NUM_UL_STAS="${NUM_UL_STAS:-2}"
NUM_DL_STAS="${NUM_DL_STAS:-2}"
DATA_RATE="${DATA_RATE:-300Mbps}"
FREQS_LIST="${FREQS_LIST:-2 5 6}"
PROTOS_LIST="${PROTOS_LIST:-UDP}"
OFDMA_VARIANTS_LIST="${OFDMA_VARIANTS_LIST:-true,false false,false true,true}"
PYTHON_BIN="python3"
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"

export BASE_DIR RESULTS_DIR SIM_TIME STATIC_SETUP NUM_UL_STAS NUM_DL_STAS DATA_RATE FREQS_LIST PROTOS_LIST OFDMA_VARIANTS_LIST PYTHON_BIN GENERATE_PYTHON_REPORTS

cd "$BASE_DIR"
mkdir -p "$RESULTS_DIR"

pick_python()
{
    [ "$GENERATE_PYTHON_REPORTS" = "true" ] || return 0
    for CANDIDATE in "$BASE_DIR/.venv/bin/python3" "/usr/bin/python3" "python3"; do
        if [ "$CANDIDATE" = "python3" ] || [ -x "$CANDIDATE" ]; then
            if "$CANDIDATE" -c "import pandas, matplotlib, numpy" >/dev/null 2>&1; then
                PYTHON_BIN="$CANDIDATE"
                return 0
            fi
        fi
    done
    echo "[WARN] Python deps não encontradas. Será ignorado gerador de gráficos."
    GENERATE_PYTHON_REPORTS="false"
}

pct_of_total()
{
    local num="${1:-0}"
    local den="${2:-0}"
    awk -v n="$num" -v d="$den" 'BEGIN { if (d > 0) printf "%.6f", (n * 100.0 / d); else printf "0" }'
}

generate_single_plots_for_scenario()
{
    local scenario_tag="$1"
    local outputs_dir="$2"
    local plots_dir="$3"

    [ "$GENERATE_PYTHON_REPORTS" = "true" ] || return 0

    local csv_single csv_loss csv_phy csv_tc csv_phy_pkt_type csv_phy_pkt_size
    csv_single=$(ls -t "$outputs_dir"/single_multi_sta_results_*.csv 2>/dev/null | head -n1 || true)
    csv_loss=$(ls -t "$outputs_dir"/single_multi_sta_loss_breakdown_*.csv 2>/dev/null | head -n1 || true)
    csv_phy=$(ls -t "$outputs_dir"/single_multi_sta_phy_rx_reasons_*.csv 2>/dev/null | head -n1 || true)
    csv_tc=$(ls -t "$outputs_dir"/single_multi_sta_tc_drop_reasons_*.csv 2>/dev/null | head -n1 || true)
    csv_phy_pkt_type=$(ls -t "$outputs_dir"/single_multi_sta_phy_rx_pkt_type_*.csv 2>/dev/null | head -n1 || true)
    csv_phy_pkt_size=$(ls -t "$outputs_dir"/single_multi_sta_phy_rx_pkt_size_*.csv 2>/dev/null | head -n1 || true)

    if [ -z "$csv_single" ] || [ -z "$csv_loss" ] || [ -z "$csv_phy" ]; then
        echo "[WARN] CSVs em falta para plots do cenário $scenario_tag"
        return 0
    fi

    mkdir -p "$plots_dir"
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_plots.py" "$csv_single" "$plots_dir" "$scenario_tag"

    if [ -n "$csv_loss" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_single_link_loss_breakdown_plots.py" "$csv_loss" "$plots_dir" "$scenario_tag" "$csv_phy"
    fi

    "$PYTHON_BIN" - "$csv_loss" "$plots_dir" "$scenario_tag" <<'PY'
import os
import sys
import pandas as pd

loss_csv, out_dir, scenario = sys.argv[1], sys.argv[2], sys.argv[3]
out_file = os.path.join(out_dir, "single_drop_count_table.txt")

if not os.path.exists(loss_csv):
    sys.exit(0)

df = pd.read_csv(loss_csv)
cols = [
    "freq_band", "protocol", "total_tx_packets", "total_rx_packets",
    "phy_tx_drop", "phy_rx_drop", "mac_tx_drop", "mac_rx_drop", "wifi_queue_drop",
    "tc_drop_before", "tc_drop_after", "tc_drop", "total_granular", "e2e_lost",
    "total_granular_pct_of_total_tx", "e2e_lost_pct_of_total_tx"
]
for c in cols:
    if c not in df.columns:
        df[c] = 0

df = df[cols].sort_values(["freq_band", "protocol"])

with open(out_file, "w") as f:
    f.write("=" * 130 + "\n")
    f.write(f"Single-Link Drop Count Table - {scenario}\n")
    f.write("=" * 130 + "\n\n")
    f.write(df.to_string(index=False))
    f.write("\n")

print(f"Drop count table generated: {out_file}")
PY

    if [ -n "$csv_tc" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tc_drop_reason_table.py" "$csv_tc" "$outputs_dir/__mlo_missing__.csv" "$plots_dir" "$scenario_tag"
    fi
}

# Build the dedicated scratch program.
echo "=============================================="
echo "Compiling single-link 2UL+2DL program..."
echo "=============================================="
./ns3 build scratch/wifi7-single-link-2ul-2dl

pick_python

read -r -a FREQS <<< "$FREQS_LIST"
read -r -a PROTOS <<< "$PROTOS_LIST"
read -r -a OFDMA_VARIANTS <<< "$OFDMA_VARIANTS_LIST"

freq_label()
{
    case "$1" in
        2) echo "2.4GHz" ;;
        5) echo "5GHz" ;;
        6) echo "6GHz" ;;
        *) echo "${1}GHz" ;;
    esac
}

freq_tag()
{
    case "$1" in
        2) echo "2_4GHz" ;;
        5) echo "5GHz" ;;
        6) echo "6GHz" ;;
        *) echo "${1}GHz" ;;
    esac
}

bool_tag()
{
    if [ "$1" = "true" ]; then
        echo "1"
    else
        echo "0"
    fi
}

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
TOTAL_STAS=$((NUM_UL_STAS + NUM_DL_STAS))
TABLES_DIR="$RESULTS_DIR/tables"
mkdir -p "$TABLES_DIR"

for variant in "${OFDMA_VARIANTS[@]}"; do
    IFS=',' read -r UL_OFDMA_ENABLED BSRP_ENABLED <<< "$variant"
    UL_TAG=$(bool_tag "$UL_OFDMA_ENABLED")
    BSRP_TAG=$(bool_tag "$BSRP_ENABLED")
    VARIANT_TAG="ul${UL_TAG}_bsrp${BSRP_TAG}"
    VARIANT_LABEL="UL-OFDMA=${UL_OFDMA_ENABLED} BSRP=${BSRP_ENABLED}"

    RUN_TAG="${DATA_RATE}_${TOTAL_STAS}stas_${VARIANT_TAG}"
    OUTPUTS_DIR="$RESULTS_DIR/outputs_${RUN_TAG}"
    PLOTS_DIR="$RESULTS_DIR/plots_${RUN_TAG}"
    mkdir -p "$OUTPUTS_DIR" "$PLOTS_DIR"
        rm -f "$PLOTS_DIR"/throughput_by_band_variant.png \
            "$PLOTS_DIR"/delay_by_band_variant.png \
            "$PLOTS_DIR"/jitter_by_band_variant.png \
            "$PLOTS_DIR"/loss_by_band_variant.png

    CSV_RESULTS="$OUTPUTS_DIR/single_multi_sta_results_${TIMESTAMP}.csv"
    echo "freq_band,protocol,throughput_mbps,delay_ms,jitter_ms,loss_rate_pct,avg_throughput_per_sta_mbps" > "$CSV_RESULTS"

    CSV_LOSS="$OUTPUTS_DIR/single_multi_sta_loss_breakdown_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,total_rx_packets,phy_tx_drop,phy_rx_drop,mac_tx_drop,mac_rx_drop,wifi_queue_drop,tc_drop_before,tc_drop_after,tc_drop,total_granular,e2e_lost,total_granular_pct_of_total_tx,e2e_lost_pct_of_total_tx,phy_pct,mac_pct,wifi_queue_pct,tc_pct,unaccounted_pct" > "$CSV_LOSS"

    CSV_PHY_REASON="$OUTPUTS_DIR/single_multi_sta_phy_rx_reasons_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,reason,count,pct_of_phy_rx,pct_of_total_granular,pct_of_total_tx_packets" > "$CSV_PHY_REASON"

    CSV_PHY_PKT_TYPE="$OUTPUTS_DIR/single_multi_sta_phy_rx_pkt_type_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,reason,mac_type,count,pct_of_reason,pct_of_phy_rx,pct_of_total_tx_packets" > "$CSV_PHY_PKT_TYPE"

    CSV_PHY_PKT_SIZE="$OUTPUTS_DIR/single_multi_sta_phy_rx_pkt_size_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,packet_size,count,pct_of_phy_rx,pct_of_total_tx_packets" > "$CSV_PHY_PKT_SIZE"

    CSV_TC_REASON="$OUTPUTS_DIR/single_multi_sta_tc_drop_reasons_${TIMESTAMP}.csv"
    echo "freq_band,protocol,total_tx_packets,stage,reason,count,pct_of_tc_stage,pct_of_total_granular,pct_of_total_tx_packets" > "$CSV_TC_REASON"

    for freq in "${FREQS[@]}"; do
        LABEL=$(freq_label "$freq")
        TAG_LABEL=$(freq_tag "$freq")
        for proto in "${PROTOS[@]}"; do
            SCENARIO_TAG="${TAG_LABEL}_${proto}_${VARIANT_TAG}_${NUM_UL_STAS}ul${NUM_DL_STAS}dl_${DATA_RATE}"
            OUTPUT_DIR_RUN="$RESULTS_DIR/$SCENARIO_TAG"
            mkdir -p "$OUTPUT_DIR_RUN"
            LOG_FILE="$OUTPUT_DIR_RUN/run.log"

            echo ""
            echo "=============================================="
            echo "Running single-link 2UL+2DL experiment"
            echo "Frequency: $LABEL | Protocol: $proto | DataRate: $DATA_RATE | UL_STAs: $NUM_UL_STAS | DL_STAs: $NUM_DL_STAS | $VARIANT_LABEL"
            echo "=============================================="

            ./ns3 run "scratch/wifi7-single-link-2ul-2dl --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --simTime=$SIM_TIME --staticSetup=$STATIC_SETUP --numUlStas=$NUM_UL_STAS --numDlStas=$NUM_DL_STAS --enableOfdma=true --enableUlOfdma=$UL_OFDMA_ENABLED --enableBsrp=$BSRP_ENABLED" 2>&1 | tee "$LOG_FILE"

            # Parse results
            TP=$(grep 'FLOW_SUMMARY_TOTAL' "$LOG_FILE" | grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' | sed 's/Throughput_Mbps=//' || true)
            DELAY=$(grep 'FLOW_SUMMARY_TOTAL' "$LOG_FILE" | grep -oE 'AvgDelay_ms=[0-9]+\.?[0-9]*' | sed 's/AvgDelay_ms=//' || true)
            JITTER=$(grep 'FLOW_SUMMARY_TOTAL' "$LOG_FILE" | grep -oE 'AvgJitter_ms=[0-9]+\.?[0-9]*' | sed 's/AvgJitter_ms=//' || true)
            LOSS=$(grep 'FLOW_SUMMARY_TOTAL' "$LOG_FILE" | grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' | sed 's/LossRate_pct=//' || true)
            AVGTP=$(grep 'FLOW_SUMMARY_TOTAL' "$LOG_FILE" | grep -oE 'AvgThroughputPerSTA_Mbps=[0-9]+\.?[0-9]*' | sed 's/AvgThroughputPerSTA_Mbps=//' || true)
            TOTAL_TX=$(grep 'FLOW_SUMMARY_TOTAL' "$LOG_FILE" | grep -oE 'TX_Packets=[0-9]+' | sed 's/TX_Packets=//' || true)
            TOTAL_RX=$(grep 'FLOW_SUMMARY_TOTAL' "$LOG_FILE" | grep -oE 'RX_Packets=[0-9]+' | sed 's/RX_Packets=//' || true)

            TP=${TP:-0}
            DELAY=${DELAY:-0}
            JITTER=${JITTER:-0}
            LOSS=${LOSS:-0}
            AVGTP=${AVGTP:-0}
            TOTAL_TX=${TOTAL_TX:-0}
            TOTAL_RX=${TOTAL_RX:-0}

            echo "$LABEL,$proto,$TP,$DELAY,$JITTER,$LOSS,$AVGTP" >> "$CSV_RESULTS"

            # Parse loss breakdown
            PHY_TX=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'PhyTxDrop=[0-9]+' | sed 's/PhyTxDrop=//' || echo "0")
            PHY_RX=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'PhyRxDrop=[0-9]+' | sed 's/PhyRxDrop=//' || echo "0")
            MAC_TX=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'MacTxDrop=[0-9]+' | sed 's/MacTxDrop=//' || echo "0")
            MAC_RX=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'MacRxDrop=[0-9]+' | sed 's/MacRxDrop=//' || echo "0")
            WIFI_Q=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'WifiQueueDrop=[0-9]+' | sed 's/WifiQueueDrop=//' || echo "0")
            TC_BEFORE=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'TcDropBeforeEnqueue=[0-9]+' | sed 's/TcDropBeforeEnqueue=//' || echo "0")
            TC_AFTER=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'TcDropAfterDequeue=[0-9]+' | sed 's/TcDropAfterDequeue=//' || echo "0")
            TC_DROP=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'TcDrop=[0-9]+' | sed 's/TcDrop=//' || echo "0")
            TOTAL_GRAN=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'TotalGranularDrops=[0-9]+' | sed 's/TotalGranularDrops=//' || echo "0")
            E2E_LOST=$(grep 'PACKET_LOSS_BREAKDOWN' "$LOG_FILE" | grep -oE 'E2E_LostPackets=[0-9]+' | sed 's/E2E_LostPackets=//' || echo "0")

            PHY_PCT=$(grep 'LOSS_ATTRIBUTION' "$LOG_FILE" | grep -oE 'PHY_pct=-?[0-9]+\.?[0-9]*' | sed 's/PHY_pct=//' || echo "0")
            MAC_PCT=$(grep 'LOSS_ATTRIBUTION' "$LOG_FILE" | grep -oE 'MAC_pct=-?[0-9]+\.?[0-9]*' | sed 's/MAC_pct=//' || echo "0")
            WIFI_Q_PCT=$(grep 'LOSS_ATTRIBUTION' "$LOG_FILE" | grep -oE 'WifiQueue_pct=-?[0-9]+\.?[0-9]*' | sed 's/WifiQueue_pct=//' || echo "0")
            TC_PCT=$(grep 'LOSS_ATTRIBUTION' "$LOG_FILE" | grep -oE 'TC_pct=-?[0-9]+\.?[0-9]*' | sed 's/TC_pct=//' || echo "0")
            UNACC_PCT=$(grep 'LOSS_ATTRIBUTION' "$LOG_FILE" | grep -oE 'Unaccounted_pct=-?[0-9]+\.?[0-9]*' | sed 's/Unaccounted_pct=//' || echo "0")

            TOTAL_GRAN_PCT_TX=$(pct_of_total "$TOTAL_GRAN" "$TOTAL_TX")
            E2E_LOST_PCT_TX=$(pct_of_total "$E2E_LOST" "$TOTAL_TX")

            echo "$LABEL,$proto,$TOTAL_TX,$TOTAL_RX,$PHY_TX,$PHY_RX,$MAC_TX,$MAC_RX,$WIFI_Q,$TC_BEFORE,$TC_AFTER,$TC_DROP,$TOTAL_GRAN,$E2E_LOST,$TOTAL_GRAN_PCT_TX,$E2E_LOST_PCT_TX,$PHY_PCT,$MAC_PCT,$WIFI_Q_PCT,$TC_PCT,$UNACC_PCT" >> "$CSV_LOSS"

            while IFS= read -r line; do
                REASON=$(echo "$line" | grep -oE 'Reason=[^ ]+' | sed 's/Reason=//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                [ -n "$REASON" ] || continue
                echo "$LABEL,$proto,$TOTAL_TX,$REASON,${COUNT:-0},${PCT_PHY:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_PHY_REASON"
            done < <(grep 'PHY_RX_DROP_REASON:' "$LOG_FILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Reason=/){sub(/^Reason=/,"",$i); print $i; break}}}')
                MACTYPE=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^MacType=/){sub(/^MacType=/,"",$i); print $i; break}}}')
                COUNT=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Count=/){sub(/^Count=/,"",$i); print $i; break}}}')
                PCT_REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctReason=/){sub(/^PctReason=/,"",$i); print $i; break}}}')
                PCT_PHY=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctPhyRx=/){sub(/^PctPhyRx=/,"",$i); print $i; break}}}')
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                [ -n "$REASON" ] || continue
                [ -n "$MACTYPE" ] || MACTYPE="UNKNOWN"
                echo "$LABEL,$proto,$TOTAL_TX,$REASON,$MACTYPE,${COUNT:-0},${PCT_REASON:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_PHY_PKT_TYPE"
            done < <(grep 'PHY_RX_DROP_PKT_TYPE:' "$LOG_FILE" || true)

            while IFS= read -r line; do
                PKT_SIZE=$(echo "$line" | grep -oE 'PacketSize=[0-9]+' | sed 's/PacketSize=//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                [ -n "$PKT_SIZE" ] || continue
                echo "$LABEL,$proto,$TOTAL_TX,${PKT_SIZE:-0},${COUNT:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_PHY_PKT_SIZE"
            done < <(grep 'PHY_RX_DROP_PKT_SIZE:' "$LOG_FILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_STAGE=$(echo "$line" | grep -oE 'PctTcBefore=-?[0-9]+\.?[0-9]*' | sed 's/PctTcBefore=//')
                PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                [ -n "$REASON" ] || continue
                echo "$LABEL,$proto,$TOTAL_TX,before_enqueue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_TC_REASON"
            done < <(grep 'TC_DROP_BEFORE_REASON:' "$LOG_FILE" || true)

            while IFS= read -r line; do
                REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                PCT_STAGE=$(echo "$line" | grep -oE 'PctTcAfter=-?[0-9]+\.?[0-9]*' | sed 's/PctTcAfter=//')
                PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                [ -n "$REASON" ] || continue
                echo "$LABEL,$proto,$TOTAL_TX,after_dequeue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_TC_REASON"
            done < <(grep 'TC_DROP_AFTER_REASON:' "$LOG_FILE" || true)
        done
    done

    generate_single_plots_for_scenario "$RUN_TAG" "$OUTPUTS_DIR" "$PLOTS_DIR"

    echo "[OK] Variant concluído: $VARIANT_TAG"
    echo "     CSV main: $CSV_RESULTS"
    echo "     CSV loss: $CSV_LOSS"
    echo "     Plots:    $PLOTS_DIR"
done

echo ""
echo "=============================================="
echo "All scenarios complete"
echo "=============================================="
echo "Outputs by flags:       $RESULTS_DIR/outputs_*_ul*_bsrp*/"
echo "Plots by flags:         $RESULTS_DIR/plots_*_ul*_bsrp*/"
echo "Tables in plots dirs:   single_drop_count_table.txt, loss_breakdown_table.txt, tc_drop_reason_table.txt"
echo "Scenario results dirs:  $RESULTS_DIR/"
echo "=============================================="
