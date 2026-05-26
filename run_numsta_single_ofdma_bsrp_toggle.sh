#!/bin/bash
# run_numsta_single_ofdma_bsrp_toggle.sh
#
# Wrapper dedicado para o estudo da troca dos flags:
#   - EnableUlOfdma
#   - EnableBsrp
#
# Mantém a mesma pipeline de métricas, tabelas e gráficos do runner single-link
# original, mas restringe os cenários a:
#   - 1 STA  -> 600 Mbps
#   - 4 STAs -> 150 Mbps
#   - UL-OFDMA=true,  BSRP=false
#   - UL-OFDMA=false, BSRP=false
#
# Os resultados são guardados numa pasta dedicada para distinguir claramente
# este conjunto de testes.

set -euo pipefail

BASE_DIR="${BASE_DIR:-/home/ricardosantos/ns-3.47}"
RESULTS_DIR="${RESULTS_DIR:-$BASE_DIR/results_single_link_ulofdma_bsrp_toggle}"
SIM_TIME="${SIM_TIME:-12}"
STATIC_SETUP="${STATIC_SETUP:-true}"
GENERATE_PYTHON_REPORTS="${GENERATE_PYTHON_REPORTS:-true}"

export BASE_DIR
export RESULTS_DIR
export SIM_TIME
export STATIC_SETUP
export GENERATE_PYTHON_REPORTS

# Apenas os pares pedidos.
export SCENARIO_PAIRS_LIST="${SCENARIO_PAIRS_LIST:-1:600Mbps 4:150Mbps}"

# Duas combinações pedidas para a troca das variáveis do scheduler.
export OFDMA_VARIANTS_LIST="${OFDMA_VARIANTS_LIST:-true,false false,false}"

# Mantém os restantes parâmetros iguais ao runner original.
export PROTOS_LIST="${PROTOS_LIST:-UDP}"
export FREQS_LIST="${FREQS_LIST:-2 5 6}"

exec "$BASE_DIR/run_numsta_single_only.sh"
