#!/bin/bash
set -e
BASE_DIR="/home/ricardosantos/ns-3.47"
cd "$BASE_DIR"

RESULTS_DIR="$BASE_DIR/results_multi_sta_scaling_8stas" \
NUM_STAS_LIST="8" \
GENERATE_PYTHON_REPORTS="true" \
./run_numsta_comparison.sh
