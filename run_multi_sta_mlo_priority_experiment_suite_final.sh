#!/bin/bash
# Suite "final" — testa o scheduler variando 3 fatores: nº STAs, apps por STA, banda.
# Reutiliza run_multi_sta_mlo_priority_experiments_with_scheduler.sh (mesmos gráficos/tabelas).
#
# ============================================================================
# COMO EDITAR OS CENÁRIOS (à mão): edita o array SCENARIOS abaixo.
# Cada linha:  "nome|banda|appsSpec"
#   banda    : dual (corre 2.4+5, 2.4+6, 5+6) | um par (2+5 / 2+6 / 5+6) | tri (Fase B)
#   appsSpec : STAs separados por ';'  ;  apps por '+' (concorrente) e '>' (switch a meio)
#              ACs: vo, vi, be, bk.   nº de STAs = nº de entradas ';'.
#     ex.:  vo;vi;be;bk                 -> 4 STAs, 1 AC cada
#           vo>be;vo>be;vi;vi           -> 2VO+2VI; os VO passam a BE a meio da simulação
#           vo+be;vo+be;vi+bk;vi+bk     -> 4 STAs, 2 apps concorrentes cada
# O data-rate total (600 Mbps) é repartido por STA e, dentro do STA, pelas apps.
# ============================================================================

set -uo pipefail

BASE_DIR="/home/ricardosantos/ns-3.47"
RUNNER_SCRIPT="$BASE_DIR/run_multi_sta_mlo_priority_experiments_with_scheduler.sh"

# --- Configuração global ---
TOTAL_RATE_MBPS="${TOTAL_RATE_MBPS:-600}"
OFDMA_MODES="${OFDMA_MODES:-false}"
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"
RESULTS_ROOT_DIR="${RESULTS_ROOT_DIR:-$BASE_DIR/results_multi_sta_mlo_priority_with_scheduler_suite_final}"
APPEND_TIMESTAMP="${APPEND_TIMESTAMP:-false}"

# ============================================================================
# CENÁRIOS  ("nome|banda|appsSpec")
# ============================================================================
SCENARIOS=(
  # --- Dualband (Fase A) ---
  "S1_base_1vo1vi1be1bk|dual|vo;vi;be;bk"
  #"S2_scale8_2each|dual|vo;vo;vi;vi;be;be;bk;bk"
  "S4_concurrent_mixed|dual|vo+be;vo+be;vi+bk;vi+vo"
  "S5_switch_2vo2vi_vo2be|dual|vo>be;vo>be;vi;vi"
  "S3_scale8_4VO_2VI_2BE|dual|vo;vo;vo;vo;vi;vi;be;be"

  # --- Triband (Fase B — scheduler N-link ativo; 3 links 2.4+5+6) ---
  "S6_tri_base_4ac_2VO_1VI_1BE|tri|vo;vo;vi;be"
  "S7_tri_base_4ac_1VO_1VI_2BE|tri|vo;vi;be;be"
  "S8_tri_switch_allBE2_2VO_2VI|tri|be>vo;be>vo;be>vi;be>vi"
  "S9_tri_concurrent8|tri|vo+be+vi;vi+bk;vo;vi;be;bk;vo+bk;vi+be"
  #"S7_tri_scale8|tri|vo;vo;vi;vi;be;be;bk;bk"
  #"S8_tri_concurrent8|tri|vo+be;vi+bk;vo;vi;be;bk;vo+bk;vi+be"
  #"S9_tri_stress12_switch|tri|vo>bk;vo>bk;vi>be;vi>be;vo;vi;be;bk;be;bk;vo+be;vi+bk"
)

 #"two_vo_two_vi|video,video,voice,voice",
  #"one_vo_one_vi_one_be_one_bg|voice,video,besteffort,background",
  #"all_be|besteffort,besteffort,besteffort,besteffort",
  #"one_vo_three_be|voice,besteffort,besteffort,besteffort",
  #"one_vo_one_vi_two_be|voice,video,besteffort,besteffort",
  #"one_vi_one_be_two_vo|voice,voice,besteffort,video"

mkdir -p "$RESULTS_ROOT_DIR"
[ -f "$RUNNER_SCRIPT" ] || { echo "[ERRO] Runner não encontrado: $RUNNER_SCRIPT"; exit 1; }
cd "$BASE_DIR"

make_target_dir() {
  local exp_name="$1" base_name="$1" candidate i=2
  candidate="$RESULTS_ROOT_DIR/$base_name"
  [ "$APPEND_TIMESTAMP" = "true" ] && { base_name="${exp_name}_$(date +%Y%m%d_%H%M%S)"; candidate="$RESULTS_ROOT_DIR/$base_name"; }
  while [ -e "$candidate" ]; do candidate="$RESULTS_ROOT_DIR/${base_name}_$i"; i=$((i+1)); done
  printf '%s\n' "$candidate"
}

for item in "${SCENARIOS[@]}"; do
  IFS='|' read -r NAME BAND APPS_SPEC <<< "$item"
  [ -z "${NAME:-}" ] || [ -z "${BAND:-}" ] || [ -z "${APPS_SPEC:-}" ] && { echo "[WARN] Entrada inválida: '$item'"; continue; }

  # nº de STAs = nº de entradas ';' + 1
  NSTAS=$(( $(tr -cd ';' <<< "$APPS_SPEC" | wc -c) + 1 ))
  TARGET_DIR="$(make_target_dir "$NAME")"

  echo ""
  echo "============================================================"
  echo "Cenário: $NAME   banda=$BAND   STAs=$NSTAS"
  echo "appsSpec=$APPS_SPEC"
  echo "============================================================"

  NUM_STAS_LIST="$NSTAS" \
  DATA_RATES_LIST="${TOTAL_RATE_MBPS}Mbps" \
  OFDMA_MODES="$OFDMA_MODES" \
  RESULTS_DIR="$TARGET_DIR" \
  APPS_SPEC="$APPS_SPEC" \
  BAND="$BAND" \
  TOTAL_RATE_MBPS="$TOTAL_RATE_MBPS" \
  STA_TRAFFIC_TYPES="besteffort" \
  QUEUE_OCCUPANCY_LABEL="$NAME" \
  GENERATE_PYTHON_REPORTS="$GENERATE_PYTHON_REPORTS" \
  "$RUNNER_SCRIPT"

  [ $? -ne 0 ] && echo "[WARN] Runner saiu com erro no cenário $NAME"
  [ -d "$TARGET_DIR" ] && echo "[OK] Resultados em: $TARGET_DIR" || echo "[ERRO] Pasta não encontrada: $TARGET_DIR"
done

echo ""
echo "============================================================"
echo "Suite final concluída. Resultados em: $RESULTS_ROOT_DIR"
echo "============================================================"
