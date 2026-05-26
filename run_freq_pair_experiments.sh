#!/bin/bash
#
# run_freq_pair_experiments.sh
# Script para correr experiências com todos os pares de frequências WiFi 7
# Combinações: 2.4+5, 2.4+6, 5+6 GHz
#
# Resultados guardados em results/outputs/
# Gráficos gerados em results/plots/
#

set -e

# Diretório base
BASE_DIR="/home/ricardosantos/ns-3.47"
RESULTS_DIR="$BASE_DIR/results"
OUTPUTS_DIR="$RESULTS_DIR/outputs"
PLOTS_DIR="$RESULTS_DIR/plots"

# Parâmetros da simulação
DATA_RATE="2000Mbps"
SIM_TIME="12"

# Criar diretórios se não existirem
mkdir -p "$OUTPUTS_DIR"
mkdir -p "$PLOTS_DIR"

# Timestamp para identificar esta execução
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

echo "=============================================="
echo "WiFi 7 Frequency Pair Experiments"
echo "Started at: $(date)"
echo "=============================================="
echo ""

# Array com os pares de frequências a testar
# Formato: "freq1 freq2 nome_legível"
declare -a FREQ_PAIRS=(
    "2 5 2.4GHz+5GHz"
    "2 6 2.4GHz+6GHz"
    "5 6 5GHz+6GHz"
)

# Ficheiro de resumo
SUMMARY_FILE="$OUTPUTS_DIR/summary_${TIMESTAMP}.txt"
echo "=== WiFi 7 Frequency Pair Experiments Summary ===" > "$SUMMARY_FILE"
echo "Timestamp: $TIMESTAMP" >> "$SUMMARY_FILE"
echo "Data Rate: $DATA_RATE" >> "$SUMMARY_FILE"
echo "Simulation Time: $SIM_TIME s" >> "$SUMMARY_FILE"
echo "" >> "$SUMMARY_FILE"

# Compilar o programa primeiro
echo "Building wifi7-freq-pair-test..."
cd "$BASE_DIR"
./ns3 build scratch/wifi7-freq-pair-test 2>&1 | tail -5

if [ $? -ne 0 ]; then
    echo "ERROR: Build failed!"
    exit 1
fi

echo ""
echo "Build successful. Starting experiments..."
echo ""

# Correr cada par de frequências
for pair in "${FREQ_PAIRS[@]}"; do
    read -r FREQ1 FREQ2 PAIR_NAME <<< "$pair"
    
    OUTPUT_FILE="$OUTPUTS_DIR/freq${FREQ1}_${FREQ2}_${TIMESTAMP}.txt"
    
    echo "----------------------------------------------"
    echo "Running: $PAIR_NAME"
    echo "  Link 1: ${FREQ1}GHz"
    echo "  Link 2: ${FREQ2}GHz"
    echo "  Output: $OUTPUT_FILE"
    echo "----------------------------------------------"
    
    # Correr simulação e guardar output
    ./ns3 run "scratch/wifi7-freq-pair-test --freq1=$FREQ1 --freq2=$FREQ2 --dataRate=$DATA_RATE --simTime=$SIM_TIME --enablePcaps=false" 2>&1 | tee "$OUTPUT_FILE"
    
    # Extrair métricas principais do output
    THROUGHPUT=$(grep "Throughput_Mbps:" "$OUTPUT_FILE" | head -1 | awk '{print $2}')
    DELAY=$(grep "Delay_ms:" "$OUTPUT_FILE" | head -1 | awk '{print $2}')
    JITTER=$(grep "Jitter_ms:" "$OUTPUT_FILE" | head -1 | awk '{print $2}')
    
    # Adicionar ao resumo
    echo "--- $PAIR_NAME ---" >> "$SUMMARY_FILE"
    echo "Throughput: $THROUGHPUT Mbps" >> "$SUMMARY_FILE"
    echo "Delay: $DELAY ms" >> "$SUMMARY_FILE"
    echo "Jitter: $JITTER ms" >> "$SUMMARY_FILE"
    echo "" >> "$SUMMARY_FILE"
    
    echo ""
    echo "  Results: Throughput=${THROUGHPUT}Mbps, Delay=${DELAY}ms, Jitter=${JITTER}ms"
    echo ""
done

# Criar ficheiro CSV com os resultados para fácil processamento pelo Python
CSV_FILE="$OUTPUTS_DIR/results_${TIMESTAMP}.csv"
echo "pair,freq1,freq2,throughput_mbps,delay_ms,jitter_ms" > "$CSV_FILE"

for pair in "${FREQ_PAIRS[@]}"; do
    read -r FREQ1 FREQ2 PAIR_NAME <<< "$pair"
    OUTPUT_FILE="$OUTPUTS_DIR/freq${FREQ1}_${FREQ2}_${TIMESTAMP}.txt"
    
    THROUGHPUT=$(grep "Throughput_Mbps:" "$OUTPUT_FILE" | head -1 | awk '{print $2}')
    DELAY=$(grep "Delay_ms:" "$OUTPUT_FILE" | head -1 | awk '{print $2}')
    JITTER=$(grep "Jitter_ms:" "$OUTPUT_FILE" | head -1 | awk '{print $2}')
    
    echo "$PAIR_NAME,$FREQ1,$FREQ2,$THROUGHPUT,$DELAY,$JITTER" >> "$CSV_FILE"
done

echo "=============================================="
echo "Experiments Complete!"
echo "=============================================="
echo ""
echo "Output files saved in: $OUTPUTS_DIR"
echo "  - Individual results: freq{X}_{Y}_${TIMESTAMP}.txt"
echo "  - Summary: summary_${TIMESTAMP}.txt"
echo "  - CSV data: results_${TIMESTAMP}.csv"
echo ""
echo "To generate plots, run:"
echo "  python3 $BASE_DIR/generate_plots.py $CSV_FILE"
echo ""

# Chamar o script Python para gerar gráficos automaticamente
if [ -f "$BASE_DIR/generate_plots.py" ]; then
    echo "Generating plots..."
    python3 "$BASE_DIR/generate_plots.py" "$CSV_FILE" "$PLOTS_DIR" "$TIMESTAMP" "$DATA_RATE"
    echo "Plots saved in: $PLOTS_DIR"
fi

echo ""
echo "Done at: $(date)"
