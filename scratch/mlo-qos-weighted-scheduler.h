/*
 * QoS-Aware Weighted Metrics MLO Scheduler.
 *
 * This scheduler monitors metrics (throughput, delay, jitter, loss) for each link
 * and Access Category (AC) in real-time. It uses dynamic weighted scoring based on
 * real-world QoS targets to select the optimal transmission link.
 */
#ifndef MLO_QOS_WEIGHTED_SCHEDULER_H
#define MLO_QOS_WEIGHTED_SCHEDULER_H

#include "ns3/fcfs-wifi-queue-scheduler.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/wifi-mac.h"
#include "ns3/qos-txop.h"
#include "ns3/wifi-mac-queue.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>

namespace ns3
{

// QoS metrics weights for each Access Category
struct AcWeights {
    double delayWeight;       // Weight of latency/delay
    double jitterWeight;      // Weight of delay variation (jitter)
    double lossWeight;        // Weight of packet drops/loss
    double throughputWeight;  // Weight of throughput
};

// Real-world QoS targets/requirements for each Access Category
struct AcTargets {
    double maxDelayMs;        // Max acceptable delay (ms) - ITU-T G.114
    double maxJitterMs;       // Max acceptable jitter (ms)
    double maxLossRate;       // Max acceptable packet loss rate (ratio 0-1)
    double minThroughputMbps; // Min desired throughput (Mbps)
};

const double MAX_QUEUE_LENGTH = 500.0;
const double MAX_DELAY = 100.0;
const double MAX_HOL_DELAY = 100.0;
const double MAX_THROUGHPUT = 150.0;

// Internal metric counters and statistics per (link, AC)
struct LinkState {
    uint32_t queueLength{0};
    double avgQueueDelay{0.0};
    double holDelay{0.0};
    double packetErrorRate{0.0};
    double retransmissionRate{0.0};
    double channelUtilization{0.0};
    double averageSinr{0.0};
    uint8_t currentMcs{0};
    double achievedThroughput{0.0};
    double averageAccessDelay{0.0};
    uint64_t packetsAssigned{0};

    // Tracking variables for calculations
    uint64_t txBytes{0};
    uint64_t dropFrames{0};
    uint64_t enqueueCount{0};
    double lastEnqueueTimeMs{0.0};
    double lastDequeueTimeMs{0.0};
    uint32_t queueBytes{0};

    // Real per-link TX tracking for PER calculation
    uint64_t txAttempts{0};   // frames attempted (from PhyTxPsduBegin)
    uint64_t txSuccess{0};    // frames confirmed (from AckedMpdu)
};

struct AcState
{
    uint32_t queueLength{0};
    double averageDelay{0.0};
    double throughput{0.0};
    uint64_t packetsSent{0};
    uint64_t bytesSent{0};
};

class QosWeightedMloScheduler : public WifiMacQueueScheduler
{
  public:
    static TypeId GetTypeId();

    QosWeightedMloScheduler();
    ~QosWeightedMloScheduler() override;

    void ConfigureForPair(int freq1, int freq2);
    void SetWifiMac(Ptr<WifiMac> mac) override;

    // External interfaces for feeding metrics
    void FeedLinkMetrics(uint8_t linkId, AcIndex ac, uint32_t txBytes, uint32_t txFrames, uint32_t dropFrames);
    void FeedLinkDrop(uint8_t linkId, AcIndex ac);
    void FeedLinkTxAttempt(uint8_t linkId, AcIndex ac, uint32_t frames);
    void FeedLinkTxStart(uint8_t linkId);
    void FeedLinkTxEnd(uint8_t linkId);
    
    // Configuration interfaces
    void EnableDecisionCsv(const std::string& filename, const std::string& nodeContext);
    void SetWeights(AcIndex ac, double delay, double jitter, double loss, double tp);
    void SetTargets(AcIndex ac, double delayMs, double jitterMs, double lossRate, double tpMbps);

    // Core selection interface
    std::optional<WifiContainerQueueId> GetNext(AcIndex ac,
                                                std::optional<uint8_t> linkId,
                                                bool skipBlockedQueues = true) override;
    std::optional<WifiContainerQueueId> GetNext(AcIndex ac,
                                                std::optional<uint8_t> linkId,
                                                const WifiContainerQueueId& prevQueueId,
                                                bool skipBlockedQueues = true) override;
    
    // Core Link Selection Decision
    std::list<uint8_t> GetLinkIds(AcIndex ac,
                                  Ptr<const WifiMpdu> mpdu,
                                  const std::list<WifiQueueBlockedReason>& ignoredReasons = {}) override;

    // Delegations to FCFS Wifi Queue Scheduler delegate
    void BlockQueues(WifiQueueBlockedReason reason,
                     AcIndex ac,
                     const std::list<WifiContainerQueueType>& types,
                     const Mac48Address& rxAddress,
                     const Mac48Address& txAddress,
                     const std::set<uint8_t>& tids = {},
                     const std::set<uint8_t>& linkIds = {}) override;
    void UnblockQueues(WifiQueueBlockedReason reason,
                       AcIndex ac,
                       const std::list<WifiContainerQueueType>& types,
                       const Mac48Address& rxAddress,
                       const Mac48Address& txAddress,
                       const std::set<uint8_t>& tids = {},
                       const std::set<uint8_t>& linkIds = {}) override;
    void BlockAllQueues(WifiQueueBlockedReason reason,
                         const std::set<uint8_t>& linkIds = {}) override;
    void UnblockAllQueues(WifiQueueBlockedReason reason,
                           const std::set<uint8_t>& linkIds = {}) override;
    bool GetAllQueuesBlockedOnLink(uint8_t linkId,
                                   WifiQueueBlockedReason reason = WifiQueueBlockedReason::REASONS_COUNT) override;
    std::optional<Mask> GetQueueLinkMask(AcIndex ac,
                                         const WifiContainerQueueId& queueId,
                                         uint8_t linkId) override;

