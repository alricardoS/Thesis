/*
 * wifi7-mlo-multi-sta.cc
 * 802.11be MLO (Multi-Link Operation) Test with Multiple STAs (2 STAs)
 * 
 * Layout (AP no centro, STAs nos lados):
 *    STA0 (esq) --AP-- STA1 (dir)
 * 
 * Usa WifiStaticSetupHelper para evitar colisões durante associação ML
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
#include "ns3/frame-exchange-manager.h"
#include "ns3/wifi-phy.h"
#include "ns3/log.h"
#include "ns3/wifi-static-setup-helper.h"
#include "ns3/neighbor-cache-helper.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/qos-txop.h"
#include "mlo-qos-weighted-scheduler.h"
#include "ns3/traffic-control-module.h"
#include "ns3/wifi-ru.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <cmath>
#include <set>
#include <tuple>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("wifi7-mlo-multi-sta-priority-sch");

// ========== GRANULAR PACKET LOSS COUNTERS ==========
// PHY Layer Drops
uint64_t g_phyTxDrop = 0;   // Packets dropped during TX at PHY
uint64_t g_phyRxDrop = 0;   // Packets dropped during RX at PHY (low SNR, collision)
std::map<std::string, uint64_t> g_phyRxDropReasons; // PHY RX drops by reason
std::map<std::string, uint64_t> g_phyRxDropMacTypeByReason; // PHY RX drops by (reason, MAC frame type)
std::map<uint32_t, uint64_t> g_phyRxDropPacketSizes; // PHY RX drops by packet size
std::map<uint32_t, uint64_t> g_phyRxDropBySecond; // PHY RX drops timeline (second -> count)

// MAC Layer Drops  
uint64_t g_macTxDrop = 0;   // Packets dropped at MAC TX (exceeded retry limit)
uint64_t g_macRxDrop = 0;   // Packets dropped at MAC RX

// WiFi Queue Drops
uint64_t g_wifiQueueDrop = 0;   // Packets dropped due to full WiFi MAC queue

// IP/Traffic Control Layer Drops
uint64_t g_tcDropBeforeEnqueue = 0;  // Packets dropped before entering TC queue
uint64_t g_tcDropAfterDequeue = 0;   // Packets dropped after dequeue
uint64_t g_tcDrop = 0;               // TC layer drops
std::map<std::string, uint64_t> g_tcDropBeforeReasons; // TC drops by reason (before enqueue)
std::map<std::string, uint64_t> g_tcDropAfterReasons;  // TC drops by reason (after dequeue)

// ========== MLO LINK/RU USAGE COUNTERS ==========
std::map<uint8_t, uint32_t> g_linkTxDepth;
std::map<uint8_t, double> g_linkTxTimeSec;
std::map<uint8_t, uint64_t> g_linkMuTxCount;
std::map<uint8_t, uint64_t> g_linkSuTxCount;
std::map<uint8_t, std::map<std::string, uint64_t>> g_linkRuTypeCount;

double g_overlapBothLinksSec = 0.0;
double g_lastTxStateUpdateSec = 0.0;
bool g_txStateInitialized = false;

// ========== CONTROL RETURN-LINK TRACKING (AP side) ==========
struct PendingCtrlRequest
{
    Mac48Address peer;
    uint8_t txLink;
    double timeSec;
    std::string expectedResponse; // CTS | BLOCKACK | ACK_OR_BLOCKACK
};

struct ControlResponseStats
{
    uint64_t matched{0};
    uint64_t sameLink{0};
    uint64_t crossLink{0};
    uint64_t unmatched{0};
};

std::vector<PendingCtrlRequest> g_pendingCtrlRequests;
std::map<std::string, ControlResponseStats> g_ctrlResponseStats;
Mac48Address g_apAddress;
std::set<Mac48Address> g_apRxAddresses;
std::map<std::string, uint8_t> g_apLinkAddressToLinkId;
std::map<std::string, uint32_t> g_staAddressToIndex;
uint64_t g_ctrlTxRequestsRegistered = 0;
uint64_t g_ctrlRxControlSeen = 0;

// ========== LINK TRAFFIC TRACKING ==========
struct LinkTrafficBucket
{
    uint64_t frame_count = 0;
    uint64_t bytes = 0;
};

std::map<std::tuple<double, uint8_t, std::string, uint32_t, std::string, std::string, std::string>, LinkTrafficBucket>
    g_linkTrafficData;
std::ofstream g_linkTrafficStream;
std::string g_linkTrafficCsvPath;
std::string g_linkTrafficRunLabel;
std::string g_linkTrafficPairLabel;
std::string g_protocol;
double g_linkTrafficSampleInterval = 1.0;
Time g_linkTrafficEndTime;

// Backwards-compatible frame distribution sampling used by other reports.
std::map<std::tuple<double, uint8_t, std::string, std::string, std::string>, LinkTrafficBucket>
    g_frameDistributionData;
std::ofstream g_frameDistributionStream;
std::string g_frameDistributionCsvPath;
std::string g_frameDistributionRunLabel;
std::string g_linkFrequencyPair;
double g_frameDistributionSampleInterval = 1.0;
Time g_frameDistributionEndTime;

// Global flag to enable/disable the traffic-aware link-selection wrapper.
bool g_useCustomMloScheduler = true;

// Goal-Oriented scheduler thresholds
double g_stayThreshold      = 0.75; // stay if currentSatisfaction >= this
double g_migrationThreshold = 0.25; // migrate only if expectedGain > this

Ptr<QosWeightedMloScheduler> g_apScheduler;
// STAs use the default FcfsWifiQueueScheduler (AP-only scope for goal-oriented scheduler)
// std::vector<Ptr<QosWeightedMloScheduler>> g_staSchedulers; // removed — STAs use FCFS
std::string g_schedulerDecisionCsvPath = "";

// Default QoS Weights (Delay, Jitter, Loss, Throughput)
double g_voDelayWeight = 0.40; double g_voJitterWeight = 0.30; double g_voLossWeight = 0.25; double g_voTpWeight = 0.05;
double g_viDelayWeight = 0.25; double g_viJitterWeight = 0.15; double g_viLossWeight = 0.20; double g_viTpWeight = 0.40;
double g_beDelayWeight = 0.20; double g_beJitterWeight = 0.05; double g_beLossWeight = 0.15; double g_beTpWeight = 0.60;
double g_bkDelayWeight = 0.05; double g_bkJitterWeight = 0.05; double g_bkLossWeight = 0.10; double g_bkTpWeight = 0.80;

std::string
GetLinkName(uint8_t linkId, const std::string& freqPair)
{
    std::vector<std::string> freqs;
    std::stringstream ss(freqPair);
    std::string token;
    while (std::getline(ss, token, '+'))
    {
        freqs.push_back(token);
    }
    if (linkId < freqs.size())
    {
        return freqs[linkId] + "GHz";
    }
    return "Link" + std::to_string(linkId);
}

std::string
MacAddressToString(const Mac48Address& address)
{
    std::ostringstream oss;
    oss << address;
    return oss.str();
}

std::string
GetFrameType(const WifiMacHeader& hdr)
{
    if (hdr.IsData())
    {
        return "Data";
    }
    if (hdr.IsRts() || hdr.IsCts() || hdr.IsAck() || hdr.IsBlockAckReq() || hdr.IsBlockAck())
    {
        return "Control";
    }
    if (hdr.IsMgt())
    {
        return "Management";
    }
    return "Other";
}

std::string
AcIndexToShortName(AcIndex ac)
{
    switch (ac)
    {
    case AC_BE:
        return "BE";
    case AC_BK:
        return "BK";
    case AC_VI:
        return "VI";
    case AC_VO:
        return "VO";
    default:
        return "BE";
    }
}

AcIndex
GetAcFromPsdu(Ptr<const WifiPsdu> psdu)
{
    AcIndex ac = AC_BE;
    if (psdu && psdu->GetNMpdus() > 0)
    {
        const WifiMacHeader& hdr = psdu->GetHeader(0);
        if (hdr.IsQosData())
        {
            ac = QosUtilsMapTidToAc(hdr.GetQosTid());
        }
    }
    return ac;
}

void
RecordLinkTraffic(uint8_t linkId, Ptr<const WifiPsdu> psdu)
{
    if (!psdu || psdu->GetNMpdus() == 0)
    {
        return;
    }

    const WifiMacHeader& hdr = psdu->GetHeader(0);
    if (!hdr.IsData())
    {
        return;
    }

    const Mac48Address peer = hdr.GetAddr1();
    const auto peerIt = g_staAddressToIndex.find(MacAddressToString(peer));
    if (peerIt == g_staAddressToIndex.end())
    {
        return;
    }

    const double now = Simulator::Now().GetSeconds();
    const double timeBucket = std::floor(now / g_linkTrafficSampleInterval) * g_linkTrafficSampleInterval;
    const uint32_t staId = peerIt->second;
    const std::string staAddress = MacAddressToString(peer);
    const std::string frameType = GetFrameType(hdr);
    const AcIndex ac = GetAcFromPsdu(psdu);
    const std::string acStr = AcIndexToShortName(ac);
    const std::string linkName = GetLinkName(linkId, g_linkFrequencyPair);

    auto key = std::make_tuple(timeBucket, linkId, linkName, staId, staAddress, acStr, frameType);
    g_linkTrafficData[key].frame_count += psdu->GetNMpdus();
    g_linkTrafficData[key].bytes += psdu->GetSize();
}

void
RecordLinkTrafficFromMpdu(Ptr<const WifiMpdu> mpdu)
{
    if (!mpdu)
    {
        return;
    }

    const WifiMacHeader& hdr = mpdu->GetHeader();
    if (!hdr.IsData())
    {
        return;
    }

    const auto linkIt = g_apLinkAddressToLinkId.find(MacAddressToString(hdr.GetAddr2()));
    if (linkIt == g_apLinkAddressToLinkId.end())
    {
        return;
    }

    const Mac48Address peer = hdr.GetAddr1();
    const auto peerIt = g_staAddressToIndex.find(MacAddressToString(peer));
    if (peerIt == g_staAddressToIndex.end())
    {
        return;
    }

    const uint8_t linkId = linkIt->second;
    const double now = Simulator::Now().GetSeconds();
    const double timeBucket = std::floor(now / g_linkTrafficSampleInterval) * g_linkTrafficSampleInterval;
    const uint32_t staId = peerIt->second;
    const std::string staAddress = MacAddressToString(peer);
    const std::string frameType = GetFrameType(hdr);
    const AcIndex ac = hdr.IsQosData() ? QosUtilsMapTidToAc(hdr.GetQosTid()) : AC_BE;
    const std::string acStr = AcIndexToShortName(ac);
    const std::string linkName = GetLinkName(linkId, g_linkFrequencyPair);

    auto key = std::make_tuple(timeBucket, linkId, linkName, staId, staAddress, acStr, frameType);
    g_linkTrafficData[key].frame_count += 1;
    g_linkTrafficData[key].bytes += mpdu->GetSize();

    // Feed confirmed transmission to scheduler (real throughput + PER success)
    if (g_apScheduler) {
        g_apScheduler->FeedLinkMetrics(linkId, ac, mpdu->GetSize(), 1, 0);
    }
}

void
RecordFrameDistribution(uint8_t linkId, Ptr<const WifiPsdu> psdu)
{
    RecordLinkTraffic(linkId, psdu);

    if (!psdu || psdu->GetNMpdus() == 0)
    {
        return;
    }

    const double now = Simulator::Now().GetSeconds();
    const double timeBucket = std::floor(now / g_frameDistributionSampleInterval) *
                              g_frameDistributionSampleInterval;

    const WifiMacHeader& hdr = psdu->GetHeader(0);
    const std::string frameType = GetFrameType(hdr);
    const AcIndex ac = GetAcFromPsdu(psdu);
    const std::string acStr = AcIndexToShortName(ac);
    const std::string linkName = GetLinkName(linkId, g_linkFrequencyPair);

    auto key = std::make_tuple(timeBucket, linkId, linkName, acStr, frameType);
    g_frameDistributionData[key].frame_count += psdu->GetNMpdus();
    g_frameDistributionData[key].bytes += psdu->GetSize();
}

void
FlushLinkTrafficData()
{
    if (!g_linkTrafficStream.is_open())
    {
        return;
    }

    for (const auto& kv : g_linkTrafficData)
    {
        const auto& key = kv.first;
        const auto& bucket = kv.second;
        std::ostringstream rowoss;
        rowoss << g_linkTrafficRunLabel << ',' << std::get<0>(key) << ',' << g_linkTrafficPairLabel << ',' << g_protocol << ','
               << static_cast<int>(std::get<1>(key)) << ',' << std::get<2>(key) << ',' << std::get<3>(key) << ','
               << std::get<4>(key) << ',' << std::get<5>(key) << ',' << std::get<6>(key) << ',' << bucket.frame_count << ',' << bucket.bytes;
        g_linkTrafficStream << rowoss.str() << '\n';
    }
    if (g_linkTrafficStream.is_open()) {
        g_linkTrafficStream.flush();
    }

    // Link traffic is now only logged to CSV (no scheduler ingestion)
    g_linkTrafficStream.flush();
    g_linkTrafficData.clear();
}

void
ScheduleLinkTrafficFlush()
{
    FlushLinkTrafficData();
    if (Simulator::Now() + Seconds(g_linkTrafficSampleInterval) <= g_linkTrafficEndTime)
    {
        Simulator::Schedule(Seconds(g_linkTrafficSampleInterval), &ScheduleLinkTrafficFlush);
    }
}

void
FlushFrameDistributionData()
{
    if (!g_frameDistributionStream.is_open())
    {
        return;
    }

    for (const auto& kv : g_frameDistributionData)
    {
        const auto& key = kv.first;
        const auto& bucket = kv.second;
        g_frameDistributionStream << g_frameDistributionRunLabel << ',' << std::get<0>(key) << ','
                                  << static_cast<int>(std::get<1>(key)) << ',' << std::get<2>(key) << ','
                                  << std::get<3>(key) << ',' << std::get<4>(key) << ','
                                  << bucket.frame_count << ',' << bucket.bytes << '\n';
    }

    g_frameDistributionStream.flush();
    g_frameDistributionData.clear();
}

void
ScheduleFrameDistributionFlush()
{
    FlushFrameDistributionData();
    if (Simulator::Now() + Seconds(g_frameDistributionSampleInterval) <= g_frameDistributionEndTime)
    {
        Simulator::Schedule(Seconds(g_frameDistributionSampleInterval), &ScheduleFrameDistributionFlush);
    }
}

void
RegisterPendingCtrlRequest(uint8_t linkId,
                           Mac48Address peer,
                           const std::string& expectedResponse)
{
    g_pendingCtrlRequests.push_back(
        PendingCtrlRequest{peer, linkId, Simulator::Now().GetSeconds(), expectedResponse});
}

bool
ResponseMatchesExpectation(const std::string& expectedResponse, const std::string& responseType)
{
    if (expectedResponse == responseType)
    {
        return true;
    }
    if (expectedResponse == "ACK_OR_BLOCKACK" &&
        (responseType == "ACK" || responseType == "BLOCKACK"))
    {
        return true;
    }
    return false;
}

bool
IsAddressedToAp(const Mac48Address& ra)
{
    return g_apRxAddresses.find(ra) != g_apRxAddresses.end();
}

void
UpdateLinkActivityAccounting()
{
    const double now = Simulator::Now().GetSeconds();
    if (!g_txStateInitialized)
    {
        g_lastTxStateUpdateSec = now;
        g_txStateInitialized = true;
        return;
    }

    const double dt = std::max(0.0, now - g_lastTxStateUpdateSec);
    uint32_t activeLinks = 0;
    for (const auto& kv : g_linkTxDepth)
    {
        if (kv.second > 0)
        {
            activeLinks++;
            g_linkTxTimeSec[kv.first] += dt;
        }
    }

    if (activeLinks > 0)
    {
    }
    if (activeLinks > 1)
    {
        g_overlapBothLinksSec += dt;
    }

    g_lastTxStateUpdateSec = now;
}

// FeedLinkUtilizationToSchedulers removed — utilization is now computed
// internally by the scheduler from FeedLinkTxStart/FeedLinkTxEnd callbacks.

void
LinkPhyTxBeginCallback(uint8_t linkId, Ptr<const Packet> packet, double txPowerW)
{
    UpdateLinkActivityAccounting();
    g_linkTxDepth[linkId]++;

    // Feed airtime start to AP scheduler for real channel utilization
    if (g_apScheduler) {
        g_apScheduler->FeedLinkTxStart(linkId);
    }
    // STAs use FCFS — no feed needed
}

void
LinkPhyTxEndCallback(uint8_t linkId, Ptr<const Packet> packet)
{
    UpdateLinkActivityAccounting();
    if (g_linkTxDepth[linkId] > 0)
    {
        g_linkTxDepth[linkId]--;
    }

    // Feed airtime end to AP scheduler for real channel utilization
    if (g_apScheduler) {
        g_apScheduler->FeedLinkTxEnd(linkId);
    }
    // STAs use FCFS — no feed needed
}

void
LinkPhyTxPsduBeginCallback(uint8_t linkId,
                           WifiConstPsduMap psduMap,
                           WifiTxVector txVector,
                           double txPowerW)
{
    for (const auto& kv : psduMap)
    {
        RecordFrameDistribution(linkId, kv.second);
    }

    if (txVector.IsDlMu() || txVector.IsUlMu())
    {
        g_linkMuTxCount[linkId]++;

        const auto& userMap = txVector.GetHeMuUserInfoMap();
        for (const auto& kv : userMap)
        {
            std::ostringstream ruName;
            ruName << WifiRu::GetRuType(txVector.GetRu(kv.first));
            g_linkRuTypeCount[linkId][ruName.str()]++;
        }
    }
    else
    {
        for (const auto& kv : psduMap)
        {
            Ptr<const WifiPsdu> psdu = kv.second;
            if (psdu && psdu->GetNMpdus() > 0)
            {
                g_linkSuTxCount[linkId] += psdu->GetNMpdus();
                AcIndex ac = GetAcFromPsdu(psdu);

                if (g_apScheduler) {
                    // Phase 2: Feed PHY state so LinkCapabilityEngine can compute
                    // estimatedCapacity = dataRate * (1 - PER). Use current PER
                    // from the last window (not known yet for this TX — use 0 as
                    // initial proxy; real PER is updated via FeedLinkTxAttempt +
                    // FeedLinkMetrics in AckedMpdu).
                    g_apScheduler->FeedLinkTxAttempt(linkId, ac, psdu->GetNMpdus());
                    g_apScheduler->FeedLinkPhyState(linkId, txVector, 0.0);

                    // Phase 3: Feed packet transmitted (bytes + airtime proxy).
                    // Airtime = PSDU duration; ns-3 3.47 exposes txVector.GetPpduDuration
                    // only on EhtPpdu — use a conservative proxy from the PSDU size
                    // and data rate instead.
                    double dataRateMbps = 1.0;
                    try {
                        dataRateMbps = txVector.GetMode().GetDataRate(txVector) / 1e6;
                    } catch (...) {}
                    if (dataRateMbps < 0.001) dataRateMbps = 1.0;

                    uint32_t psduBytes = psdu->GetSize();
                    double   airtimeSec = (psduBytes * 8.0) / (dataRateMbps * 1e6);

                    // Use first MPDU's header for peer/tid
                    const WifiMacHeader& hdr = psdu->GetHeader(0);
                    std::ostringstream peerSs;
                    peerSs << hdr.GetAddr1();
                    uint8_t tid = hdr.IsQosData() ? hdr.GetQosTid() : 0;

                    g_apScheduler->FeedPacketTransmitted(
                        linkId, ac, psduBytes, airtimeSec, peerSs.str(), tid);
                }
            }
        }
    }
}

void
ApControlTxPsduBeginCallback(uint8_t linkId,
                             WifiConstPsduMap psduMap,
                             WifiTxVector txVector,
                             double txPowerW)
{
    for (const auto& kv : psduMap)
    {
        Ptr<const WifiPsdu> psdu = kv.second;
        if (!psdu || psdu->GetNMpdus() == 0)
        {
            continue;
        }

        const WifiMacHeader& hdr = psdu->GetHeader(0);
        const Mac48Address peer = hdr.GetAddr1();

        if (peer.IsGroup())
        {
            continue;
        }

        if (hdr.IsRts())
        {
            RegisterPendingCtrlRequest(linkId, peer, "CTS");
            g_ctrlTxRequestsRegistered++;
        }
        else if (hdr.IsBlockAckReq())
        {
            RegisterPendingCtrlRequest(linkId, peer, "BLOCKACK");
            g_ctrlTxRequestsRegistered++;
        }
        else if (hdr.IsData())
        {
            RegisterPendingCtrlRequest(linkId, peer, "ACK_OR_BLOCKACK");
            g_ctrlTxRequestsRegistered++;
        }
    }
}

void
ApControlRxMacHeaderEndCallback(uint8_t linkId,
                                const WifiMacHeader& hdr,
                                const WifiTxVector& txVector,
                                Time psduDuration)
{
    std::string responseType;
    if (hdr.IsCts())
    {
        responseType = "CTS";
    }
    else if (hdr.IsAck())
    {
        responseType = "ACK";
    }
    else if (hdr.IsBlockAck())
    {
        responseType = "BLOCKACK";
    }
    else
    {
        return;
    }
    g_ctrlRxControlSeen++;

    // response must be addressed to AP
    if (!IsAddressedToAp(hdr.GetAddr1()))
    {
        return;
    }

    auto& stats = g_ctrlResponseStats[responseType];
    const bool requiresPeerMatch = (responseType == "BLOCKACK");
    const Mac48Address peer = hdr.GetAddr2();

    // Find latest unmatched pending request from the same peer
    // and compatible expected response, within a short time window.
    const double nowSec = Simulator::Now().GetSeconds();
    const double maxDeltaSec = 0.02;
    for (auto it = g_pendingCtrlRequests.rbegin(); it != g_pendingCtrlRequests.rend(); ++it)
    {
        if (requiresPeerMatch && !(it->peer == peer))
        {
            continue;
        }
        if (!ResponseMatchesExpectation(it->expectedResponse, responseType))
        {
            continue;
        }
        if ((nowSec - it->timeSec) > maxDeltaSec)
        {
            continue;
        }

        stats.matched++;
        if (it->txLink == linkId)
        {
            stats.sameLink++;
        }
        else
        {
            stats.crossLink++;
        }
        it->expectedResponse = "MATCHED";
        return;
    }

    stats.unmatched++;
}

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

// PHY Drop Callbacks
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
}
/*
// MAC Drop Callbacks
void MacTxDropCallback(Ptr<const Packet> packet)
{
    g_macTxDrop++;
}
*/

