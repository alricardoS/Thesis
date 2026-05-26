/*
 * wifi7-single-link-multi-sta.cc
 * 802.11be Single Link Test with Multiple STAs (2 STAs)
 * 
 * Layout (AP no centro, STAs nos lados):
 *    STA0 (esq) --AP-- STA1 (dir)
 * 
 * Usa WifiStaticSetupHelper para evitar colisões durante associação
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
#include "ns3/wifi-static-setup-helper.h"
#include "ns3/neighbor-cache-helper.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/wifi-phy.h"
#include "ns3/wifi-mac-header.h"
#include "ns3/qos-txop.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("wifi7-single-link-multi-sta");

// ========== GRANULAR PACKET LOSS COUNTERS ==========
// PHY Layer Drops
uint64_t g_phyTxDrop = 0;   // Packets dropped during TX at PHY (e.g., interference during TX)
uint64_t g_phyRxDrop = 0;   // Packets dropped during RX at PHY (e.g., low SNR, collision)
std::map<std::string, uint64_t> g_phyRxDropReasons; // PHY RX drops by reason
std::map<std::string, uint64_t> g_phyRxDropMacTypeByReason; // PHY RX drops by (reason, MAC frame type)
std::map<uint32_t, uint64_t> g_phyRxDropPacketSizes; // PHY RX drops by packet size
std::map<uint32_t, uint64_t> g_phyRxDropBySecond; // PHY RX drops timeline (second -> count)
uint32_t g_phyRxDropSamplesPrinted = 0;
const uint32_t g_phyRxDropSampleLimit = 30;

// MAC Layer Drops  
uint64_t g_macTxDrop = 0;   // Packets dropped at MAC TX (e.g., exceeded retry limit)
uint64_t g_macRxDrop = 0;   // Packets dropped at MAC RX (e.g., CRC error after PHY decode)

// WiFi Queue Drops
uint64_t g_wifiQueueDrop = 0;   // Packets dropped due to full WiFi MAC queue

// IP/Traffic Control Layer Drops
uint64_t g_tcDropBeforeEnqueue = 0;  // Packets dropped before entering TC queue (queue full)
uint64_t g_tcDropAfterDequeue = 0;   // Packets dropped after dequeue (policy decision)
uint64_t g_tcDrop = 0;               // TC layer drops (no queue disc installed)
std::map<std::string, uint64_t> g_tcDropBeforeReasons; // TC drops by reason (before enqueue)
std::map<std::string, uint64_t> g_tcDropAfterReasons;  // TC drops by reason (after dequeue)

// PHY Drop Callbacks
std::string
GetMacTypeFromPacket(Ptr<const Packet> packet)
{
    if (!packet)
    {
        return "NULL_PACKET";
    }

    WifiMacHeader hdr;
    Ptr<Packet> copy = packet->Copy();
    if (copy->PeekHeader(hdr) > 0)
    {
        return std::string(hdr.GetTypeString());
    }

    return "UNPARSED";
}

void PhyTxDropCallback(Ptr<const Packet> packet)
{
    g_phyTxDrop++;
}

void PhyRxDropCallback(Ptr<const Packet> packet, WifiPhyRxfailureReason reason)
{
    g_phyRxDrop++;
    std::ostringstream oss;
    oss << reason;
    std::string reasonStr = oss.str();
    g_phyRxDropReasons[reasonStr]++;

    const std::string macType = GetMacTypeFromPacket(packet);
    g_phyRxDropMacTypeByReason[reasonStr + "|" + macType]++;

    uint32_t size = packet ? packet->GetSize() : 0;
    g_phyRxDropPacketSizes[size]++;

    uint32_t sec = static_cast<uint32_t>(Simulator::Now().GetSeconds());
    g_phyRxDropBySecond[sec]++;

    if (g_phyRxDropSamplesPrinted < g_phyRxDropSampleLimit)
    {
        NS_LOG_UNCOND("PHY_RX_DROP_SAMPLE: Time_s=" << Simulator::Now().GetSeconds()
                                                    << " Reason=" << reasonStr << " MacType=" << macType
                                                    << " PacketSize=" << size
                                                    << " Uid=" << (packet ? packet->GetUid() : 0));
        g_phyRxDropSamplesPrinted++;
    }
}

// MAC Drop Callbacks
void MacTxDropCallback(Ptr<const Packet> packet)
{
    g_macTxDrop++;
}

void MacRxDropCallback(Ptr<const Packet> packet)
{
    g_macRxDrop++;
}

// WiFi Queue Drop Callback
void WifiQueueDropCallback(Ptr<const WifiMpdu> mpdu)
{
    g_wifiQueueDrop++;
}

// Traffic Control Drop Callbacks
void TcDropBeforeEnqueueCallback(Ptr<const QueueDiscItem> item, const char* reason)
{
    g_tcDropBeforeEnqueue++;
    g_tcDropBeforeReasons[(reason != nullptr) ? std::string(reason) : std::string("UNKNOWN")]++;
}

void TcDropAfterDequeueCallback(Ptr<const QueueDiscItem> item, const char* reason)
{
    g_tcDropAfterDequeue++;
    g_tcDropAfterReasons[(reason != nullptr) ? std::string(reason) : std::string("UNKNOWN")]++;
}

void TcDropCallback(Ptr<const Packet> packet)
{
    g_tcDrop++;
}

// Número de STAs (configurável via linha de comando)
uint32_t g_numStas = 2;

// Distância dos STAs ao AP (em metros)
const double STA_DISTANCE = 5.0;

/* global variable for easier access (cumulative delay/jitter) - per STA */
std::vector<double> g_delaySumMs;
std::vector<uint64_t> g_delaySamples;
std::vector<double> g_jitterSumMs;
std::vector<uint64_t> g_jitterSamples;
std::vector<double> g_lastDelayMs;
std::vector<bool> g_firstPacket;

