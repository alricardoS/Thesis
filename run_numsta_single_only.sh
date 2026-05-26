#!/bin/bash
# run_numsta_single_only.sh
#
# Runner focado APENAS em Single-Link (SLO), reutilizando o mesmo estilo de
# run_numsta_comparison.sh, mas limitado a:
#   - Num STAs: 2, 4, 6, 8
#   - Data rates: 50, 100, 150 Mbps
#   - OFDMA: off/on
#
# Permite override por variáveis de ambiente:
#   BASE_DIR, RESULTS_DIR, SIM_TIME, NUM_STAS_LIST, DATA_RATES_LIST,
#   OFDMA_MODES_LIST, PROTOS_LIST, FREQS_LIST, GENERATE_PYTHON_REPORTS

set -euo pipefail

BASE_DIR="${BASE_DIR:-/home/ricardosantos/ns-3.47}"
RESULTS_DIR="${RESULTS_DIR:-$BASE_DIR/results_single_link_scaling}"
SIM_TIME="${SIM_TIME:-12}"
STATIC_SETUP="${STATIC_SETUP:-true}"
PYTHON_BIN="python3"
BUILD_LOCK_FILE="$BASE_DIR/.ns3_build.lock"
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"
FINALIZE_DONE="false"

NUM_STAS_LIST="${NUM_STAS_LIST:-1}" #2 4 6 8 STAs
DATA_RATES_LIST="${DATA_RATES_LIST:-600Mbps}" #50Mbps 100Mbps 150Mbps
OFDMA_MODES_LIST="${OFDMA_MODES_LIST:-false true}"
OFDMA_VARIANTS_LIST="${OFDMA_VARIANTS_LIST:-}"
SCENARIO_PAIRS_LIST="${SCENARIO_PAIRS_LIST:-}"
PROTOS_LIST="${PROTOS_LIST:-UDP}"
FREQS_LIST="${FREQS_LIST:-2 5 6}"

read -r -a NUM_STAS <<< "$NUM_STAS_LIST"
read -r -a DATA_RATES <<< "$DATA_RATES_LIST"
read -r -a OFDMA_MODES <<< "$OFDMA_MODES_LIST"
if [ -n "$OFDMA_VARIANTS_LIST" ]; then
    read -r -a OFDMA_VARIANTS <<< "$OFDMA_VARIANTS_LIST"
else
    OFDMA_VARIANTS=()
fi
if [ -n "$SCENARIO_PAIRS_LIST" ]; then
    read -r -a SCENARIO_PAIRS <<< "$SCENARIO_PAIRS_LIST"
else
    SCENARIO_PAIRS=()
fi
read -r -a PROTOS <<< "$PROTOS_LIST"
read -r -a FREQS <<< "$FREQS_LIST"

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

    echo "[WARN] Python deps (pandas/matplotlib/numpy) não encontradas."
    echo "[WARN] Relatórios Python serão ignorados."
    GENERATE_PYTHON_REPORTS="false"
}

build_ns3_single_with_lock()
{
    echo "=============================================="
    echo "Compiling single-link program..."
    echo "=============================================="

    if command -v flock >/dev/null 2>&1; then
        exec 9>"$BUILD_LOCK_FILE"
        echo "[INFO] Waiting for build lock: $BUILD_LOCK_FILE"
        flock 9
        ./ns3 build scratch/wifi7-single-link-multi-sta
        flock -u 9
        exec 9>&-
    else
        ./ns3 build scratch/wifi7-single-link-multi-sta
    fi
}