    // Dynamic metrics collection points
    Ptr<WifiMpdu> HasToDropBeforeEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu) override;
    void NotifyEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu) override;
    void NotifyDequeue(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus) override;
    void NotifyRemove(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus) override;

    // Debugging
    void PrintFinalScores();

  protected:
    void DoDispose() override;

  private:
    static int GetFrequencyRank(int frequency);
    //NOVO
    std::map<uint64_t, double> m_packetEnqueueTimes;
    std::map<uint8_t, double> m_lastSelectionTimeMs;
    
    // QoS Evaluation Engine
    double ComputeLinkScore(uint8_t linkId, AcIndex ac) const;
    double UtilityFunction(double value, double target, bool lowerIsBetter) const;
    uint8_t SelectBestLink(AcIndex ac, const std::list<uint8_t>& eligibleLinks);
    
    // Periodic update function for window-based metrics (throughput, drop rate)
    void UpdatePeriodicMetrics();
    void EnsureDelegate();

    Ptr<WifiMacQueueScheduler> m_delegate;
    
    // Internal metrics table: linkId -> (ac -> metrics)
    std::map<uint8_t, std::map<uint8_t, LinkState>> m_metrics;

    // QoS parameters per AC
    std::map<uint8_t, AcWeights> m_weights;
    std::map<uint8_t, AcTargets> m_targets;
    
    // Scheduler control variables
    double m_metricsIntervalSec{0.5}; // 500 ms periodic metrics update
    double m_lastPeriodicUpdateTime{0.0};
    EventId m_updateEvent;
    double m_hysteresisThreshold{0.10}; // 10% hysteresis
    std::map<uint8_t, uint8_t> m_lastSelectedLink;

    uint8_t m_slowLinkId{0};
    uint8_t m_fastLinkId{1};
    int m_freq1{5};
    int m_freq2{6};

    // AC State and credit variables for anti-starvation & fairness
    std::map<uint8_t, AcState> m_acStates;
    std::map<uint8_t, uint64_t> m_lastAcBytesSent;
    
    // Per-link airtime tracking for real channel utilization
    std::map<uint8_t, Time> m_linkTxStartTime;    // timestamp of last PhyTxBegin per link
    std::map<uint8_t, double> m_linkBusyTime;      // accumulated busy time (seconds) per link
    std::map<uint8_t, uint32_t> m_linkTxDepth;     // TX depth counter per link

    // Logging
    std::ofstream m_decisionCsv;
    std::string m_nodeContext;
    bool m_csvEnabled{false};
};

inline TypeId
QosWeightedMloScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::QosWeightedMloScheduler")
                            .SetParent<WifiMacQueueScheduler>()
                            .SetGroupName("Wifi")
                            .AddConstructor<QosWeightedMloScheduler>();
    return tid;
}

inline QosWeightedMloScheduler::QosWeightedMloScheduler()
    : m_delegate(CreateObject<FcfsWifiQueueScheduler>())
{
    // Define standard weights based on real-world QoS specifications
    
    // VO: Voice (low latency, low jitter, high reliability, throughput insensitive)
    m_weights[AC_VO] = {0.40, 0.30, 0.25, 0.05};
    m_targets[AC_VO] = {15.0, 1.0, 0.01, 150.0};
    //m_targets[AC_VO] = {150.0, 30.0, 0.01, 0.1}; // 150ms delay, 30ms jitter, 1% loss, 0.1 Mbps min tp

    // VI: Video (low delay, low jitter, high throughput, moderate loss tolerance)
    m_weights[AC_VI] = {0.25, 0.15, 0.20, 0.40};
    m_targets[AC_VI] = {30.0, 10.0, 0.01, 150.0};
    //m_targets[AC_VI] = {150.0, 50.0, 0.01, 5.0}; // 150ms delay, 50ms jitter, 1% loss, 5 Mbps min tp

    // BE: Best effort (throughput oriented, latency/jitter tolerant)
    m_weights[AC_BE] = {0.10, 0.05, 0.15, 0.70};
    m_targets[AC_BE] = {200.0, 50.0, 0.2, 150.0};
   // m_targets[AC_BE] = {500.0, 200.0, 0.05, 1.0}; // 500ms delay, 200ms jitter, 5% loss, 1 Mbps min tp

    // BK: Background (throughput oriented, very high delay/jitter tolerance)
    m_weights[AC_BK] = {0.05, 0.05, 0.10, 0.80};
    m_targets[AC_BK] = {300.0, 100.0, 0.10, 150.0};
    //m_targets[AC_BK] = {1000.0, 500.0, 0.10, 0.5}; // 1000ms delay, 500ms jitter, 10% loss, 0.5 Mbps min tp

    m_acStates[AC_VO] = {0, 0.0, 0.0, 0, 0};
    m_acStates[AC_VI] = {0, 0.0, 0.0, 0, 0};
    m_acStates[AC_BE] = {0, 0.0, 0.0, 0, 0};
    m_acStates[AC_BK] = {0, 0.0, 0.0, 0, 0};

    m_lastAcBytesSent[AC_VO] = 0;
    m_lastAcBytesSent[AC_VI] = 0;
    m_lastAcBytesSent[AC_BE] = 0;
    m_lastAcBytesSent[AC_BK] = 0;
}

inline QosWeightedMloScheduler::~QosWeightedMloScheduler()
{
    if (m_decisionCsv.is_open())
    {
        m_decisionCsv.close();
    }
}

inline void
QosWeightedMloScheduler::ConfigureForPair(int freq1, int freq2)
{
    m_freq1 = freq1;
    m_freq2 = freq2;
    const int rank1 = GetFrequencyRank(freq1);
    const int rank2 = GetFrequencyRank(freq2);

    if (rank1 >= rank2)
    {
        m_fastLinkId = 0;
        m_slowLinkId = 1;
    }
    else
    {
        m_fastLinkId = 1;
        m_slowLinkId = 0;
    }
    
    // Initialize metrics maps
    for (uint8_t linkId : {m_slowLinkId, m_fastLinkId}) {
        for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
            m_metrics[linkId][static_cast<uint8_t>(ac)] = LinkState();
        }
    }
}

inline void
QosWeightedMloScheduler::SetWeights(AcIndex ac, double delay, double jitter, double loss, double tp)
{
    m_weights[static_cast<uint8_t>(ac)] = {delay, jitter, loss, tp};
}

inline void
QosWeightedMloScheduler::SetTargets(AcIndex ac, double delayMs, double jitterMs, double lossRate, double tpMbps)
{
    m_targets[static_cast<uint8_t>(ac)] = {delayMs, jitterMs, lossRate, tpMbps};
}

