#include "ns3/core-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/friis-spectrum-propagation-loss.h"
#include "ns3/propagation-module.h"
#include "ns3/block-ack-manager.h"
#include <cmath>
#include <sstream>
#include <vector>

using namespace ns3;


/**
 * \brief Used to translate the TypeOfStation enum to a string
 * 
 * \return the corresponding enum string
**/
std::string
TypeOfStationToString(ns3::TypeOfStation type)
{
    switch(type)
    {
        case ns3::TypeOfStation::STA:
            return "STA";
        case ns3::TypeOfStation::AP:
            return "AP";
        case ns3::TypeOfStation::ADHOC_STA:
            return "ADHOC_STA";
        case ns3::TypeOfStation::MESH:
            return "MESH";
        case ns3::TypeOfStation::OCB:
            return "OCB";
        default:
            return "INVALID_STATION_TYPE";
    }
}

/**
 * \brief Used to find the main frequency range
 * 
 * \param frequency the raw frequency value
 * 
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
 * 
 * \param nodes the node container
**/
void PrintNodes(NodeContainer nodes) 
{
    for(uint32_t nodeIdx = 0; nodeIdx < nodes.GetN(); ++nodeIdx) 
    {
        Ptr<Node> node = nodes.Get(nodeIdx);

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

                std::string nodeType = TypeOfStationToString(wifiDevice->GetMac()->GetTypeOfStation());

                NS_LOG_UNCOND(nodeType << " Node " << nodeIdx);
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
 * \brief Used to translate the DropReason enum to a string
 * 
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
 * \brief Map an Access Category name to the IP TOS byte.
 *   UP 6,7 -> AC_VO     
 *   UP 4,5 -> AC_VI
 *   UP 0,3 -> AC_BE     
 *   UP 1,2 -> AC_BK
 */
uint8_t AcStringToTos(const std::string& ac)
{
    if (ac == "AC_VO") return 0xC0; // UP 6
    if (ac == "AC_VI") return 0xA0; // UP 5
    if (ac == "AC_BE") return 0x00; // UP 0
    if (ac == "AC_BK") return 0x20; // UP 1
    NS_FATAL_ERROR("Unknown AC string: " << ac);
    return 0x00;
}

int main(int argc, char* argv[])
{
    /* this script is based on NS-3 v3.47 */

    /* global variables */

    Time APP_START_TIME(Seconds(1));
    Time SIMULATION_END_TIME(Seconds(15));

    // enable STR MLO mode
        bool enableStrMlo = false; // Change to false to disable STR MLO by default
        bool enable2G = false; // Change to false to disable 2.4 GHz extra link by default

    // number of STAs around the AP
    uint32_t nStas = 4;

    // per-STA data rates and QoS Access Categories (one entry per STA)
    // AC must be one of: AC_VO, AC_VI, AC_BE, AC_BK
    std::vector<std::string> staDataRates = {"150Mbps", "150Mbps", "150Mbps", "150Mbps"};
    std::vector<std::string> staAcs       = {"AC_BE",    "AC_VO",    "AC_VI",    "AC_VI"};

    // payload size for each IP packet
    // defaults to an MTU of 300 bytes, minus the maximum TCP header size of 60 bytes
    // the maximum UDP header size is 8 bytes, so this will work for either TCP or UDP
    uint32_t payloadSize = 1440;

    // max bytes to send in the application
    // set to 0 for unlimited
    uint32_t bytesMax = 0;

    // socket type to use for the applications
    // it can either be ns3::TcpSocketFactory or ns3::UdpSocketFactory
    std::string appSocketType = "ns3::UdpSocketFactory";

    // wifi rate manager (controls MCS, NSS, GI and channel width)
    std::string rateManager = "ns3::IdealWifiManager";

    // if we care about pcap files
    // although, the files can be large for longer simulations
    bool enablePcaps = false;

    // add some noise levels to the links
    bool enableLossyLinks = false;

    // enable RTS/CTS frame protection
    bool enableRtsCts = false;

    // set the delta position for all devices
    double positionDelta = 3.0;

    // change the run number for different probabilistic outcomes
    uint64_t run = 1;

    // block ack window size (512 or 1024 for 802.11be)
    uint16_t baw = 512;

    // A-MPDU max byte size
    uint32_t ampduMaxBytes = 65535;

    // mac queue max size
    uint64_t macQueueSize = 512;

    // flag for OFDMA scheduler
    bool enableOfdma = false;
    bool enableUlOfdma = false;

    /* cmd parameter parsing */

    CommandLine cmd;
    cmd.AddValue("start", "App start time", APP_START_TIME);
    cmd.AddValue("end", "Simulation end time", SIMULATION_END_TIME);
    cmd.AddValue("enableStrMlo", "Enable STR MLO mode", enableStrMlo);
    cmd.AddValue("enable2G", "Enable 2.4 GHz link", enable2G);
    cmd.AddValue("nStas", "Number of STAs around the AP", nStas);
    cmd.AddValue("payloadSize", "Payload size", payloadSize);
    cmd.AddValue("bytesMax", "Max bytes to send (0=unlimited)", bytesMax);
    cmd.AddValue("appSocketType", "ns3::TcpSocketFactory|ns3::UdpSocketFactory", appSocketType);
    cmd.AddValue("rateManager", "Wi-Fi Rate Manager", rateManager);
    cmd.AddValue("enablePcaps", "Enable wireshark capture", enablePcaps);
    cmd.AddValue("enableLossyLinks", "Enable the use of channel noise", enableLossyLinks);
    cmd.AddValue("enableRtsCts", "Enable the use of RTS/CTS frame protection", enableRtsCts);
    cmd.AddValue("positionDelta", "Select the delta position for all devices", positionDelta);
    cmd.AddValue("run", "Change the run number for different probabilistic outcomes", run);
    cmd.AddValue("baw", "Max buffer size of the block ack window", baw);
    cmd.AddValue("ampduSize", "Max size in bytes of a single A-MPDU frame", ampduMaxBytes);
    cmd.AddValue("macQueueSize", "Max buffer size of the MAC queue", macQueueSize);
    cmd.AddValue("enableOfdma", "Enable the use of the OFDMA scheduler", enableOfdma);
    cmd.AddValue("enableUlOfdma", "Enable the use of UL OFDMA", enableUlOfdma);
    cmd.Parse(argc, argv);

    // pad the per-STA config vectors to nStas, if short
    if (staDataRates.empty()) staDataRates.push_back("30Mbps");
    if (staAcs.empty())       staAcs.push_back("AC_BE");
    while (staDataRates.size() < nStas) staDataRates.push_back(staDataRates.back());
    while (staAcs.size()       < nStas) staAcs.push_back(staAcs.back());

    /* global configs */

    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(run);

    NS_LOG_UNCOND("Current seed : " << RngSeedManager::GetSeed());
    NS_LOG_UNCOND("Current run : " << RngSeedManager::GetRun());

    Config::SetDefault("ns3::TcpL4Protocol::SocketType", 
                        TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));     
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1 * 131072));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1 * 131072));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));

    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));    
    ns3::Packet::EnablePrinting();
    ns3::Packet::EnableChecking(); 

    if(enableRtsCts)
        Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold", UintegerValue(2346));

    //LogComponentEnable("OriginatorCodingAgreement", LOG_ALL); 
    //LogComponentEnable("RecipientCodingAgreement", LOG_ALL); 
    //LogComponentEnable("PacketSink", LOG_ALL);
    //LogComponentEnable("EmlsrManager", LOG_ALL); 
    //LogComponentEnable("MacRxMiddle", LOG_ALL);
    //LogComponentEnable("BlockAckManager", LOG_ALL);
    //LogComponentEnable("RecipientBlockAckAgreement", LOG_ALL);
    //LogComponentEnable("OriginatorBlockAckAgreement", LOG_ALL);
    //LogComponentEnable("BlockAckWindow", LOG_ALL);
    //LogComponentEnable("WifiMacQueue", LOG_ALL);
    //LogComponentEnable("QosTxop", LOG_ALL);
    //LogComponentEnable("Txop", LOG_ALL);
    //LogComponentEnable("WifiDefaultAckManager", LOG_ALL);
    //LogComponentEnable("EhtFrameExchangeManager", LOG_ALL);
    //LogComponentEnable("HeFrameExchangeManager", LOG_ALL);
    //LogComponentEnable("HtFrameExchangeManager", LOG_ALL);
    //LogComponentEnable("QosFrameExchangeManager", LOG_ALL);
    //LogComponentEnable("FrameExchangeManager", LOG_ALL);
    //LogComponentEnable("WifiMpdu", LOG_ALL);
    //LogComponentEnable("SpectrumWifiPhy", LOG_ALL);
    //LogComponentEnable("WifiPhy", LOG_ALL);
    //LogComponentEnable("WifiPhyStateHelper", LOG_ALL);
    //LogComponentEnable("PhyEntity", LOG_ALL);
    //LogComponentEnable("InterferenceHelper", LOG_ALL);
    //LogComponentEnable("MultiUserScheduler", LOG_ALL);
    //LogComponentEnable("RrMultiUserScheduler", LOG_ALL);
    //LogComponentEnable("ApWifiMac", LOG_ALL);
    //LogComponentEnable("StaWifiMac", LOG_ALL);
    //LogComponentEnable("TcpSocketBase", LOG_ALL);
    //LogComponentEnable("TcpCubic", LOG_ALL);
    //LogComponentEnable("TcpL4Protocol", LOG_ALL);
    //LogComponentEnable("Ipv4FlowProbe", LOG_ALL);
    //LogComponentEnable("FlowMonitor", LOG_ALL);

    /* network setup */

    NodeContainer wifiApNodes(1);
    NodeContainer wifiStaNodes(nStas);

    WifiHelper wifiHelper;
    wifiHelper.SetStandard(WIFI_STANDARD_80211be);
    wifiHelper.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifiHelper.ConfigEhtOptions("EmlsrActivated", BooleanValue(false));
    
    uint8_t nLinks = enableStrMlo ? 2 : 1;

    if(enable2G) nLinks++;

    SpectrumWifiPhyHelper wifiPhy(nLinks); 
    
    wifiPhy.Set("Antennas", UintegerValue(2));
    wifiPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    wifiPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    Ptr<MultiModelSpectrumChannel> spectrumChannel6g = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel6g->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    Ptr<MultiModelSpectrumChannel> spectrumChannel5g = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel5g->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    Ptr<MultiModelSpectrumChannel> spectrumChannel2g = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel2g->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    if(enableLossyLinks)
    {
        spectrumChannel6g->AddSpectrumPropagationLossModel(CreateObject<FriisSpectrumPropagationLossModel>());
        spectrumChannel6g->AddPropagationLossModel(CreateObject<NakagamiPropagationLossModel>());
        spectrumChannel5g->AddSpectrumPropagationLossModel(CreateObject<FriisSpectrumPropagationLossModel>());
        spectrumChannel5g->AddPropagationLossModel(CreateObject<NakagamiPropagationLossModel>());
        spectrumChannel2g->AddSpectrumPropagationLossModel(CreateObject<FriisSpectrumPropagationLossModel>());
        spectrumChannel2g->AddPropagationLossModel(CreateObject<NakagamiPropagationLossModel>());
    }
    else
    {
        spectrumChannel6g->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
        spectrumChannel5g->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
        spectrumChannel2g->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    }

    uint8_t nextLinkId = 0;


    wifiPhy.AddChannel(spectrumChannel2g, WIFI_SPECTRUM_2_4_GHZ);
    wifiPhy.Set(nextLinkId, "ChannelSettings", StringValue("{3, 40, BAND_2_4GHZ, 0}"));
    wifiPhy.AddPhyToFreqRangeMapping(nextLinkId, WIFI_SPECTRUM_2_4_GHZ);
    wifiHelper.SetRemoteStationManager(nextLinkId, rateManager);
    nextLinkId++;

    /*
    wifiPhy.AddChannel(spectrumChannel6g, WIFI_SPECTRUM_6_GHZ);
    // the ChannelSettings format is as follows:
    // -- channel number
    // -- channel bandwidth
    // -- constant indicating the frequency range
    // -- the primary 20 MHz channel number inside the given channel 
    //    (or can be set to 0 to pick the first available one)
    wifiPhy.Set(nextLinkId, "ChannelSettings", StringValue("{15, 160, BAND_6GHZ, 0}"));
    wifiPhy.AddPhyToFreqRangeMapping(nextLinkId, WIFI_SPECTRUM_6_GHZ);
    wifiHelper.SetRemoteStationManager(nextLinkId, rateManager);
    nextLinkId++;*/

    if(enableStrMlo)
    {
        wifiPhy.AddChannel(spectrumChannel5g, WIFI_SPECTRUM_5_GHZ);
        wifiPhy.Set(nextLinkId, "ChannelSettings", StringValue("{50, 160, BAND_5GHZ, 0}"));
        wifiPhy.AddPhyToFreqRangeMapping(nextLinkId, WIFI_SPECTRUM_5_GHZ);
        wifiHelper.SetRemoteStationManager(nextLinkId, rateManager);
        nextLinkId++;
    }

    if(enable2G)
    {
        wifiPhy.AddChannel(spectrumChannel2g, WIFI_SPECTRUM_2_4_GHZ);
        wifiPhy.Set(nextLinkId, "ChannelSettings", StringValue("{3, 40, BAND_2_4GHZ, 0}"));
        wifiPhy.AddPhyToFreqRangeMapping(nextLinkId, WIFI_SPECTRUM_2_4_GHZ);
        wifiHelper.SetRemoteStationManager(nextLinkId, rateManager);
        nextLinkId++;
    }

    Ssid ssid = Ssid("wifi7-test");

    WifiMacHelper wifiApMac;
    wifiApMac.SetType("ns3::ApWifiMac", 
                      "Ssid", SsidValue(ssid),
                      "BeaconGeneration", BooleanValue(true),             
                      "BeaconInterval", TimeValue(MicroSeconds(102400)),  // 100 TU (one TU = 1024 us)
                      "QosSupported", BooleanValue(true),                 // enable EDCA
                      "BE_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                      "BK_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                      "VI_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                      "VO_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                      "MpduBufferSize", UintegerValue(baw));   

    WifiMacHelper wifiStaMac;
    wifiStaMac.SetType("ns3::StaWifiMac", 
                       "Ssid", SsidValue(ssid),
                       "ActiveProbing", BooleanValue(false),
                       "QosSupported", BooleanValue(true),                // enable EDCA
                       "BE_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                       "BK_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                       "VI_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                       "VO_MaxAmpduSize", UintegerValue(ampduMaxBytes),
                       "MpduBufferSize", UintegerValue(baw));   

    if(enableOfdma)
    {
        wifiApMac.SetMultiUserScheduler("ns3::RrMultiUserScheduler",
                                    "EnableUlOfdma", BooleanValue(enableUlOfdma),
                                    "EnableBsrp", BooleanValue(true));

        Config::SetDefault("ns3::WifiDefaultAckManager::DlMuAckSequenceType",
                   EnumValue(WifiAcknowledgment::DL_MU_BAR_BA_SEQUENCE));
    }

    NetDeviceContainer wifiApDevices = wifiHelper.Install(wifiPhy, wifiApMac, wifiApNodes);
    NetDeviceContainer wifiStaDevices = wifiHelper.Install(wifiPhy, wifiStaMac, wifiStaNodes);

    // mobility also needs to be added, even if all devices are stationary
    // this is so the simulator knows about their positions
    // AP is placed at the origin using the original grid allocator;
    // STAs are placed on a fixed circle around it.
    MobilityHelper mobility;
    mobility.SetPositionAllocator("ns3::GridPositionAllocator",
                                  "MinX", DoubleValue(0.0),
                                  "MinY", DoubleValue(0.0),
                                  "DeltaX", DoubleValue(positionDelta),
                                  "DeltaY", DoubleValue(positionDelta),
                                  "GridWidth", UintegerValue(100),
                                  "LayoutType", StringValue("RowFirst"));
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(wifiApNodes);
 
    // STAs: positions are computed on a circle around the AP (which is at 0,0,0)
    Ptr<ListPositionAllocator> staAlloc = CreateObject<ListPositionAllocator>();
    for (uint32_t s = 0; s < nStas; ++s)
    {
        double theta = 2.0 * M_PI * s / static_cast<double>(nStas);
        double x = positionDelta * std::cos(theta);
        double y = positionDelta * std::sin(theta);
        staAlloc->Add(Vector(x, y, 0.0));
    }
 
    MobilityHelper staMobility;
    staMobility.SetPositionAllocator(staAlloc);
    staMobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    staMobility.Install(wifiStaNodes);

    InternetStackHelper internetStack;
    internetStack.Install(wifiApNodes);
    internetStack.Install(wifiStaNodes);

    Ipv4AddressHelper address;
    address.SetBase("192.168.1.0", "255.255.255.0");

    Ipv4InterfaceContainer wifiApInterfaces = address.Assign(wifiApDevices);
    Ipv4InterfaceContainer wifiStaInterfaces = address.Assign(wifiStaDevices);

    // one sink + one sender per STA, each with its own AC (QoS)
    std::vector<Ptr<PacketSink>>       sinksSta(nStas);
    std::vector<Ptr<OnOffApplication>> clientsAp(nStas);

    for (uint32_t s = 0; s < nStas; ++s)
    {
        uint16_t portSta = 5001 + s;
        Address sinkAddressSta(InetSocketAddress(wifiStaInterfaces.GetAddress(s), portSta));
        PacketSinkHelper sinkHelperSta(appSocketType, sinkAddressSta);

        ApplicationContainer sinkAppsSta = sinkHelperSta.Install(wifiStaNodes.Get(s));
        sinksSta[s] = DynamicCast<PacketSink>(sinkAppsSta.Get(0));

        OnOffHelper clientAp(appSocketType, sinkAddressSta);
        clientAp.SetAttribute("PacketSize", UintegerValue(payloadSize));
        clientAp.SetAttribute("MaxBytes", UintegerValue(bytesMax));
        clientAp.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
        clientAp.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        clientAp.SetAttribute("DataRate", DataRateValue(DataRate(staDataRates[s])));
        clientAp.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));
        // QoS: set IP TOS so ns-3 maps it to the right EDCA Access Category
        clientAp.SetAttribute("Tos", UintegerValue(AcStringToTos(staAcs[s])));

        ApplicationContainer clientAppsAp = clientAp.Install(wifiApNodes.Get(0));
        clientsAp[s] = DynamicCast<OnOffApplication>(clientAppsAp.Get(0));

        sinkAppsSta.Start(APP_START_TIME);
        sinkAppsSta.Stop(SIMULATION_END_TIME - Seconds(1));
        clientAppsAp.Start(APP_START_TIME + Seconds(1));
        clientAppsAp.Stop(SIMULATION_END_TIME - Seconds(2));

        NS_LOG_UNCOND("Flow " << s << " : AP -> STA" << s
            << " | rate=" << staDataRates[s]
            << " | AC="   << staAcs[s]
            << " | port=" << portSta);
    }

    if(enablePcaps)
    {
        wifiPhy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        wifiPhy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_DEVICE);
        wifiPhy.EnablePcap("ap-wifi", wifiApDevices.Get(0));
        for (uint32_t s = 0; s < nStas; ++s)
        {
            std::ostringstream name;
            name << "sta" << s << "-wifi";
            wifiPhy.EnablePcap(name.str(), wifiStaDevices.Get(s));
            internetStack.EnablePcapIpv4("sta" + std::to_string(s) + "-ip",
                                         wifiStaNodes.Get(s)->GetId(), 1, false);
        }
        internetStack.EnablePcapIpv4("ap-ip", wifiApNodes.Get(0)->GetId(), 1, false);
    }

    FlowMonitorHelper flowMonitorHelper;
    Ptr<FlowMonitor> flowMonitor = flowMonitorHelper.InstallAll();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowMonitorHelper.GetClassifier());

    WifiCoTraceHelper coAp{APP_START_TIME, SIMULATION_END_TIME};
    coAp.Enable(wifiApDevices);

    WifiCoTraceHelper coSta{APP_START_TIME, SIMULATION_END_TIME};
    coSta.Enable(wifiStaDevices);

    // add custom monitors after initial config is done
    for (uint32_t s = 0; s < nStas; ++s)
    {
        Ptr<WifiNetDevice> staDevice = DynamicCast<WifiNetDevice>(wifiStaNodes.Get(s)->GetDevice(0));

        //uint64_t streamBase = 1;
        if(staDevice)
        {
            Ptr<WifiMac> mac = staDevice->GetMac();
            Ptr<QosTxop> txopBE = mac->GetQosTxop(AC_BE);
            Ptr<QosTxop> txopBK = mac->GetQosTxop(AC_BK);
            Ptr<QosTxop> txopVI = mac->GetQosTxop(AC_VI);
            Ptr<QosTxop> txopVO = mac->GetQosTxop(AC_VO);

            txopBE->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
            txopBK->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
            txopVI->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
            txopVO->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
        }
    }

    Ptr<WifiNetDevice> apDevice = DynamicCast<WifiNetDevice>(wifiApNodes.Get(0)->GetDevice(0));

    if(apDevice)
    {
        Ptr<WifiMac> mac = apDevice->GetMac();
        Ptr<QosTxop> txopBE = mac->GetQosTxop(AC_BE);
        Ptr<QosTxop> txopBK = mac->GetQosTxop(AC_BK);
        Ptr<QosTxop> txopVI = mac->GetQosTxop(AC_VI);
        Ptr<QosTxop> txopVO = mac->GetQosTxop(AC_VO);

        txopBE->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
        txopBK->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
        txopVI->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
        txopVO->GetWifiMacQueue()->SetMaxSize(QueueSize(QueueSizeUnit::PACKETS, macQueueSize));
    }

    NS_LOG_UNCOND("----------------------------");
    PrintNodes(wifiApNodes);
    PrintNodes(wifiStaNodes);

    NS_LOG_UNCOND("Simulator Start");

    Simulator::Stop(SIMULATION_END_TIME + Seconds(1));
    Simulator::Run();
    Simulator::Destroy();

    NS_LOG_UNCOND("Simulator Done");

    // the flow monitor is used as a sanity check
    // to make sure the custom monitors have similar results
    // note that the flow monitor operates at IP level
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
            packetLossRatio = 
            (static_cast<double>(flowStats.lostPackets) / static_cast<double>(flowStats.txPackets)) * 100.0;
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
        NS_LOG_UNCOND("Packet Loss Percentage: " << packetLossRatio << " %");
        NS_LOG_UNCOND("Packet Drops by Reason:");
        for (size_t i = 0; i < numDropReasons; ++i)
        {
            ns3::Ipv4FlowProbe::DropReason reason = static_cast<ns3::Ipv4FlowProbe::DropReason>(i);
            uint32_t count = (i < flowStats.packetsDropped.size()) ? flowStats.packetsDropped[i] : 0;
            NS_LOG_UNCOND("  " << DropReasonToString(reason) << ": " << count << " packets");
        }
        NS_LOG_UNCOND("----------------------------");
    }

    coAp.PrintStatistics(std::cout);
    coSta.PrintStatistics(std::cout);

    NS_LOG_UNCOND("----------------------------");

    return 0;
}