// Per-STA throughput tracking
std::vector<uint64_t> g_lastTotalRx;

/**
 * PrintNodes - mostra configuração de dispositivos/PHY
 */
void PrintNodes(NodeContainer nodes, const std::string type) {
    for(uint32_t nodeIdx = 0; nodeIdx < nodes.GetN(); ++nodeIdx) {
        Ptr<Node> node = nodes.Get(nodeIdx);
        NS_LOG_UNCOND(type << " Node " << nodeIdx);
        uint32_t numDevices = node->GetNDevices(); 
        Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();  
        for(uint32_t i = 0; i < numDevices; ++i) {
            Ptr<NetDevice> netDevice = node->GetDevice(i);
            Ptr<WifiNetDevice> wifiDevice = DynamicCast<WifiNetDevice>(netDevice);
            if(wifiDevice) {
                Mac48Address upperMacAddress = wifiDevice->GetMac()->GetAddress();
                NS_LOG_UNCOND("  Standard: " << wifiDevice->GetStandard());    
                Ipv4Address ipv4Address = Ipv4Address::GetAny();
                int32_t interfaceIndex = ipv4->GetInterfaceForDevice(netDevice);
                if(interfaceIndex != -1) {
                    ipv4Address = ipv4->GetAddress(interfaceIndex, 0).GetLocal();
                }
                NS_LOG_UNCOND("    Interface " << i << ":");
                NS_LOG_UNCOND("      IPv4 Address = " << ipv4Address);
                NS_LOG_UNCOND("      Upper MAC Address = " << upperMacAddress);
                NS_LOG_UNCOND("      Number of PHYs = " << wifiDevice->GetNPhys());
            }
        }
        NS_LOG_UNCOND("----------------------------");
    }
}

// Callback para monitorizar recepção de pacotes no sink (usa SeqTsSizeHeader)
// staIdx é o índice da STA (0-3)
void MonitorPacketSinkRx(uint32_t staIdx, Ptr<const Packet> packet, const Address &address)
{
    SeqTsSizeHeader header;
    packet->PeekHeader(header);
    Time txTime = header.GetTs();
    Time delay = Simulator::Now() - txTime;

    double delayMs = delay.GetSeconds() * 1000.0;
    g_delaySumMs[staIdx] += delayMs;
    g_delaySamples[staIdx]++;

    if (!g_firstPacket[staIdx]) {
        double jitter = std::fabs(delayMs - g_lastDelayMs[staIdx]);
        g_jitterSumMs[staIdx] += jitter;
        g_jitterSamples[staIdx]++;
    } else {
        g_firstPacket[staIdx] = false;
    }
    g_lastDelayMs[staIdx] = delayMs;
}

