/*
 * wifi7-single-link-test.cc
 * 802.11be Single Link Baseline Test
 */
#include "ns3/core-module.h"
#include "ns3/mobility-helper.h"
#include "ns3/ssid.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/propagation-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/wifi-mac.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("wifi7-single-link-test");

/* global variable for easier access (cumulative delay/jitter) */
double g_delaySumMs = 0.0;
uint64_t g_delaySamples = 0;
double g_jitterSumMs = 0.0;
uint64_t g_jitterSamples = 0;
double g_lastDelayMs = 0.0;
bool g_firstPacket = true;

std::string
DropReasonToString(ns3::Ipv4FlowProbe::DropReason reason)
{
    switch(reason)
    {
        case ns3::Ipv4FlowProbe::DROP_NO_ROUTE:
            return "DROP_NO_ROUTE";
        case ns3::Ipv4FlowProbe::DROP_TTL_EXPIRE:
            return "DROP_TTL_EXPIRE";
        case ns3::Ipv4FlowProbe::DROP_BAD_CHECKSUM:
            return "DROP_BAD_CHECKSUM";
        case ns3::Ipv4FlowProbe::DROP_QUEUE:
            return "DROP_QUEUE";
        case ns3::Ipv4FlowProbe::DROP_QUEUE_DISC:
            return "DROP_QUEUE_DISC";
        case ns3::Ipv4FlowProbe::DROP_INTERFACE_DOWN:
            return "DROP_INTERFACE_DOWN";
        case ns3::Ipv4FlowProbe::DROP_ROUTE_ERROR:
            return "DROP_ROUTE_ERROR";
        case ns3::Ipv4FlowProbe::DROP_FRAGMENT_TIMEOUT:
            return "DROP_FRAGMENT_TIMEOUT";
        default:
            return "DROP_INVALID_REASON";
    }
}

/**
 * PrintNodes copied from base test to show device/PHY config
 */
void PrintNodes(NodeContainer nodes, const std::string type) {
        for(uint32_t nodeIdx = 0; nodeIdx < nodes.GetN(); ++nodeIdx) {
            Ptr<Node> node = nodes.Get(nodeIdx);
  
            NS_LOG_UNCOND(type << " Node " << nodeIdx);
  
            uint32_t numDevices = node->GetNDevices(); 
            Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();  
  
            for(uint32_t i = 0; i < numDevices; ++i) 
            {
                Ptr<NetDevice> netDevice = node->GetDevice(i);
                Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(netDevice);
  
                if(wifiDevice) 
                {
                    Mac48Address upperMacAddress = wifiDevice->GetMac()->GetAddress();
  
                    NS_LOG_UNCOND("  Standard: " << wifiDevice->GetStandard());    
                    NS_LOG_UNCOND("  Number of wifi interfaces: " << (numDevices - 1));    
  
                    Ipv4Address ipv4Address = Ipv4Address::GetAny();
                    int32_t interfaceIndex = ipv4->GetInterfaceForDevice(netDevice);
                    if(interfaceIndex != -1) 
                    {
                        ipv4Address = ipv4->GetAddress(interfaceIndex, 0).GetLocal();
                    }
  
                    uint32_t numPhys = wifiDevice->GetNPhys();
  
                    NS_LOG_UNCOND("    Interface " << i << ":");
                    NS_LOG_UNCOND("      IPv4 Address = " << ipv4Address);
                    NS_LOG_UNCOND("      Upper MAC Address = " << upperMacAddress);
                    NS_LOG_UNCOND("      Number of PHYs = " << numPhys);
                }
            }
            NS_LOG_UNCOND("----------------------------");
        }
}

// Callback to monitor the reception of packets at the sink (uses SeqTsSizeHeader)
void MonitorPacketSinkRx(std::string context, Ptr< const Packet > packet, const Address &address)
{
    SeqTsSizeHeader header;
    packet->PeekHeader(header);
    Time txTime = header.GetTs();
    Time delay = Simulator::Now() - txTime;

    double delayMs = delay.GetSeconds() * 1000.0;
    g_delaySumMs += delayMs;
    g_delaySamples++;

    if (!g_firstPacket) {
        double jitter = std::fabs(delayMs - g_lastDelayMs);
        g_jitterSumMs += jitter;
        g_jitterSamples++;
    } else {
        g_firstPacket = false;
    }
    g_lastDelayMs = delayMs;
}

