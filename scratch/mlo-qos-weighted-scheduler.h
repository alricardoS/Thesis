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

const double MAX_QUEUE_LENGTH = 1000.0;
const double MAX_DELAY = 100.0;
const double MAX_HOL_DELAY = 100.0;
const double MAX_THROUGHPUT = 150.0;
const double MAX_SINR = 50.0;

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
    void FeedLinkUtilization(uint8_t linkId, double utilization);
    
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
    double m_metricsIntervalSec{0.5};
    double m_lastPeriodicUpdateTime{0.0};
    EventId m_updateEvent;
    double m_hysteresisThreshold{0.10}; // 10% hysteresis
    std::map<uint8_t, uint8_t> m_lastSelectedLink;

    uint8_t m_slowLinkId{0};
    uint8_t m_fastLinkId{1};
    int m_freq1{5};
    int m_freq2{6};
    
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
    m_targets[AC_VO] = {150.0, 30.0, 0.01, 0.1}; // 150ms delay, 30ms jitter, 1% loss, 0.1 Mbps min tp
    
    // VI: Video (low delay, low jitter, high throughput, moderate loss tolerance)
    m_weights[AC_VI] = {0.25, 0.15, 0.20, 0.40};
    m_targets[AC_VI] = {150.0, 50.0, 0.01, 5.0}; // 150ms delay, 50ms jitter, 1% loss, 5 Mbps min tp

    // BE: Best effort (throughput oriented, latency/jitter tolerant)
    m_weights[AC_BE] = {0.10, 0.05, 0.15, 0.70};
    m_targets[AC_BE] = {500.0, 200.0, 0.05, 1.0}; // 500ms delay, 200ms jitter, 5% loss, 1 Mbps min tp

    // BK: Background (throughput oriented, very high delay/jitter tolerance)
    m_weights[AC_BK] = {0.05, 0.05, 0.10, 0.80};
    m_targets[AC_BK] = {1000.0, 500.0, 0.10, 0.5}; // 1000ms delay, 500ms jitter, 10% loss, 0.5 Mbps min tp
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
    metrics.dropFrames += dropFrames;
}

inline void
QosWeightedMloScheduler::FeedLinkDrop(uint8_t linkId, AcIndex ac)
{
    m_metrics[linkId][static_cast<uint8_t>(ac)].dropFrames++;
}

inline void
QosWeightedMloScheduler::FeedLinkUtilization(uint8_t linkId, double utilization)
{
    for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        m_metrics[linkId][static_cast<uint8_t>(ac)].channelUtilization = utilization;
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
        score = 0.40 * ThNorm + 0.25 * Qnorm + 0.20 * UtilNorm + 0.10 * PerNorm + 0.05 * DelayNorm;
    } else if (ac == AC_BK) {
        score = 0.40 * UtilNorm + 0.30 * Qnorm + 0.20 * ThNorm + 0.10 * PerNorm;
    } else {
        score = 0.40 * ThNorm + 0.25 * Qnorm + 0.20 * UtilNorm + 0.10 * PerNorm + 0.05 * DelayNorm; // Default to BE
    }
                   
    return score;
}

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
        
        m_decisionCsv << now << "," << m_nodeContext << "," << +ac << ","
                      << +bestLink << "," << bestScore << ","
                      << metrics.queueLength << "," << metrics.avgQueueDelay << ","
                      << metrics.holDelay << "," << metrics.packetErrorRate << ","
                      << metrics.averageSinr << "," << metrics.achievedThroughput << ","
                      << metrics.channelUtilization << "\n";
    }
    
    return bestLink;
}

inline void
QosWeightedMloScheduler::UpdatePeriodicMetrics()
{
    double now = Simulator::Now().GetSeconds();
    double dt = now - m_lastPeriodicUpdateTime;
    if (dt <= 0.0) dt = m_metricsIntervalSec;
    
    for (auto& [linkId, acMap] : m_metrics) {
        for (auto& [ac, metrics] : acMap) {
            // Recalculate throughput: (bytes * 8) / (dt * 1e6)
            metrics.achievedThroughput = (metrics.txBytes * 8.0) / (dt * 1e6);
            metrics.txBytes = 0;
            
            // Recalculate loss rate: drops / (drops + enqueued)
            uint64_t totalPackets = metrics.enqueueCount + metrics.dropFrames;
            if (totalPackets > 0) {
                metrics.packetErrorRate = static_cast<double>(metrics.dropFrames) / totalPackets;
            } else {
                metrics.packetErrorRate = 0.0;
            }
            metrics.enqueueCount = 0;
            metrics.dropFrames = 0;
            
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
