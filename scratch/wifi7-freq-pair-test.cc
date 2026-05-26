/*
 * wifi7-freq-pair-test.cc
 * 802.11be MLO test with configurable frequency pairs.
 * Supports combinations: 2.4+5, 2.4+6, 5+6 GHz
 * Channel widths: 2.4GHz=40MHz, 5GHz=80MHz, 6GHz=160MHz
 * 
 * Based on wifi7-base-test.cc
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
#include "ns3/wifi-mac-queue.h"
#include "ns3/frame-exchange-manager.h"
#include "ns3/traffic-control-module.h"
#include "ns3/wifi-phy.h"
#include "ns3/log.h"
#include <fstream>
#include <iostream>
#include <deque>
#include <string>
#include <vector>
#include <map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("wifi7-freq-pair-test");

/* global variable for easier access */
double g_delaySumMs = 0.0;
uint64_t g_delaySamples = 0;
double g_jitterSumMs = 0.0;
uint64_t g_jitterSamples = 0;

double g_lastDelayMs = 0.0;
bool g_firstPacket = true;

/**
 * \brief Used to translate the DropReason enum to a string
 */
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
 * \brief Used to find the main frequency range
 */
double RoundFrequency(double frequency) {
    if (frequency < 3000) {
        return 2.4;
    } else if (frequency < 5500) {
        return 5.0;
    } else {
        return 6.0;
    }
}

/**
 * \brief Prints the list of nodes and their configuration
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
  
          for(uint32_t phyIdx = 0; phyIdx < numPhys; ++phyIdx) 
          {
              Ptr<WifiPhy> phy = wifiDevice->GetPhy(phyIdx);
              double frequency = RoundFrequency(phy->GetFrequency()); 
  
              Ptr<FrameExchangeManager> fm = wifiDevice->GetMac()->GetFrameExchangeManager(phyIdx);
              Mac48Address lowerMacAddress = fm->GetAddress();
  
              NS_LOG_UNCOND("        PHY " << phyIdx << ":");
              NS_LOG_UNCOND("          Lower MAC Address = " << lowerMacAddress);
              NS_LOG_UNCOND("          Frequency = " << frequency << " GHz");
              NS_LOG_UNCOND("          Bandwidth = " << phy->GetChannelWidth() << " MHz");
              NS_LOG_UNCOND("          Channel Index = " << static_cast<int>(phy->GetChannelNumber()));
              NS_LOG_UNCOND("          Primary 20 MHz Channel Index = " << static_cast<int>(phy->GetPrimaryChannelNumber(MHz_u(20))));
              NS_LOG_UNCOND("          Number of Antennas = " << static_cast<int>(phy->GetNumberOfAntennas()));
              NS_LOG_UNCOND("          Max Supported TX Spatial Streams = " << static_cast<int>(phy->GetMaxSupportedTxSpatialStreams()));
              NS_LOG_UNCOND("          Max Supported RX Spatial Streams = " << static_cast<int>(phy->GetMaxSupportedRxSpatialStreams()));
  
              std::map<ns3::WifiModulationClass, std::pair<int, int>> groups;
  
              for (const auto &mcs : phy->GetMcsList())
              {
                  int mcsIndex = static_cast<int>(mcs.GetMcsValue());
                  ns3::WifiModulationClass mcsClass = mcs.GetModulationClass();
                  
                  if (groups.find(mcsClass) == groups.end())
                  {
                      groups[mcsClass] = std::make_pair(mcsIndex, mcsIndex);
                  }
                  else
                  {
                      groups[mcsClass].first = std::min(groups[mcsClass].first, mcsIndex);
                      groups[mcsClass].second = std::max(groups[mcsClass].second, mcsIndex);
                  }
              }
  
              NS_LOG_UNCOND("          Max Supported Modulation = " << phy->GetMaxModulationClassSupported());
  
              for (const auto &group : groups)
              {  
                  NS_LOG_UNCOND("          " << group.first << " MCS Range = " 
                                << group.second.first << "-" << group.second.second);
              }
          }
        } 
      }
      NS_LOG_UNCOND("----------------------------");
    }
}

/**
 * \brief Callback to monitor the reception of packets at the sink
 */
void MonitorPacketSinkRx(std::string context, Ptr<const Packet> packet, const Address &address)
{
    SeqTsSizeHeader header;
    packet->PeekHeader(header);
    Time txTime = header.GetTs();
    Time delay = Simulator::Now() - txTime;

    double delayMs = delay.GetSeconds() * 1000;
    g_delaySumMs += delayMs;
    g_delaySamples++;

    if(!g_firstPacket) 
    {
        double jitter = std::fabs(delayMs - g_lastDelayMs);
        g_jitterSumMs += jitter;
        g_jitterSamples++;
    } 
    else 
    {
        g_firstPacket = false;
    }

    g_lastDelayMs = delayMs;
}