// Periodic statistics printer (per-second)
void CalculateStats(Ptr<PacketSink> sink, uint64_t &lastTotalRx)
{
    uint64_t currentTotalRx = sink->GetTotalRx();
    uint64_t currentTime = Simulator::Now().GetSeconds();

    double throughput = ((currentTotalRx - lastTotalRx) * 8.0) / (1.0 * 1e6);

    double avgDelay = (g_delaySamples > 0) ? (g_delaySumMs / g_delaySamples) : 0.0;
    double avgJitter = (g_jitterSamples > 0) ? (g_jitterSumMs / g_jitterSamples) : 0.0;

    lastTotalRx = currentTotalRx;

    NS_LOG_UNCOND("Time: " << currentTime << " s, Throughput: " << static_cast<int>(throughput) << " Mbps"
                    << " Delay: " << avgDelay << " ms" << " Jitter: " << avgJitter << " ms");

    Simulator::Schedule(Seconds(1), &CalculateStats, sink, std::ref(lastTotalRx));
}

int main(int argc, char* argv[])
{
    int freq = 6; // 2, 5, or 6
    std::string dataRateStr = "2000Mbps";
    double simTime = 12.0;
    std::string protocol = "UDP";

    CommandLine cmd;
    cmd.AddValue("freq", "Frequency (2, 5, 6)", freq);
    cmd.AddValue("dataRate", "Data rate", dataRateStr);
    cmd.AddValue("protocol", "TCP or UDP", protocol);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.Parse(argc, argv);

    // Simulation timing and application defaults (match base test)
    Time APP_START_TIME(Seconds(1));
    Time SIMULATION_END_TIME(Seconds(simTime));

    DataRate dataRate(dataRateStr);
    uint32_t payloadSize = 1440;
    uint32_t maxBytes = 0; // unlimited

    std::string appSocketType = (protocol == "TCP") ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";

    bool enablePcaps = false;

    // Global defaults: only set TCP-specific settings when using TCP
    if (protocol == "TCP") {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));
        Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));
    }
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));
    ns3::Packet::EnablePrinting();
    ns3::Packet::EnableChecking();

    // Configuração de Canal
    int channel = 15; int width = 160; 
    FrequencyRange freqRange = WIFI_SPECTRUM_6_GHZ;
    std::string band = "BAND_6GHZ";

    if (freq == 2) { 
        channel = 6; width = 40; band = "BAND_2_4GHZ"; freqRange = WIFI_SPECTRUM_2_4_GHZ;
    } else if (freq == 5) {
        channel = 42; width = 80; band = "BAND_5GHZ"; freqRange = WIFI_SPECTRUM_5_GHZ;
    }

    NodeContainer nodes;
    nodes.Create(2); // 0=AP, 1=STA

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifi.ConfigEhtOptions("EmlsrActivated", BooleanValue(false));

    SpectrumWifiPhyHelper phy(1);
    phy.Set("Antennas", UintegerValue(2));
    phy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    phy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    // Canal Único
    Ptr<MultiModelSpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    spectrumChannel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    
    phy.AddChannel(spectrumChannel, freqRange);
    std::string chanSettings = "{" + std::to_string(channel) + ", " + std::to_string(width) + ", " + band + ", 0}";
    phy.Set(0, "ChannelSettings", StringValue(chanSettings));
    phy.AddPhyToFreqRangeMapping(0, freqRange);

    // set rate control / station manager for the PHY
    wifi.SetRemoteStationManager(0, std::string("ns3::IdealWifiManager"));

    // configure MACs
    Ssid ssid = Ssid("wifi7-single");
    WifiMacHelper apMac;
    apMac.SetType("ns3::ApWifiMac",
                  "Ssid", SsidValue(ssid),
                  "BeaconGeneration", BooleanValue(true),
                  "BeaconInterval", TimeValue(MicroSeconds(102400)),
                  "QosSupported", BooleanValue(true));

    WifiMacHelper staMac;
    staMac.SetType("ns3::StaWifiMac",
                   "Ssid", SsidValue(ssid),
                   "QosSupported", BooleanValue(true),
                   "ActiveProbing", BooleanValue(false));

    NetDeviceContainer apDev = wifi.Install(phy, apMac, nodes.Get(0));
    NetDeviceContainer staDev = wifi.Install(phy, staMac, nodes.Get(1));

    

    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::GridPositionAllocator",
                                 "MinX", DoubleValue(0.0), 
                                 "MinY", DoubleValue(0.0), 
                                 "DeltaX", DoubleValue(1.0), 
                                 "DeltaY", DoubleValue(0.0), 
                                 "GridWidth", UintegerValue(3), 
                                 "LayoutType", StringValue("RowFirst"));
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);
    Ipv4AddressHelper address;
    address.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifaces = address.Assign(NetDeviceContainer(apDev, staDev));

    // App Selection
    std::string socketType = (protocol == "TCP") ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";
    uint16_t port = 9;
    Address sinkAddr(InetSocketAddress(ifaces.GetAddress(1), port));

    // Applications (PacketSink receiver + OnOff sender)
    PacketSinkHelper sinkHelper(appSocketType, sinkAddr);
    ApplicationContainer sinkApps = sinkHelper.Install(nodes.Get(1));
    sinkApps.Start(APP_START_TIME);
    sinkApps.Stop(SIMULATION_END_TIME - Seconds(1));
    Ptr<PacketSink> sinkPtr = DynamicCast<PacketSink>(sinkApps.Get(0));
    sinkPtr->TraceConnect("Rx", sinkPtr->GetTypeId().GetName(), MakeCallback(&MonitorPacketSinkRx));

    OnOffHelper client(appSocketType, sinkAddr);
    client.SetAttribute("PacketSize", UintegerValue(payloadSize));
    client.SetAttribute("MaxBytes", UintegerValue(maxBytes));
    client.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    client.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    client.SetAttribute("DataRate", DataRateValue(dataRate));
    client.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));

    ApplicationContainer clientApps = client.Install(nodes.Get(0));
    clientApps.Start(APP_START_TIME + Seconds(1));
    clientApps.Stop(SIMULATION_END_TIME - Seconds(2));

    // Flow monitor
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    // periodic stats
    uint64_t lastTotalRx = 0;
    Simulator::Schedule(Seconds(1), &CalculateStats, sinkPtr, std::ref(lastTotalRx));

    // Print node info
    PrintNodes(nodes, "DEV");

    Simulator::Stop(SIMULATION_END_TIME + Seconds(1));
    Simulator::Run();

    // Extract final flow stats and print a FLOW_SUMMARY-like line
    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    auto stats = monitor->GetFlowStats();
    for (const auto &kv : stats) {
        FlowId id = kv.first;
        const FlowMonitor::FlowStats &st = kv.second;
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(id);
        // we expect sender to be nodes.Get(0) and receiver nodes.Get(1)
        if (t.sourceAddress == Ipv4Address::GetAny() || t.destinationAddress == Ipv4Address::GetAny()) continue;
        double duration = st.timeLastRxPacket.GetSeconds() - st.timeFirstTxPacket.GetSeconds();
        double tp = (duration > 0) ? (st.rxBytes * 8.0) / (duration * 1e6) : 0.0;
        
        // Calculate delay, jitter and packet loss
        double delayMs = (st.rxPackets > 0) ? (st.delaySum.GetSeconds() * 1000.0 / st.rxPackets) : 0.0;
        double jitterMs = (st.rxPackets > 1) ? (st.jitterSum.GetSeconds() * 1000.0 / (st.rxPackets - 1)) : 0.0;
        uint64_t lostPackets = st.txPackets - st.rxPackets;
        double lossRate = (st.txPackets > 0) ? (lostPackets * 100.0 / st.txPackets) : 0.0;
        
        NS_LOG_UNCOND("FLOW_SUMMARY: freq=" << freq << " proto=" << protocol 
                      << " Throughput_Mbps=" << tp 
                      << " Delay_ms=" << delayMs 
                      << " Jitter_ms=" << jitterMs
                      << " LostPackets=" << lostPackets
                      << " LossRate_pct=" << lossRate
                      << " TX_Packets=" << st.txPackets << " RX_Packets=" << st.rxPackets);
    }

    Simulator::Destroy();
    return 0;
}