freq_label()
{
    case "$1" in
        2) echo "2.4GHz" ;;
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

    # Gráficos de loss breakdown (single-only com tabela e stacked bar/pie charts)
    if [ -n "$csv_loss" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_single_link_loss_breakdown_plots.py" "$csv_loss" "$plots_dir" "$scenario_tag" "$csv_phy"
    fi

    # Tabela de contagens de drops (com total de pacotes TX e percentagens relativas a TX)
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

    # Tabela de razões de drop PHY RX
    if [ -n "$csv_phy" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_phy_drop_reason_table.py" "$csv_phy" "$plots_dir" "$scenario_tag"
    fi

    # Tabela de razões de drop TC
    if [ -n "$csv_tc" ]; then
        "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_tc_drop_reason_table.py" "$csv_tc" "$outputs_dir/__mlo_missing__.csv" "$plots_dir" "$scenario_tag"
    fi

    # Tabela: tipos de pacote que mais contribuem para PhyRxDrop
    if [ -n "$csv_phy_pkt_type" ]; then
        "$PYTHON_BIN" - "$csv_phy_pkt_type" "$plots_dir" "$scenario_tag" <<'PY'
import os
import sys
import pandas as pd

csv_file, out_dir, scenario = sys.argv[1], sys.argv[2], sys.argv[3]
out_file = os.path.join(out_dir, "phy_rx_drop_packet_type_table.txt")

if not os.path.exists(csv_file):
    sys.exit(0)

df = pd.read_csv(csv_file)
if df.empty:
    sys.exit(0)

for c in ["freq_band", "protocol", "reason", "mac_type", "count", "pct_of_reason", "pct_of_phy_rx", "pct_of_total_tx_packets"]:
    if c not in df.columns:
        df[c] = 0

df = df.sort_values(["freq_band", "protocol", "count"], ascending=[True, True, False])

with open(out_file, "w") as f:
    f.write("=" * 140 + "\n")
    f.write(f"PHY RX DROP - PACKET TYPE BREAKDOWN - {scenario}\n")
    f.write("=" * 140 + "\n\n")
    f.write(f"{'Band':<10} {'Proto':<8} {'Reason':<20} {'MacType':<24} {'Count':<12} {'%Reason':<12} {'%PhyRx':<12} {'%TotalTX':<12}\n")
    f.write("-" * 140 + "\n")
    for _, r in df.iterrows():
        f.write(f"{str(r['freq_band']):<10} {str(r['protocol']):<8} {str(r['reason'])[:20]:<20} {str(r['mac_type'])[:24]:<24} {int(r['count']):<12} {float(r['pct_of_reason']):<12.3f} {float(r['pct_of_phy_rx']):<12.3f} {float(r['pct_of_total_tx_packets']):<12.6f}\n")

print(f"PHY packet-type table generated: {out_file}")
PY
    fi

    # Tabela: tamanhos de pacote que mais contribuem para PhyRxDrop
    if [ -n "$csv_phy_pkt_size" ]; then
        "$PYTHON_BIN" - "$csv_phy_pkt_size" "$plots_dir" "$scenario_tag" <<'PY'
import os
import sys
import pandas as pd

csv_file, out_dir, scenario = sys.argv[1], sys.argv[2], sys.argv[3]
out_file = os.path.join(out_dir, "phy_rx_drop_packet_size_table.txt")

if not os.path.exists(csv_file):
    sys.exit(0)

df = pd.read_csv(csv_file)
if df.empty:
    sys.exit(0)

for c in ["freq_band", "protocol", "packet_size", "count", "pct_of_phy_rx", "pct_of_total_tx_packets"]:
    if c not in df.columns:
        df[c] = 0

df = df.sort_values(["freq_band", "protocol", "count"], ascending=[True, True, False])

with open(out_file, "w") as f:
    f.write("=" * 120 + "\n")
    f.write(f"PHY RX DROP - PACKET SIZE BREAKDOWN - {scenario}\n")
    f.write("=" * 120 + "\n\n")
    f.write(f"{'Band':<10} {'Proto':<8} {'PacketSize(B)':<16} {'Count':<12} {'%PhyRx':<12} {'%TotalTX':<12}\n")
    f.write("-" * 120 + "\n")
    for _, r in df.iterrows():
        f.write(f"{str(r['freq_band']):<10} {str(r['protocol']):<8} {int(r['packet_size']):<16} {int(r['count']):<12} {float(r['pct_of_phy_rx']):<12.3f} {float(r['pct_of_total_tx_packets']):<12.6f}\n")

print(f"PHY packet-size table generated: {out_file}")
PY
    fi
}

finalize_reports()
{
    [ "$GENERATE_PYTHON_REPORTS" = "true" ] || return 0
    [ "$FINALIZE_DONE" = "true" ] && return 0
    FINALIZE_DONE="true"

    set +e

    echo ""
    echo "=============================================="
    echo "Generating comparison plots across scenarios..."
    echo "=============================================="
    "$PYTHON_BIN" "$BASE_DIR/generate_multi_sta_single_comparison_plots.py" "$RESULTS_DIR"

    echo ""
    echo "=============================================="
    echo "Generating per-STA tables..."
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

pick_python
build_ns3_single_with_lock

if [ "${#OFDMA_VARIANTS[@]}" -gt 0 ]; then
    OFDMA_SCENARIOS=("${OFDMA_VARIANTS[@]}")
else
    OFDMA_SCENARIOS=("${OFDMA_MODES[@]}")
fi

for OFDMA_SCENARIO in "${OFDMA_SCENARIOS[@]}"; do
    if [ "${#OFDMA_VARIANTS[@]}" -gt 0 ]; then
        IFS=',' read -r UL_OFDMA_ENABLED BSRP_ENABLED <<< "$OFDMA_SCENARIO"
        UL_TAG=$(bool_tag "$UL_OFDMA_ENABLED")
        BSRP_TAG=$(bool_tag "$BSRP_ENABLED")
        OFDMA_SUFFIX="_ul${UL_TAG}_bsrp${BSRP_TAG}"
        OFDMA_LABEL="UL-OFDMA=${UL_OFDMA_ENABLED} BSRP=${BSRP_ENABLED}"
    else
        OFDMA_ENABLED="$OFDMA_SCENARIO"
        if [ "$OFDMA_ENABLED" = "true" ]; then
            OFDMA_SUFFIX="_OFDMA"
            OFDMA_LABEL="with OFDMA"
        else
            OFDMA_SUFFIX=""
            OFDMA_LABEL="without OFDMA"
        fi
    fi

    if [ "${#SCENARIO_PAIRS[@]}" -gt 0 ]; then
        SCENARIO_ITERATIONS=("${SCENARIO_PAIRS[@]}")
    else
        SCENARIO_ITERATIONS=()
    fi

    if [ "${#SCENARIO_ITERATIONS[@]}" -gt 0 ]; then
        for SCENARIO_PAIR in "${SCENARIO_ITERATIONS[@]}"; do
            IFS=':' read -r NSTAS DATA_RATE <<< "$SCENARIO_PAIR"
            TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
            SCENARIO_TAG="${DATA_RATE}_${NSTAS}stas${OFDMA_SUFFIX}"

            OUTPUTS_DIR="$RESULTS_DIR/outputs_${SCENARIO_TAG}"
            PLOTS_DIR="$RESULTS_DIR/plots_${SCENARIO_TAG}"
            mkdir -p "$OUTPUTS_DIR" "$PLOTS_DIR"

            echo ""
            echo "=============================================="
            echo "Running Single-Link experiments"
            echo "Data Rate: $DATA_RATE | STAs: $NSTAS | $OFDMA_LABEL"
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

            for freq in "${FREQS[@]}"; do
                LABEL=$(freq_label "$freq")

                for proto in "${PROTOS[@]}"; do
                    echo "Running: $LABEL ($proto) @ $DATA_RATE | STAs=$NSTAS | $OFDMA_LABEL"

                    OUTFILE="$OUTPUTS_DIR/single_multi_sta_${freq}_${proto}_${TIMESTAMP}.txt"
                    if [ "${#OFDMA_VARIANTS[@]}" -gt 0 ]; then
                        ./ns3 run "scratch/wifi7-single-link-multi-sta --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --numStas=$NSTAS --simTime=$SIM_TIME --staticSetup=$STATIC_SETUP --enableUlOfdma=$UL_OFDMA_ENABLED --enableBsrp=$BSRP_ENABLED" 2>&1 | tee "$OUTFILE"
                    else
                        ./ns3 run "scratch/wifi7-single-link-multi-sta --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --numStas=$NSTAS --simTime=$SIM_TIME --staticSetup=$STATIC_SETUP --enableOfdma=$OFDMA_ENABLED" 2>&1 | tee "$OUTFILE"
                    fi

                    TP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' | sed 's/Throughput_Mbps=//' || true)
                    DELAY=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgDelay_ms=[0-9]+\.?[0-9]*' | sed 's/AvgDelay_ms=//' || true)
                    JITTER=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgJitter_ms=[0-9]+\.?[0-9]*' | sed 's/AvgJitter_ms=//' || true)
                    LOSS=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' | sed 's/LossRate_pct=//' || true)
                    AVGTP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgThroughputPerSTA_Mbps=[0-9]+\.?[0-9]*' | sed 's/AvgThroughputPerSTA_Mbps=//' || true)
                    TOTAL_TX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'TX_Packets=[0-9]+' | sed 's/TX_Packets=//' || true)
                    TOTAL_RX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'RX_Packets=[0-9]+' | sed 's/RX_Packets=//' || true)

                    TP=${TP:-0}
                    DELAY=${DELAY:-0}
                    JITTER=${JITTER:-0}
                    LOSS=${LOSS:-0}
                    AVGTP=${AVGTP:-0}
                    TOTAL_TX=${TOTAL_TX:-0}
                    TOTAL_RX=${TOTAL_RX:-0}

                    echo "$LABEL,$proto,$TP,$DELAY,$JITTER,$LOSS,$AVGTP" >> "$CSV_SINGLE"

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

                    TOTAL_GRAN_PCT_TX=$(pct_of_total "$TOTAL_GRAN" "$TOTAL_TX")
                    E2E_LOST_PCT_TX=$(pct_of_total "$E2E_LOST" "$TOTAL_TX")

                    echo "$LABEL,$proto,$TOTAL_TX,$TOTAL_RX,$PHY_TX,$PHY_RX,$MAC_TX,$MAC_RX,$WIFI_Q,$TC_BEFORE,$TC_AFTER,$TC_DROP,$TOTAL_GRAN,$E2E_LOST,$TOTAL_GRAN_PCT_TX,$E2E_LOST_PCT_TX,$PHY_PCT,$MAC_PCT,$WIFI_Q_PCT,$TC_PCT,$UNACC_PCT" >> "$CSV_SINGLE_LOSS"

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | grep -oE 'Reason=[^ ]+' | sed 's/Reason=//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                        PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,$REASON,${COUNT:-0},${PCT_PHY:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_REASON"
                    done < <(grep 'PHY_RX_DROP_REASON:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Reason=/){sub(/^Reason=/,"",$i); print $i; break}}}')
                        MACTYPE=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^MacType=/){sub(/^MacType=/,"",$i); print $i; break}}}')
                        COUNT=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Count=/){sub(/^Count=/,"",$i); print $i; break}}}')
                        PCT_REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctReason=/){sub(/^PctReason=/,"",$i); print $i; break}}}')
                        PCT_PHY=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctPhyRx=/){sub(/^PctPhyRx=/,"",$i); print $i; break}}}')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        [ -n "$MACTYPE" ] || MACTYPE="UNKNOWN"
                        echo "$LABEL,$proto,$TOTAL_TX,$REASON,$MACTYPE,${COUNT:-0},${PCT_REASON:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_PKT_TYPE"
                    done < <(grep 'PHY_RX_DROP_PKT_TYPE:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        PKT_SIZE=$(echo "$line" | grep -oE 'PacketSize=[0-9]+' | sed 's/PacketSize=//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$PKT_SIZE" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,${PKT_SIZE:-0},${COUNT:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_PKT_SIZE"
                    done < <(grep 'PHY_RX_DROP_PKT_SIZE:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_STAGE=$(echo "$line" | grep -oE 'PctTcBefore=-?[0-9]+\.?[0-9]*' | sed 's/PctTcBefore=//')
                        PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,before_enqueue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_TC_REASON"
                    done < <(grep 'TC_DROP_BEFORE_REASON:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_STAGE=$(echo "$line" | grep -oE 'PctTcAfter=-?[0-9]+\.?[0-9]*' | sed 's/PctTcAfter=//')
                        PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,after_dequeue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_TC_REASON"
                    done < <(grep 'TC_DROP_AFTER_REASON:' "$OUTFILE" || true)
                done
            done

            echo "[OK] Scenario concluído: $SCENARIO_TAG"
            echo "     CSV main: $CSV_SINGLE"

            generate_single_plots_for_scenario "$SCENARIO_TAG" "$OUTPUTS_DIR" "$PLOTS_DIR"
        done
    else
        for NSTAS in "${NUM_STAS[@]}"; do
            for DATA_RATE in "${DATA_RATES[@]}"; do
            TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
            SCENARIO_TAG="${DATA_RATE}_${NSTAS}stas${OFDMA_SUFFIX}"

            OUTPUTS_DIR="$RESULTS_DIR/outputs_${SCENARIO_TAG}"
            PLOTS_DIR="$RESULTS_DIR/plots_${SCENARIO_TAG}"
            mkdir -p "$OUTPUTS_DIR" "$PLOTS_DIR"

            echo ""
            echo "=============================================="
            echo "Running Single-Link experiments"
            echo "Data Rate: $DATA_RATE | STAs: $NSTAS | $OFDMA_LABEL"
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

            for freq in "${FREQS[@]}"; do
                LABEL=$(freq_label "$freq")

                for proto in "${PROTOS[@]}"; do
                    echo "Running: $LABEL ($proto) @ $DATA_RATE | STAs=$NSTAS | $OFDMA_LABEL"

                    OUTFILE="$OUTPUTS_DIR/single_multi_sta_${freq}_${proto}_${TIMESTAMP}.txt"
                    if [ "${#OFDMA_VARIANTS[@]}" -gt 0 ]; then
                        ./ns3 run "scratch/wifi7-single-link-multi-sta --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --numStas=$NSTAS --simTime=$SIM_TIME --staticSetup=$STATIC_SETUP --enableUlOfdma=$UL_OFDMA_ENABLED --enableBsrp=$BSRP_ENABLED" 2>&1 | tee "$OUTFILE"
                    else
                        ./ns3 run "scratch/wifi7-single-link-multi-sta --freq=$freq --protocol=$proto --dataRate=$DATA_RATE --numStas=$NSTAS --simTime=$SIM_TIME --staticSetup=$STATIC_SETUP --enableOfdma=$OFDMA_ENABLED" 2>&1 | tee "$OUTFILE"
                    fi

                    TP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'Throughput_Mbps=[0-9]+\.?[0-9]*' | sed 's/Throughput_Mbps=//' || true)
                    DELAY=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgDelay_ms=[0-9]+\.?[0-9]*' | sed 's/AvgDelay_ms=//' || true)
                    JITTER=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgJitter_ms=[0-9]+\.?[0-9]*' | sed 's/AvgJitter_ms=//' || true)
                    LOSS=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'LossRate_pct=[0-9]+\.?[0-9]*' | sed 's/LossRate_pct=//' || true)
                    AVGTP=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'AvgThroughputPerSTA_Mbps=[0-9]+\.?[0-9]*' | sed 's/AvgThroughputPerSTA_Mbps=//' || true)
                    TOTAL_TX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'TX_Packets=[0-9]+' | sed 's/TX_Packets=//' || true)
                    TOTAL_RX=$(grep 'FLOW_SUMMARY_TOTAL' "$OUTFILE" | grep -oE 'RX_Packets=[0-9]+' | sed 's/RX_Packets=//' || true)

                    TP=${TP:-0}
                    DELAY=${DELAY:-0}
                    JITTER=${JITTER:-0}
                    LOSS=${LOSS:-0}
                    AVGTP=${AVGTP:-0}
                    TOTAL_TX=${TOTAL_TX:-0}
                    TOTAL_RX=${TOTAL_RX:-0}

                    echo "$LABEL,$proto,$TP,$DELAY,$JITTER,$LOSS,$AVGTP" >> "$CSV_SINGLE"

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

                    TOTAL_GRAN_PCT_TX=$(pct_of_total "$TOTAL_GRAN" "$TOTAL_TX")
                    E2E_LOST_PCT_TX=$(pct_of_total "$E2E_LOST" "$TOTAL_TX")

                    echo "$LABEL,$proto,$TOTAL_TX,$TOTAL_RX,$PHY_TX,$PHY_RX,$MAC_TX,$MAC_RX,$WIFI_Q,$TC_BEFORE,$TC_AFTER,$TC_DROP,$TOTAL_GRAN,$E2E_LOST,$TOTAL_GRAN_PCT_TX,$E2E_LOST_PCT_TX,$PHY_PCT,$MAC_PCT,$WIFI_Q_PCT,$TC_PCT,$UNACC_PCT" >> "$CSV_SINGLE_LOSS"

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | grep -oE 'Reason=[^ ]+' | sed 's/Reason=//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                        PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,$REASON,${COUNT:-0},${PCT_PHY:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_REASON"
                    done < <(grep 'PHY_RX_DROP_REASON:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Reason=/){sub(/^Reason=/,"",$i); print $i; break}}}')
                        MACTYPE=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^MacType=/){sub(/^MacType=/,"",$i); print $i; break}}}')
                        COUNT=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^Count=/){sub(/^Count=/,"",$i); print $i; break}}}')
                        PCT_REASON=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctReason=/){sub(/^PctReason=/,"",$i); print $i; break}}}')
                        PCT_PHY=$(echo "$line" | awk '{for(i=1;i<=NF;i++){if($i ~ /^PctPhyRx=/){sub(/^PctPhyRx=/,"",$i); print $i; break}}}')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        [ -n "$MACTYPE" ] || MACTYPE="UNKNOWN"
                        echo "$LABEL,$proto,$TOTAL_TX,$REASON,$MACTYPE,${COUNT:-0},${PCT_REASON:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_PKT_TYPE"
                    done < <(grep 'PHY_RX_DROP_PKT_TYPE:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        PKT_SIZE=$(echo "$line" | grep -oE 'PacketSize=[0-9]+' | sed 's/PacketSize=//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_PHY=$(echo "$line" | grep -oE 'PctPhyRx=-?[0-9]+\.?[0-9]*' | sed 's/PctPhyRx=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$PKT_SIZE" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,${PKT_SIZE:-0},${COUNT:-0},${PCT_PHY:-0},$PCT_TX" >> "$CSV_SINGLE_PHY_PKT_SIZE"
                    done < <(grep 'PHY_RX_DROP_PKT_SIZE:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_STAGE=$(echo "$line" | grep -oE 'PctTcBefore=-?[0-9]+\.?[0-9]*' | sed 's/PctTcBefore=//')
                        PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,before_enqueue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_TC_REASON"
                    done < <(grep 'TC_DROP_BEFORE_REASON:' "$OUTFILE" || true)

                    while IFS= read -r line; do
                        REASON=$(echo "$line" | grep -oE 'Reason=.* Count=' | sed 's/Reason=//' | sed 's/ Count=$//')
                        COUNT=$(echo "$line" | grep -oE 'Count=[0-9]+' | sed 's/Count=//')
                        PCT_STAGE=$(echo "$line" | grep -oE 'PctTcAfter=-?[0-9]+\.?[0-9]*' | sed 's/PctTcAfter=//')
                        PCT_TOTAL=$(echo "$line" | grep -oE 'PctTotalDrops=-?[0-9]+\.?[0-9]*' | sed 's/PctTotalDrops=//')
                        PCT_TX=$(pct_of_total "${COUNT:-0}" "$TOTAL_TX")
                        [ -n "$REASON" ] || continue
                        echo "$LABEL,$proto,$TOTAL_TX,after_dequeue,$REASON,${COUNT:-0},${PCT_STAGE:-0},${PCT_TOTAL:-0},$PCT_TX" >> "$CSV_SINGLE_TC_REASON"
                    done < <(grep 'TC_DROP_AFTER_REASON:' "$OUTFILE" || true)
                done
            done

            echo "[OK] Scenario concluído: $SCENARIO_TAG"
            echo "     CSV main: $CSV_SINGLE"

            generate_single_plots_for_scenario "$SCENARIO_TAG" "$OUTPUTS_DIR" "$PLOTS_DIR"
        done
    done
    fi
done

echo ""
echo "=============================================="
echo "Single-link matrix run complete"
echo "Results dir: $RESULTS_DIR"
if [ "$GENERATE_PYTHON_REPORTS" = "true" ]; then
    echo "Comparison plots: $RESULTS_DIR/comparison_plots_single"
    echo "Tables:           $RESULTS_DIR/tables"
    echo "Individual plots: $RESULTS_DIR/individual_sta_plots"
fi
echo "=============================================="