inline void
QosWeightedMloScheduler::EnableDecisionCsv(const std::string& filename, const std::string& nodeContext)
{
    m_csvEnabled = true;
    m_nodeContext = nodeContext;
    m_decisionCsv.open(filename, std::ios::out | std::ios::app);
    
    // Write header if new file
    if (m_decisionCsv.is_open() && m_decisionCsv.tellp() == std::streampos(0))
    {
        m_decisionCsv << "Timestamp,NodeContext,AC,SelectedLink,Score,QueueLength,QueueDelay,HolDelay,PER,SINR,Throughput,Utilization\n";
    }
}

inline void
QosWeightedMloScheduler::FeedLinkMetrics(uint8_t linkId, AcIndex ac, uint32_t txBytes, uint32_t txFrames, uint32_t dropFrames)
{
    auto& metrics = m_metrics[linkId][static_cast<uint8_t>(ac)];
    metrics.txBytes += txBytes;
    metrics.txSuccess += txFrames;  // confirmed frames (from AckedMpdu)
    metrics.dropFrames += dropFrames;
}

inline void
QosWeightedMloScheduler::FeedLinkDrop(uint8_t linkId, AcIndex ac)
{
    m_metrics[linkId][static_cast<uint8_t>(ac)].dropFrames++;
}

inline void
QosWeightedMloScheduler::FeedLinkTxAttempt(uint8_t linkId, AcIndex ac, uint32_t frames)
{
    m_metrics[linkId][static_cast<uint8_t>(ac)].txAttempts += frames;
}

inline void
QosWeightedMloScheduler::FeedLinkTxStart(uint8_t linkId)
{
    if (m_linkTxDepth[linkId] == 0) {
        m_linkTxStartTime[linkId] = Simulator::Now();
    }
    m_linkTxDepth[linkId]++;
}

inline void
QosWeightedMloScheduler::FeedLinkTxEnd(uint8_t linkId)
{
    if (m_linkTxDepth[linkId] > 0) {
        m_linkTxDepth[linkId]--;
        if (m_linkTxDepth[linkId] == 0) {
            Time elapsed = Simulator::Now() - m_linkTxStartTime[linkId];
            m_linkBusyTime[linkId] += elapsed.GetSeconds();
        }
    }
}

inline void
QosWeightedMloScheduler::EnsureDelegate()
{
    if (!m_delegate)
    {
        m_delegate = CreateObject<FcfsWifiQueueScheduler>();
    }
}

inline void
QosWeightedMloScheduler::SetWifiMac(Ptr<WifiMac> mac)
{
    WifiMacQueueScheduler::SetWifiMac(mac);
    EnsureDelegate();
    m_delegate->SetWifiMac(mac);
    
    // Setup periodic metrics updates in simulator
    if (!m_updateEvent.IsPending()) {
        m_lastPeriodicUpdateTime = Simulator::Now().GetSeconds();
        m_updateEvent = Simulator::Schedule(Seconds(m_metricsIntervalSec), 
                                            &QosWeightedMloScheduler::UpdatePeriodicMetrics, this);
    }
}

inline std::optional<WifiContainerQueueId>
QosWeightedMloScheduler::GetNext(AcIndex ac,
                                       std::optional<uint8_t> linkId,
                                       bool skipBlockedQueues)
{
    EnsureDelegate();
    return m_delegate->GetNext(ac, linkId, skipBlockedQueues);
}

inline std::optional<WifiContainerQueueId>
QosWeightedMloScheduler::GetNext(AcIndex ac,
                                       std::optional<uint8_t> linkId,
                                       const WifiContainerQueueId& prevQueueId,
                                       bool skipBlockedQueues)
{
    EnsureDelegate();
    return m_delegate->GetNext(ac, linkId, prevQueueId, skipBlockedQueues);
}

inline std::list<uint8_t>
QosWeightedMloScheduler::GetLinkIds(AcIndex ac,
                                          Ptr<const WifiMpdu> mpdu,
                                          const std::list<WifiQueueBlockedReason>& ignoredReasons)
{
    EnsureDelegate();

    const auto eligibleLinks = m_delegate->GetLinkIds(ac, mpdu, ignoredReasons);
    if (eligibleLinks.empty())
    {
        return {};
    }

    // Dynamic QoS-Weighted metric selection!
    return {SelectBestLink(ac, eligibleLinks)};
}

inline void
QosWeightedMloScheduler::BlockQueues(WifiQueueBlockedReason reason,
                                           AcIndex ac,
                                           const std::list<WifiContainerQueueType>& types,
                                           const Mac48Address& rxAddress,
                                           const Mac48Address& txAddress,
                                           const std::set<uint8_t>& tids,
                                           const std::set<uint8_t>& linkIds)
{
    EnsureDelegate();
    m_delegate->BlockQueues(reason, ac, types, rxAddress, txAddress, tids, linkIds);
}

inline void
QosWeightedMloScheduler::UnblockQueues(WifiQueueBlockedReason reason,
                                             AcIndex ac,
                                             const std::list<WifiContainerQueueType>& types,
                                             const Mac48Address& rxAddress,
                                             const Mac48Address& txAddress,
                                             const std::set<uint8_t>& tids,
                                             const std::set<uint8_t>& linkIds)
{
    EnsureDelegate();
    m_delegate->UnblockQueues(reason, ac, types, rxAddress, txAddress, tids, linkIds);
}

inline void
QosWeightedMloScheduler::BlockAllQueues(WifiQueueBlockedReason reason,
                                              const std::set<uint8_t>& linkIds)
{
    EnsureDelegate();
    m_delegate->BlockAllQueues(reason, linkIds);
}

inline void
QosWeightedMloScheduler::UnblockAllQueues(WifiQueueBlockedReason reason,
                                                const std::set<uint8_t>& linkIds)
{
    EnsureDelegate();
    m_delegate->UnblockAllQueues(reason, linkIds);
}

inline bool
QosWeightedMloScheduler::GetAllQueuesBlockedOnLink(
    uint8_t linkId,
    WifiQueueBlockedReason reason)
{
    EnsureDelegate();
    return m_delegate->GetAllQueuesBlockedOnLink(linkId, reason);
}

inline std::optional<WifiMacQueueScheduler::Mask>
QosWeightedMloScheduler::GetQueueLinkMask(AcIndex ac,
                                                const WifiContainerQueueId& queueId,
                                                uint8_t linkId)
{
    EnsureDelegate();
    return m_delegate->GetQueueLinkMask(ac, queueId, linkId);
}