// Impressão periódica de estatísticas (por segundo) - agregada para todas as STAs
void CalculateStats(std::vector<Ptr<PacketSink>> sinks)
{
    uint64_t currentTime = Simulator::Now().GetSeconds();
    double totalThroughput = 0.0;
    double totalDelay = 0.0;
    double totalJitter = 0.0;
    uint32_t validStas = 0;

    for (uint32_t i = 0; i < g_numStas; ++i) {
        uint64_t currentTotalRx = sinks[i]->GetTotalRx();
        double throughput = ((currentTotalRx - g_lastTotalRx[i]) * 8.0) / (1.0 * 1e6);
        g_lastTotalRx[i] = currentTotalRx;

        if (throughput > 0) {
            totalThroughput += throughput;
            validStas++;

            double avgDelay = (g_delaySamples[i] > 0) ? (g_delaySumMs[i] / g_delaySamples[i]) : 0.0;
            double avgJitter = (g_jitterSamples[i] > 0) ? (g_jitterSumMs[i] / g_jitterSamples[i]) : 0.0;

            totalDelay += avgDelay;
            totalJitter += avgJitter;
        }
    }

    double avgDelay = (validStas > 0) ? totalDelay / validStas : 0.0;
    double avgJitter = (validStas > 0) ? totalJitter / validStas : 0.0;

    NS_LOG_UNCOND("TIME_STATS: Time=" << currentTime << "s TotalThroughput=" << static_cast<int>(totalThroughput)
                  << "Mbps AvgDelay=" << avgDelay << "ms AvgJitter=" << avgJitter << "ms ActiveSTAs=" << validStas);

    Simulator::Schedule(Seconds(1), &CalculateStats, sinks);
}

uint8_t
PriorityClassToTos(uint32_t priorityClass)
{
    switch (priorityClass)
    {
    case 0:
        return 0x20; // CS1 -> BK
    case 1:
        return 0x00; // BE
    case 2:
        return 0x88; // AF41 -> VI
    case 3:
        return 0xB8; // EF -> VO
    default:
        return 0x00;
    }
}

std::string
PriorityClassToAcName(uint32_t priorityClass)
{
    switch (priorityClass)
    {
    case 0:
        return "BK";
    case 1:
        return "BE";
    case 2:
        return "VI";
    case 3:
        return "VO";
    default:
        return "BE";
    }
}

std::vector<uint32_t>
ParseStaPriorities(const std::string& csv, uint32_t numStas)
{
    std::vector<uint32_t> priorities(numStas, 1);
    if (csv.empty() || numStas == 0)
    {
        return priorities;
    }

    std::stringstream ss(csv);
    std::string token;
    uint32_t idx = 0;
    while (std::getline(ss, token, ',') && idx < numStas)
    {
        auto begin = token.find_first_not_of(" \t\n\r");
        auto end = token.find_last_not_of(" \t\n\r");
        if (begin == std::string::npos)
        {
            continue;
        }
        token = token.substr(begin, end - begin + 1);

        try
        {
            int parsed = std::stoi(token);
            if (parsed < 0)
            {
                parsed = 0;
            }
            if (parsed > 3)
            {
                parsed = 3;
            }
            priorities[idx] = static_cast<uint32_t>(parsed);
        }
        catch (...)
        {
            priorities[idx] = 1;
        }
        idx++;
    }

    uint32_t fillValue = (idx > 0) ? priorities[idx - 1] : 1;
    for (; idx < numStas; ++idx)
    {
        priorities[idx] = fillValue;
    }

    return priorities;
}