void MacRxDropCallback(Ptr<const Packet> packet)
{
    g_macRxDrop++;
}

// WiFi Queue Drop Callback
void WifiQueueDropCallback(Ptr<const WifiMpdu> mpdu)
{
    g_wifiQueueDrop++;
}


//NOVO

void WifiQueueDropCallbackReal(AcIndex ac, Ptr<const WifiMpdu> mpdu)
{
    g_wifiQueueDrop++;
    
    // Alimenta as perdas reais para a categoria de acesso no escalonador do AP
    if (g_apScheduler) {
        g_apScheduler->FeedLinkDrop(0, ac);
        g_apScheduler->FeedLinkDrop(1, ac);
    }
}

void MacTxDropCallbackReal(Ptr<const Packet> packet)
{
    g_macTxDrop++;
    
    // Descobre a Categoria de Acesso inspecionando o cabeçalho do pacote
    WifiMacHeader hdr;
    if (packet->PeekHeader(hdr) > 0 && hdr.IsQosData()) {
        AcIndex ac = QosUtilsMapTidToAc(hdr.GetQosTid());
        if (g_apScheduler) {
            g_apScheduler->FeedLinkDrop(0, ac);
            g_apScheduler->FeedLinkDrop(1, ac);
        }
    }
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

// Per-second per-STA QoS metrics time series (CSV opcional, puramente observacional)
std::ofstream g_perStaMetricsStream;
std::string g_perStaMetricsCsvPath;
std::string g_perStaMetricsRunLabel;
std::vector<uint64_t> g_lastStaTx;
std::vector<uint64_t> g_lastStaRx;

// Per-STA identity/goal, for feeding real per-STA QoS to the AP scheduler
std::vector<std::vector<Mac48Address>> g_staIndexToMacs; // todos os MACs (MLD + link) por STA
std::vector<AcIndex> g_staAc;                             // AC de cada STA
// FlowMonitor para loss REAL acumulada por-STA (alimentada ao scheduler)
Ptr<FlowMonitor> g_flowMonitor;
Ptr<Ipv4FlowClassifier> g_flowClassifier;
std::map<Ipv4Address, uint32_t> g_staIpToIndex;          // IP destino -> índice STA
// Baselines por janela (para converter somas cumulativas em médias por janela)
std::vector<double>   g_lastDelaySumMs;
std::vector<uint64_t> g_lastDelaySamples;
std::vector<double>   g_lastJitterSumMs;
std::vector<uint64_t> g_lastJitterSamples;

struct QueueOccupancyTarget
{
    std::string role;
    uint32_t nodeId;
    Ptr<WifiMac> mac;
};

std::vector<QueueOccupancyTarget> g_queueOccupancyTargets;
std::ofstream g_queueOccupancyStream;
std::string g_queueOccupancyCsvPath;
std::string g_queueOccupancyRunLabel;
double g_queueSampleInterval = 0.1;
Time g_queueOccupancyEndTime;

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
        config.channel = 1;
        config.width = 20;
        config.band = "BAND_2_4GHZ";
        config.freqRange = WIFI_SPECTRUM_2_4_GHZ;
    } else if (freq == 5) {
        config.channel = 50;
        config.width = 160;
        config.band = "BAND_5GHZ";
        config.freqRange = WIFI_SPECTRUM_5_GHZ;
    } else {
        config.channel = 31;
        config.width = 320;
        config.band = "BAND_6GHZ";
        config.freqRange = WIFI_SPECTRUM_6_GHZ;
    }
    return config;
}