inline Ptr<WifiMpdu>
QosWeightedMloScheduler::HasToDropBeforeEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu)
{
    EnsureDelegate();
    Ptr<WifiMpdu> droppedMpdu = m_delegate->HasToDropBeforeEnqueue(ac, mpdu);
    if (droppedMpdu) {
        // Global enqueue drop (not per-link yet as we don't know the link)
        // Add to enqueueCount to allow loss rate calculation
        for (auto& [linkId, acMap] : m_metrics) {
            acMap[static_cast<uint8_t>(ac)].dropFrames++;
        }
    }
    return droppedMpdu;
}

/*
inline void
QosWeightedMloScheduler::NotifyEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu)
{
    EnsureDelegate();
    m_delegate->NotifyEnqueue(ac, mpdu);
    
    double nowMs = Simulator::Now().GetMilliSeconds();
    
    // Update queue stats for this AC on all links
    Ptr<WifiMac> mac = GetMac();
    if (mac) {
        Ptr<QosTxop> qosTxop = mac->GetQosTxop(ac);
        if (qosTxop) {
            Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
            if (queue) {
                for (auto& [linkId, acMap] : m_metrics) {
                    acMap[static_cast<uint8_t>(ac)].queueLength = queue->GetNPackets();
                    acMap[static_cast<uint8_t>(ac)].queueBytes = queue->GetNBytes();
                    acMap[static_cast<uint8_t>(ac)].enqueueCount++;
                    acMap[static_cast<uint8_t>(ac)].lastEnqueueTimeMs = nowMs;
                }
            }
        }
    }
}
*/

inline void
QosWeightedMloScheduler::NotifyEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu)
{
    EnsureDelegate();
    m_delegate->NotifyEnqueue(ac, mpdu);
    
    double nowMs = Simulator::Now().GetMilliSeconds();
    
    // Guardar o tempo real de entrada através do UID único do pacote
    if (mpdu && mpdu->GetPacket()) {
        m_packetEnqueueTimes[mpdu->GetPacket()->GetUid()] = nowMs;
    }
    
    Ptr<WifiMac> mac = GetMac();
    if (mac) {
        Ptr<QosTxop> qosTxop = mac->GetQosTxop(ac);
        if (qosTxop) {
            Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
            if (queue) {
                for (auto& [linkId, acMap] : m_metrics) {
                    acMap[static_cast<uint8_t>(ac)].queueLength = queue->GetNPackets();
                    acMap[static_cast<uint8_t>(ac)].queueBytes = queue->GetNBytes();
                    acMap[static_cast<uint8_t>(ac)].enqueueCount++;
                    acMap[static_cast<uint8_t>(ac)].lastEnqueueTimeMs = nowMs;
                }
            }
        }
    }
}

/*
inline void
QosWeightedMloScheduler::NotifyDequeue(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus)
{
    EnsureDelegate();
    m_delegate->NotifyDequeue(ac, mpdus);
    
    double nowMs = Simulator::Now().GetMilliSeconds();
    
    for (auto& [linkId, acMap] : m_metrics) {
        auto& metrics = acMap[static_cast<uint8_t>(ac)];
        metrics.lastDequeueTimeMs = nowMs;
    }

    // Update global AC monitoring state and consume credits
    auto& acState = m_acStates[static_cast<uint8_t>(ac)];
    for (const auto& mpdu : mpdus) {
        if (mpdu) {
            acState.bytesSent += mpdu->GetSize();
            acState.packetsSent += 1;
            
            // Consume credit per packet transmitted
            m_credits[static_cast<uint8_t>(ac)] = std::max(0.0, m_credits[static_cast<uint8_t>(ac)] - 1.0);
        }
    }
}
*/

inline void
QosWeightedMloScheduler::NotifyDequeue(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus)
{
    EnsureDelegate();
    m_delegate->NotifyDequeue(ac, mpdus);
    
    double nowMs = Simulator::Now().GetMilliSeconds();
    auto& acState = m_acStates[static_cast<uint8_t>(ac)];
    
    for (const auto& mpdu : mpdus) {
        if (mpdu && mpdu->GetPacket()) {
            uint64_t uid = mpdu->GetPacket()->GetUid();
            auto it = m_packetEnqueueTimes.find(uid);
            
            // Se o pacote estava registado calculamos o atraso exato
            if (it != m_packetEnqueueTimes.end()) {
                double packetDelay = nowMs - it->second;
                m_packetEnqueueTimes.erase(it);
                
                // Média Móvel (EWMA) para suavizar variações bruscas
                acState.averageDelay = (acState.averageDelay == 0.0) ? packetDelay : 
                                       (0.8 * acState.averageDelay + 0.2 * packetDelay);
                
                for (auto& [linkId, acMap] : m_metrics) {
                    auto& metrics = acMap[static_cast<uint8_t>(ac)];
                    metrics.lastDequeueTimeMs = nowMs;
                    metrics.avgQueueDelay = (metrics.avgQueueDelay == 0.0) ? packetDelay : 
                                            (0.8 * metrics.avgQueueDelay + 0.2 * packetDelay);
                }
            }
            
            acState.bytesSent += mpdu->GetSize();
            acState.packetsSent += 1;
        }
    }
}

inline void
QosWeightedMloScheduler::NotifyRemove(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus)
{
    EnsureDelegate();
    m_delegate->NotifyRemove(ac, mpdus);
    
    // NotifyRemove is called when packets are dropped from queue (e.g. lifetime expired)
    for (const auto& mpdu : mpdus) {
        if (!mpdu) continue;
        for (auto& [linkId, acMap] : m_metrics) {
            acMap[static_cast<uint8_t>(ac)].dropFrames++;
        }
    }
}

inline void
QosWeightedMloScheduler::DoDispose()
{
    if (m_updateEvent.IsPending()) {
        m_updateEvent.Cancel();
    }
    m_metrics.clear();
    m_delegate = nullptr;
    if (m_decisionCsv.is_open())
    {
        m_decisionCsv.close();
    }
    WifiMacQueueScheduler::DoDispose();
}

inline int
QosWeightedMloScheduler::GetFrequencyRank(int frequency)
{
    switch (frequency)
    {
    case 6:
        return 2;
    case 5:
        return 1;
    case 2:
        return 0;
    default:
        return 0;
    }
}