int main(int argc, char* argv[])
{
    int freq = 2; // 2, 5, or 6
    std::string dataRateStr = "30Mbps"; // Per-STA data rate (reduzido para evitar saturação)
    double simTime = 12.0;
    std::string protocol = "UDP";
    double staDistance = STA_DISTANCE;
    bool useStaticSetup = true; // Usar WifiStaticSetupHelper

    uint32_t numStas = 4; // Number of STAs (configurable)

    CommandLine cmd;
    cmd.AddValue("freq", "Frequency (2, 5, 6)", freq);
    cmd.AddValue("dataRate", "Per-STA data rate", dataRateStr);
    cmd.AddValue("protocol", "TCP or UDP", protocol);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.AddValue("distance", "Distance from STAs to AP (m)", staDistance);
    cmd.AddValue("staticSetup", "Use WifiStaticSetupHelper (avoids association collisions)", useStaticSetup);
    cmd.AddValue("numStas", "Number of STAs (2, 4, 8, 16, etc.)", numStas);
    cmd.Parse(argc, argv);

    // Set global numStas and initialize vectors
    g_numStas = numStas;
    g_delaySumMs.resize(g_numStas, 0.0);
    g_delaySamples.resize(g_numStas, 0);
    g_jitterSumMs.resize(g_numStas, 0.0);
    g_jitterSamples.resize(g_numStas, 0);
    g_lastDelayMs.resize(g_numStas, 0.0);
    g_firstPacket.resize(g_numStas, true);
    g_lastTotalRx.resize(g_numStas, 0);

    // Timing
    Time APP_START_TIME(Seconds(1));
    Time SIMULATION_END_TIME(Seconds(simTime));

    DataRate dataRate(dataRateStr);
    uint32_t payloadSize = 1440;
    uint32_t maxBytes = 0; // unlimited

    std::string appSocketType = (protocol == "TCP") ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";

    // Global defaults
    if (protocol == "TCP") {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));
        Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(payloadSize));
    }
    GlobalValue::Bind("ChecksumEnabled", BooleanValue(true));
    ns3::Packet::EnablePrinting();
    ns3::Packet::EnableChecking();

    NS_LOG_UNCOND("wifi7-single-link-multi-sta: freq=" << freq << " proto=" << protocol
                  << " rate=" << dataRateStr << " numSTAs=" << g_numStas << " distance=" << staDistance << "m"
                  << " staticSetup=" << (useStaticSetup ? "yes" : "no"));

    // Configuração de Canal
    int channel = 15; int width = 160; 
    FrequencyRange freqRange = WIFI_SPECTRUM_6_GHZ;
    std::string band = "BAND_6GHZ";

    if (freq == 2) { 
        channel = 6; width = 40; band = "BAND_2_4GHZ"; freqRange = WIFI_SPECTRUM_2_4_GHZ;
    } else if (freq == 5) {
        channel = 42; width = 80; band = "BAND_5GHZ"; freqRange = WIFI_SPECTRUM_5_GHZ;
    }

    // Criar nós: 1 AP + N STAs
    NodeContainer apNode;
    apNode.Create(1);
    NodeContainer staNodes;
    staNodes.Create(g_numStas);

    // WiFi Helper
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifi.ConfigEhtOptions("EmlsrActivated", BooleanValue(false));

    // PHY Helper (1 link)
    SpectrumWifiPhyHelper phy(1);
    phy.Set("Antennas", UintegerValue(2));
    phy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    phy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    // Canal único
    Ptr<MultiModelSpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel>();
    spectrumChannel->AddPropagationLossModel(CreateObject<LogDistancePropagationLossModel>());
    spectrumChannel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    
    phy.AddChannel(spectrumChannel, freqRange);
    std::string chanSettings = "{" + std::to_string(channel) + ", " + std::to_string(width) + ", " + band + ", 0}";
    phy.Set(0, "ChannelSettings", StringValue(chanSettings));
    phy.AddPhyToFreqRangeMapping(0, freqRange);

    // Rate manager
    wifi.SetRemoteStationManager(0, std::string("ns3::IdealWifiManager"));
    //wifi.SetRemoteStationManager(0, std::string("ns3::MinstrelHtWifiManager"));

    // Configure MACs
    Ssid ssid = Ssid("wifi7-multi-sta");
    
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

    // Instalar devices
    NetDeviceContainer apDev = wifi.Install(phy, apMac, apNode);
    NetDeviceContainer staDev = wifi.Install(phy, staMac, staNodes);

    // ===== CONECTAR TRACE SOURCES PARA PACKET LOSS GRANULAR =====
    // AP device traces
    Ptr<WifiNetDevice> apWifiNetDev = DynamicCast<WifiNetDevice>(apDev.Get(0));
    
    // PHY traces (AP)
    for (uint8_t linkId = 0; linkId < apWifiNetDev->GetNPhys(); ++linkId) {
        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyTxDrop", MakeCallback(&PhyTxDropCallback));
        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyRxDrop", MakeCallback(&PhyRxDropCallback));
    }
    
    // MAC traces (AP) - Queue drops
    Ptr<WifiMac> apMacPtr = apWifiNetDev->GetMac();
    // Get all AC queues and connect drop traces
    for (auto ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        Ptr<QosTxop> qosTxop = apMacPtr->GetQosTxop(ac);
        if (qosTxop) {
            Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
            if (queue) {
                queue->TraceConnectWithoutContext("DropBeforeEnqueue", MakeCallback(&WifiQueueDropCallback));
                queue->TraceConnectWithoutContext("Expired", MakeCallback(&WifiQueueDropCallback));
            }
        }
    }
    
    // STA device traces
    for (uint32_t i = 0; i < staDev.GetN(); ++i) {
        Ptr<WifiNetDevice> staWifiNetDev = DynamicCast<WifiNetDevice>(staDev.Get(i));
        
        // PHY traces (STA)
        for (uint8_t linkId = 0; linkId < staWifiNetDev->GetNPhys(); ++linkId) {
            staWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyTxDrop", MakeCallback(&PhyTxDropCallback));
            staWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyRxDrop", MakeCallback(&PhyRxDropCallback));
        }
        
        // MAC traces (STA) - Queue drops
        Ptr<WifiMac> staMacPtr = staWifiNetDev->GetMac();
        for (auto ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
            Ptr<QosTxop> qosTxop = staMacPtr->GetQosTxop(ac);
            if (qosTxop) {
                Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
                if (queue) {
                    queue->TraceConnectWithoutContext("DropBeforeEnqueue", MakeCallback(&WifiQueueDropCallback));
                    queue->TraceConnectWithoutContext("Expired", MakeCallback(&WifiQueueDropCallback));
                }
            }
        }
    }
    NS_LOG_UNCOND("Granular packet loss tracing enabled (PHY/MAC/Queue drops)");

    // ===== CONFIGURAÇÃO ESTÁTICA (WifiStaticSetupHelper) =====
    // Evita colisões durante a associação quando múltiplas STAs tentam associar-se simultaneamente
    if (useStaticSetup) {
        NS_LOG_UNCOND("Using WifiStaticSetupHelper for static association...");
        
        // Configurar associação estática (salta scanning e troca de frames de gestão)
        Ptr<WifiNetDevice> apWifiDev = DynamicCast<WifiNetDevice>(apDev.Get(0));
        WifiStaticSetupHelper::SetStaticAssociation(apWifiDev, staDev);
        
        // Configurar Block ACK agreements estaticamente para TID 0 (Best Effort)
        // Isto evita a troca de ADDBA Request/Response
        WifiStaticSetupHelper::SetStaticBlockAck(apWifiDev, staDev, {0});
        
        NS_LOG_UNCOND("Static association and Block ACK configured for " << g_numStas << " STAs");
    }

    // ===== MOBILIDADE (Layout linear) =====
    // AP no centro (0,0,0)
    // STAs distribuídas em círculo em volta do AP
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    
    // Posicionar AP no centro
    mobility.Install(apNode);
    apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
    NS_LOG_UNCOND("AP position: (0, 0, 0)");
    
    // Posicionar STAs em círculo em volta do AP
    mobility.Install(staNodes);
    NS_LOG_UNCOND("STAs positioned in circle at distance " << staDistance << "m from AP:");
    for (uint32_t i = 0; i < g_numStas; ++i) {
        double angle = 2.0 * M_PI * i / g_numStas;
        double x = staDistance * std::cos(angle);
        double y = staDistance * std::sin(angle);
        staNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(Vector(x, y, 0.0));
        NS_LOG_UNCOND("  STA" << i << ": (" << x << ", " << y << ", 0)");
    }

    // ===== INTERNET STACK =====
    InternetStackHelper stack;
    stack.Install(apNode);
    stack.Install(staNodes);
    
    Ipv4AddressHelper address;
    address.SetBase("192.168.1.0", "255.255.255.0");
    
    // Combinar todos os devices para atribuir IPs
    NetDeviceContainer allDevices;
    allDevices.Add(apDev);
    allDevices.Add(staDev);
    Ipv4InterfaceContainer ifaces = address.Assign(allDevices);
    // ifaces[0] = AP, ifaces[1..4] = STAs

    // ===== TRAFFIC CONTROL LAYER DROP TRACES =====
    // Conectar aos traces de drop do Traffic Control Layer no AP
    Ptr<TrafficControlLayer> tcAp = apNode.Get(0)->GetObject<TrafficControlLayer>();
    if (tcAp) {
        tcAp->TraceConnectWithoutContext("TcDrop", MakeCallback(&TcDropCallback));
        
        // Get root queue disc for AP WiFi device and connect drop traces
        Ptr<QueueDisc> rootQdAp = tcAp->GetRootQueueDiscOnDevice(apDev.Get(0));
        if (rootQdAp) {
            rootQdAp->TraceConnectWithoutContext("DropBeforeEnqueue", MakeCallback(&TcDropBeforeEnqueueCallback));
            rootQdAp->TraceConnectWithoutContext("DropAfterDequeue", MakeCallback(&TcDropAfterDequeueCallback));
        }
    }
    
    // Conectar aos traces de drop nas STAs também
    for (uint32_t i = 0; i < g_numStas; ++i) {
        Ptr<TrafficControlLayer> tcSta = staNodes.Get(i)->GetObject<TrafficControlLayer>();
        if (tcSta) {
            tcSta->TraceConnectWithoutContext("TcDrop", MakeCallback(&TcDropCallback));
            
            Ptr<QueueDisc> rootQdSta = tcSta->GetRootQueueDiscOnDevice(staDev.Get(i));
            if (rootQdSta) {
                rootQdSta->TraceConnectWithoutContext("DropBeforeEnqueue", MakeCallback(&TcDropBeforeEnqueueCallback));
                rootQdSta->TraceConnectWithoutContext("DropAfterDequeue", MakeCallback(&TcDropAfterDequeueCallback));
            }
        }
    }
    NS_LOG_UNCOND("Traffic Control layer drop tracing enabled");

    // ===== CONFIGURAÇÃO ARP ESTÁTICA (NeighborCacheHelper) =====
    // Evita troca de ARP Request/Response
    if (useStaticSetup) {
        NeighborCacheHelper neighborCache;
        neighborCache.PopulateNeighborCache();
        NS_LOG_UNCOND("Static ARP cache populated");
    }

    // ===== APLICAÇÕES =====
    // Tráfego downlink: AP envia para cada STA
    uint16_t basePort = 9;
    std::vector<Ptr<PacketSink>> sinks(g_numStas);
    
    for (uint32_t i = 0; i < g_numStas; ++i) {
        uint16_t port = basePort + i;
        
        // Sink na STA
        Address sinkAddr(InetSocketAddress(ifaces.GetAddress(1 + i), port));
        PacketSinkHelper sinkHelper(appSocketType, sinkAddr);
        ApplicationContainer sinkApp = sinkHelper.Install(staNodes.Get(i));
        sinkApp.Start(APP_START_TIME);
        sinkApp.Stop(SIMULATION_END_TIME - Seconds(1));
        sinks[i] = DynamicCast<PacketSink>(sinkApp.Get(0));
        
        // Conectar callback de RX (captura staIdx)
        uint32_t staIdx = i;
        sinks[i]->TraceConnectWithoutContext("Rx", 
            MakeBoundCallback(&MonitorPacketSinkRx, staIdx));
        
        // OnOff sender no AP
        OnOffHelper client(appSocketType, sinkAddr);
        client.SetAttribute("PacketSize", UintegerValue(payloadSize));
        client.SetAttribute("MaxBytes", UintegerValue(maxBytes));
        client.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        client.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        client.SetAttribute("DataRate", DataRateValue(dataRate));
        client.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));

        ApplicationContainer clientApp = client.Install(apNode.Get(0));
        // Escalonar início ligeiramente diferente para cada STA para evitar burst inicial
        clientApp.Start(APP_START_TIME + Seconds(1) + MilliSeconds(i * 10));
        clientApp.Stop(SIMULATION_END_TIME - Seconds(2));
        
        NS_LOG_UNCOND("App configured: AP -> STA" << i << " (port " << port << ")"
                  << " AC=BE"
                  << " TOS=" << static_cast<uint32_t>(PriorityClassToTos(1)));
    }

    // Flow monitor
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    // Estatísticas periódicas
    Simulator::Schedule(Seconds(2), &CalculateStats, sinks);

    // Print node info
    PrintNodes(apNode, "AP");
    PrintNodes(staNodes, "STA");

    Simulator::Stop(SIMULATION_END_TIME + Seconds(1));
    Simulator::Run();

    // ===== RESULTADOS FINAIS =====
    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmon.GetClassifier());
    auto stats = monitor->GetFlowStats();
    
    double totalThroughput = 0.0;
    uint64_t totalRxBytes = 0;
    uint64_t totalTxPackets = 0;
    uint64_t totalRxPackets = 0;
    
    Ipv4Address apAddr = ifaces.GetAddress(0);
    
    double totalDelay = 0.0;
    double totalJitter = 0.0;
    uint64_t totalLostPackets = 0;
    uint32_t validStas = 0;
    
    for (const auto &kv : stats) {
        FlowId id = kv.first;
        const FlowMonitor::FlowStats &st = kv.second;
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(id);
        
        // Apenas fluxos downlink do AP
        if (t.sourceAddress == apAddr) {
            double duration = st.timeLastRxPacket.GetSeconds() - st.timeFirstTxPacket.GetSeconds();
            double tp = (duration > 0) ? (st.rxBytes * 8.0) / (duration * 1e6) : 0.0;
            
            // Calcular delay e jitter médios do fluxo
            double delayMs = (st.rxPackets > 0) ? (st.delaySum.GetSeconds() * 1000.0 / st.rxPackets) : 0.0;
            double jitterMs = (st.rxPackets > 1) ? (st.jitterSum.GetSeconds() * 1000.0 / (st.rxPackets - 1)) : 0.0;
            uint64_t lostPackets = st.txPackets - st.rxPackets;
            double lossRate = (st.txPackets > 0) ? (lostPackets * 100.0 / st.txPackets) : 0.0;
            
            // Identificar qual STA é o destino
            for (uint32_t i = 0; i < g_numStas; ++i) {
                if (t.destinationAddress == ifaces.GetAddress(1 + i)) {
                    NS_LOG_UNCOND("FLOW_SUMMARY_STA" << i << ": freq=" << freq << " proto=" << protocol 
                                  << " Throughput_Mbps=" << tp 
                                  << " Delay_ms=" << delayMs 
                                  << " Jitter_ms=" << jitterMs
                                  << " LostPackets=" << lostPackets
                                  << " LossRate_pct=" << lossRate
                                  << " TX_Packets=" << st.txPackets << " RX_Packets=" << st.rxPackets);
                    break;
                }
            }
            
            totalThroughput += tp;
            totalDelay += delayMs;
            totalJitter += jitterMs;
            totalRxBytes += st.rxBytes;
            totalTxPackets += st.txPackets;
            totalRxPackets += st.rxPackets;
            totalLostPackets += lostPackets;
            validStas++;
        }
    }
    
    double avgDelay = (validStas > 0) ? totalDelay / validStas : 0.0;
    double avgJitter = (validStas > 0) ? totalJitter / validStas : 0.0;
    double avgThroughputPerSta = (validStas > 0) ? totalThroughput / validStas : 0.0;
    double totalLossRate = (totalTxPackets > 0) ? (totalLostPackets * 100.0 / totalTxPackets) : 0.0;
    
    NS_LOG_UNCOND("FLOW_SUMMARY_TOTAL: freq=" << freq << " proto=" << protocol << " numSTAs=" << g_numStas
                  << " Throughput_Mbps=" << totalThroughput 
                  << " AvgThroughputPerSTA_Mbps=" << avgThroughputPerSta
                  << " AvgDelay_ms=" << avgDelay 
                  << " AvgJitter_ms=" << avgJitter
                  << " TotalLostPackets=" << totalLostPackets
                  << " LossRate_pct=" << totalLossRate
                  << " TX_Packets=" << totalTxPackets << " RX_Packets=" << totalRxPackets);
    
    // ===== GRANULAR PACKET LOSS BREAKDOWN =====
    uint64_t totalPhyDrops = g_phyTxDrop + g_phyRxDrop;
    uint64_t totalMacDrops = g_macTxDrop + g_macRxDrop;
    uint64_t totalTcDrops = g_tcDropBeforeEnqueue + g_tcDropAfterDequeue + g_tcDrop;
    uint64_t totalGranularDrops = totalPhyDrops + totalMacDrops + g_wifiQueueDrop + totalTcDrops;
    
    NS_LOG_UNCOND("PACKET_LOSS_BREAKDOWN: freq=" << freq << " proto=" << protocol
                  << " PhyTxDrop=" << g_phyTxDrop
                  << " PhyRxDrop=" << g_phyRxDrop  
                  << " MacTxDrop=" << g_macTxDrop
                  << " MacRxDrop=" << g_macRxDrop
                  << " WifiQueueDrop=" << g_wifiQueueDrop
                  << " TcDropBeforeEnqueue=" << g_tcDropBeforeEnqueue
                  << " TcDropAfterDequeue=" << g_tcDropAfterDequeue
                  << " TcDrop=" << g_tcDrop
                  << " TotalGranularDrops=" << totalGranularDrops
                  << " E2E_LostPackets=" << totalLostPackets);
    
    // Percentagens por camada (relativas ao total de drops granulares)
    if (totalGranularDrops > 0) {
        double phyPct = (totalPhyDrops * 100.0) / totalGranularDrops;
        double macPct = (totalMacDrops * 100.0) / totalGranularDrops;
        double wifiQueuePct = (g_wifiQueueDrop * 100.0) / totalGranularDrops;
        double tcPct = (totalTcDrops * 100.0) / totalGranularDrops;
        double unaccountedPct = 100.0 - phyPct - macPct - wifiQueuePct - tcPct;
        
        NS_LOG_UNCOND("LOSS_ATTRIBUTION: PHY_pct=" << phyPct 
                      << " MAC_pct=" << macPct 
                      << " WifiQueue_pct=" << wifiQueuePct
                      << " TC_pct=" << tcPct
                      << " Unaccounted_pct=" << unaccountedPct);
    }

    // Breakdown de razões para Traffic Control drops
    for (const auto& kv : g_tcDropBeforeReasons)
    {
        const std::string& reasonName = kv.first;
        uint64_t reasonCount = kv.second;
        double pctTcBefore = (g_tcDropBeforeEnqueue > 0) ? (reasonCount * 100.0 / g_tcDropBeforeEnqueue) : 0.0;
        double pctTotalDrops = (totalGranularDrops > 0) ? (reasonCount * 100.0 / totalGranularDrops) : 0.0;

        NS_LOG_UNCOND("TC_DROP_BEFORE_REASON: Reason=" << reasonName
                      << " Count=" << reasonCount
                      << " PctTcBefore=" << pctTcBefore
                      << " PctTotalDrops=" << pctTotalDrops);
    }

    for (const auto& kv : g_tcDropAfterReasons)
    {
        const std::string& reasonName = kv.first;
        uint64_t reasonCount = kv.second;
        double pctTcAfter = (g_tcDropAfterDequeue > 0) ? (reasonCount * 100.0 / g_tcDropAfterDequeue) : 0.0;
        double pctTotalDrops = (totalGranularDrops > 0) ? (reasonCount * 100.0 / totalGranularDrops) : 0.0;

        NS_LOG_UNCOND("TC_DROP_AFTER_REASON: Reason=" << reasonName
                      << " Count=" << reasonCount
                      << " PctTcAfter=" << pctTcAfter
                      << " PctTotalDrops=" << pctTotalDrops);
    }

    // Breakdown de razões para PhyRxDrop
    for (auto it = g_phyRxDropReasons.begin(); it != g_phyRxDropReasons.end(); ++it)
    {
        const std::string& reasonName = it->first;
        uint64_t reasonCount = it->second;
        double pctPhyRx = (g_phyRxDrop > 0) ? (reasonCount * 100.0 / g_phyRxDrop) : 0.0;
        double pctTotalDrops = (totalGranularDrops > 0) ? (reasonCount * 100.0 / totalGranularDrops) : 0.0;

        NS_LOG_UNCOND("PHY_RX_DROP_REASON: Reason=" << reasonName
                      << " Count=" << reasonCount
                      << " PctPhyRx=" << pctPhyRx
                      << " PctTotalDrops=" << pctTotalDrops);
    }

    // Breakdown por tipo de frame MAC dentro de cada razão de PHY RX drop
    for (auto it = g_phyRxDropMacTypeByReason.begin(); it != g_phyRxDropMacTypeByReason.end(); ++it)
    {
        const std::string& key = it->first;
        const uint64_t count = it->second;

        std::size_t sep = key.find('|');
        std::string reasonName = (sep == std::string::npos) ? key : key.substr(0, sep);
        std::string macType = (sep == std::string::npos) ? std::string("UNKNOWN") : key.substr(sep + 1);

        uint64_t reasonTotal = 0;
        auto reasonIt = g_phyRxDropReasons.find(reasonName);
        if (reasonIt != g_phyRxDropReasons.end())
        {
            reasonTotal = reasonIt->second;
        }

        double pctReason = (reasonTotal > 0) ? (count * 100.0 / reasonTotal) : 0.0;
        double pctPhyRx = (g_phyRxDrop > 0) ? (count * 100.0 / g_phyRxDrop) : 0.0;

        NS_LOG_UNCOND("PHY_RX_DROP_PKT_TYPE: Reason=" << reasonName << " MacType=" << macType
                      << " Count=" << count << " PctReason=" << pctReason
                      << " PctPhyRx=" << pctPhyRx);
    }

    // Breakdown por tamanho de pacote PHY RX drop (top 10)
    std::vector<std::pair<uint32_t, uint64_t>> sizeCounts(g_phyRxDropPacketSizes.begin(),
                                                          g_phyRxDropPacketSizes.end());
    std::sort(sizeCounts.begin(),
              sizeCounts.end(),
              [](const auto& a, const auto& b) {
                  if (a.second == b.second)
                  {
                      return a.first < b.first;
                  }
                  return a.second > b.second;
              });

    uint32_t topN = std::min<uint32_t>(10, sizeCounts.size());
    for (uint32_t i = 0; i < topN; ++i)
    {
        uint32_t pktSize = sizeCounts[i].first;
        uint64_t count = sizeCounts[i].second;
        double pctPhyRx = (g_phyRxDrop > 0) ? (count * 100.0 / g_phyRxDrop) : 0.0;
        NS_LOG_UNCOND("PHY_RX_DROP_PKT_SIZE: PacketSize=" << pktSize << " Count=" << count
                      << " PctPhyRx=" << pctPhyRx);
    }

    // Timeline: PHY RX drops por segundo de simulação
    for (auto it = g_phyRxDropBySecond.begin(); it != g_phyRxDropBySecond.end(); ++it)
    {
        NS_LOG_UNCOND("PHY_RX_DROP_BY_SECOND: Sec=" << it->first << " Count=" << it->second);
    }

    Simulator::Destroy();
    return 0;
}
