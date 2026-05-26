#!/bin/bash
#
# run_80211be_experiments.sh
# Comprehensive 802.11be experiment suite: 3 frequencies × 3 saturation scenarios = 9 tests
# Tests measure MAC-layer and transport-layer throughput across different frequency bands
# and saturation conditions (under-saturation, at-saturation, over-saturation).
#

set -e

NS3_DIR="/home/ricardosantos/ns-3.47"
SCRATCH_FILE="scratch/wifi_80211be_test"
RESULTS_DIR="experiment_results"
SUMMARY_FILE="${RESULTS_DIR}/summary.txt"
LOG_FILE="${RESULTS_DIR}/experiment_log.txt"

# Create results directory
mkdir -p "${RESULTS_DIR}"

# Initialize comprehensive log file
cat > "${LOG_FILE}" << 'EOF'
================================================================================
                        802.11be WiFi Experiment Log
================================================================================
Date: $(date)
Workspace: /home/ricardosantos/ns-3.47
Test Framework: wifi_80211be_test.cc
Total Tests: 9 (3 frequencies × 3 saturation scenarios)
Simulation Time: 12 seconds per test
Actual Traffic Duration: 10 seconds (0.2s to 10.0s)
================================================================================

EOF

# Saturation rates (Mbps) for each frequency band
declare -A SAT_RATES
SAT_RATES[2]=230
SAT_RATES[5]=350
SAT_RATES[6]=430

# Scenario multipliers
declare -A SCENARIO_MULT
SCENARIO_MULT["under_sat"]=0.5
SCENARIO_MULT["at_sat"]=1.0
SCENARIO_MULT["over_sat"]=1.5

# Scenario descriptive labels
declare -A SCENARIO_LABEL
SCENARIO_LABEL["under_sat"]="Below Saturation"
SCENARIO_LABEL["at_sat"]="At Saturation"
SCENARIO_LABEL["over_sat"]="Above Saturation"

echo "=========================================="
echo "802.11be WiFi Experiment Suite"
echo "=========================================="
echo "Test Configuration:"
echo "  - Frequencies: 2.4 GHz (40 MHz), 5 GHz (160 MHz), 6 GHz (320 MHz)"
echo "  - Scenarios: under-saturation, at-saturation, over-saturation"
echo "  - Total tests: 9"
echo "  - Simulation time: 12 seconds per test (10 secs of actual OnOff traffic)"
echo "=========================================="
echo "Logging results to: ${LOG_FILE}"
echo "Summary file: ${SUMMARY_FILE}"
echo "Individual results in: ${RESULTS_DIR}/test_*.txt"
echo "=========================================="
echo ""

# Initialize summary file
cat > "${SUMMARY_FILE}" << 'EOF'
================================================================================
                802.11be WiFi Experiment Results Summary
================================================================================
Test# | Freq(GHz) | Scenario        | OnOff Rate(Mbps) | MAC Throughput(Mbps) | Transport Throughput(Mbps) | Efficiency(%)
------ +-----------+-------------- -+ ----------------+ ---------------------+--------------------------+ ---------------
EOF

TEST_NUM=1