inline double
QosWeightedMloScheduler::UtilityFunction(double value, double target, bool lowerIsBetter) const
{
    if (target <= 0.0) return 0.5;
    
    double ratio = value / target;
    
    if (lowerIsBetter) {
        // Lower is better (delay, jitter, loss rate).
        // Ratio <= 1.0 (meeting requirements) gives high utility (around 0.8 to 1.0)
        // Ratio > 1.0 (failing requirements) gives steeply decreasing utility
        return 1.0 / (1.0 + std::exp(5.0 * (ratio - 1.0)));
    } else {
        // Higher is better (throughput).
        // Ratio >= 1.0 (meeting target throughput) gives high utility
        // Ratio < 1.0 gives steeply decreasing utility
        return 1.0 / (1.0 + std::exp(5.0 * (1.0 - ratio)));
    }
}

/*
inline double
QosWeightedMloScheduler::ComputeLinkScore(uint8_t linkId, AcIndex ac) const
{
    auto linkIt = m_metrics.find(linkId);
    if (linkIt == m_metrics.end()) return 0.0;
    
    auto acIt = linkIt->second.find(static_cast<uint8_t>(ac));
    if (acIt == linkIt->second.end()) return 0.5;
    
    const auto& metrics = acIt->second;
    
    double Qnorm = std::min(1.0, metrics.queueLength / MAX_QUEUE_LENGTH);
    double DelayNorm = std::min(1.0, metrics.avgQueueDelay / MAX_DELAY);
    double HolNorm = std::min(1.0, metrics.holDelay / MAX_HOL_DELAY);
    double PerNorm = std::min(1.0, metrics.packetErrorRate);
    double UtilNorm = std::min(1.0, metrics.channelUtilization);
    double ThNorm = std::max(0.0, 1.0 - (metrics.achievedThroughput / MAX_THROUGHPUT));
    // double SinrNorm = std::max(0.0, 1.0 - (metrics.averageSinr / MAX_SINR));

    double score = 0.0;
    
    if (ac == AC_VO) {
        score = 0.40 * DelayNorm + 0.30 * HolNorm + 0.15 * PerNorm + 0.10 * Qnorm + 0.05 * UtilNorm;
    } else if (ac == AC_VI) {
        score = 0.30 * DelayNorm + 0.20 * PerNorm + 0.10 * Qnorm + 0.10 * UtilNorm + 0.30 * ThNorm;
    } else if (ac == AC_BE) {
        double originalScore = 0.40 * ThNorm + 0.25 * Qnorm + 0.20 * UtilNorm + 0.10 * PerNorm + 0.05 * DelayNorm;
        
        // Starvation detection
        double averageDelayBE = m_acStates.at(AC_BE).averageDelay;
        double BEStarvation = std::min(averageDelayBE / 100.0, 10.0);
        double normStarvation = BEStarvation / 10.0;
        
        // Credit system
        double normCredit = std::max(0.0, std::min(1.0, m_credits.at(AC_BE) / m_maxCredit));
        
        // Fairness Share
        double totalThroughput = 0.0;
        for (auto acIdx : {AC_VO, AC_VI, AC_BE, AC_BK}) {
            totalThroughput += m_acStates.at(acIdx).throughput;
        }
        double currentShareBE = 0.0;
        if (totalThroughput > 0.0) {
            currentShareBE = m_acStates.at(AC_BE).throughput / totalThroughput;
        }
        double ShareDeficitBE = 0.25 - currentShareBE; // TargetShareBE = 0.25
        double FairnessBoostBE = std::max(0.0, ShareDeficitBE);
        double normFairnessBoost = std::max(0.0, std::min(1.0, FairnessBoostBE / 0.25));
        
        score = originalScore - 0.30 * normStarvation - 0.20 * normCredit - 0.20 * normFairnessBoost;
    } else if (ac == AC_BK) {
        score = 0.40 * UtilNorm + 0.30 * Qnorm + 0.20 * ThNorm + 0.10 * PerNorm;
    } else {
        score = 0.40 * ThNorm + 0.25 * Qnorm + 0.20 * UtilNorm + 0.10 * PerNorm + 0.05 * DelayNorm; // Default to BE
        // Default doesn't apply fairness/starvation boosts unless explicitly AC_BE
    }
                   
    return score;
}
*/

inline double
QosWeightedMloScheduler::ComputeLinkScore(uint8_t linkId, AcIndex ac) const
{
    auto linkIt = m_metrics.find(linkId);
    if (linkIt == m_metrics.end()) return 0.0;
    
    auto acIt = linkIt->second.find(static_cast<uint8_t>(ac));
    if (acIt == linkIt->second.end()) return 0.5;
    
    const auto& metrics = acIt->second;
    
    auto targetIt = m_targets.find(static_cast<uint8_t>(ac));
    if (targetIt == m_targets.end()) return 0.5;
    const auto& targets = targetIt->second;
    
    double DelayUtil = UtilityFunction(metrics.avgQueueDelay, targets.maxDelayMs, true);
    double PerUtil = UtilityFunction(metrics.packetErrorRate, targets.maxLossRate, true);

    // === NOVA ABORDAGEM: ESTIMATIVA DO DÉBITO POTENCIAL ===
    double estimatedThroughput = metrics.achievedThroughput;
    if (estimatedThroughput < 0.001) {
        // Se a nossa categoria nao usa este link calculamos a velocidade que a antena conseguiria oferecer
        // Um canal totalmente livre permite atingir o limite maximo definido na simulação
        estimatedThroughput = MAX_THROUGHPUT * (1.0 - metrics.channelUtilization);
    }

    double ThUtil = UtilityFunction(estimatedThroughput, targets.minThroughputMbps, false);
    
    double Qnorm = std::min(1.0, metrics.queueLength / MAX_QUEUE_LENGTH);
    double UtilNorm = std::min(1.0, metrics.channelUtilization);
    double HolNorm = std::min(1.0, metrics.holDelay / MAX_HOL_DELAY);

    double score = 0.0;
    
    if (ac == AC_VO) {
        score = 0.40 * (1.0 - DelayUtil) + 0.30 * HolNorm + 0.15 * (1.0 - PerUtil) + 0.10 * Qnorm + 0.05 * UtilNorm;
    } else if (ac == AC_VI) {
        score = 0.30 * (1.0 - DelayUtil) + 0.20 * (1.0 - PerUtil) + 0.10 * Qnorm + 0.10 * UtilNorm + 0.30 * (1.0 - ThUtil);
    } else if (ac == AC_BE) {
        score = 0.40 * (1.0 - ThUtil) + 0.25 * Qnorm + 0.20 * UtilNorm + 0.10 * (1.0 - PerUtil) + 0.05 * (1.0 - DelayUtil);
    } else if (ac == AC_BK) {
        score = 0.40 * UtilNorm + 0.30 * Qnorm + 0.20 * (1.0 - ThUtil) + 0.10 * (1.0 - PerUtil);
    } else {
        score = 0.40 * (1.0 - ThUtil) + 0.25 * Qnorm + 0.20 * UtilNorm + 0.10 * (1.0 - PerUtil) + 0.05 * (1.0 - DelayUtil);
    }
                   
    return score;
}

