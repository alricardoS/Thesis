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

/* global variable for easier access */
double g_delaySumMs = 0.0;
uint64_t g_delaySamples = 0;
double g_jitterSumMs = 0.0;
uint64_t g_jitterSamples = 0;

double g_lastDelayMs = 0.0;
bool g_firstPacket = true;

/**
 * \brief Used to translate the DropReason enum to a string
 * \return the corresponding enum string
**/
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
 * \param frequency the raw frequency value
 * \return the corresponding frequency
**/
double RoundFrequency(double frequency) {
    if (frequency < 3000) {
        return 2.4; // 2.4 GHz band
    } else if (frequency < 5500) {
        return 5.0; // 5 GHz band
    } else {
        return 6.0; // 6 GHz band
    }
}

/**
 * \brief Prints the list of nodes and their configuration
 * \param nodes the node container
 * \param type a string indicating if we are printing APs or STAs
**/
void PrintNodes(NodeContainer nodes, const std::string type) {
    for(uint32_t nodeIdx = 0; nodeIdx < nodes.GetN(); ++nodeIdx) {
      Ptr<Node> node = nodes.Get(nodeIdx);
  
      NS_LOG_UNCOND(type << " Node " << nodeIdx);
  
      // get the number of Net device interfaces
      // this includes the loopback interface device
      uint32_t numDevices = node->GetNDevices(); 
  
      // get the Ipv4 object for the node
      Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();  
  
      for(uint32_t i = 0; i < numDevices; ++i) 
      {
        // we need to make sure that we have a WifiNetDevice
        Ptr<NetDevice> netDevice = node->GetDevice(i);
        Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(netDevice);
  
        if(wifiDevice) 
        {
          // upper MAC address for the MLD
          Mac48Address upperMacAddress = wifiDevice->GetMac()->GetAddress();
  
          NS_LOG_UNCOND("  Standard: " << wifiDevice->GetStandard());    
          NS_LOG_UNCOND("  Number of wifi interfaces: " << (numDevices - 1));    
  
          // get IPv4 Address for the MLD
          Ipv4Address ipv4Address = Ipv4Address::GetAny();
          int32_t interfaceIndex = ipv4->GetInterfaceForDevice(netDevice);
          if(interfaceIndex != -1) 
          {
            ipv4Address = ipv4->GetAddress(interfaceIndex, 0).GetLocal();
          }
  
          // get the number of PHY links for the MLD
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
  
              // lower MAC address for the MLD
              // this will be the same as the upper MAC if we just have a single PHY in the device
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
  
              // process each supported MCS 
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
 * \param context context string
 * \param packet pointer to the packet
 * \param Address the address of the packet
**/
void MonitorPacketSinkRx(std::string context, Ptr< const Packet > packet, const Address &address)
{
    // this can only be done if EnableSeqTsSizeHeader is set to true at the sender
    // otherwise this header will not exist
    SeqTsSizeHeader header;

    packet->PeekHeader(header);
    Time txTime = header.GetTs();
    Time delay = Simulator::Now() - txTime;

    double delayMs = delay.GetSeconds() * 1000; // convert to ms
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
 * \param sink pointer to the sink app
 * \param lastTotalRx storage for the total number of bytes received 
 * in the last iteration
**/
void CalculateStats(Ptr<PacketSink> sink, uint64_t &lastTotalRx)
{
    uint64_t currentTotalRx = sink->GetTotalRx();
    uint64_t currentTime = Simulator::Now().GetSeconds();

    double throughput = ((currentTotalRx - lastTotalRx) * 8.0) / (1.0 * 1e6); // convert to Mbps

    double avgDelay = (g_delaySamples > 0) ? (g_delaySumMs / g_delaySamples) : 0.0;
    double avgJitter = (g_jitterSamples > 0) ? (g_jitterSumMs / g_jitterSamples) : 0.0;

    lastTotalRx = currentTotalRx;

    NS_LOG_UNCOND("Time: " << currentTime << " s, Throughput: " << static_cast<int>(throughput) << " Mbps"
                    << " Delay: " << avgDelay << " ms" << " Jitter: " << avgJitter << " ms");

    Simulator::Schedule(Seconds(1), &CalculateStats, sink, std::ref(lastTotalRx));
}

int main(int argc, char* argv[])
{
    /* DISCLAIMER: this script is based on NS-3 v3.43 */


    /* useful simulation parameters */

    // time to start the application and end the simulation
    Time APP_START_TIME(Seconds(1));
    Time SIMULATION_END_TIME(Seconds(12));

    // constant data rate to use for the application to saturate the links
    // in the case of TCP, the rate will be dropped to the max rate that TCP can handle
    // in the case of UDP, we need to be more careful because of the packet loss
    // still, UDP can be used to test the maximum link capacity, even if most packets would be lost
    DataRate dataRate("2000Mbps");      

    // payload size for each IP packet
    // defaults to an MTU of 1500 bytes, minus the maximum TCP header size of 60 bytes
    // the maximum UDP header size is 8 bytes, so this will work for either TCP or UDP
    uint32_t payloadSize = 1440;       

    // max bytes to send in the application
    // set to 0 for unlimited
    uint32_t maxBytes = 0;    

    // socket type to use for the applications
    // it can either be ns3::TcpSocketFactory or ns3::UdpSocketFactory
    std::string appSocketType = "ns3::TcpSocketFactory";

    // if we care about pcap files
    // although, the files can be large for longer simulations
    bool enablePcaps = true;

    /* global configs */

    // the default TCP congestion algorithm used in linux is TCP Cubic
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", 
                        TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));        

    // make sure the TCP segment size is the same as the payload
    // otherwise we will have a bunch of fragments
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));  

    // enabling packet checksums, to make it more realistic
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));    

    // this can be useful for printing packet info when needed
    // and also to make sure we don't break anything when modifying packets
    ns3::Packet::EnablePrinting();
    ns3::Packet::EnableChecking();  


    // global log functions for any class that we want
    // some of these can generate a ridiculously high amount of data
    // so logging through trace sources or directly in the source code might still be best 
    //LogComponentEnable("TcpCubic", LOG_LEVEL_DEBUG);
    //LogComponentEnable("TcpL4Protocol", LOG_LEVEL_DEBUG);
    //LogComponentEnable("TcpSocketBase", LOG_LEVEL_DEBUG);
    //LogComponentEnable("Ipv4L3Protocol", LOG_LEVEL_DEBUG);
    //LogComponentEnable("WifiMac", LOG_LEVEL_DEBUG);
    //LogComponentEnable("ApWifiMac", LOG_LEVEL_DEBUG);
    //LogComponentEnable("StaWifiMac", LOG_LEVEL_DEBUG);
    //LogComponentEnable("WifiPhy", LOG_LEVEL_DEBUG);
    //LogComponentEnable("IdealWifiManager", LOG_LEVEL_DEBUG);
    //LogComponentEnable("WifiMacQueue", LOG_LEVEL_DEBUG);
    //LogComponentEnable("OnOffApplication", LOG_LEVEL_DEBUG);
    //LogComponentEnable("PacketSink", LOG_LEVEL_DEBUG);

    /* create wifi nodes */

    NodeContainer wifiApNodes(1);
    NodeContainer wifiStaNodes(1);

    /* configure wifi helper */

    // an abstract helper is used to set up the WiFi layer for us
    // it is a wrapper for ns3::WifiNetDevice
    WifiHelper wifiHelper;

    // define standard, which also defines some default supported configurations
    wifiHelper.SetStandard(WIFI_STANDARD_80211be);

    // enable 160 MHz channels for 5 GHz (VHT or WiFi 5) if we want
    // although, using the 80 MHz channels is more realistic for the 5 GHz
    // more detail can be seen in the ns3::VhtConfiguration class
    //wifiHelper.ConfigVhtOptions("Support160MHzOperation", BooleanValue(true));    
    
    // allow shortest guard interval (GI) possible of 800 ns for HE (WiFi 6/6E)
    // more detail can be seen in the ns3::HeConfiguration class
    wifiHelper.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));

    // if we want to use the eMLSR MLO mode, this needs to be active
    // otherwise it defaults to the STR MLO mode
    // more detail can be seen in the ns3::EhtConfiguration class (EHT or WiFi 7)
    // currently, most configurations for EHT are taken from the ones set in HE
    wifiHelper.ConfigEhtOptions("EmlsrActivated", BooleanValue(false));

    /* configure wifi PHY for multi-link */

    // the SpectrumWifiPhy appears to be the only class that supports multi-link
    // so we use it to configure the number of PHY links
    // this helper is a wrapper for the ns3::SpectrumWifiPhy
    SpectrumWifiPhyHelper wifiPhy(2); 
    
    // we can increase the number of spatial streams (NSS) to improve throughput
    // most devices support at least 2 NSS
    // more detail can be seen in the MCS index table
    wifiPhy.Set("Antennas", UintegerValue(2));
    wifiPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    wifiPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    // we can add signal propagation models to each channel
    // otherwise the links would always be in perfect conditions
    Ptr<MultiModelSpectrumChannel> spectrumChannel6Ghz = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel6Ghz->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    spectrumChannel6Ghz->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    Ptr<MultiModelSpectrumChannel> spectrumChannel5Ghz = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel5Ghz->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    spectrumChannel5Ghz->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    // all channels are then added, with one channel for each PHY link
    // the ChannelSettings format is as follows:
    // -- channel number
    // -- channel bandwidth
    // -- constant indicating the frequency range
    // -- the primary 20 MHz channel number inside the given channel 
    //    (or can be set to 0 to pick the first available one)
    wifiPhy.AddChannel(spectrumChannel6Ghz, WIFI_SPECTRUM_6_GHZ);
    wifiPhy.Set(0, "ChannelSettings", StringValue("{15, 160, BAND_6GHZ, 0}"));
    wifiPhy.AddPhyToFreqRangeMapping(0, WIFI_SPECTRUM_6_GHZ);

    wifiPhy.AddChannel(spectrumChannel5Ghz, WIFI_SPECTRUM_5_GHZ);
    wifiPhy.Set(1, "ChannelSettings", StringValue("{42, 80, BAND_5GHZ, 0}"));
    wifiPhy.AddPhyToFreqRangeMapping(1, WIFI_SPECTRUM_5_GHZ);

    // finally we need to set up a rate control algorithm for each PHY link
    // this controls the MCS and NSS to use for each transmission
    // linux actually uses the ns3::MinstrelWifiManager by default, but it might be bugged in NS-3 (issue 1138 in gitlab)
    // so just use the NS-3 default one instead
    wifiHelper.SetRemoteStationManager(0, std::string("ns3::IdealWifiManager"));
    wifiHelper.SetRemoteStationManager(1, std::string("ns3::IdealWifiManager"));

    /* configure wifi MAC for AP and STA nodes */

    // SSID shared by all links
    Ssid ssid = Ssid("wifi7-test");

    // the WifiMacHelper is a wrapper for the ns3::ApWifiMac and for the ns3::StaWifiMac
    // since most devices use QoS, we enable it here as well to be more realistic
    // this means we will have 4 Tx queues at the MAC level for each access category (AC):
    // -- AC_VO, for packets tagged as voice
    // -- AC_VI, for packets tagged as video
    // -- AC_BE, for best effort, which is the default for most packets
    // -- AC_BK, for background, which is mainly non-time-sensitive bulk data transfers
    WifiMacHelper wifiApMac;
    wifiApMac.SetType("ns3::ApWifiMac", 
                      "Ssid", SsidValue(ssid),
                      "BeaconGeneration", BooleanValue(true),
                      "BeaconInterval", TimeValue(MicroSeconds(102400)),  // 100 ms
                      "QosSupported", BooleanValue(true));
    
    WifiMacHelper wifiStaMac;
    wifiStaMac.SetType("ns3::StaWifiMac", 
                       "Ssid", SsidValue(ssid),
                       "QosSupported", BooleanValue(true),
                       "ActiveProbing", BooleanValue(false));

    // these will hold our actual wifi devices, after configuring everything
    NetDeviceContainer wifiApDevices = wifiHelper.Install(wifiPhy, wifiApMac, wifiApNodes);
    NetDeviceContainer wifiStaDevices = wifiHelper.Install(wifiPhy, wifiStaMac, wifiStaNodes);

    // mobility also needs to be added, even if all devices are stationary
    // this is so the simulator knows about their positions
    // the MobilityHelper is a wrapper for the ns3::PositionAllocator and ns3::MobilityModel
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

    /* add protocol stack to wifi nodes */

    // this is a wrapper that adds IP/TCP/UDP and routing functionality to nodes
    InternetStackHelper internetStack;
    internetStack.Install(wifiApNodes);
    internetStack.Install(wifiStaNodes);

    /* add IPv4 to wifi devices */

    // this is a wrapper for the ns3::Ipv4Address
    Ipv4AddressHelper address;
    address.SetBase("192.168.1.0", "255.255.255.0");

    // this will contain the IPs from all interfaces in each device
    Ipv4InterfaceContainer wifiApInterfaces = address.Assign(wifiApDevices);
    Ipv4InterfaceContainer wifiStaInterfaces = address.Assign(wifiStaDevices);

    /* add applications */

    // a PacketSinkHelper is used for the receiver, which is a wrapper for the ns3::PacketSink
    uint16_t port = 5001;
    Address sinkAddress(InetSocketAddress(wifiStaInterfaces.GetAddress(0), port));
    PacketSinkHelper sinkHelper(appSocketType, sinkAddress);

    ApplicationContainer sinkApps = sinkHelper.Install(wifiStaNodes.Get(0));
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(0));
    sink->TraceConnect("Rx", sink->GetTypeId().GetName(), MakeCallback(&MonitorPacketSinkRx));

    // an OnOff app is used for the sender, which can be used either for UDP or TCP
    // this is a wrapper for the ns3::OnOffApplication
    // for constant traffic, we can set the OnTime to always be 1 and the OffTime to always be 0
    // the EnableSeqTsSizeHeader can be useful for tracking Tx timestamps across network layers
    OnOffHelper client(appSocketType, sinkAddress);
    client.SetAttribute("PacketSize", UintegerValue(payloadSize));
    client.SetAttribute("MaxBytes", UintegerValue(maxBytes));
    client.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    client.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    client.SetAttribute("DataRate", DataRateValue(dataRate));
    client.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));

    ApplicationContainer clientApps = client.Install(wifiApNodes.Get(0));
    Ptr<OnOffApplication> onOff = DynamicCast<OnOffApplication>(clientApps.Get(0));

    // start the applications a bit after the simulation start
    // start the sink app first and end it last
    // start the onoff app second and end it first
    sinkApps.Start(APP_START_TIME);
    sinkApps.Stop(SIMULATION_END_TIME - Seconds(1));
    clientApps.Start(APP_START_TIME + Seconds(1));
    clientApps.Stop(SIMULATION_END_TIME - Seconds(2));

    /* add pcaps */
    /*  
    if(enablePcaps)
    {
        // for WiFi 7 with MLO active, the EnablePcap at the WiFi level appears to be bugged (issue 1179 in gitlab)
        // so its better to use the EnablePcapIpv4 at the IP level, but we lose the vision at the WiFi layer
        wifiPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        wifiPhy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_DEVICE);
        wifiPhy.EnablePcap("ap-wifi", wifiApDevices.Get(0));
        wifiPhy.EnablePcap("sta-wifi", wifiStaDevices.Get(0));
        internetStack.EnablePcapIpv4("ap-ip", wifiApNodes.Get(0)->GetId(), 1, false);
        internetStack.EnablePcapIpv4("sta-ip", wifiStaNodes.Get(0)->GetId(), 1, false);
    }
    */  
    /* add flow monitor */

    // this can be used to get some overall final statistics
    // but trace routes are still needed to get statistics over time
    // it is a wrapper for the ns3::FlowMonitor
    FlowMonitorHelper flowMonitorHelper;
    Ptr<FlowMonitor> flowMonitor = flowMonitorHelper.InstallAll();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowMonitorHelper.GetClassifier());

    /* run simulation */

    NS_LOG_UNCOND("Running Simulator");
    NS_LOG_UNCOND("----------------------------");

    // compute statistics each second, on average
    uint64_t lastTotalRx = 0;
    Simulator::Schedule(Seconds(1), &CalculateStats, sink, std::ref(lastTotalRx));

    // making sure that the main WiFi attributes were configured correctly
    PrintNodes(wifiApNodes, "AP");
    PrintNodes(wifiStaNodes, "STA");

    Simulator::Stop(SIMULATION_END_TIME + Seconds(1));
    Simulator::Run();
    Simulator::Destroy();

    NS_LOG_UNCOND("Simulator Done");

    /* get flow stats */

    flowMonitor->CheckForLostPackets();
    auto stats = flowMonitor->GetFlowStats();

    for(const auto &flow : stats) 
    {
        uint32_t flowId = flow.first;
        auto flowStats = flow.second;

        // get flow classification information (5-tuple)
        Ipv4FlowClassifier::FiveTuple fiveTuple = classifier->FindFlow(flowId);

        double duration = (flowStats.timeLastRxPacket.GetSeconds() - flowStats.timeFirstTxPacket.GetSeconds());
        double throughput = 0.0;
        double avgDelay = 0.0;
        double avgJitter = 0.0;
        double packetLossRatio = 0.0;
        uint32_t numDropReasons = ns3::Ipv4FlowProbe::DROP_INVALID_REASON;

        if(duration > 0) 
        {
            throughput = (flowStats.rxBytes * 8.0) / (duration * 1e6); // convert to Mbps
        }

        if(flowStats.rxPackets > 0)
        {
            avgDelay = flowStats.delaySum.GetSeconds() / flowStats.rxPackets;
            avgDelay *= 1000;   // convert to ms
        }

        if(flowStats.rxPackets > 1)
        {
            avgJitter = flowStats.jitterSum.GetSeconds() / (flowStats.rxPackets - 1);
            avgJitter *= 1000;  // convert to ms
        }

        if(flowStats.txPackets > 0)
        {
            packetLossRatio = (static_cast<double>(flowStats.lostPackets) / flowStats.txPackets) * 100.0;
        }

        NS_LOG_UNCOND("----------------------------");
        NS_LOG_UNCOND("Flow ID: " << flowId);
        NS_LOG_UNCOND("Protocol: " << (fiveTuple.protocol == 6 ? "TCP" : (fiveTuple.protocol == 17 ? "UDP" : "Other")));
        NS_LOG_UNCOND("Sender (Source IP:Port): " << fiveTuple.sourceAddress << ":" << fiveTuple.sourcePort);
        NS_LOG_UNCOND("Receiver (Destination IP:Port): " << fiveTuple.destinationAddress << ":" << fiveTuple.destinationPort);
        NS_LOG_UNCOND("Duration: " << static_cast<int>(duration) << " s");
        NS_LOG_UNCOND("Transmitted Bytes: " << flowStats.txBytes << " bytes");
        NS_LOG_UNCOND("Received Bytes: " << flowStats.rxBytes << " bytes");
        NS_LOG_UNCOND("Transmitted Packets: " << flowStats.txPackets << " packets");
        NS_LOG_UNCOND("Received Packets: " << flowStats.rxPackets << " packets");
        NS_LOG_UNCOND("Average Throughput: " << static_cast<int>(throughput) << " Mbps");
        NS_LOG_UNCOND("Average Delay: " << avgDelay << " ms");
        NS_LOG_UNCOND("Average Jitter: " << avgJitter << " ms");

        // the lost packets can be annoying because the monitor will flag packets as lost
        // if they are still in flight or they have not been processed yet once the simulation ended
        // but if the 'Packet Drops by Reason' is empty, then it can probably be ignored
        NS_LOG_UNCOND("Lost Packets: " << flowStats.lostPackets << " packets");
        NS_LOG_UNCOND("Packet Loss Percentage: " << static_cast<int>(packetLossRatio) << " %");
        NS_LOG_UNCOND("Packet Drops by Reason:");
        for (size_t i = 0; i < numDropReasons; ++i)
        {
            ns3::Ipv4FlowProbe::DropReason reason = static_cast<ns3::Ipv4FlowProbe::DropReason>(i);
            uint32_t count = (i < flowStats.packetsDropped.size()) ? flowStats.packetsDropped[i] : 0;
            NS_LOG_UNCOND("  " << DropReasonToString(reason) << ": " << count << " packets");
        }
        NS_LOG_UNCOND("----------------------------");
    }

    return 0;
}
