/*
 * wifi7-freq-pair-test-proto.cc
 * 802.11be MLO test with configurable frequency pairs AND Protocol.
 * Supports: TCP vs UDP selection
 * This is a separate file to avoid changing the original `wifi7-freq-pair-test.cc`.
 */
#include "ns3/core-module.h"
#include "ns3/mobility-helper.h"
#include "ns3/ssid.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/wifi-mac.h"
#include "ns3/wifi-phy.h"
#include "ns3/log.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("wifi7-freq-pair-test-proto");

/* global variable for easier access (cumulative delay/jitter) */
double g_delaySumMs = 0.0;
uint64_t g_delaySamples = 0;
double g_jitterSumMs = 0.0;
uint64_t g_jitterSamples = 0;
double g_lastDelayMs = 0.0;
bool g_firstPacket = true;

// Minimal helper: GetChannelConfig
struct ChannelConfig {
    int channel;
    int width;
    std::string band;
    FrequencyRange freqRange;
};

ChannelConfig GetChannelConfig(int freq) {
    ChannelConfig config;
    if (freq == 2) {
        config.channel = 6;
        config.width = 40;
        config.band = "BAND_2_4GHZ";
        config.freqRange = WIFI_SPECTRUM_2_4_GHZ;
    } else if (freq == 5) {
        config.channel = 42;
        config.width = 80;
        config.band = "BAND_5GHZ";
        config.freqRange = WIFI_SPECTRUM_5_GHZ;
    } else {
        config.channel = 15;
        config.width = 160;
        config.band = "BAND_6GHZ";
        config.freqRange = WIFI_SPECTRUM_6_GHZ;
    }
    return config;
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
    // Default params
    int freq1 = 5;
    int freq2 = 6;
    std::string dataRateStr = "2000Mbps";
    double simTime = 12.0;
    bool enablePcaps = false;
    std::string protocol = "UDP"; // "TCP" or "UDP"

    CommandLine cmd;
    cmd.AddValue("freq1", "First link frequency (2,5,6)", freq1);
    cmd.AddValue("freq2", "Second link frequency (2,5,6)", freq2);
    cmd.AddValue("dataRate", "Application data rate", dataRateStr);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.AddValue("enablePcaps", "Enable PCAP captures", enablePcaps);
    cmd.AddValue("protocol", "Application protocol: TCP or UDP", protocol);
    cmd.Parse(argc, argv);

    if (freq1 == freq2) {
        NS_FATAL_ERROR("freq1 and freq2 must be different");
    }

    std::string appSocketType;
    if (protocol == "TCP") {
        appSocketType = "ns3::TcpSocketFactory";
        // TCP-specific defaults
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));
        Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1440));
    } else if (protocol == "UDP") {
        appSocketType = "ns3::UdpSocketFactory";
    } else {
        NS_FATAL_ERROR("protocol must be TCP or UDP");
    }
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));
    ns3::Packet::EnablePrinting();
    ns3::Packet::EnableChecking();

    NS_LOG_UNCOND("wifi7-freq-pair-test-proto: freq1=" << freq1 << " freq2=" << freq2 << " proto=" << protocol << " rate=" << dataRateStr);

    // Nodes
    NodeContainer apNode; apNode.Create(1);
    NodeContainer staNode; staNode.Create(1);

    // Wifi helper
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifi.ConfigEhtOptions("EmlsrActivated", BooleanValue(false));

    // PHY (2 links)
    SpectrumWifiPhyHelper phy(2);
    phy.Set("Antennas", UintegerValue(2));
    phy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    phy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    Ptr<MultiModelSpectrumChannel> ch1 = CreateObject<MultiModelSpectrumChannel>();
    ch1->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    ch1->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    Ptr<MultiModelSpectrumChannel> ch2 = CreateObject<MultiModelSpectrumChannel>();
    ch2->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    ch2->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    ChannelConfig c1 = GetChannelConfig(freq1);
    ChannelConfig c2 = GetChannelConfig(freq2);

    phy.AddChannel(ch1, c1.freqRange);
    phy.Set(0, "ChannelSettings", StringValue("{" + std::to_string(c1.channel) + ", " + std::to_string(c1.width) + ", " + c1.band + ", 0}"));
    phy.AddPhyToFreqRangeMapping(0, c1.freqRange);

    phy.AddChannel(ch2, c2.freqRange);
    phy.Set(1, "ChannelSettings", StringValue("{" + std::to_string(c2.channel) + ", " + std::to_string(c2.width) + ", " + c2.band + ", 0}"));
    phy.AddPhyToFreqRangeMapping(1, c2.freqRange);

    // Rate manager
    wifi.SetRemoteStationManager(0, std::string("ns3::IdealWifiManager"));
    wifi.SetRemoteStationManager(1, std::string("ns3::IdealWifiManager"));

    // MAC
    Ssid ssid = Ssid("wifi7-mlo-proto");
    WifiMacHelper apMac; apMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid), "QosSupported", BooleanValue(true));
    WifiMacHelper staMac; staMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "QosSupported", BooleanValue(true), "ActiveProbing", BooleanValue(false));

    NetDeviceContainer apDev = wifi.Install(phy, apMac, apNode);
    NetDeviceContainer staDev = wifi.Install(phy, staMac, staNode);

    // Mobility
    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::GridPositionAllocator", "MinX", DoubleValue(0.0), "MinY", DoubleValue(0.0), "DeltaX", DoubleValue(1.0), "DeltaY", DoubleValue(0.0), "GridWidth", UintegerValue(3), "LayoutType", StringValue("RowFirst"));
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(apNode);
    mobility.Install(staNode);
    apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0,0.0,0.0));
    staNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(5.0,0.0,0.0));

    // Internet
    InternetStackHelper stack; stack.Install(apNode); stack.Install(staNode);
    Ipv4AddressHelper address; address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifAp = address.Assign(apDev);
    Ipv4InterfaceContainer ifSta = address.Assign(staDev);

    // Applications
    uint16_t port = 5001;
    Address sinkAddress(InetSocketAddress(ifSta.GetAddress(0), port));
    PacketSinkHelper sinkHelper((protocol=="TCP")?"ns3::TcpSocketFactory":"ns3::UdpSocketFactory", sinkAddress);
    ApplicationContainer sinkApp = sinkHelper.Install(staNode.Get(0));
    sinkApp.Start(Seconds(1.0));
    sinkApp.Stop(Seconds(simTime+0.5));
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));
    // connect RX trace to compute per-packet delay/jitter from SeqTsSizeHeader
    sink->TraceConnect("Rx", sink->GetTypeId().GetName(), MakeCallback(&MonitorPacketSinkRx));

    OnOffHelper onoff((protocol=="TCP")?"ns3::TcpSocketFactory":"ns3::UdpSocketFactory", sinkAddress);
    onoff.SetAttribute("PacketSize", UintegerValue(1440));
    onoff.SetAttribute("DataRate", DataRateValue(DataRate(dataRateStr)));
    onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    onoff.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));

    ApplicationContainer app = onoff.Install(apNode.Get(0));
    app.Start(Seconds(1.5));
    app.Stop(Seconds(simTime-0.5));

    /*
    // PCAP
    if (enablePcaps) {
        std::string pref = "mlo_p_" + std::to_string(freq1) + "_" + std::to_string(freq2) + "_" + protocol;
        phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_DEVICE);
        phy.EnablePcap(pref + "_ap", apDev.Get(0));
        phy.EnablePcap(pref + "_sta", staDev.Get(0));
    }
    */
    // Flow monitor
    FlowMonitorHelper fm; Ptr<FlowMonitor> monitor = fm.InstallAll();

    // periodic stats printing (per-second) using the PacketSink
    uint64_t lastTotalRx = 0;
    Simulator::Schedule(Seconds(1), &CalculateStats, sink, std::ref(lastTotalRx));

    Simulator::Stop(Seconds(simTime + 1.0));
    Simulator::Run();

    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fm.GetClassifier());
    auto stats = monitor->GetFlowStats();

    for (const auto &kv : stats) {
        FlowId id = kv.first;
        const FlowMonitor::FlowStats &st = kv.second;
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(id);
        // report only downlink from AP
        if (t.sourceAddress == ifAp.GetAddress(0) && t.destinationAddress == ifSta.GetAddress(0)) {
            double duration = st.timeLastRxPacket.GetSeconds() - st.timeFirstTxPacket.GetSeconds();
            double tp = (duration > 0) ? (st.rxBytes * 8.0) / (duration * 1e6) : 0.0;
            
            // Calculate delay, jitter and packet loss
            double delayMs = (st.rxPackets > 0) ? (st.delaySum.GetSeconds() * 1000.0 / st.rxPackets) : 0.0;
            double jitterMs = (st.rxPackets > 1) ? (st.jitterSum.GetSeconds() * 1000.0 / (st.rxPackets - 1)) : 0.0;
            uint64_t lostPackets = st.txPackets - st.rxPackets;
            double lossRate = (st.txPackets > 0) ? (lostPackets * 100.0 / st.txPackets) : 0.0;
            
            NS_LOG_UNCOND("FLOW_SUMMARY: pair=" << freq1 << "+" << freq2 << " proto=" << protocol 
                          << " Throughput_Mbps=" << tp 
                          << " Delay_ms=" << delayMs 
                          << " Jitter_ms=" << jitterMs
                          << " LostPackets=" << lostPackets
                          << " LossRate_pct=" << lossRate
                          << " TX_Packets=" << st.txPackets << " RX_Packets=" << st.rxPackets);
        }
    }

    Simulator::Destroy();
    return 0;
}