/*
//ORIGINAL

inline uint8_t
QosWeightedMloScheduler::SelectBestLink(AcIndex ac, const std::list<uint8_t>& eligibleLinks)
{
    if (eligibleLinks.empty())
    {
        return m_slowLinkId;
    }
    if (eligibleLinks.size() == 1)
    {
        m_lastSelectedLink[static_cast<uint8_t>(ac)] = eligibleLinks.front();
        m_metrics[eligibleLinks.front()][static_cast<uint8_t>(ac)].packetsAssigned++;
        return eligibleLinks.front();
    }

    uint8_t bestLink = eligibleLinks.front();
    double bestScore = std::numeric_limits<double>::max();
    
    uint64_t totalPacketsAssigned = 0;
    for (uint8_t linkId : eligibleLinks) {
        totalPacketsAssigned += m_metrics[linkId][static_cast<uint8_t>(ac)].packetsAssigned;
    }

    std::map<uint8_t, double> finalScores;

    for (uint8_t linkId : eligibleLinks) {
        double score = ComputeLinkScore(linkId, ac);

        double loadPenalty = 0.0;
        if (totalPacketsAssigned > 0) {
            loadPenalty = static_cast<double>(m_metrics[linkId][static_cast<uint8_t>(ac)].packetsAssigned) / totalPacketsAssigned;
        }

        double finalScore = score + 0.05 * loadPenalty;
        finalScores[linkId] = finalScore;

        if (finalScore < bestScore) {
            bestScore = finalScore;
            bestLink = linkId;
        }
    }
    
    bool switched = false;
    
    auto lastIt = m_lastSelectedLink.find(static_cast<uint8_t>(ac));
    if (lastIt != m_lastSelectedLink.end()) {
        uint8_t lastLink = lastIt->second;
        if (std::find(eligibleLinks.begin(), eligibleLinks.end(), lastLink) != eligibleLinks.end()) {
            double lastScore = finalScores[lastLink];
            if (bestScore > lastScore * (1.0 - 0.05)) { // bestScore < lastScore * 0.95 means 5% improvement
                bestLink = lastLink;
                bestScore = lastScore; // Update to reflect we stayed
            } else {
                switched = true;
            }
        }
    }
    
    m_lastSelectedLink[static_cast<uint8_t>(ac)] = bestLink;
    m_metrics[bestLink][static_cast<uint8_t>(ac)].packetsAssigned++;
    
    // Log Decision
    if (m_csvEnabled && m_decisionCsv.is_open()) {
        double now = Simulator::Now().GetSeconds();
        auto& metrics = m_metrics[bestLink][static_cast<uint8_t>(ac)];
        
        double starvation = 0.0;
        double credit = 0.0;
        double fairnessBoost = 0.0;
        if (ac == AC_BE) {
            double averageDelayBE = m_acStates.at(AC_BE).averageDelay;
            starvation = std::min(averageDelayBE / 100.0, 10.0);
            credit = m_credits.at(AC_BE);
            
            double totalThroughput = 0.0;
            for (auto acIdx : {AC_VO, AC_VI, AC_BE, AC_BK}) {
                totalThroughput += m_acStates.at(acIdx).throughput;
            }
            double currentShareBE = 0.0;
            if (totalThroughput > 0.0) {
                currentShareBE = m_acStates.at(AC_BE).throughput / totalThroughput;
            }
            fairnessBoost = std::max(0.0, 0.25 - currentShareBE);
        }
        
        m_decisionCsv << now << "," << m_nodeContext << "," << +ac << ","
                      << +bestLink << "," << bestScore << ","
                      << metrics.queueLength << "," << metrics.avgQueueDelay << ","
                      << metrics.holDelay << "," << metrics.packetErrorRate << ","
                      << metrics.averageSinr << "," << metrics.achievedThroughput << ","
                      << metrics.channelUtilization << ","
                      << starvation << "," << credit << "," << fairnessBoost << "\n";
    }
    
    return bestLink;
}*/