// Callback para monitorizar recepção de pacotes no sink (usa SeqTsSizeHeader)
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

    // Loss REAL acumulada por-STA (FlowMonitor, downlink) — mesma fonte do relatório final.
    std::vector<uint64_t> staTx(g_numStas, 0), staRx(g_numStas, 0);
    if (g_flowMonitor && g_flowClassifier) {
        auto fstats = g_flowMonitor->GetFlowStats();
        for (const auto& kv : fstats) {
            Ipv4FlowClassifier::FiveTuple tup = g_flowClassifier->FindFlow(kv.first);
            auto it = g_staIpToIndex.find(tup.destinationAddress);
            if (it != g_staIpToIndex.end() && it->second < g_numStas) {
                staTx[it->second] += kv.second.txPackets;
                staRx[it->second] += kv.second.rxPackets;
            }
        }
    }

    for (uint32_t i = 0; i < g_numStas; ++i) {
        uint64_t currentTotalRx = sinks[i]->GetTotalRx();
        double throughput = ((currentTotalRx - g_lastTotalRx[i]) * 8.0) / (1.0 * 1e6);
        g_lastTotalRx[i] = currentTotalRx;

        // ---- Métricas REAIS por-STA nesta janela (delta das somas cumulativas) ----
        uint64_t dSamples = g_delaySamples[i]  - g_lastDelaySamples[i];
        double   dDelaySum = g_delaySumMs[i]   - g_lastDelaySumMs[i];
        uint64_t jSamples = g_jitterSamples[i] - g_lastJitterSamples[i];
        double   dJitterSum = g_jitterSumMs[i] - g_lastJitterSumMs[i];
        g_lastDelaySamples[i]  = g_delaySamples[i];
        g_lastDelaySumMs[i]    = g_delaySumMs[i];
        g_lastJitterSamples[i] = g_jitterSamples[i];
        g_lastJitterSumMs[i]   = g_jitterSumMs[i];

        double winDelay  = (dSamples > 0) ? (dDelaySum / dSamples) : 0.0;
        double winJitter = (jSamples > 0) ? (dJitterSum / jSamples) : 0.0;
        double winLoss   = (staTx[i] > 0)
                             ? std::min(1.0, std::max(0.0,
                                 double(staTx[i] - staRx[i]) / double(staTx[i])))
                             : 0.0;

        // Alimentar o scheduler do AP com a QoS real deste STA (para todos os
        // seus MACs, para casar com o Addr1 que o scheduler observa).
        if (g_apScheduler && i < g_staIndexToMacs.size()) {
            for (const auto& mac : g_staIndexToMacs[i]) {
                g_apScheduler->FeedStaQos(mac, g_staAc[i],
                                          throughput, winDelay, winJitter, winLoss);
            }
        }

        // ---- Série temporal por-STA (opcional; não afecta nenhum cálculo acima) ----
        if (g_perStaMetricsStream.is_open() && i < g_staAc.size()) {
            // Subtracção em double (com sinal): evita o underflow de uint64_t quando
            // uma janela "sobre-entrega" (dRx > dTx, pacotes in-flight da janela
            // anterior chegam agora), que gerava 100% de loss falso.
            double dTxD = double(staTx[i]) - double(g_lastStaTx[i]);
            double dRxD = double(staRx[i]) - double(g_lastStaRx[i]);
            g_lastStaTx[i] = staTx[i];
            g_lastStaRx[i] = staRx[i];
            double winLossPct = (dTxD > 0.0)
                                  ? std::min(100.0, std::max(0.0,
                                      100.0 * (dTxD - dRxD) / dTxD))
                                  : 0.0;

            g_perStaMetricsStream << '"' << g_perStaMetricsRunLabel << "\","
                                  << currentTime << ',' << i << ','
                                  << AcIndexToShortName(g_staAc[i]) << ','
                                  << throughput << ',' << winDelay << ','
                                  << winJitter << ',' << winLossPct << '\n';
        }

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

    if (g_perStaMetricsStream.is_open()) {
        g_perStaMetricsStream.flush();
    }

    Simulator::Schedule(Seconds(1), &CalculateStats, sinks);
}