/**
 * \brief Callback to monitor packet statistics each second
 */
void CalculateStats(Ptr<PacketSink> sink, uint64_t &lastTotalRx)
{
    uint64_t currentTotalRx = sink->GetTotalRx();
    uint64_t currentTime = Simulator::Now().GetSeconds();

    double throughput = ((currentTotalRx - lastTotalRx) * 8.0) / (1.0 * 1e6);

    double avgDelay = (g_delaySamples > 0) ? (g_delaySumMs / g_delaySamples) : 0.0;
    double avgJitter = (g_jitterSamples > 0) ? (g_jitterSumMs / g_jitterSamples) : 0.0;

    lastTotalRx = currentTotalRx;

    // Format: TIME_STATS: time throughput delay jitter (for easy parsing)
    NS_LOG_UNCOND("TIME_STATS: " << currentTime << " " << static_cast<int>(throughput) 
                  << " " << avgDelay << " " << avgJitter);

    Simulator::Schedule(Seconds(1), &CalculateStats, sink, std::ref(lastTotalRx));
}

/**
 * \brief Get channel settings for a given frequency
 */
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
    } else { // freq == 6
        config.channel = 15;
        config.width = 160;
        config.band = "BAND_6GHZ";
        config.freqRange = WIFI_SPECTRUM_6_GHZ;
    }
    return config;
}