/*
// With temporal locking
inline uint8_t
QosWeightedMloScheduler::SelectBestLink(AcIndex ac, const std::list<uint8_t>& eligibleLinks)
{
    if (eligibleLinks.empty())
    {
        return m_slowLinkId;
    }
    if (eligibleLinks.size() == 1)
    {
        m_lastSelectedLink[static_cast<uint8_t>(ac)] = eligibleLinks.front();
        m_metrics[eligibleLinks.front()][static_cast<uint8_t>(ac)].packetsAssigned++;
        return eligibleLinks.front();
    }

    double nowMs = Simulator::Now().GetMilliSeconds();
    uint8_t acIndex = static_cast<uint8_t>(ac);

    // Define o tempo de bloqueio de forma dinamica consoante a categoria
    double lockDurationMs = 0; // Valor por omissao 3.0

    
    if (ac == AC_VO) {
        lockDurationMs = 2.0;  // Voz precisa de pular rapidamente se o link falhar
    } else if (ac == AC_VI) {
        lockDurationMs = 4.0;  // Video permite uma ligeira espera para agregar mais
    } else if (ac == AC_BE) {
        lockDurationMs = 24.0; // Best Effort pode esperar bastante para criar agregacoes massivas
    } else if (ac == AC_BK) {
        lockDurationMs = 20.0; // Background tem foco total na eficiencia e ignorar o atraso
    }
    

    // Bloqueio temporal para permitir agregacao A MPDU
    if (m_lastSelectionTimeMs.find(acIndex) != m_lastSelectionTimeMs.end()) {
        if (nowMs - m_lastSelectionTimeMs[acIndex] < lockDurationMs) {
            auto lastIt = m_lastSelectedLink.find(acIndex);
            if (lastIt != m_lastSelectedLink.end()) {
                uint8_t lockedLink = lastIt->second;
                if (std::find(eligibleLinks.begin(), eligibleLinks.end(), lockedLink) != eligibleLinks.end()) {
                    m_metrics[lockedLink][acIndex].packetsAssigned++;
                    return lockedLink;
                }
            }
        }
    }
    

    uint8_t bestLink = eligibleLinks.front();
    double bestScore = std::numeric_limits<double>::max();
    
    uint64_t totalPacketsAssigned = 0;
    for (uint8_t linkId : eligibleLinks) {
        totalPacketsAssigned += m_metrics[linkId][acIndex].packetsAssigned;
    }

    std::map<uint8_t, double> finalScores;

    for (uint8_t linkId : eligibleLinks) {
        double score = ComputeLinkScore(linkId, ac);

        double loadPenalty = 0.0;
        if (totalPacketsAssigned > 0) {
            loadPenalty = static_cast<double>(m_metrics[linkId][acIndex].packetsAssigned) / totalPacketsAssigned;
        }

        double finalScore = score + 0.05 * loadPenalty;
        finalScores[linkId] = finalScore;

        if (finalScore < bestScore) {
            bestScore = finalScore;
            bestLink = linkId;
        }
    }
    
    bool switched = false;
    
    auto lastIt = m_lastSelectedLink.find(acIndex);
    if (lastIt != m_lastSelectedLink.end()) {
        uint8_t lastLink = lastIt->second;
        if (std::find(eligibleLinks.begin(), eligibleLinks.end(), lastLink) != eligibleLinks.end()) {
            double lastScore = finalScores[lastLink];
            if (bestScore > lastScore * (1.0 - 0.05)) { 
                bestLink = lastLink;
                bestScore = lastScore; 
            } else {
                switched = true;
            }
        }
    }
    
    m_lastSelectedLink[acIndex] = bestLink;
    m_lastSelectionTimeMs[acIndex] = nowMs;
    m_metrics[bestLink][acIndex].packetsAssigned++;
    
    if (m_csvEnabled && m_decisionCsv.is_open()) {
        double now = Simulator::Now().GetSeconds();
        auto& metrics = m_metrics[bestLink][acIndex];
        
        double starvation = 0.0;
        double credit = 0.0;
        double fairnessBoost = 0.0;
        if (ac == AC_BE) {
            double averageDelayBE = m_acStates.at(AC_BE).averageDelay;
            starvation = std::min(averageDelayBE / 100.0, 10.0);
            credit = m_credits.at(AC_BE);
            
            double totalThroughput = 0.0;
            for (auto acIdx : {AC_VO, AC_VI, AC_BE, AC_BK}) {
                totalThroughput += m_acStates.at(acIdx).throughput;
            }
            double currentShareBE = 0.0;
            if (totalThroughput > 0.0) {
                currentShareBE = m_acStates.at(AC_BE).throughput / totalThroughput;
            }
            fairnessBoost = std::max(0.0, 0.25 - currentShareBE);
        }
        
        m_decisionCsv << now << "," << m_nodeContext << "," << +ac << ","
                      << +bestLink << "," << bestScore << ","
                      << metrics.queueLength << "," << metrics.avgQueueDelay << ","
                      << metrics.holDelay << "," << metrics.packetErrorRate << ","
                      << metrics.averageSinr << "," << metrics.achievedThroughput << ","
                      << metrics.channelUtilization << ","
                      << starvation << "," << credit << "," << fairnessBoost << "\n";
    }
    
    return bestLink;
}
*/

inline uint8_t
QosWeightedMloScheduler::SelectBestLink(AcIndex ac, const std::list<uint8_t>& eligibleLinks)
{
    if (eligibleLinks.empty()) {
        return m_slowLinkId;
    }
    if (eligibleLinks.size() == 1) {
        m_lastSelectedLink[static_cast<uint8_t>(ac)] = eligibleLinks.front();
        m_metrics[eligibleLinks.front()][static_cast<uint8_t>(ac)].packetsAssigned++;
        return eligibleLinks.front();
    }

    uint8_t acIndex = static_cast<uint8_t>(ac);
    double currentScore = std::numeric_limits<double>::max();
    double alternativeScore = std::numeric_limits<double>::max();
    
    uint8_t currentLink = eligibleLinks.front();
    uint8_t alternativeLink = eligibleLinks.back();

    bool hasAnchor = false;
    auto lastIt = m_lastSelectedLink.find(acIndex);
    
    // Verifica se ja temos um link ancorado para esta categoria
    if (lastIt != m_lastSelectedLink.end()) {
        hasAnchor = true;
        currentLink = lastIt->second;
        
        for (uint8_t link : eligibleLinks) {
            if (link != currentLink) {
                alternativeLink = link;
                break;
            }
        }
    }

    currentScore = ComputeLinkScore(currentLink, ac);
    uint8_t finalLink = currentLink;
    double finalScore = currentScore;
    
    if (!hasAnchor) {
        alternativeScore = ComputeLinkScore(alternativeLink, ac);
        if (alternativeScore < currentScore) {
            finalLink = alternativeLink;
            finalScore = alternativeScore;
        }
    } else {
        // Se a pontuacao do link atual ultrapassar a barreira de dor de 3 decimas
        if (currentScore >= 0.2) {
            alternativeScore = ComputeLinkScore(alternativeLink, ac);

            // Exigimos 33 por cento de melhoria real para efetivar a mudanca de fluxo
            if (alternativeScore < (currentScore * 0.70)) {
                finalLink = alternativeLink;
                finalScore = alternativeScore;
            }
        }
    }

    m_lastSelectedLink[acIndex] = finalLink;
    m_metrics[finalLink][acIndex].packetsAssigned++;
    
    if (m_csvEnabled && m_decisionCsv.is_open()) {
        double now = Simulator::Now().GetSeconds();
        auto& metrics = m_metrics[finalLink][acIndex];
        
        m_decisionCsv << now << "," << m_nodeContext << "," << +ac << ","
                      << +finalLink << "," << finalScore << ","
                      << metrics.queueLength << "," << metrics.avgQueueDelay << ","
                      << metrics.holDelay << "," << metrics.packetErrorRate << ","
                      << metrics.averageSinr << "," << metrics.achievedThroughput << ","
                      << metrics.channelUtilization << "\n";
    }
    
    return finalLink;
}

