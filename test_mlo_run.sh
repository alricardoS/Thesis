#!/bin/bash
OUTPUTS_DIR="test_link_traffic_output"
PROTOCOL="tcp"
DATARATE="30Mbps"
NUM_STA=4
SIM_TIME=6

# Test parameters
FREQ_PAIR="2.4+5"
IFS='+' read -r FREQ1 FREQ2 <<< "$FREQ_PAIR"

# Run short test
./ns3 run "scratch/wifi7-mlo-multi-sta-priority \
  --freq1=${FREQ1} \
  --freq2=${FREQ2} \
  --numStas=${NUM_STA} \
  --protocol=${PROTOCOL} \
  --dataRate=${DATARATE} \
  --simTime=${SIM_TIME} \
  --linkTrafficCsv=${OUTPUTS_DIR}/mlo_multi_sta_link_traffic.csv \
  --linkTrafficSampleInterval=0.1" 2>&1 | head -100

echo "====== Test completed ======"
ls -lh ${OUTPUTS_DIR}/
