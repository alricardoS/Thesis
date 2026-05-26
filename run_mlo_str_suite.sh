#!/bin/bash
# Run STR pair scenarios:
#  1) 2.4 GHz + 5 GHz
#  2) 5 GHz + 6 GHz
#  3) 2.4 GHz + 6 GHz
# Saves logs for each run.

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${ROOT_DIR}/mlo_str_results"
SIM_TIME=12.0
ONOFF_RATE="800Mbps"

mkdir -p "${RESULTS_DIR}"

scenarios=(
  "2 5"
  "5 6"
  "2 6"
)

for pair in "${scenarios[@]}"; do
  set -- $pair
  freqA=$1
  freqB=$2
  label="${freqA}-${freqB}"
  log_file="${RESULTS_DIR}/str_${label}.log"

  echo "=========================================="
  echo "Running STR scenario: ${freqA} GHz + ${freqB} GHz"
  echo "Log: ${log_file}"
  echo "=========================================="

  (cd "${ROOT_DIR}" && ./ns3 run scratch/MLO_STR_pair_test -- \
      --freqA="${freqA}" \
      --freqB="${freqB}" \
      --simulationTime="${SIM_TIME}" \
      --onOffRate="${ONOFF_RATE}") | tee "${log_file}"
  echo ""

done

echo "All STR scenarios completed. Logs saved under ${RESULTS_DIR}"