inline void
QosWeightedMloScheduler::UpdatePeriodicMetrics()
{
    double now = Simulator::Now().GetSeconds();
    double dt = now - m_lastPeriodicUpdateTime;
    if (dt <= 0.0) dt = m_metricsIntervalSec;
    
    // Update global AC throughputs
    for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        auto& acState = m_acStates[static_cast<uint8_t>(ac)];
        uint64_t acBytes = acState.bytesSent - m_lastAcBytesSent[static_cast<uint8_t>(ac)];
        acState.throughput = (acBytes * 8.0) / (dt * 1e6); // in Mbps
        m_lastAcBytesSent[static_cast<uint8_t>(ac)] = acState.bytesSent;
    }
    
    for (auto& [linkId, acMap] : m_metrics) {
        // Compute real channel utilization for this link (shared across all ACs)
        double linkUtilization = 0.0;
        if (dt > 0.0) {
            linkUtilization = m_linkBusyTime[linkId] / dt;
            if (linkUtilization > 1.0) linkUtilization = 1.0;
            if (linkUtilization < 0.0) linkUtilization = 0.0;
        }
        m_linkBusyTime[linkId] = 0.0; // reset for next window

        for (auto& [ac, metrics] : acMap) {
            // Recalculate throughput: (bytes * 8) / (dt * 1e6)
            metrics.achievedThroughput = (metrics.txBytes * 8.0) / (dt * 1e6);
            metrics.txBytes = 0;
            
            // Recalculate PER from real TX attempts vs confirmed frames
            if (metrics.txAttempts > 0) {
                double successRate = static_cast<double>(metrics.txSuccess) / metrics.txAttempts;
                metrics.packetErrorRate = 1.0 - successRate;
                if (metrics.packetErrorRate < 0.0) metrics.packetErrorRate = 0.0;
            } else {
                metrics.packetErrorRate = 0.0;
            }
            metrics.txAttempts = 0;
            metrics.txSuccess = 0;
            // Also reset legacy drop/enqueue counters
            metrics.enqueueCount = 0;
            metrics.dropFrames = 0;
            
            // Apply real channel utilization to this AC
            metrics.channelUtilization = linkUtilization;
            
            /*
            // Delay estimation using Little's Law (Queue Bytes / Throughput)
            if (metrics.queueBytes > 0) {
                if (metrics.achievedThroughput > 0.001) { // 1 kbps minimum
                    double estimatedDelayMs = (metrics.queueBytes * 8.0) / (metrics.achievedThroughput * 1000.0);
                    // Cap estimated delay
                    if (estimatedDelayMs > MAX_DELAY) estimatedDelayMs = MAX_DELAY;
                    metrics.avgQueueDelay = (metrics.avgQueueDelay == 0.0) ? estimatedDelayMs : (0.7 * metrics.avgQueueDelay + 0.3 * estimatedDelayMs);
                } else {
                    metrics.avgQueueDelay = MAX_DELAY; 
                }
            } else {
                metrics.avgQueueDelay = 0.0;
            }
            */

            // HOL Delay Estimate (simplified)
            if (metrics.queueLength > 0) {
                double nowMs = Simulator::Now().GetMilliSeconds();
                double waitTime = nowMs - metrics.lastEnqueueTimeMs;
                metrics.holDelay = (waitTime > MAX_HOL_DELAY) ? MAX_HOL_DELAY : waitTime;
            } else {
                metrics.holDelay = 0.0;
            }
        }
    }
    
    // Update global AC queue lengths and average delays
    Ptr<WifiMac> mac = GetMac();
    for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        auto& acState = m_acStates[static_cast<uint8_t>(ac)];
        uint32_t queueBytes = 0;
        uint32_t queuePackets = 0;
        if (mac) {
            Ptr<QosTxop> qosTxop = mac->GetQosTxop(ac);
            if (qosTxop) {
                Ptr<WifiMacQueue> queue = qosTxop->GetWifiMacQueue();
                if (queue) {
                    queueBytes = queue->GetNBytes();
                    queuePackets = queue->GetNPackets();
                }
            }
        }
        acState.queueLength = queuePackets;
        
        /*
        // Calculate average delay globally using Little's Law: Delay = Queue Bytes / Throughput
        if (queueBytes > 0) {
            if (acState.throughput > 0.001) { // 1 kbps minimum
                double estimatedDelayMs = (queueBytes * 8.0) / (acState.throughput * 1000.0);
                acState.averageDelay = (acState.averageDelay == 0.0) ? estimatedDelayMs : (0.7 * acState.averageDelay + 0.3 * estimatedDelayMs);
            } else {
                acState.averageDelay = 500.0; // default cap when queued but no throughput
            }
        } else {
            acState.averageDelay = 0.0;
        }
        */
    }
    
    m_lastPeriodicUpdateTime = now;
    
    // Reschedule next update
    m_updateEvent = Simulator::Schedule(Seconds(m_metricsIntervalSec), 
                                        &QosWeightedMloScheduler::UpdatePeriodicMetrics, this);
}

inline void
QosWeightedMloScheduler::PrintFinalScores()
{
    std::cout << "\n=== FINAL MLO SCHEDULER SCORES (" << m_nodeContext << ") ===\n";
    for (auto ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        std::cout << "AC " << +ac << ": ";
        for (uint8_t linkId : {0, 1}) {
            double score = ComputeLinkScore(linkId, ac);
            std::cout << "Link" << +linkId << "=" << score << " | ";
        }
        std::cout << "\n";
    }
}

// Installation Helper function
inline Ptr<QosWeightedMloScheduler>
InstallQosWeightedScheduler(Ptr<WifiMac> mac, int freq1, int freq2)
{
    if (!mac)
    {
        return nullptr;
    }

    Ptr<QosWeightedMloScheduler> scheduler = CreateObject<QosWeightedMloScheduler>();
    scheduler->ConfigureForPair(freq1, freq2);
    mac->SetMacQueueScheduler(scheduler);
    return scheduler;
}

} // namespace ns3

#endif /* MLO_QOS_WEIGHTED_SCHEDULER_H */
