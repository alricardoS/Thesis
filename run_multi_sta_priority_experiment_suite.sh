#!/bin/bash
# Run a suite of priority experiments and store each scenario in its own results folder.
# Reuses run_multi_sta_priority_experiments.sh so plots/tables/artifacts stay exactly the same.

set -euo pipefail

BASE_DIR="/home/ricardosantos/ns-3.47"
RUNNER_SCRIPT="$BASE_DIR/run_multi_sta_priority_experiments.sh"

# ==============================
# Configuração global (editável)
# ==============================
NUM_STAS_LIST="${NUM_STAS_LIST:-4}"
DATA_RATES_LIST="${DATA_RATES_LIST:-30Mbps 150Mbps 700Mbps}"
OFDMA_MODES="${OFDMA_MODES:-false}"
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"

# Pasta base onde cada experiência vai ter a sua subpasta
RESULTS_ROOT_DIR="${RESULTS_ROOT_DIR:-$BASE_DIR/results_multi_sta_priority_suite}"

# Se true, adiciona timestamp ao nome da pasta
APPEND_TIMESTAMP="${APPEND_TIMESTAMP:-false}"

# ==============================
# Cenários de experiência (editável)
# Formato: "nome|staTrafficTypesCsv"
# ==============================
EXPERIMENTS=(
  "one_vi_one_be_two_vo|voice,voice,besteffort,video"
)

  #"one_vo_one_vi_one_be_one_bg|voice,video,besteffort,background"
  #"all_be|besteffort,besteffort,besteffort,besteffort"
  #"one_vo_three_be|voice,besteffort,besteffort,besteffort"
  #"one_vo_one_vi_two_be|voice,video,besteffort,besteffort"

mkdir -p "$RESULTS_ROOT_DIR"

if [ ! -f "$RUNNER_SCRIPT" ]; then
  echo "[ERRO] Runner não encontrado: $RUNNER_SCRIPT"
  exit 1
fi

cd "$BASE_DIR"

make_target_dir() {
  local exp_name="$1"
  local base_name="$exp_name"
  local candidate="$RESULTS_ROOT_DIR/$base_name"
  local i=2

  if [ "$APPEND_TIMESTAMP" = "true" ]; then
    base_name="${exp_name}_$(date +"%Y%m%d_%H%M%S")"
    candidate="$RESULTS_ROOT_DIR/$base_name"
  fi

  while [ -e "$candidate" ]; do
    candidate="$RESULTS_ROOT_DIR/${base_name}_$i"
    i=$((i + 1))
  done

  printf '%s\n' "$candidate"
}

for item in "${EXPERIMENTS[@]}"; do
  IFS='|' read -r EXP_NAME STA_TRAFFIC_TYPES <<< "$item"

  if [ -z "${EXP_NAME:-}" ] || [ -z "${STA_TRAFFIC_TYPES:-}" ]; then
    echo "[WARN] Entrada inválida em EXPERIMENTS: '$item'"
    continue
  fi

  TARGET_DIR="$(make_target_dir "$EXP_NAME")"

  echo ""
  echo "============================================================"
  echo "A correr experiência: $EXP_NAME"
  echo "STA_TRAFFIC_TYPES=$STA_TRAFFIC_TYPES"
  echo "NUM_STAS_LIST=$NUM_STAS_LIST | DATA_RATES_LIST=$DATA_RATES_LIST | OFDMA_MODES=$OFDMA_MODES"
  echo "============================================================"

  NUM_STAS_LIST="$NUM_STAS_LIST" \
  DATA_RATES_LIST="$DATA_RATES_LIST" \
  OFDMA_MODES="$OFDMA_MODES" \
  RESULTS_DIR="$TARGET_DIR" \
  STA_TRAFFIC_TYPES="$STA_TRAFFIC_TYPES" \
  QUEUE_OCCUPANCY_LABEL="$EXP_NAME|$STA_TRAFFIC_TYPES" \
  GENERATE_PYTHON_REPORTS="$GENERATE_PYTHON_REPORTS" \
  "$RUNNER_SCRIPT"

  if [ -d "$TARGET_DIR" ]; then
    echo "[OK] Resultados guardados em: $TARGET_DIR"
  else
    echo "[ERRO] Não foi encontrada a pasta de resultados esperada: $TARGET_DIR"
    exit 1
  fi
done

echo ""
echo "============================================================"
echo "Suite concluída. Resultados em: $RESULTS_ROOT_DIR"
echo "============================================================"