# Main loop: for each frequency
for freq in 2 5 6; do
    sat_rate=${SAT_RATES[$freq]}
    
    echo "=========================================="
    echo "Testing at ${freq} GHz (Channel: ${channel_width} MHz, Saturation: ${sat_rate} Mbps)"
    echo "=========================================="
    echo ""
    
    # For each saturation scenario
    for scenario in under_sat at_sat over_sat; do
        mult=${SCENARIO_MULT[$scenario]}
        label=${SCENARIO_LABEL[$scenario]}
        
        # Calculate OnOff rate for this scenario
        onoff_rate=$(echo "${sat_rate} * ${mult}" | bc)
        onoff_rate_int=${onoff_rate%.*}  # Remove decimal part
        
        # Create results file for this test
        result_file="${RESULTS_DIR}/test_${freq}ghz_${scenario}.txt"
        
        echo "[TEST ${TEST_NUM}] ${freq} GHz - ${label} (${channel_width} MHz, ${onoff_rate_int} Mbps)"
        
        # Log test start to log file
        {
            echo "────────────────────────────────────────────────────────────────────────────"
            echo "TEST #${TEST_NUM} | Frequency: ${freq} GHz | Scenario: ${label}"
            echo "────────────────────────────────────────────────────────────────────────────"
            echo "Configuration:"
            echo "  • Channel Width: ${channel_width} MHz"
            echo "  • OnOff DataRate: ${onoff_rate_int} Mbps"
            echo "  • Simulation Time: 12.0 seconds"
            echo "Start Time: $(date '+%Y-%m-%d %H:%M:%S')"
            echo ""
        } >> "${LOG_FILE}"
        
        # Run the test
        cd "${NS3_DIR}"
        ./ns3 run "${SCRATCH_FILE}" -- \
            --frequency="${freq}" \
            --onOffRate="${onoff_rate_int}Mbps" \
            --simulationTime=12.0 \
            > "${result_file}" 2>&1
        
        # Parse results (throughput now in Mbps instead of Kbps)
        mac_throughput=$(grep -A 100 "MAC-LAYER STATS" "${result_file}" | grep "unknown" | awk '{print $NF}' | head -1)
        # Column order: Port RxPkts MeanDelay MeanJitter Goodput(overall) Avg100msGoodput -> avg 100ms goodput is $6
        transport_throughput=$(grep -A 100 "TRANSPORT-LAYER STATS" "${result_file}" | grep "60000" | awk '{print $6}' | head -1)
        
        # Already in Mbps, no conversion needed
        mac_throughput_mbps=$(echo "${mac_throughput}" | bc 2>/dev/null || echo "N/A")
        transport_throughput_mbps=$(echo "${transport_throughput}" | bc 2>/dev/null || echo "N/A")
        
        # Calculate efficiency (actual MAC throughput / requested OnOff rate)
        if [[ "$mac_throughput_mbps" != "N/A" ]]; then
            efficiency=$(echo "scale=2; ${mac_throughput_mbps} / ${onoff_rate_int} * 100" | bc 2>/dev/null || echo "N/A")
        else
            efficiency="N/A"
        fi
        
        # Log results to log file
        {
            echo "Results:"
            echo "  • MAC-Layer Throughput: ${mac_throughput_mbps} Mbps"
            echo "  • Transport-Layer Goodput: ${transport_throughput_mbps} Mbps"
            echo "  • Efficiency: ${efficiency}%"
            echo "  • Result File: ${result_file}"
            echo "End Time: $(date '+%Y-%m-%d %H:%M:%S')"
            echo ""
        } >> "${LOG_FILE}"
        
        # Append to summary
        printf "%4d  | %9d | %-15s | %16d | %21s | %26s | %13s\n" \
            "${TEST_NUM}" "${freq}" "${label}" "${onoff_rate_int}" \
            "${mac_throughput_mbps}" "${transport_throughput_mbps}" "${efficiency}" \
            >> "${SUMMARY_FILE}"
        
        echo "  -> MAC throughput: ${mac_throughput_mbps} Mbps"
        echo "  -> Transport throughput: ${transport_throughput_mbps} Mbps"
        echo "  -> Efficiency: ${efficiency}%"
        echo ""
        
        TEST_NUM=$((TEST_NUM + 1))
    done
done

echo "=========================================="
echo "All tests completed!"
echo "=========================================="
echo ""
echo "📁 RESULTS SAVED:"
echo "   • Comprehensive Log: ${LOG_FILE}"
echo "   • Summary Table: ${SUMMARY_FILE}"
echo "   • Individual Results: ${RESULTS_DIR}/test_*.txt"
echo ""
echo "Summary of Results:"
echo "────────────────────────────────────────────────────────────────────────"
cat "${SUMMARY_FILE}"
echo ""
echo "To view the full log with all details:"
echo "   cat ${LOG_FILE}"