void
RecordQueueOccupancySample()
{
    const double now = Simulator::Now().GetSeconds();

    if (g_queueOccupancyStream.is_open())
    {
        for (const auto& target : g_queueOccupancyTargets)
        {
            if (!target.mac)
            {
                continue;
            }

            for (const auto ac : {AC_BE, AC_BK, AC_VI, AC_VO})
            {
                Ptr<QosTxop> qosTxop = target.mac->GetQosTxop(ac);
                if (!qosTxop)
                {
                    continue;
                }

                Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
                if (!queue)
                {
                    continue;
                }

                g_queueOccupancyStream << g_queueOccupancyRunLabel << ',' << now << ',' << target.role << ','
                                       << target.nodeId << ',' << AcIndexToShortName(ac) << ", "
                                       << queue->GetNPackets() << ',' << queue->GetNBytes() << '\n';

                // Scheduler now uses only static policy (no queue occupancy tracking)

            }
        }

        g_queueOccupancyStream.flush();
    }

    if (Simulator::Now() + Seconds(g_queueSampleInterval) <= g_queueOccupancyEndTime)
    {
        Simulator::Schedule(Seconds(g_queueSampleInterval), &RecordQueueOccupancySample);
    }
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
        return 0xA0; // AF41 -> VI
    case 3:
        return 0xC0; // CS6/EF -> VO
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

// Classe de prioridade (0=BK,1=BE,2=VI,3=VO) -> AcIndex (BE=0,BK=1,VI=2,VO=3)
AcIndex
PriorityClassToAc(uint32_t priorityClass)
{
    switch (priorityClass)
    {
    case 0: return AC_BK;
    case 1: return AC_BE;
    case 2: return AC_VI;
    case 3: return AC_VO;
    default: return AC_BE;
    }
}

std::vector<uint32_t>
ParseStaTrafficTypes(const std::string& csv, uint32_t numStas)
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

        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (token == "background" || token == "bk" || token == "bulk" || token == "cs1")
        {
            priorities[idx] = 0;
        }
        else if (token == "best-effort" || token == "best_effort" || token == "besteffort" ||
                 token == "be" || token == "data" || token == "default")
        {
            priorities[idx] = 1;
        }
        else if (token == "video" || token == "vi" || token == "af41" || token == "streaming")
        {
            priorities[idx] = 2;
        }
        else if (token == "voice" || token == "vo" || token == "ef" || token == "realtime" ||
                 token == "real-time")
        {
            priorities[idx] = 3;
        }
        else
        {
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
    // Default params
    int freq1 = 5;
    int freq2 = 6;
    std::string dataRateStr = "150Mbps"; // Per-STA (reduzido para evitar saturação com 4 STAs)
    double simTime = 12.0;
    bool enablePcaps = false;
    std::string protocol = "UDP";
    double staDistance = STA_DISTANCE;
    bool useStaticSetup = true;
    double queueSampleInterval = 0.1;
    double linkTrafficSampleInterval = 0.1;
    uint32_t beMaxAmpduBytes = 65535;
    uint32_t viMaxAmpduBytes = 65535;
    uint32_t voMaxAmpduBytes = 65535;
    uint32_t bkMaxAmpduBytes = 0;
    std::string queueOccupancyCsvPath;
    std::string queueOccupancyLabel;
    std::string linkTrafficCsvPath;
    std::string frameDistributionCsvPath;
    std::string staTrafficTypesCsv = "best-effort,voice,voice,video";

    uint32_t numStas = 4; // Number of STAs (configurable)

    CommandLine cmd;
    cmd.AddValue("freq1", "First link frequency (2,5,6)", freq1);
    cmd.AddValue("freq2", "Second link frequency (2,5,6)", freq2);
    cmd.AddValue("dataRate", "Per-STA application data rate", dataRateStr);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.AddValue("enablePcaps", "Enable PCAP captures", enablePcaps);
    cmd.AddValue("protocol", "Application protocol: TCP or UDP", protocol);
    cmd.AddValue("distance", "Distance from STAs to AP (m)", staDistance);
    cmd.AddValue("staticSetup", "Use WifiStaticSetupHelper", useStaticSetup);
    cmd.AddValue("numStas", "Number of STAs (2, 4, 8, 16, etc.)", numStas);
    cmd.AddValue("staTrafficTypes", "CSV por STA (voice,video,besteffort,background) ou classes 0..3", staTrafficTypesCsv);
    cmd.AddValue("beMaxAmpdu", "BE A-MPDU max bytes (0=disabled)", beMaxAmpduBytes);
    cmd.AddValue("viMaxAmpdu", "VI A-MPDU max bytes (0=disabled)", viMaxAmpduBytes);
    cmd.AddValue("voMaxAmpdu", "VO A-MPDU max bytes (0=disabled, default)", voMaxAmpduBytes);
    cmd.AddValue("bkMaxAmpdu", "BK A-MPDU max bytes (0=disabled)", bkMaxAmpduBytes);
    cmd.AddValue("queueSampleInterval", "Queue occupancy sampling interval (s)", queueSampleInterval);
    cmd.AddValue("linkTrafficSampleInterval", "Per-link per-STA traffic sampling interval (s)", linkTrafficSampleInterval);
    cmd.AddValue("queueOccupancyCsv", "CSV file for MAC queue occupancy samples", queueOccupancyCsvPath);
    cmd.AddValue("queueOccupancyLabel", "Label prefix for MAC queue occupancy samples", queueOccupancyLabel);
    cmd.AddValue("linkTrafficCsv", "CSV file for per-link per-STA traffic samples", linkTrafficCsvPath);
    cmd.AddValue("frameDistributionCsv", "CSV file for link frame distribution samples", frameDistributionCsvPath);
    cmd.AddValue("useCustomMloScheduler", "Enable custom MLO scheduler (VO/VI prefer fast link, BE/BK prefer slow link)", g_useCustomMloScheduler);
    cmd.AddValue("schedulerDecisionCsv", "CSV file for scheduler decisions", g_schedulerDecisionCsvPath);
    cmd.AddValue("stayThreshold", "Satisfaction index above which the AP scheduler keeps the current link (0..1)", g_stayThreshold);
    cmd.AddValue("migrationThreshold", "Minimum satisfaction gain required to trigger link migration (0..1)", g_migrationThreshold);
    cmd.AddValue("voDelayWeight", "VO Delay Weight", g_voDelayWeight);
    cmd.AddValue("voJitterWeight", "VO Jitter Weight", g_voJitterWeight);
    cmd.AddValue("voLossWeight", "VO Loss Weight", g_voLossWeight);
    cmd.AddValue("voTpWeight", "VO Throughput Weight", g_voTpWeight);
    cmd.AddValue("viDelayWeight", "VI Delay Weight", g_viDelayWeight);
    cmd.AddValue("viJitterWeight", "VI Jitter Weight", g_viJitterWeight);
    cmd.AddValue("viLossWeight", "VI Loss Weight", g_viLossWeight);
    cmd.AddValue("viTpWeight", "VI Throughput Weight", g_viTpWeight);
    cmd.AddValue("beDelayWeight", "BE Delay Weight", g_beDelayWeight);
    cmd.AddValue("beJitterWeight", "BE Jitter Weight", g_beJitterWeight);
    cmd.AddValue("beLossWeight", "BE Loss Weight", g_beLossWeight);
    cmd.AddValue("beTpWeight", "BE Throughput Weight", g_beTpWeight);
    cmd.AddValue("bkDelayWeight", "BK Delay Weight", g_bkDelayWeight);
    cmd.AddValue("bkJitterWeight", "BK Jitter Weight", g_bkJitterWeight);
    cmd.AddValue("bkLossWeight", "BK Loss Weight", g_bkLossWeight);
    cmd.AddValue("bkTpWeight", "BK Throughput Weight", g_bkTpWeight);
    cmd.AddValue("perStaMetricsCsv",
                 "CSV path for per-second per-STA QoS metrics time series",
                 g_perStaMetricsCsvPath);
    cmd.Parse(argc, argv);

    if (queueSampleInterval <= 0.0)
    {
        queueSampleInterval = 0.1;
    }

    if (linkTrafficSampleInterval <= 0.0)
    {
        linkTrafficSampleInterval = 0.1;
    }

    // Set global numStas and initialize vectors
    g_numStas = numStas;
    g_delaySumMs.resize(g_numStas, 0.0);
    g_delaySamples.resize(g_numStas, 0);
    g_jitterSumMs.resize(g_numStas, 0.0);
    g_jitterSamples.resize(g_numStas, 0);
    g_lastDelayMs.resize(g_numStas, 0.0);
    g_firstPacket.resize(g_numStas, true);
    g_lastTotalRx.resize(g_numStas, 0);
    g_lastDelaySumMs.resize(g_numStas, 0.0);
    g_lastDelaySamples.resize(g_numStas, 0);
    g_lastJitterSumMs.resize(g_numStas, 0.0);
    g_lastJitterSamples.resize(g_numStas, 0);
    g_lastStaTx.resize(g_numStas, 0);
    g_lastStaRx.resize(g_numStas, 0);
    g_queueOccupancyCsvPath = queueOccupancyCsvPath;
    g_queueSampleInterval = queueSampleInterval;
    g_linkTrafficCsvPath = linkTrafficCsvPath;
    g_linkTrafficSampleInterval = linkTrafficSampleInterval;
    g_frameDistributionCsvPath = frameDistributionCsvPath;
    g_frameDistributionRunLabel = std::to_string(freq1) + "+" + std::to_string(freq2) + "|" + protocol;
    g_linkTrafficRunLabel = g_frameDistributionRunLabel;
    g_protocol = protocol;
    g_linkTrafficPairLabel = std::to_string(freq1) + "+" + std::to_string(freq2);
    g_linkFrequencyPair = g_linkTrafficPairLabel;
    g_linkTrafficEndTime = Seconds(simTime);
    g_frameDistributionEndTime = Seconds(simTime);
    const std::vector<uint32_t> staPriorities = ParseStaTrafficTypes(staTrafficTypesCsv, g_numStas);

    if (freq1 == freq2) {
        NS_FATAL_ERROR("freq1 and freq2 must be different");
    }

    std::string appSocketType;
    if (protocol == "TCP") {
        appSocketType = "ns3::TcpSocketFactory";
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

    NS_LOG_UNCOND("wifi7-mlo-multi-sta: freq1=" << freq1 << " freq2=" << freq2 << " proto=" << protocol
                  << " rate=" << dataRateStr << " numSTAs=" << g_numStas << " distance=" << staDistance << "m"
                  << " staticSetup=" << (useStaticSetup ? "yes" : "no")
                  << " staTrafficTypesCsv=" << staTrafficTypesCsv);

    for (uint32_t i = 0; i < g_numStas; ++i)
    {
        NS_LOG_UNCOND("STA_PRIORITY_CLASS: Sta=" << i
                      << " Class=" << staPriorities[i]
                      << " AC=" << PriorityClassToAcName(staPriorities[i])
                      << " TOS=" << static_cast<uint32_t>(PriorityClassToTos(staPriorities[i])));
    }

    

    if (!g_frameDistributionCsvPath.empty())
    {
        g_frameDistributionStream.open(g_frameDistributionCsvPath, std::ios::out | std::ios::trunc);
        if (!g_frameDistributionStream.is_open())
        {
            NS_FATAL_ERROR("Could not open frame distribution CSV: " << g_frameDistributionCsvPath);
        }
        g_frameDistributionStream << "run_label,time_bucket_s,link_id,link_name,ac,frame_type,frame_count,bytes\n";
        g_frameDistributionStream.flush();
        Simulator::Schedule(Seconds(g_frameDistributionSampleInterval), &ScheduleFrameDistributionFlush);
    }

    // Criar nós: 1 AP + 2 STAs
    NodeContainer apNode;
    apNode.Create(1);
    NodeContainer staNodes;
    staNodes.Create(g_numStas);

    // Wifi helper
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211be);
    wifi.ConfigHeOptions("GuardInterval", TimeValue(NanoSeconds(800)));
    wifi.ConfigEhtOptions("EmlsrActivated", BooleanValue(false));

    // PHY (2 links para MLO)
    SpectrumWifiPhyHelper phy(2);
    phy.Set("Antennas", UintegerValue(2));
    phy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    phy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    // Canais para cada link
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
    phy.Set("NotifyMacHdrRxEnd", BooleanValue(true));

    // Rate manager para cada link
    // wifi.SetRemoteStationManager(0, std::string("ns3::IdealWifiManager"));
    // wifi.SetRemoteStationManager(1, std::string("ns3::IdealWifiManager"));
    wifi.SetRemoteStationManager(0, std::string("ns3::IdealWifiManager"));
    wifi.SetRemoteStationManager(1, std::string("ns3::IdealWifiManager"));

    // MAC
    Ssid ssid = Ssid("wifi7-mlo-multi-sta");
    
    WifiMacHelper apMac;
    
    apMac.SetType("ns3::ApWifiMac", 
                  "Ssid", SsidValue(ssid), 
                  "QosSupported", BooleanValue(true));
    
    WifiMacHelper staMac;
    staMac.SetType("ns3::StaWifiMac", 
                   "Ssid", SsidValue(ssid), 
                   "QosSupported", BooleanValue(true), 
                   "ActiveProbing", BooleanValue(false));

    NetDeviceContainer apDev = wifi.Install(phy, apMac, apNode);
    NetDeviceContainer staDev = wifi.Install(phy, staMac, staNodes);

    // ===== CONFIGURAR A-MPDU PARA CADA ACCESS CATEGORY =====
    Ptr<WifiNetDevice> apAmpduDev = DynamicCast<WifiNetDevice>(apDev.Get(0));
    apAmpduDev->GetMac()->SetAttribute("BE_MaxAmpduSize", UintegerValue(beMaxAmpduBytes));
    apAmpduDev->GetMac()->SetAttribute("VI_MaxAmpduSize", UintegerValue(viMaxAmpduBytes));
    apAmpduDev->GetMac()->SetAttribute("VO_MaxAmpduSize", UintegerValue(voMaxAmpduBytes));
    apAmpduDev->GetMac()->SetAttribute("BK_MaxAmpduSize", UintegerValue(bkMaxAmpduBytes));

    for (uint32_t i = 0; i < staDev.GetN(); ++i)
    {
        Ptr<WifiNetDevice> staAmpduDev = DynamicCast<WifiNetDevice>(staDev.Get(i));
        staAmpduDev->GetMac()->SetAttribute("BE_MaxAmpduSize", UintegerValue(beMaxAmpduBytes));
        staAmpduDev->GetMac()->SetAttribute("VI_MaxAmpduSize", UintegerValue(viMaxAmpduBytes));
        staAmpduDev->GetMac()->SetAttribute("VO_MaxAmpduSize", UintegerValue(voMaxAmpduBytes));
        staAmpduDev->GetMac()->SetAttribute("BK_MaxAmpduSize", UintegerValue(bkMaxAmpduBytes));
    }

    // ===== CONECTAR TRACE SOURCES PARA PACKET LOSS GRANULAR (MLO - 2 links) =====
    Ptr<WifiNetDevice> apWifiNetDev = DynamicCast<WifiNetDevice>(apDev.Get(0));
    g_apAddress = apWifiNetDev->GetMac()->GetAddress();
    g_apRxAddresses.clear();
    g_apLinkAddressToLinkId.clear();
    g_apRxAddresses.insert(g_apAddress);
    for (uint8_t linkId = 0; linkId < apWifiNetDev->GetNPhys(); ++linkId)
    {
        const auto linkAddress = apWifiNetDev->GetMac()->GetFrameExchangeManager(linkId)->GetAddress();
        g_apRxAddresses.insert(linkAddress);
        g_apLinkAddressToLinkId[MacAddressToString(linkAddress)] = linkId;
    }
    
    // PHY traces (AP) - para cada link
    for (uint8_t linkId = 0; linkId < apWifiNetDev->GetNPhys(); ++linkId) {
        g_linkTxDepth[linkId] = 0;
        g_linkTxTimeSec[linkId] = 0.0;
        g_linkMuTxCount[linkId] = 0;
        g_linkSuTxCount[linkId] = 0;

        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext(
            "PhyTxBegin",
            MakeBoundCallback(&LinkPhyTxBeginCallback, linkId));
        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext(
            "PhyTxEnd",
            MakeBoundCallback(&LinkPhyTxEndCallback, linkId));
        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext(
            "PhyTxPsduBegin",
            MakeBoundCallback(&LinkPhyTxPsduBeginCallback, linkId));
        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext(
            "PhyTxPsduBegin",
            MakeBoundCallback(&ApControlTxPsduBeginCallback, linkId));
        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext(
            "PhyRxMacHeaderEnd",
            MakeBoundCallback(&ApControlRxMacHeaderEndCallback, linkId));

        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyTxDrop", MakeCallback(&PhyTxDropCallback));
        apWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyRxDrop", MakeCallback(&PhyRxDropCallback));
    }

    /*
    // MAC traces (AP) - Queue drops para cada AC
    Ptr<WifiMac> apMacPtr = apWifiNetDev->GetMac();

    apMacPtr->TraceConnectWithoutContext("AckedMpdu", MakeCallback(&RecordLinkTrafficFromMpdu));

    for (auto ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        Ptr<QosTxop> qosTxop = apMacPtr->GetQosTxop(ac);
        if (qosTxop) {
            Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
            if (queue) {
                queue->TraceConnectWithoutContext("DropBeforeEnqueue", MakeCallback(&WifiQueueDropCallback));
                queue->TraceConnectWithoutContext("Expired", MakeCallback(&WifiQueueDropCallback));
            }
        }
    }*/

    //NOVO
    // MAC traces (AP)
    Ptr<WifiMac> apMacPtr = apWifiNetDev->GetMac();
    apMacPtr->TraceConnectWithoutContext("AckedMpdu", MakeCallback(&RecordLinkTrafficFromMpdu));

    // Ligar o MAC TX Drop real (que faltava no teu script)
    apMacPtr->TraceConnectWithoutContext("MacTxDrop", 
        MakeCallback(&MacTxDropCallbackReal));

    for (auto ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        Ptr<QosTxop> qosTxop = apMacPtr->GetQosTxop(ac);
        if (qosTxop) {
            Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
            if (queue) {
                // Passamos o 'ac' para a nova função com MakeBoundCallback
                queue->TraceConnectWithoutContext("DropBeforeEnqueue", 
                    MakeBoundCallback(&WifiQueueDropCallbackReal, ac));
                queue->TraceConnectWithoutContext("Expired", 
                    MakeBoundCallback(&WifiQueueDropCallbackReal, ac));
            }
        }
    }
    
    // STA device traces (para cada STA e cada link)
    for (uint32_t i = 0; i < staDev.GetN(); ++i) {
        Ptr<WifiNetDevice> staWifiNetDev = DynamicCast<WifiNetDevice>(staDev.Get(i));
        
        // PHY traces para cada link
        for (uint8_t linkId = 0; linkId < staWifiNetDev->GetNPhys(); ++linkId) {
            staWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyTxDrop", MakeCallback(&PhyTxDropCallback));
            staWifiNetDev->GetPhy(linkId)->TraceConnectWithoutContext("PhyRxDrop", MakeCallback(&PhyRxDropCallback));
        }
        
        // MAC traces
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
    NS_LOG_UNCOND("Granular packet loss tracing enabled for MLO (PHY/MAC/Queue drops on both links)");

    g_queueOccupancyTargets.clear();
    g_queueOccupancyTargets.push_back({"AP", 0, apMacPtr});
    for (uint32_t i = 0; i < staDev.GetN(); ++i)
    {
        Ptr<WifiNetDevice> staWifiNetDev = DynamicCast<WifiNetDevice>(staDev.Get(i));
        if (staWifiNetDev)
        {
            g_queueOccupancyTargets.push_back({"STA", i, staWifiNetDev->GetMac()});
        }
    }

    if (!g_linkTrafficCsvPath.empty())
    {
           NS_LOG_UNCOND("Opening linkTrafficCsvPath: " << g_linkTrafficCsvPath);
           g_linkTrafficStream.open(g_linkTrafficCsvPath, std::ios::out | std::ios::app);
        if (!g_linkTrafficStream.is_open())
        {
            NS_FATAL_ERROR("Could not open link traffic CSV: " << g_linkTrafficCsvPath);
        }
        if (g_linkTrafficStream.tellp() == std::streampos(0))
        {
            g_linkTrafficStream << "run_label,time_bucket_s,pair,protocol,link_id,link_name,sta_id,sta_address,ac,frame_type,frame_count,bytes\n";
            g_linkTrafficStream.flush();
        }
        Simulator::Schedule(Seconds(g_linkTrafficSampleInterval), &ScheduleLinkTrafficFlush);
    }

    if (!g_queueOccupancyCsvPath.empty())
    {
        g_queueOccupancyStream.open(g_queueOccupancyCsvPath, std::ios::out | std::ios::app);
        if (!g_queueOccupancyStream.is_open())
        {
            NS_FATAL_ERROR("Could not open queue occupancy CSV: " << g_queueOccupancyCsvPath);
        }
        if (g_queueOccupancyStream.tellp() == std::streampos(0))
        {
            g_queueOccupancyStream << "run_label,time_s,role,node_id,ac,packets,bytes\n";
        }
        g_queueOccupancyRunLabel = queueOccupancyLabel.empty()
                                       ? std::to_string(freq1) + "+" + std::to_string(freq2) + "|" + protocol
                                       : queueOccupancyLabel + "|" + std::to_string(freq1) + "+" + std::to_string(freq2) + "|" + protocol;
        g_queueOccupancyEndTime = Seconds(simTime);
    }

    if (!g_perStaMetricsCsvPath.empty())
    {
        g_perStaMetricsStream.open(g_perStaMetricsCsvPath, std::ios::out | std::ios::app);
        if (!g_perStaMetricsStream.is_open())
        {
            NS_FATAL_ERROR("Could not open per-STA metrics CSV: " << g_perStaMetricsCsvPath);
        }
        if (g_perStaMetricsStream.tellp() == std::streampos(0))
        {
            g_perStaMetricsStream
                << "run_label,time_s,sta_id,ac,throughput_mbps,delay_ms,jitter_ms,loss_pct\n";
        }
        g_perStaMetricsRunLabel =
            queueOccupancyLabel.empty()
                ? std::to_string(freq1) + "+" + std::to_string(freq2) + "|" + protocol
                : queueOccupancyLabel + "|" + std::to_string(freq1) + "+" + std::to_string(freq2) +
                      "|" + protocol;
    }

    // ===== CONFIGURAÇÃO ESTÁTICA (WifiStaticSetupHelper) =====
    // Evita colisões durante ML setup quando múltiplas STAs tentam associar-se simultaneamente
    if (useStaticSetup) {
        NS_LOG_UNCOND("Using WifiStaticSetupHelper for static ML association...");
        
        Ptr<WifiNetDevice> apWifiDev = DynamicCast<WifiNetDevice>(apDev.Get(0));
        
        // Configurar associação ML estática para todas as STAs
        WifiStaticSetupHelper::SetStaticAssociation(apWifiDev, staDev);
        
        // Configurar Block ACK agreements estaticamente para BE, VI e VO
        WifiStaticSetupHelper::SetStaticBlockAck(apWifiDev, staDev, {0, 4, 5, 6, 7});
        
        NS_LOG_UNCOND("Static ML association and Block ACK configured for " << g_numStas << " STAs on both links");
    }

    // ===== APPLY QoS-AWARE GOAL-ORIENTED MLO SCHEDULER (AP ONLY) =====
    // STAs use the default FcfsWifiQueueScheduler — goal-oriented logic is AP-only (downlink).
    if (g_useCustomMloScheduler)
    {
        NS_LOG_UNCOND("===== MLO GOAL-ORIENTED SCHEDULER ENABLED (AP only) =====");
        NS_LOG_UNCOND("Policy: Goal-Oriented QoS satisfaction with 5 internal engines");
        NS_LOG_UNCOND("Thresholds: StayThreshold=" << g_stayThreshold
                      << " MigrationThreshold=" << g_migrationThreshold);
        NS_LOG_UNCOND("Weights: VO(delay=0.40, jitter=0.30, loss=0.25, tp=0.05)");
        NS_LOG_UNCOND("         VI(delay=0.25, jitter=0.15, loss=0.20, tp=0.40)");
        NS_LOG_UNCOND("         BE(delay=0.10, jitter=0.05, loss=0.15, tp=0.70)");
        NS_LOG_UNCOND("         BK(delay=0.05, jitter=0.05, loss=0.10, tp=0.80)");

        Ptr<WifiNetDevice> apWifiDev = DynamicCast<WifiNetDevice>(apDev.Get(0));
        g_apScheduler = InstallQosWeightedScheduler(apWifiDev->GetMac(), freq1, freq2);
        if (g_apScheduler) {
            // Set decision thresholds
            g_apScheduler->SetAttribute("StayThreshold",
                                        DoubleValue(g_stayThreshold));
            g_apScheduler->SetAttribute("MigrationThreshold",
                                        DoubleValue(g_migrationThreshold));

            // QoS weights
            g_apScheduler->SetWeights(AC_VO, g_voDelayWeight, g_voJitterWeight, g_voLossWeight, g_voTpWeight);
            g_apScheduler->SetWeights(AC_VI, g_viDelayWeight, g_viJitterWeight, g_viLossWeight, g_viTpWeight);
            g_apScheduler->SetWeights(AC_BE, g_beDelayWeight, g_beJitterWeight, g_beLossWeight, g_beTpWeight);
            g_apScheduler->SetWeights(AC_BK, g_bkDelayWeight, g_bkJitterWeight, g_bkLossWeight, g_bkTpWeight);

            if (!g_schedulerDecisionCsvPath.empty()) {
                g_apScheduler->EnableDecisionCsv(g_schedulerDecisionCsvPath, "AP");
            }
        }

        // STAs: use default FCFS scheduler (no custom installation needed;
        // FcfsWifiQueueScheduler is already the ns-3 default).
        NS_LOG_UNCOND("STAs: using default FcfsWifiQueueScheduler (AP-only scope)");
        NS_LOG_UNCOND("Goal-Oriented MLO scheduler installation complete (AP only)");
    }
    else
    {
        NS_LOG_UNCOND("===== MLO GOAL-ORIENTED SCHEDULER DISABLED =====");
        NS_LOG_UNCOND("Using the default ns-3 FCFS scheduler on all nodes");
    }


    // ===== MOBILIDADE (STAs em círculo) =====
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    
    // Posicionar AP no centro
    mobility.Install(apNode);
    apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
    NS_LOG_UNCOND("AP position: (0, 0, 0) - MLO with links at " << freq1 << "GHz + " << freq2 << "GHz");
    
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
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifAp = address.Assign(apDev);
    Ipv4InterfaceContainer ifSta = address.Assign(staDev);
    // ifAp[0] = AP, ifSta[0..3] = STAs

    g_staAddressToIndex.clear();
    g_staIndexToMacs.assign(staDev.GetN(), {});
    g_staAc.assign(staDev.GetN(), AC_BE);
    for (uint32_t i = 0; i < staDev.GetN(); ++i)
    {
        Ptr<WifiNetDevice> staWifiNetDev = DynamicCast<WifiNetDevice>(staDev.Get(i));
        if (staWifiNetDev)
        {
            Mac48Address mldMac = Mac48Address::ConvertFrom(staWifiNetDev->GetMac()->GetAddress());
            g_staAddressToIndex[MacAddressToString(staWifiNetDev->GetMac()->GetAddress())] = i;
            g_staIndexToMacs[i].push_back(mldMac);
            for (uint8_t linkId = 0; linkId < staWifiNetDev->GetNPhys(); ++linkId)
            {
                Mac48Address linkMac = Mac48Address::ConvertFrom(
                    staWifiNetDev->GetMac()->GetFrameExchangeManager(linkId)->GetAddress());
                g_staAddressToIndex[MacAddressToString(
                    staWifiNetDev->GetMac()->GetFrameExchangeManager(linkId)->GetAddress())] = i;
                g_staIndexToMacs[i].push_back(linkMac);
            }
            if (i < staPriorities.size())
                g_staAc[i] = PriorityClassToAc(staPriorities[i]);
            g_staIpToIndex[ifSta.GetAddress(i)] = i; // para loss real do FlowMonitor
        }
    }

    // ===== TRAFFIC CONTROL LAYER DROP TRACES =====
    Ptr<TrafficControlLayer> tcAp = apNode.Get(0)->GetObject<TrafficControlLayer>();
    if (tcAp) {
        tcAp->TraceConnectWithoutContext("TcDrop", MakeCallback(&TcDropCallback));
        
        Ptr<QueueDisc> rootQdAp = tcAp->GetRootQueueDiscOnDevice(apDev.Get(0));
        if (rootQdAp) {
            rootQdAp->TraceConnectWithoutContext("DropBeforeEnqueue", MakeCallback(&TcDropBeforeEnqueueCallback));
            rootQdAp->TraceConnectWithoutContext("DropAfterDequeue", MakeCallback(&TcDropAfterDequeueCallback));
        }
    }
    
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
    NS_LOG_UNCOND("Traffic Control layer drop tracing enabled for MLO");

    // ===== CONFIGURAÇÃO ARP ESTÁTICA =====
    if (useStaticSetup) {
        NeighborCacheHelper neighborCache;
        neighborCache.PopulateNeighborCache();
        NS_LOG_UNCOND("Static ARP cache populated");
    }

    // ===== APLICAÇÕES =====
    // Tráfego downlink: AP envia para cada STA
    uint16_t basePort = 5001;
    std::vector<Ptr<PacketSink>> sinks(g_numStas);
    uint32_t payloadSize = 1440;
    
    for (uint32_t i = 0; i < g_numStas; ++i) {
        uint16_t port = basePort + i;
        
        // Sink na STA
        Address sinkAddress(InetSocketAddress(ifSta.GetAddress(i), port));
        PacketSinkHelper sinkHelper(appSocketType, sinkAddress);
        ApplicationContainer sinkApp = sinkHelper.Install(staNodes.Get(i));
        sinkApp.Start(Seconds(1.0));
        sinkApp.Stop(Seconds(simTime + 0.5));
        sinks[i] = DynamicCast<PacketSink>(sinkApp.Get(0));
        
        // Conectar callback de RX
        uint32_t staIdx = i;
        sinks[i]->TraceConnectWithoutContext("Rx", 
            MakeBoundCallback(&MonitorPacketSinkRx, staIdx));
        
        // OnOff sender no AP
        OnOffHelper onoff(appSocketType, sinkAddress);
        onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
        onoff.SetAttribute("DataRate", DataRateValue(DataRate(dataRateStr)));
        onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        onoff.SetAttribute("EnableSeqTsSizeHeader", BooleanValue(true));
        onoff.SetAttribute("Tos", UintegerValue(PriorityClassToTos(staPriorities[i])));

        ApplicationContainer app = onoff.Install(apNode.Get(0));
        //Início ligeiramente diferente para cada STA
        app.Start(Seconds(1.5) + MilliSeconds(i * 10));
        app.Stop(Seconds(simTime - 0.5));
        
        NS_LOG_UNCOND("App configured: AP -> STA" << i << " (port " << port << ")"
              << " AC=" << PriorityClassToAcName(staPriorities[i])
              << " TOS=" << static_cast<uint32_t>(PriorityClassToTos(staPriorities[i])));
    }

    // PCAP (opcional)
    if (enablePcaps) {
        std::string pref = "mlo_multi_" + std::to_string(freq1) + "_" + std::to_string(freq2) + "_" + protocol;
        phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
        phy.SetPcapCaptureType(WifiPhyHelper::PcapCaptureType::PCAP_PER_DEVICE);
        phy.EnablePcap(pref + "_ap", apDev.Get(0));
        for (uint32_t i = 0; i < g_numStas; ++i) {
            phy.EnablePcap(pref + "_sta" + std::to_string(i), staDev.Get(i));
        }
    }

    // Flow monitor
    FlowMonitorHelper fm;
    Ptr<FlowMonitor> monitor = fm.InstallAll();
    g_flowMonitor = monitor;
    g_flowClassifier = DynamicCast<Ipv4FlowClassifier>(fm.GetClassifier());

    // Estatísticas periódicas
    Simulator::Schedule(Seconds(2), &CalculateStats, sinks);
    // FeedLinkUtilizationToSchedulers removed — utilization computed internally by scheduler
    if (!g_queueOccupancyCsvPath.empty())
    {
        Simulator::Schedule(Seconds(0), &RecordQueueOccupancySample);
    }

    Simulator::Stop(Seconds(simTime + 1.0));
    Simulator::Run();

    // ===== RESULTADOS FINAIS =====
    UpdateLinkActivityAccounting();
    FlushLinkTrafficData();
    FlushFrameDistributionData();

    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(fm.GetClassifier());
    auto stats = monitor->GetFlowStats();

    double totalThroughput = 0.0;
    uint64_t totalRxBytes = 0;
    uint64_t totalTxPackets = 0;
    uint64_t totalRxPackets = 0;

    double totalDelay = 0.0;
    double totalJitter = 0.0;
    uint64_t totalLostPackets = 0;
    uint32_t validStas = 0;

    for (const auto &kv : stats) {
        FlowId id = kv.first;
        const FlowMonitor::FlowStats &st = kv.second;
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(id);
        
        // Apenas fluxos downlink do AP
        if (t.sourceAddress == ifAp.GetAddress(0)) {
            double duration = st.timeLastRxPacket.GetSeconds() - st.timeFirstTxPacket.GetSeconds();
            double tp = (duration > 0) ? (st.rxBytes * 8.0) / (duration * 1e6) : 0.0;
            
            // Calcular delay e jitter médios do fluxo
            double delayMs = (st.rxPackets > 0) ? (st.delaySum.GetSeconds() * 1000.0 / st.rxPackets) : 0.0;
            double jitterMs = (st.rxPackets > 1) ? (st.jitterSum.GetSeconds() * 1000.0 / (st.rxPackets - 1)) : 0.0;
            uint64_t lostPackets = st.txPackets - st.rxPackets;
            double lossRate = (st.txPackets > 0) ? (lostPackets * 100.0 / st.txPackets) : 0.0;
            
            // Identificar qual STA é o destino
            for (uint32_t i = 0; i < g_numStas; ++i) {
                if (t.destinationAddress == ifSta.GetAddress(i)) {
                    NS_LOG_UNCOND("FLOW_SUMMARY_STA" << i << ": pair=" << freq1 << "+" << freq2 
                                  << " proto=" << protocol << " Throughput_Mbps=" << tp 
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
    
    NS_LOG_UNCOND("FLOW_SUMMARY_TOTAL: pair=" << freq1 << "+" << freq2 << " proto=" << protocol 
                  << " numSTAs=" << g_numStas << " Throughput_Mbps=" << totalThroughput 
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
    
    NS_LOG_UNCOND("PACKET_LOSS_BREAKDOWN: pair=" << freq1 << "+" << freq2 << " proto=" << protocol
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

    const double observedTimeSec = Simulator::Now().GetSeconds();
    const double overlapPct = (observedTimeSec > 0.0) ? (g_overlapBothLinksSec * 100.0 / observedTimeSec) : 0.0;
    for (const auto& kv : g_linkTxDepth)
    {
        const uint8_t linkId = kv.first;
        const double txTimeSec = g_linkTxTimeSec[linkId];
        const double dutyPct = (observedTimeSec > 0.0) ? (txTimeSec * 100.0 / observedTimeSec) : 0.0;

        NS_LOG_UNCOND("MLO_LINK_ACTIVITY: pair=" << freq1 << "+" << freq2
                      << " proto=" << protocol
                      << " Link=" << +linkId
                      << " TxTime_s=" << txTimeSec
                      << " Duty_pct=" << dutyPct
                      << " OverlapTime_s=" << g_overlapBothLinksSec
                      << " Overlap_pct=" << overlapPct
                      << " MuTxCount=" << g_linkMuTxCount[linkId]
                      << " SuTxCount=" << g_linkSuTxCount[linkId]);

        for (const auto& ruKv : g_linkRuTypeCount[linkId])
        {
            NS_LOG_UNCOND("MLO_RU_ALLOCATION: pair=" << freq1 << "+" << freq2
                          << " proto=" << protocol
                          << " Link=" << +linkId
                          << " RuType=" << ruKv.first
                          << " Count=" << ruKv.second);
        }
    }

    uint64_t totalMatched = 0;
    uint64_t totalSameLink = 0;
    uint64_t totalCrossLink = 0;
    uint64_t totalUnmatched = 0;

    for (const auto& kv : g_ctrlResponseStats)
    {
        const auto& responseType = kv.first;
        const auto& s = kv.second;
        const double samePct = (s.matched > 0) ? (100.0 * s.sameLink / s.matched) : 0.0;

        NS_LOG_UNCOND("CONTROL_LINK_SUMMARY: pair=" << freq1 << "+" << freq2
                      << " proto=" << protocol
                      << " ResponseType=" << responseType
                      << " Matched=" << s.matched
                      << " SameLink=" << s.sameLink
                      << " CrossLink=" << s.crossLink
                      << " Unmatched=" << s.unmatched
                      << " SameLink_pct=" << samePct);

        totalMatched += s.matched;
        totalSameLink += s.sameLink;
        totalCrossLink += s.crossLink;
        totalUnmatched += s.unmatched;
    }

    const double totalSamePct = (totalMatched > 0) ? (100.0 * totalSameLink / totalMatched) : 0.0;
    NS_LOG_UNCOND("CONTROL_LINK_SUMMARY_TOTAL: pair=" << freq1 << "+" << freq2
                  << " proto=" << protocol
                  << " Matched=" << totalMatched
                  << " SameLink=" << totalSameLink
                  << " CrossLink=" << totalCrossLink
                  << " Unmatched=" << totalUnmatched
                  << " SameLink_pct=" << totalSamePct);

    NS_LOG_UNCOND("CONTROL_LINK_DEBUG: pair=" << freq1 << "+" << freq2
                  << " proto=" << protocol
                  << " TxRequestsRegistered=" << g_ctrlTxRequestsRegistered
                  << " RxControlSeen=" << g_ctrlRxControlSeen
                  << " PendingRequests=" << g_pendingCtrlRequests.size());

    if (g_linkTrafficStream.is_open())
    {
        g_linkTrafficStream.close();
    }
    if (g_frameDistributionStream.is_open())
    {
        g_frameDistributionStream.close();
    }

    if (g_perStaMetricsStream.is_open())
    {
        g_perStaMetricsStream.close();
    }

    if (g_apScheduler) {
        g_apScheduler->PrintFinalScores();
    }

    Simulator::Destroy();
    return 0;
}