int main(int argc, char* argv[])
{
    /* Command line parameters */
    int freq1 = 5;  // First link frequency: 2, 5, or 6
    int freq2 = 6;  // Second link frequency: 2, 5, or 6
    std::string dataRateStr = "2000Mbps";
    double simTime = 12.0;
    bool enablePcaps = true;
    std::string protocol = "UDP";

    CommandLine cmd;
    cmd.AddValue("freq1", "First link frequency (2=2.4GHz, 5=5GHz, 6=6GHz)", freq1);
    cmd.AddValue("freq2", "Second link frequency (2=2.4GHz, 5=5GHz, 6=6GHz)", freq2);
    cmd.AddValue("dataRate", "Application data rate (e.g., 2000Mbps)", dataRateStr);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("enablePcaps", "Enable PCAP captures", enablePcaps);
    cmd.AddValue("protocol", "Application protocol: TCP or UDP", protocol);
    cmd.Parse(argc, argv);

    // Validate frequencies
    if ((freq1 != 2 && freq1 != 5 && freq1 != 6) ||
        (freq2 != 2 && freq2 != 5 && freq2 != 6)) {
        NS_FATAL_ERROR("Frequencies must be 2 (2.4GHz), 5 (5GHz), or 6 (6GHz)");
    }
    if (freq1 == freq2) {
        NS_FATAL_ERROR("freq1 and freq2 must be different for MLO");
    }

    // Get channel configurations
    ChannelConfig config1 = GetChannelConfig(freq1);
    ChannelConfig config2 = GetChannelConfig(freq2);

    NS_LOG_UNCOND("=== WiFi 7 Frequency Pair Test ===");
    NS_LOG_UNCOND("Link 1: " << (freq1 == 2 ? "2.4" : (freq1 == 5 ? "5" : "6")) << " GHz, " 
                  << config1.width << " MHz");
    NS_LOG_UNCOND("Link 2: " << (freq2 == 2 ? "2.4" : (freq2 == 5 ? "5" : "6")) << " GHz, " 
                  << config2.width << " MHz");
    NS_LOG_UNCOND("Data Rate: " << dataRateStr);
    NS_LOG_UNCOND("Simulation Time: " << simTime << " s");
    NS_LOG_UNCOND("==================================");

    /* Simulation parameters */
    Time APP_START_TIME(Seconds(1));
    Time SIMULATION_END_TIME(Seconds(simTime));
    DataRate dataRate(dataRateStr);
    uint32_t payloadSize = 1440;
    uint32_t maxBytes = 0;
    std::string appSocketType;

    // Determine socket type from protocol string
    if (protocol == "TCP") {
        appSocketType = "ns3::TcpSocketFactory";
    } else if (protocol == "UDP") {
        appSocketType = "ns3::UdpSocketFactory";
    } else {
        NS_FATAL_ERROR("Unsupported protocol: " << protocol << ". Use TCP or UDP.");
    }

    /* Global configs */
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", 
                        TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));
    ns3::Packet::EnablePrinting();
    ns3::Packet::EnableChecking();

    /* Create wifi nodes */
    NodeContainer wifiApNodes(1);
    NodeContainer wifiStaNodes(1);

    /* Configure wifi helper */
    WifiHelper wifiHelper;
    wifiHelper.SetStandard(WIFI_STANDARD_80211be);
    wifiHelper.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifiHelper.ConfigEhtOptions("EmlsrActivated", BooleanValue(false));

    /* Configure wifi PHY for multi-link */
    SpectrumWifiPhyHelper wifiPhy(2);
    wifiPhy.Set("Antennas", UintegerValue(2));
    wifiPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    wifiPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    // Create spectrum channels for each frequency
    Ptr<MultiModelSpectrumChannel> spectrumChannel1 = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel1->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    spectrumChannel1->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    Ptr<MultiModelSpectrumChannel> spectrumChannel2 = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel2->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    spectrumChannel2->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    // Configure Link 0 (freq1)
    wifiPhy.AddChannel(spectrumChannel1, config1.freqRange);
    std::string channelSettings1 = "{" + std::to_string(config1.channel) + ", " 
                                   + std::to_string(config1.width) + ", " 
                                   + config1.band + ", 0}";
    wifiPhy.Set(0, "ChannelSettings", StringValue(channelSettings1));
    wifiPhy.AddPhyToFreqRangeMapping(0, config1.freqRange);

    // Configure Link 1 (freq2)
    wifiPhy.AddChannel(spectrumChannel2, config2.freqRange);
    std::string channelSettings2 = "{" + std::to_string(config2.channel) + ", " 
                                   + std::to_string(config2.width) + ", " 
                                   + config2.band + ", 0}";
    wifiPhy.Set(1, "ChannelSettings", StringValue(channelSettings2));
    wifiPhy.AddPhyToFreqRangeMapping(1, config2.freqRange);

    // Rate managers for each link
    wifiHelper.SetRemoteStationManager(0, std::string("ns3::IdealWifiManager"));
    wifiHelper.SetRemoteStationManager(1, std::string("ns3::IdealWifiManager"));

    /* Configure wifi MAC */
    Ssid ssid = Ssid("wifi7-freq-pair-test");

    WifiMacHelper wifiApMac;
    wifiApMac.SetType("ns3::ApWifiMac", 
                      "Ssid", SsidValue(ssid),
                      "BeaconGeneration", BooleanValue(true),
                      "BeaconInterval", TimeValue(MicroSeconds(102400)),
                      "QosSupported", BooleanValue(true));
    
    WifiMacHelper wifiStaMac;
    wifiStaMac.SetType("ns3::StaWifiMac", 
                       "Ssid", SsidValue(ssid),
                       "QosSupported", BooleanValue(true),
                       "ActiveProbing", BooleanValue(false));

    NetDeviceContainer wifiApDevices = wifiHelper.Install(wifiPhy, wifiApMac, wifiApNodes);
    NetDeviceContainer wifiStaDevices = wifiHelper.Install(wifiPhy, wifiStaMac, wifiStaNodes);

    /* Mobility */
    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::GridPositionAllocator",
                                  "MinX", DoubleValue(0.0),
                                  "MinY", DoubleValue(0.0),
                                  "DeltaX", DoubleValue(1.0),
                                  "DeltaY", DoubleValue(1.0),
                                  "GridWidth", UintegerValue(10),
                                  "LayoutType", StringValue("RowFirst"));
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(wifiApNodes);
    mobility.Install(wifiStaNodes);

    /* Internet stack */
    InternetStackHelper internetStack;
    internetStack.Install(wifiApNodes);
    internetStack.Install(wifiStaNodes);

    Ipv4AddressHelper address;
    address.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer wifiApInterfaces = address.Assign(wifiApDevices);
    Ipv4InterfaceContainer wifiStaInterfaces = address.Assign(wifiStaDevices);

    /* Applications */
    uint16_t port = 5001;
    Address sinkAddress(InetSocketAddress(wifiStaInterfaces.GetAddress(0), port));
    PacketSinkHelper sinkHelper(appSocketType, sinkAddress);

    ApplicationContainer sinkApps = sinkHelper.Install(wifiStaNodes.Get(0));
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(0));
    sink->TraceConnect("Rx", sink->GetTypeId().GetName(), MakeCallback(&MonitorPacketSinkRx));

    OnOffHelper client(appSocketType, sinkAddress);
    client.SetAttribute("PacketSize", UintegerValue(payloadSize));
    client.SetAttribute("MaxBytes", UintegerValue(maxBytes));
    client.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    client.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    client.SetAttribute("DataRate", DataRateValue(dataRate));
    client.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));

    ApplicationContainer clientApps = client.Install(wifiApNodes.Get(0));

    sinkApps.Start(APP_START_TIME);
    sinkApps.Stop(SIMULATION_END_TIME - Seconds(1));
    clientApps.Start(APP_START_TIME + Seconds(1));
    clientApps.Stop(SIMULATION_END_TIME - Seconds(2));

    /* PCAP captures */
    /*
    if(enablePcaps)
    {
        // Create pcap prefix with frequency info
        std::string pcapPrefix = "freq" + std::to_string(freq1) + "_" + std::to_string(freq2);
        
        wifiPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        wifiPhy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_DEVICE);
        wifiPhy.EnablePcap(pcapPrefix + "_ap", wifiApDevices.Get(0));
        wifiPhy.EnablePcap(pcapPrefix + "_sta", wifiStaDevices.Get(0));
    }
    */
    /* Flow monitor */
    FlowMonitorHelper flowMonitorHelper;
    Ptr<FlowMonitor> flowMonitor = flowMonitorHelper.InstallAll();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowMonitorHelper.GetClassifier());

    /* Run simulation */
    NS_LOG_UNCOND("Running Simulator");
    NS_LOG_UNCOND("----------------------------");

    uint64_t lastTotalRx = 0;
    Simulator::Schedule(Seconds(1), &CalculateStats, sink, std::ref(lastTotalRx));

    PrintNodes(wifiApNodes, "AP");
    PrintNodes(wifiStaNodes, "STA");

    Simulator::Stop(SIMULATION_END_TIME + Seconds(1));
    Simulator::Run();
    Simulator::Destroy();

    NS_LOG_UNCOND("Simulator Done");

    /* Flow stats */
    flowMonitor->CheckForLostPackets();
    auto stats = flowMonitor->GetFlowStats();

    for(const auto &flow : stats) 
    {
        uint32_t flowId = flow.first;
        auto flowStats = flow.second;

        Ipv4FlowClassifier::FiveTuple fiveTuple = classifier->FindFlow(flowId);

        double duration = (flowStats.timeLastRxPacket.GetSeconds() - flowStats.timeFirstTxPacket.GetSeconds());
        double throughput = 0.0;
        double avgDelay = 0.0;
        double avgJitter = 0.0;
        double packetLossRatio = 0.0;
        uint32_t numDropReasons = ns3::Ipv4FlowProbe::DROP_INVALID_REASON;

        if(duration > 0) 
        {
            throughput = (flowStats.rxBytes * 8.0) / (duration * 1e6);
        }

        if(flowStats.rxPackets > 0)
        {
            avgDelay = flowStats.delaySum.GetSeconds() / flowStats.rxPackets;
            avgDelay *= 1000;
        }

        if(flowStats.rxPackets > 1)
        {
            avgJitter = flowStats.jitterSum.GetSeconds() / (flowStats.rxPackets - 1);
            avgJitter *= 1000;
        }

        if(flowStats.txPackets > 0)
        {
            packetLossRatio = (static_cast<double>(flowStats.lostPackets) / flowStats.txPackets) * 100.0;
        }

        // Print summary in parseable format
        NS_LOG_UNCOND("----------------------------");
        NS_LOG_UNCOND("FLOW_SUMMARY:");
        NS_LOG_UNCOND("  Flow ID: " << flowId);
        NS_LOG_UNCOND("  Protocol: " << (fiveTuple.protocol == 6 ? "TCP" : (fiveTuple.protocol == 17 ? "UDP" : "Other")));
        NS_LOG_UNCOND("  Sender: " << fiveTuple.sourceAddress << ":" << fiveTuple.sourcePort);
        NS_LOG_UNCOND("  Receiver: " << fiveTuple.destinationAddress << ":" << fiveTuple.destinationPort);
        NS_LOG_UNCOND("  Duration: " << static_cast<int>(duration) << " s");
        NS_LOG_UNCOND("  TX_Bytes: " << flowStats.txBytes);
        NS_LOG_UNCOND("  RX_Bytes: " << flowStats.rxBytes);
        NS_LOG_UNCOND("  TX_Packets: " << flowStats.txPackets);
        NS_LOG_UNCOND("  RX_Packets: " << flowStats.rxPackets);
        NS_LOG_UNCOND("  Throughput_Mbps: " << static_cast<int>(throughput));
        NS_LOG_UNCOND("  Delay_ms: " << avgDelay);
        NS_LOG_UNCOND("  Jitter_ms: " << avgJitter);
        NS_LOG_UNCOND("  Lost_Packets: " << flowStats.lostPackets);
        NS_LOG_UNCOND("  Packet_Loss_Percent: " << static_cast<int>(packetLossRatio));
        
        NS_LOG_UNCOND("  Packet Drops by Reason:");
        for (size_t i = 0; i < numDropReasons; ++i)
        {
            ns3::Ipv4FlowProbe::DropReason reason = static_cast<ns3::Ipv4FlowProbe::DropReason>(i);
            uint32_t count = (i < flowStats.packetsDropped.size()) ? flowStats.packetsDropped[i] : 0;
            NS_LOG_UNCOND("    " << DropReasonToString(reason) << ": " << count);
        }
        NS_LOG_UNCOND("----------------------------");
    }

    return 0;
}
