/*
 * Goal-Oriented Traffic-Aware MLO Scheduler
 *
 * Evolução do QosWeightedMloScheduler para um scheduler orientado a objetivos QoS,
 * com cinco motores internos:
 *   1. Goal-Awareness Engine      — ComputeQosSatisfaction por AC
 *   2. Link Capability Engine     — estimatedCapacity, availableCapacity, capabilityScore
 *   3. Traffic Composition Engine — throughput/airtime/flows por (link, AC)
 *   4. EDCA Competition Engine    — CompetitionPressure, ExpectedAccessOpportunity
 *   5. Migration Decision Engine  — DecideLinkMigration com histerese e Best-Achievable
 *
 * Escopo: AP-only (downlink). STAs usam FcfsWifiQueueScheduler.
 */
#ifndef MLO_QOS_WEIGHTED_SCHEDULER_H
#define MLO_QOS_WEIGHTED_SCHEDULER_H

#include "ns3/fcfs-wifi-queue-scheduler.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/wifi-mac.h"
#include "ns3/qos-txop.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/wifi-tx-vector.h"
#include "ns3/wifi-mode.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>

namespace ns3
{

// ---------------------------------------------------------------------------
// QoS weights per AC (used in satisfaction aggregation)
// ---------------------------------------------------------------------------
struct AcWeights {
    double delayWeight{0.30};
    double jitterWeight{0.05};
    double lossWeight{0.30};
    double throughputWeight{0.35};  //ALTEREI ESTAVAM TODOS A 0.25
};

// ---------------------------------------------------------------------------
// Phase 1 — Goal-Awareness Engine structures
// ---------------------------------------------------------------------------

/// QoS goals (targets) per Access Category
struct AcGoals {
    double targetThroughputMbps{150.0};
    double maxDelayMs{100.0};
    double maxJitterMs{5.0};
    double maxPacketLoss{0.05};
};

/// Per-AC QoS satisfaction index (all values in [0,1])
struct QosSatisfaction {
    double throughput{0.0};
    double delay{0.0};
    double jitter{0.0};
    double loss{0.0};
    double index{0.0}; ///< weighted aggregate
};

/// Real per-(STA,AC) end-to-end QoS measurements fed from the simulation sinks.
struct StaQos {
    double   tpMbps{0.0};
    double   delayMs{0.0};
    double   jitterMs{0.0};
    double   lossRate{0.0};
    bool     valid{false};
    uint32_t samples{0}; ///< nº de janelas de medição alimentadas (warmup)
};

// ---------------------------------------------------------------------------
// Phase 2 — Link Capability Engine
// ---------------------------------------------------------------------------

/// Per-link (shared across ACs) PHY capacity assessment
struct LinkCapability {
    double estimatedCapacityMbps{0.0};   ///< PHY rate * (1 - PER)
    double availableCapacityMbps{0.0};   ///< estimated - occupied (sum of ALL AC throughputs) — kept for capabilityScore/diagnostics
    double freeAirtime{1.0};             ///< 1 - channelUtilization
    double capabilityScore{0.0};         ///< 0..1 normalised

    /// Priority-aware available capacity per AC: estimatedCapacity minus the
    /// throughput of ACs with EQUAL OR HIGHER 802.11 priority only. Models
    /// the real EDCA behaviour where a higher-priority AC (e.g. VO) doesn't
    /// lose access just because a lower-priority AC (e.g. BE/BK) is
    /// transmitting more — VO always wins channel access first. Indexed by
    /// AcIndex cast to uint8_t.
    std::map<uint8_t, double> availableCapacityPerAcMbps;
};

// ---------------------------------------------------------------------------
// Phase 3 — Traffic Composition Engine
// ---------------------------------------------------------------------------

/// Per-link traffic composition (updated every metrics window)
struct LinkTrafficComposition {
    double voThroughputMbps{0.0};
    double viThroughputMbps{0.0};
    double beThroughputMbps{0.0};
    double bkThroughputMbps{0.0};

    double voAirtimeFrac{0.0};
    double viAirtimeFrac{0.0};
    double beAirtimeFrac{0.0};
    double bkAirtimeFrac{0.0};

    uint32_t voFlows{0};
    uint32_t viFlows{0};
    uint32_t beFlows{0};
    uint32_t bkFlows{0};

    // Accumulators (reset each window)
    uint64_t voBytes{0}, viBytes{0}, beBytes{0}, bkBytes{0};
    double   voAirtimeSec{0.0}, viAirtimeSec{0.0}, beAirtimeSec{0.0}, bkAirtimeSec{0.0};
    std::set<std::pair<std::string, uint8_t>> voFlowSet;
    std::set<std::pair<std::string, uint8_t>> viFlowSet;
    std::set<std::pair<std::string, uint8_t>> beFlowSet;
    std::set<std::pair<std::string, uint8_t>> bkFlowSet;
};

// ---------------------------------------------------------------------------
// Phase 4 — EDCA Competition Engine
// ---------------------------------------------------------------------------

/// Per-(link, AC) EDCA competition assessment
struct EdcaCompetition {
    double competitionPressure{0.0};            ///< weighted load from other ACs
    double expectedAccessOpportunity{0.0};      ///< freeAirtime / (1 + pressure)
    double effectiveAvailableCapacityMbps{0.0}; ///< availableCapacity * opportunity
};

// ---------------------------------------------------------------------------
// Legacy per-(link, AC) raw metric counters (kept for HOL delay and PER)
// ---------------------------------------------------------------------------
struct LinkState {
    // Throughput counters (reset each window)
    uint64_t txBytes{0};
    uint64_t txAttempts{0};
    uint64_t txSuccess{0};
    uint64_t dropFrames{0};

    // Queue snapshot
    uint32_t queueLength{0};
    uint32_t queueBytes{0};
    uint64_t enqueueCount{0};
    double   lastEnqueueTimeMs{0.0};

    // Derived metrics (updated each window)
    double achievedThroughputMbps{0.0};
    double packetErrorRate{0.0};
    bool   perInitialized{false}; // true once at least one PER sample has been taken
    double channelUtilization{0.0};
    double holDelay{0.0};

    // Average delay per packet (EWMA, updated in NotifyDequeue for correct link)
    double avgQueueDelayMs{0.0};
    // Jitter (EWMA of |delay_i - delay_{i-1}|)
    double avgJitterMs{0.0};
    double lastDelayMs{-1.0}; // -1 means no prior sample
};

// Per-AC global state (bytes sent, packets sent)
struct AcState {
    uint64_t bytesSent{0};
    uint64_t packetsSent{0};
    double   throughputMbps{0.0};
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
const double MAX_QUEUE_LENGTH = 500.0;
const double MAX_DELAY_MS     = 200.0; //ALETEREI era 1000
const double MAX_THROUGHPUT   = 600.0; // Mbps (6 GHz 320 MHz 2SS EHT)

// ---------------------------------------------------------------------------
// Main scheduler class
// ---------------------------------------------------------------------------
class QosWeightedMloScheduler : public WifiMacQueueScheduler
{
  public:
    static TypeId GetTypeId();

    QosWeightedMloScheduler();
    ~QosWeightedMloScheduler() override;

    void ConfigureForPair(int freq1, int freq2);
    void SetWifiMac(Ptr<WifiMac> mac) override;

    // ---- External feed APIs (called from simulation script) ----
    void FeedLinkMetrics(uint8_t linkId, AcIndex ac, uint32_t txBytes,
                         uint32_t txFrames, uint32_t dropFrames);
    void FeedLinkDrop(uint8_t linkId, AcIndex ac);
    void FeedLinkTxAttempt(uint8_t linkId, AcIndex ac, uint32_t frames);
    void FeedLinkTxStart(uint8_t linkId);
    void FeedLinkTxEnd(uint8_t linkId);

    /// Phase 2: PHY state feed (MCS, channel width, PER from PhyTxPsduBegin)
    void FeedLinkPhyState(uint8_t linkId, const WifiTxVector& txVector, double per);

    /// Phase 3: packet transmitted feed (bytes + airtime proxy + flow id)
    void FeedPacketTransmitted(uint8_t linkId, AcIndex ac, uint32_t bytes,
                               double airtimeSec,
                               const std::string& peerAddress, uint8_t tid);

    /// Real per-(STA,AC) end-to-end QoS measurements (fed periodically from the
    /// simulation sinks). These drive the MEASURED satisfaction (curSat).
    void FeedStaQos(Mac48Address sta, AcIndex ac,
                    double tpMbps, double delayMs, double jitterMs, double lossRate);

    // ---- Configuration ----
    void EnableDecisionCsv(const std::string& filename, const std::string& nodeContext);
    void SetWeights(AcIndex ac, double delay, double jitter, double loss, double tp);
    void SetTargets(AcIndex ac, double delayMs, double jitterMs, double lossRate, double tpMbps);
    /// Alias for SetTargets using AcGoals semantics
    void SetGoals(AcIndex ac, double tpMbps, double delayMs, double jitterMs, double lossRate);

    // ---- ns-3 scheduler interface ----
    std::optional<WifiContainerQueueId> GetNext(AcIndex ac,
                                                std::optional<uint8_t> linkId,
                                                bool skipBlockedQueues = true) override;
    std::optional<WifiContainerQueueId> GetNext(AcIndex ac,
                                                std::optional<uint8_t> linkId,
                                                const WifiContainerQueueId& prevQueueId,
                                                bool skipBlockedQueues = true) override;
    std::list<uint8_t> GetLinkIds(AcIndex ac,
                                  Ptr<const WifiMpdu> mpdu,
                                  const std::list<WifiQueueBlockedReason>& ignoredReasons = {}) override;

    void BlockQueues(WifiQueueBlockedReason reason, AcIndex ac,
                     const std::list<WifiContainerQueueType>& types,
                     const Mac48Address& rxAddress, const Mac48Address& txAddress,
                     const std::set<uint8_t>& tids = {},
                     const std::set<uint8_t>& linkIds = {}) override;
    void UnblockQueues(WifiQueueBlockedReason reason, AcIndex ac,
                       const std::list<WifiContainerQueueType>& types,
                       const Mac48Address& rxAddress, const Mac48Address& txAddress,
                       const std::set<uint8_t>& tids = {},
                       const std::set<uint8_t>& linkIds = {}) override;
    void BlockAllQueues(WifiQueueBlockedReason reason,
                        const std::set<uint8_t>& linkIds = {}) override;
    void UnblockAllQueues(WifiQueueBlockedReason reason,
                          const std::set<uint8_t>& linkIds = {}) override;
    bool GetAllQueuesBlockedOnLink(uint8_t linkId,
                                   WifiQueueBlockedReason reason =
                                       WifiQueueBlockedReason::REASONS_COUNT) override;
    std::optional<Mask> GetQueueLinkMask(AcIndex ac,
                                         const WifiContainerQueueId& queueId,
                                         uint8_t linkId) override;

    Ptr<WifiMpdu> HasToDropBeforeEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu) override;
    void NotifyEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu) override;
    void NotifyDequeue(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus) override;
    void NotifyRemove(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus) override;

    void PrintFinalScores();

  protected:
    void DoDispose() override;

  private:
    // ---- Internal helpers ----
    static int GetFrequencyRank(int frequency);
    void EnsureDelegate();

    /// Sigmoid utility: maps value/target to [0,1]
    double UtilityFunction(double value, double target, bool lowerIsBetter) const;

    // ---- Phase 1: Goal-Awareness Engine ----
    /// Compute QoS satisfaction for (STA, AC) using real per-STA end-to-end
    /// measurements (m_staQos). linkId kept for signature compatibility.
    QosSatisfaction ComputeQosSatisfaction(AcIndex ac, const Mac48Address& sta,
                                           uint8_t linkId) const;
    /// Returns the weighted satisfaction index for (STA, AC) on its current link
    double CurrentQosSatisfaction(AcIndex ac, const Mac48Address& sta) const;

    // ---- Phase 2: Link Capability Engine ----
    void UpdateLinkCapability(uint8_t linkId, double dt);

    // ---- Phase 3: Traffic Composition Engine ----
    void UpdateTrafficComposition(uint8_t linkId, double dt);

    // ---- Phase 4: EDCA Competition Engine ----
    void UpdateEdcaCompetition(uint8_t linkId);
    /// Normalised load of ac on link (airtime fraction)
    double NormalisedLoad(AcIndex ac, uint8_t linkId) const;

    // ---- Phase 5+6: Migration Decision Engine ----
    /// Number of OTHER STAs of the same AC already assigned to linkId
    uint32_t CountCoResidents(AcIndex ac, const Mac48Address& sta, uint8_t linkId) const;
    /// Expected QoS satisfaction if (STA, AC) were placed on linkId
    double ComputeExpectedQosSatisfaction(AcIndex ac, const Mac48Address& sta, uint8_t linkId) const;
    /// Returns true if any link can meet goals for (STA, AC)
    bool AnyLinkMeetsGoals(AcIndex ac, const Mac48Address& sta, const std::list<uint8_t>& links) const;
    /// Best-achievable score when no link meets goals (used as tiebreaker)
    double ComputeBestAchievableScore(AcIndex ac, const Mac48Address& sta, uint8_t linkId) const;

    /// Returns true if ac's own QoS satisfaction on linkId clears a
    /// "goals met" threshold — using the same composite index as
    /// m_currentSatisfaction (CurrentQosSatisfaction), not a borrowed Mbps
    /// comparison. Generic across any AC: each AC's own weights/goals
    /// already encode what "meeting goals" means for it.
    bool MeetsOwnGoals(AcIndex ac, const Mac48Address& sta, uint8_t linkId) const;

    /// Returns true if placing ac on linkId would harm the worst-affected
    /// AC currently resident there beyond an acceptable threshold — using
    /// the same composite satisfaction index (otherSat) as the rest of the
    /// scheduler, not a single metric like loss alone, so all four
    /// dimensions (delay/jitter/loss/throughput) count with their AC-specific
    /// weights, consistent with how satisfaction is judged everywhere else.
    /// harmedAcOut/worstSatOut (optional) report which AC and how badly.
    bool WouldHarmResident(AcIndex ac, const Mac48Address& sta, uint8_t linkId,
                           AcIndex* harmedAcOut = nullptr,
                           double* worstSatOut = nullptr) const;

    /// Main decision function — replaces SelectBestLink. Implements an
    /// explicit priority cascade:
    ///   First: prefer a link where ac meets its OWN goals AND doesn't harm any
    ///      resident AC beyond the harm threshold.
    ///   Second: if no such link exists, prefer a link where ac meets its own
    ///      goals, even if it harms a resident (EDCA priority is real and
    ///      cannot be waived — ac is not required to sacrifice its own
    ///      goals for a lower/equal-priority resident).
    ///   Third: if ac cannot meet its own goals anywhere, fall back to the
    ///      best-achievable score (existing behaviour).
    uint8_t DecideLinkMigration(AcIndex ac, const Mac48Address& sta,
                                 const std::list<uint8_t>& eligibleLinks);

    // ---- Periodic update ----
    void UpdatePeriodicMetrics();

    // ---- Delegate (FCFS) ----
    Ptr<WifiMacQueueScheduler> m_delegate;

    // ---- Raw metrics: linkId -> (acIndex -> LinkState) ----
    std::map<uint8_t, std::map<uint8_t, LinkState>> m_metrics;

    // ---- Packet enqueue timestamps for accurate delay ----
    std::map<uint64_t, double> m_packetEnqueueTimes; // uid -> enqueue time (ms)
    /// Per-(destination STA MAC, AC) routing key for per-flow link assignment
    using StaAcKey = std::pair<Mac48Address, uint8_t>;
    std::map<StaAcKey, uint8_t> m_lastSelectedLink;

    // ---- Phase 1 state ----
    std::map<uint8_t, AcGoals>        m_goals;         // per AC (indexed by AcIndex cast to uint8)
    std::map<uint8_t, AcWeights>      m_weights;
    std::map<StaAcKey, QosSatisfaction> m_currentSatisfaction; // per (STA, AC), current link
    std::set<StaAcKey> m_hasMeasuredSat; // (STA,AC) com pelo menos uma medição real de throughput
    std::map<StaAcKey, StaQos> m_staQos; // (STA,AC) -> métricas reais end-to-end (dos sinks)

    // ---- Phase 2 state ----
    std::map<uint8_t, LinkCapability> m_linkCapability; // per link
    // PHY accumulators per link (EWMA of last MCS / PER)
    struct PhyAccumulator {
        double ewmaDataRateMbps{0.0};
        double ewmaPer{0.0};
        bool   valid{false};
    };
    std::map<uint8_t, PhyAccumulator> m_phyState;

    // ---- Phase 3 state ----
    std::map<uint8_t, LinkTrafficComposition> m_traffic; // per link

    // ---- Phase 4 state ----
    std::map<uint8_t, std::map<uint8_t, EdcaCompetition>> m_edca; // link -> ac -> EdcaCompetition

    // EDCA competition weights: weight[candidate_ac][other_ac]
    // Rows: VO=3, VI=2, BE=1, BK=0
    double m_edcaWeights[4][4] = {
        // other: BK    BE    VI    VO
        {0.0,  0.5,  7.0,  9.0}, // candidate BK  //ALTEREI era 0.4
        {0.5,  1.0,  6.0,  8.0}, // candidate BE  //ALTEREI era 0.4
        {0.25, 0.5,  1.0,  2.0}, // candidate VI
        {0.0,  0.05, 0.25, 1.0}  // candidate VO
    };

    // ---- Per-AC global state ----
    std::map<uint8_t, AcState>   m_acStates;
    std::map<uint8_t, uint64_t>  m_lastAcBytesSent;

    // ---- Airtime tracking for channel utilisation ----
    std::map<uint8_t, Time>     m_linkTxStartTime;
    std::map<uint8_t, double>   m_linkBusyTime;
    std::map<uint8_t, uint32_t> m_linkTxDepth;

    // ---- Decision thresholds ----
    double m_stayThreshold{0.75};       // stay if currentSatisfaction >= this //ALTEREI ERA 0.85
    double m_migrationThreshold{0.25};  // migrate if expectedGain > this      //ALTEREI ERA 0.15
    double m_metricsIntervalSec{0.5};
    double m_lastPeriodicUpdateTime{0.0};
    EventId m_updateEvent;

    // ---- Priority-cascade thresholds (DecideLinkMigration) ----
    // "Meets own goals" threshold for MeetsOwnGoals() — a candidate clears
    // its own goals on a link if ComputeExpectedQosSatisfaction there is at
    // or above this. Distinct from m_stayThreshold (which gates whether to
    // even start evaluating alternatives) — this gates whether a *candidate*
    // link counts as "good enough on its own merits" during evaluation.
    double m_ownGoalsThreshold{0.90};
    // "Harm" threshold for WouldHarmResident() — a resident AC is considered
    // harmed if its OWN current composite satisfaction would sit at or below
    // this. Uses the same composite otherSat index as the rest of the
    // scheduler (delay/jitter/loss/throughput, AC-specific weights), not a
    // single raw metric like loss alone.
    double m_harmThreshold{0.70};

    // ---- Link identity ----
    uint8_t m_slowLinkId{0};
    uint8_t m_fastLinkId{1};
    int m_freq1{5};
    int m_freq2{6};

    // ---- Legacy targets alias (for SetTargets backward compat) ----
    // Maps 1:1 with m_goals
    std::map<uint8_t, AcGoals>& m_targets{m_goals}; // same map, alias

    // ---- Logging ----
    std::ofstream m_decisionCsv;
    std::string   m_nodeContext;
    bool          m_csvEnabled{false};
};

// ===========================================================================
// TypeId
// ===========================================================================
inline TypeId
QosWeightedMloScheduler::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::QosWeightedMloScheduler")
            .SetParent<WifiMacQueueScheduler>()
            .SetGroupName("Wifi")
            .AddConstructor<QosWeightedMloScheduler>()
            .AddAttribute("StayThreshold",
                          "Satisfaction index above which the current link is kept",
                          DoubleValue(0.85),
                          MakeDoubleAccessor(&QosWeightedMloScheduler::m_stayThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("MigrationThreshold",
                          "Minimum gain in expected satisfaction required to trigger migration",
                          DoubleValue(0.15),
                          MakeDoubleAccessor(&QosWeightedMloScheduler::m_migrationThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("MetricsInterval",
                          "Periodic metrics update interval (s)",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&QosWeightedMloScheduler::m_metricsIntervalSec),
                          MakeDoubleChecker<double>(0.05, 10.0))
            .AddAttribute("OwnGoalsThreshold",
                          "Composite satisfaction threshold above which a candidate link counts as meeting the AC's own goals",
                          DoubleValue(0.90),
                          MakeDoubleAccessor(&QosWeightedMloScheduler::m_ownGoalsThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("HarmThreshold",
                          "Composite satisfaction threshold at or below which a resident AC is considered harmed by a newcomer",
                          DoubleValue(0.70),
                          MakeDoubleAccessor(&QosWeightedMloScheduler::m_harmThreshold),
                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

// ===========================================================================
// Constructor / Destructor
// ===========================================================================
inline
QosWeightedMloScheduler::QosWeightedMloScheduler()
    : m_delegate(CreateObject<FcfsWifiQueueScheduler>())
{
    // ---- Default goals per AC ----
    m_goals[AC_VO] = {150.0, 15.0,  0.1,  0.01};  //ALTEREI a loss
    m_goals[AC_VI] = {150.0, 30.0, 0.1,  0.01};  //ALTEREI a loss
    m_goals[AC_BE] = {150.0, 200.0, 1.0, 0.10};  //ALTEREI a loss
    m_goals[AC_BK] = {150.0, 300.0, 100.0, 0.10};

    // ---- Default weights per AC ----
    m_weights[AC_VO] = {0.40, 0.30, 0.25, 0.05};
    m_weights[AC_VI] = {0.25, 0.15, 0.20, 0.40};
    m_weights[AC_BE] = {0.20, 0.05, 0.15, 0.60};
    m_weights[AC_BK] = {0.05, 0.05, 0.10, 0.80};

    // ---- Init AC state ----
    for (auto ac : {AC_VO, AC_VI, AC_BE, AC_BK}) {
        m_acStates[static_cast<uint8_t>(ac)] = {};
        m_lastAcBytesSent[static_cast<uint8_t>(ac)] = 0;
        // m_currentSatisfaction is keyed by (STA MAC, AC) and populated
        // lazily as STAs are discovered — no per-AC init needed here.
    }
}

inline
QosWeightedMloScheduler::~QosWeightedMloScheduler()
{
    if (m_decisionCsv.is_open()) m_decisionCsv.close();
}

// ===========================================================================
// Setup
// ===========================================================================
inline void
QosWeightedMloScheduler::ConfigureForPair(int freq1, int freq2)
{
    m_freq1 = freq1;
    m_freq2 = freq2;
    const int rank1 = GetFrequencyRank(freq1);
    const int rank2 = GetFrequencyRank(freq2);
    m_fastLinkId = (rank1 >= rank2) ? 0 : 1;
    m_slowLinkId = (rank1 >= rank2) ? 1 : 0;

    for (uint8_t linkId : {m_slowLinkId, m_fastLinkId}) {
        for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
            m_metrics[linkId][static_cast<uint8_t>(ac)] = LinkState{};
        }
        m_linkCapability[linkId] = LinkCapability{};
        m_traffic[linkId]        = LinkTrafficComposition{};
        m_phyState[linkId]       = PhyAccumulator{};
        m_linkBusyTime[linkId]   = 0.0;
        m_linkTxDepth[linkId]    = 0;
        for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
            m_edca[linkId][static_cast<uint8_t>(ac)] = EdcaCompetition{};
        }
    }
}

inline void
QosWeightedMloScheduler::SetWifiMac(Ptr<WifiMac> mac)
{
    WifiMacQueueScheduler::SetWifiMac(mac);
    EnsureDelegate();
    m_delegate->SetWifiMac(mac);

    if (!m_updateEvent.IsPending()) {
        m_lastPeriodicUpdateTime = Simulator::Now().GetSeconds();
        m_updateEvent = Simulator::Schedule(
            Seconds(m_metricsIntervalSec),
            &QosWeightedMloScheduler::UpdatePeriodicMetrics, this);
    }
}

inline void
QosWeightedMloScheduler::EnsureDelegate()
{
    if (!m_delegate) m_delegate = CreateObject<FcfsWifiQueueScheduler>();
}

// ===========================================================================
// Configuration
// ===========================================================================
inline void
QosWeightedMloScheduler::SetWeights(AcIndex ac, double delay, double jitter,
                                    double loss, double tp)
{
    m_weights[static_cast<uint8_t>(ac)] = {delay, jitter, loss, tp};
}

inline void
QosWeightedMloScheduler::SetTargets(AcIndex ac, double delayMs, double jitterMs,
                                    double lossRate, double tpMbps)
{
    // Map legacy SetTargets call to AcGoals
    m_goals[static_cast<uint8_t>(ac)] = {tpMbps, delayMs, jitterMs, lossRate};
}

inline void
QosWeightedMloScheduler::SetGoals(AcIndex ac, double tpMbps, double delayMs,
                                   double jitterMs, double lossRate)
{
    m_goals[static_cast<uint8_t>(ac)] = {tpMbps, delayMs, jitterMs, lossRate};
}

inline void
QosWeightedMloScheduler::EnableDecisionCsv(const std::string& filename,
                                            const std::string& nodeContext)
{
    m_csvEnabled  = true;
    m_nodeContext = nodeContext;
    m_decisionCsv.open(filename, std::ios::out | std::ios::app);
    if (m_decisionCsv.is_open() && m_decisionCsv.tellp() == std::streampos(0)) {
        m_decisionCsv << "Timestamp,NodeContext,AC,SelectedLink,Decision,"
                         "CurrentSat,ExpectedSat,CompetitionPressure,"
                         "EffectiveCapMbps,CapabilityScore,"
                         "AvgDelayMs,AvgJitterMs,PER,ChannelUtil,Throughput\n";
    }
}

// ===========================================================================
// External feed APIs
// ===========================================================================
inline void
QosWeightedMloScheduler::FeedLinkMetrics(uint8_t linkId, AcIndex ac,
                                          uint32_t txBytes, uint32_t txFrames,
                                          uint32_t dropFrames)
{
    auto& s = m_metrics[linkId][static_cast<uint8_t>(ac)];
    s.txBytes    += txBytes;
    s.txSuccess  += txFrames;
    s.dropFrames += dropFrames;
}

inline void
QosWeightedMloScheduler::FeedLinkDrop(uint8_t linkId, AcIndex ac)
{
    m_metrics[linkId][static_cast<uint8_t>(ac)].dropFrames++;
}

inline void
QosWeightedMloScheduler::FeedLinkTxAttempt(uint8_t linkId, AcIndex ac,
                                             uint32_t frames)
{
    m_metrics[linkId][static_cast<uint8_t>(ac)].txAttempts += frames;
}

inline void
QosWeightedMloScheduler::FeedLinkTxStart(uint8_t linkId)
{
    if (m_linkTxDepth[linkId] == 0)
        m_linkTxStartTime[linkId] = Simulator::Now();
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

// Phase 2 feed: PHY state (called from PhyTxPsduBegin)
inline void
QosWeightedMloScheduler::FeedLinkPhyState(uint8_t linkId,
                                           const WifiTxVector& txVector,
                                           double per)
{
    double dataRateMbps = 0.0;
    try {
        // GetMode() gives the data MCS; GetDataRate returns bits/s
        WifiMode mode = txVector.GetMode();
        dataRateMbps  = mode.GetDataRate(txVector) / 1e6;
    } catch (...) {
        return;
    }

    auto& phy = m_phyState[linkId];
    if (!phy.valid) {
        phy.ewmaDataRateMbps = dataRateMbps;
        phy.ewmaPer          = per;
        phy.valid            = true;
    } else {
        constexpr double alpha = 0.4; // EWMA smoothing //ALTEREI era 0.2
        phy.ewmaDataRateMbps = (1.0 - alpha) * phy.ewmaDataRateMbps + alpha * dataRateMbps;
        phy.ewmaPer          = (1.0 - alpha) * phy.ewmaPer          + alpha * per;
    }
}

// Phase 3 feed: per-packet transmitted info
inline void
QosWeightedMloScheduler::FeedPacketTransmitted(uint8_t linkId, AcIndex ac,
                                                uint32_t bytes, double airtimeSec,
                                                const std::string& peerAddress,
                                                uint8_t tid)
{
    auto& tc = m_traffic[linkId];
    auto  flowId = std::make_pair(peerAddress, tid);

    switch (ac) {
    case AC_VO:
        tc.voBytes += bytes; tc.voAirtimeSec += airtimeSec;
        tc.voFlowSet.insert(flowId); break;
    case AC_VI:
        tc.viBytes += bytes; tc.viAirtimeSec += airtimeSec;
        tc.viFlowSet.insert(flowId); break;
    case AC_BE:
        tc.beBytes += bytes; tc.beAirtimeSec += airtimeSec;
        tc.beFlowSet.insert(flowId); break;
    case AC_BK:
        tc.bkBytes += bytes; tc.bkAirtimeSec += airtimeSec;
        tc.bkFlowSet.insert(flowId); break;
    default: break;
    }
}

inline void
QosWeightedMloScheduler::FeedStaQos(Mac48Address sta, AcIndex ac,
                                    double tpMbps, double delayMs,
                                    double jitterMs, double lossRate)
{
    auto& q = m_staQos[{sta, static_cast<uint8_t>(ac)}];
    q.tpMbps = tpMbps; q.delayMs = delayMs; q.jitterMs = jitterMs;
    q.lossRate = lossRate; q.valid = true; q.samples++;
}

// ===========================================================================
// Utility function (sigmoid)
// ===========================================================================
inline double
QosWeightedMloScheduler::UtilityFunction(double value, double target,
                                          bool lowerIsBetter) const
{
    if (target <= 0.0) return 0.5;
    double ratio = value / target;
    if (lowerIsBetter)
        return 1.0 / (1.0 + std::exp(5.0 * (ratio - 1.0)));
    else
        return 1.0 / (1.0 + std::exp(5.0 * (1.0 - ratio)));
}

// ===========================================================================
// Phase 1 — Goal-Awareness Engine
// ===========================================================================
inline QosSatisfaction
QosWeightedMloScheduler::ComputeQosSatisfaction(AcIndex ac, const Mac48Address& sta,
                                                uint8_t linkId) const
{
    (void)linkId; // as medições reais são end-to-end por-STA (link corrente)
    QosSatisfaction sat{};
    uint8_t acIdx = static_cast<uint8_t>(ac);

    auto gIt = m_goals.find(acIdx);
    auto wIt = m_weights.find(acIdx);
    if (gIt == m_goals.end() || wIt == m_weights.end()) return sat;

    const AcGoals&   g = gIt->second;
    const AcWeights& w = wIt->second;

    // Métricas REAIS por-(STA,AC), medidas end-to-end nos sinks e alimentadas
    // via FeedStaQos. Sem entrada válida ainda (arranque), devolve zeros — o
    // bootstrap sticky trata do cold-start.
    auto qIt = m_staQos.find({sta, acIdx});
    if (qIt == m_staQos.end() || !qIt->second.valid) return sat;
    const StaQos& q = qIt->second;

    sat.throughput = UtilityFunction(q.tpMbps,   g.targetThroughputMbps, false);
    sat.delay      = UtilityFunction(q.delayMs,  g.maxDelayMs,           true);
    sat.jitter     = UtilityFunction(q.jitterMs, g.maxJitterMs,          true);
    sat.loss       = UtilityFunction(q.lossRate, g.maxPacketLoss,        true);

    double totalW  = w.throughputWeight + w.delayWeight + w.jitterWeight + w.lossWeight;
    if (totalW <= 0.0) totalW = 1.0;

    sat.index = (w.throughputWeight * sat.throughput
               + w.delayWeight      * sat.delay
               + w.jitterWeight     * sat.jitter
               + w.lossWeight       * sat.loss) / totalW;

    return sat;
}

inline double
QosWeightedMloScheduler::CurrentQosSatisfaction(AcIndex ac, const Mac48Address& sta) const
{
    auto it = m_currentSatisfaction.find({sta, static_cast<uint8_t>(ac)});
    if (it == m_currentSatisfaction.end()) return 0.0;
    return it->second.index;
}

// ===========================================================================
// Phase 2 — Link Capability Engine
// ===========================================================================
inline void
QosWeightedMloScheduler::UpdateLinkCapability(uint8_t linkId, double dt)
{
    auto& cap = m_linkCapability[linkId];
    auto& phy = m_phyState[linkId];

    // Estimated capacity from PHY state
    if (phy.valid) {
        cap.estimatedCapacityMbps = phy.ewmaDataRateMbps * (1.0 - phy.ewmaPer);
    } else {
        // Fallback: use static max for the band
        cap.estimatedCapacityMbps = (linkId == m_fastLinkId) ? 400.0 : 150.0;
    }

    // Occupied capacity = sum of throughputs across all ACs on this link
    auto& tc = m_traffic[linkId];
    double occupied = tc.voThroughputMbps + tc.viThroughputMbps
                    + tc.beThroughputMbps + tc.bkThroughputMbps;

    cap.availableCapacityMbps = std::max(0.0, cap.estimatedCapacityMbps - occupied);

    // ---- Priority-aware available capacity per AC ----
    // Real 802.11 EDCA priority order (highest to lowest): VO > VI > BE > BK.
    // A higher-priority AC only "sees" the throughput of ACs at its own
    // priority level or above as competing for the channel — it doesn't lose
    // capacity just because a lower-priority AC is transmitting more, since
    // it always wins channel access first via shorter AIFS/CW.
    double voTp = tc.voThroughputMbps;
    double viTp = tc.viThroughputMbps;
    double beTp = tc.beThroughputMbps;
    double bkTp = tc.bkThroughputMbps;

    cap.availableCapacityPerAcMbps[static_cast<uint8_t>(AC_VO)] =
        std::max(0.0, cap.estimatedCapacityMbps - voTp);
    cap.availableCapacityPerAcMbps[static_cast<uint8_t>(AC_VI)] =
        std::max(0.0, cap.estimatedCapacityMbps - voTp - viTp);
    cap.availableCapacityPerAcMbps[static_cast<uint8_t>(AC_BE)] =
        std::max(0.0, cap.estimatedCapacityMbps - voTp - viTp - beTp);
    cap.availableCapacityPerAcMbps[static_cast<uint8_t>(AC_BK)] =
        std::max(0.0, cap.estimatedCapacityMbps - voTp - viTp - beTp - bkTp);

    // Free airtime
    double util = 0.0;
    if (dt > 0.0) {
        util = std::min(1.0, m_linkBusyTime[linkId] / dt);
    }
    cap.freeAirtime = std::max(0.0, 1.0 - util);

    // Capability score = normalised available fraction
    cap.capabilityScore = (cap.estimatedCapacityMbps > 0.0)
                            ? std::min(1.0, cap.availableCapacityMbps / cap.estimatedCapacityMbps)
                            : 0.0;
}

// ===========================================================================
// Phase 3 — Traffic Composition Engine
// ===========================================================================
inline void
QosWeightedMloScheduler::UpdateTrafficComposition(uint8_t linkId, double dt)
{
    auto& tc = m_traffic[linkId];
    double invDt = (dt > 0.0) ? 1.0 / dt : 0.0;

    tc.voThroughputMbps = tc.voBytes * 8.0 * invDt / 1e6;
    tc.viThroughputMbps = tc.viBytes * 8.0 * invDt / 1e6;
    tc.beThroughputMbps = tc.beBytes * 8.0 * invDt / 1e6;
    tc.bkThroughputMbps = tc.bkBytes * 8.0 * invDt / 1e6;

    double totalAirtime = tc.voAirtimeSec + tc.viAirtimeSec
                        + tc.beAirtimeSec + tc.bkAirtimeSec;
    double invTA = (totalAirtime > 0.0) ? 1.0 / totalAirtime : 0.0;
    tc.voAirtimeFrac = tc.voAirtimeSec * invTA;
    tc.viAirtimeFrac = tc.viAirtimeSec * invTA;
    tc.beAirtimeFrac = tc.beAirtimeSec * invTA;
    tc.bkAirtimeFrac = tc.bkAirtimeSec * invTA;

    tc.voFlows = static_cast<uint32_t>(tc.voFlowSet.size());
    tc.viFlows = static_cast<uint32_t>(tc.viFlowSet.size());
    tc.beFlows = static_cast<uint32_t>(tc.beFlowSet.size());
    tc.bkFlows = static_cast<uint32_t>(tc.bkFlowSet.size());

    // Reset accumulators
    tc.voBytes = tc.viBytes = tc.beBytes = tc.bkBytes = 0;
    tc.voAirtimeSec = tc.viAirtimeSec = tc.beAirtimeSec = tc.bkAirtimeSec = 0.0;
    tc.voFlowSet.clear(); tc.viFlowSet.clear();
    tc.beFlowSet.clear(); tc.bkFlowSet.clear();
}

// ===========================================================================
// Phase 4 — EDCA Competition Engine
// ===========================================================================
inline double
QosWeightedMloScheduler::NormalisedLoad(AcIndex ac, uint8_t linkId) const
{
    auto it = m_traffic.find(linkId);
    if (it == m_traffic.end()) return 0.0;
    const auto& tc = it->second;
    switch (ac) {
    case AC_VO: return tc.voAirtimeFrac;
    case AC_VI: return tc.viAirtimeFrac;
    case AC_BE: return tc.beAirtimeFrac;
    case AC_BK: return tc.bkAirtimeFrac;
    default:    return 0.0;
    }
}

inline void
QosWeightedMloScheduler::UpdateEdcaCompetition(uint8_t linkId)
{
    // Row index in m_edcaWeights: BK=0, BE=1, VI=2, VO=3
    auto acToRow = [](AcIndex ac) -> int {
        switch (ac) { case AC_BK: return 0; case AC_BE: return 1;
                      case AC_VI: return 2; case AC_VO: return 3; default: return 1; }
    };

    const AcIndex allAcs[] = {AC_BK, AC_BE, AC_VI, AC_VO};

    auto& capLink = m_linkCapability[linkId];

    for (AcIndex candAc : allAcs) {
        uint8_t  candIdx = static_cast<uint8_t>(candAc);
        int      candRow = acToRow(candAc);
        auto&    edca    = m_edca[linkId][candIdx];

        double pressure = 0.0;
        for (AcIndex otherAc : allAcs) {
            if (otherAc == candAc) continue;
            int otherRow = acToRow(otherAc);
            pressure += m_edcaWeights[candRow][otherRow] * NormalisedLoad(otherAc, linkId);
        }
        edca.competitionPressure = pressure;

        double opp = capLink.freeAirtime / (1.0 + pressure);
        edca.expectedAccessOpportunity = std::min(1.0, std::max(0.0, opp));

        edca.effectiveAvailableCapacityMbps =
            capLink.availableCapacityPerAcMbps.count(candIdx)
                ? capLink.availableCapacityPerAcMbps.at(candIdx) * edca.expectedAccessOpportunity
                : capLink.availableCapacityMbps * edca.expectedAccessOpportunity; // fallback, shouldn't normally trigger
    }
}

// ===========================================================================
// Phase 5+6 — Migration Decision Engine
// ===========================================================================
inline double
QosWeightedMloScheduler::ComputeExpectedQosSatisfaction(AcIndex ac,
                                                         const Mac48Address& sta,
                                                         uint8_t linkId) const
{
    uint8_t  acIdx = static_cast<uint8_t>(ac);
    auto gIt = m_goals.find(acIdx);
    auto wIt = m_weights.find(acIdx);
    if (gIt == m_goals.end() || wIt == m_weights.end()) return 0.0;
    const AcGoals&   g = gIt->second;
    const AcWeights& w = wIt->second;

    // Capacity-based throughput projection
    auto edcaIt = m_edca.find(linkId);
    double effCap = 0.0;
    if (edcaIt != m_edca.end()) {
        auto acEdca = edcaIt->second.find(acIdx);
        if (acEdca != edcaIt->second.end())
            effCap = acEdca->second.effectiveAvailableCapacityMbps;
    }

    // Co-occupancy: when N other STAs of the same AC already share this link,
    // divide the projected capacity by N+1 (this STA + the N others).
    // If only this STA would be here, denominator = 1 and no change applies.
    uint32_t coRes = CountCoResidents(ac, sta, linkId);
    double effCapShared = (coRes > 0)
                            ? effCap / static_cast<double>(coRes + 1)
                            : effCap;
    double expTp = UtilityFunction(effCapShared, g.targetThroughputMbps, false);

    // For delay/jitter/loss: use current measured values on this link as a proxy,
    // combined with a penalty proportional to EDCA competition pressure
    double pressure = 0.0;
    if (edcaIt != m_edca.end()) {
        auto acEdca = edcaIt->second.find(acIdx);
        if (acEdca != edcaIt->second.end())
            pressure = acEdca->second.competitionPressure;
    }

    // Read current metrics on the target link
    double delayMs  = 0.0, jitterMs = 0.0, per = 0.0;
    auto lIt = m_metrics.find(linkId);
    if (lIt != m_metrics.end()) {
        auto aIt = lIt->second.find(acIdx);
        if (aIt != lIt->second.end()) {
            delayMs  = aIt->second.avgQueueDelayMs  * (1.0 + pressure * 0.3); //ALTEREI era 0.2
            jitterMs = aIt->second.avgJitterMs       * (1.0 + pressure * 0.2);
            per      = aIt->second.packetErrorRate;
        }
    }

    double expDelay  = UtilityFunction(delayMs,  g.maxDelayMs,    true);
    double expJitter = UtilityFunction(jitterMs, g.maxJitterMs,    true);
    double expLoss   = UtilityFunction(per,      g.maxPacketLoss,  true);

    // Penalise by PER and capability score
    auto capIt = m_linkCapability.find(linkId);
    double capScore = (capIt != m_linkCapability.end()) ? capIt->second.capabilityScore : 0.5;
    double perPenalty = 1.0 - per; // PER already in [0,1]

    double totalW = w.throughputWeight + w.delayWeight + w.jitterWeight + w.lossWeight;
    if (totalW <= 0.0) totalW = 1.0;

    double baseSat = (w.throughputWeight * expTp
                    + w.delayWeight      * expDelay
                    + w.jitterWeight     * expJitter
                    + w.lossWeight       * expLoss) / totalW;

    // Blend with capability and PER
    double expectedScore = baseSat * 0.65 + capScore * 0.25 + perPenalty * 0.1; //ALTEREI era 0.8, 0.1, 0.1

    // --- ALTRUISTIC PENALTY ---
    // Two components (no per-scenario hardcoding — both work for any AC
    // combination):
    //
    // (A) REACTIVE: if another AC is already starving on this link, penalise
    //     entering it further (original behaviour — kept as-is).
    // (B) PREVENTIVE: even if the other AC is NOT yet starving, check whether
    //     it would lose its "comfortable" capacity headroom if this candidate
    //     joins. This reuses otherAc's OWN effectiveAvailableCapacityMbps
    //     (already computed by UpdateEdcaCompetition using the correct,
    //     already-validated direction of m_edcaWeights — "how much otherAc,
    //     as resident, feels the load already on this link"). We do NOT
    //     introduce a second, reverse-direction matrix lookup here: that
    //     would silently assume m_edcaWeights is symmetric (that "how much X
    //     feels Y" equals "how much Y would be hurt by X joining"), which is
    //     not a property the matrix actually has or needs to have.
    //
    //     Instead, we ask a self-contained question: "does otherAc's current
    //     effective capacity already sit close to what it needs (its own
    //     target throughput)?" If otherAc's headroom is thin, ANY new
    //     candidate adding load is risky — penalise candidates proportionally
    //     to how much load they'd actually bring (their own normalised load),
    //     not to a borrowed/inverted weight.
    double altruisticPenalty = 0.0;
    for (AcIndex otherAc : {AC_BK, AC_BE, AC_VI, AC_VO}) {
        if (otherAc == ac) continue;
        uint8_t otherAcIdx = static_cast<uint8_t>(otherAc);

        // Find worst-satisfied STA of this AC type that is resident on linkId.
        bool anyResident = false;
        double worstOtherSat = 1.0;
        for (const auto& [key, resLink] : m_lastSelectedLink) {
            if (key.second != otherAcIdx || resLink != linkId) continue;
            anyResident = true;
            double sat = m_currentSatisfaction.count(key)
                           ? m_currentSatisfaction.at(key).index : 1.0;
            if (sat < worstOtherSat) worstOtherSat = sat;
        }
        if (!anyResident) continue;

        double otherSat = worstOtherSat;

        // (A) Reactive: other AC already starving — penalise as before.
        if (otherSat < 0.45) {
            altruisticPenalty += (0.6 - otherSat);
        }

        // (B) Preventive: how much capacity headroom does otherAc currently
        // have on THIS link, relative to its own goal? If headroom is thin,
        // treat any incoming load from the candidate as a fairness risk.
        auto otherGIt = m_goals.find(otherAcIdx);
        if (edcaIt != m_edca.end() && otherGIt != m_goals.end()) {
            auto otherEdca = edcaIt->second.find(otherAcIdx);
            if (otherEdca != edcaIt->second.end()) {
                double headroomRatio = otherEdca->second.effectiveAvailableCapacityMbps
                                      / std::max(1.0, otherGIt->second.targetThroughputMbps);
                // headroomRatio >= 1.0 means otherAc already has at least its
                // full target available — comfortable, low risk from a newcomer.
                // headroomRatio < 1.0 means otherAc is already tight — a
                // newcomer's load is a real threat to it.
                if (headroomRatio < 1.0 && otherSat < 0.75) {
                    double candidateLoadProxy = NormalisedLoad(ac, linkId);
                    if (candidateLoadProxy <= 0.0) candidateLoadProxy = 0.15; // cold-start default
                    double tightness = (1.0 - headroomRatio); // 0 = fine, 1 = no headroom at all
                    double preventiveTerm = std::min(0.5, tightness * candidateLoadProxy * 2.0);
                    altruisticPenalty = std::max(altruisticPenalty, preventiveTerm);
                }
            }
        }
    }

    return std::max(0.0, expectedScore - altruisticPenalty);
}

inline uint32_t
QosWeightedMloScheduler::CountCoResidents(AcIndex ac, const Mac48Address& sta,
                                           uint8_t linkId) const
{
    uint8_t acIdx = static_cast<uint8_t>(ac);
    uint32_t count = 0;
    for (const auto& [key, link] : m_lastSelectedLink) {
        if (link == linkId && key.second == acIdx && key.first != sta) ++count;
    }
    return count;
}

inline bool
QosWeightedMloScheduler::AnyLinkMeetsGoals(AcIndex ac, const Mac48Address& sta,
                                             const std::list<uint8_t>& links) const
{
    for (uint8_t linkId : links) {
        if (ComputeExpectedQosSatisfaction(ac, sta, linkId) >= 1.0) return true;
    }
    return false;
}

inline double
QosWeightedMloScheduler::ComputeBestAchievableScore(AcIndex ac, const Mac48Address& sta,
                                                     uint8_t linkId) const
{
    return ComputeExpectedQosSatisfaction(ac, sta, linkId);
}

inline bool
QosWeightedMloScheduler::MeetsOwnGoals(AcIndex ac, const Mac48Address& sta,
                                         uint8_t linkId) const
{
    return ComputeExpectedQosSatisfaction(ac, sta, linkId) >= m_ownGoalsThreshold;
}

inline bool
QosWeightedMloScheduler::WouldHarmResident(AcIndex ac, const Mac48Address& sta,
                                            uint8_t linkId,
                                            AcIndex* harmedAcOut,
                                            double* worstSatOut) const
{
    // Find the worst-affected resident (STA, AC) pair on linkId, excluding
    // this (sta, ac) entry. Uses the same composite satisfaction index as
    // the rest of the scheduler — all four QoS dimensions count.
    double worstSat = 1.0;
    uint8_t worstAcIdx = static_cast<uint8_t>(ac);
    bool foundResident = false;

    for (const auto& [key, residentLink] : m_lastSelectedLink) {
        if (residentLink != linkId) continue;
        // Skip self
        if (key.second == static_cast<uint8_t>(ac)) continue; // skip ALL same-AC (cross-AC harm only)

        double otherSat = m_currentSatisfaction.count(key)
                              ? m_currentSatisfaction.at(key).index : 1.0;
        foundResident = true;
        if (otherSat < worstSat) {
            worstSat   = otherSat;
            worstAcIdx = key.second;
        }
    }

    if (harmedAcOut) *harmedAcOut = static_cast<AcIndex>(worstAcIdx);
    if (worstSatOut) *worstSatOut = foundResident ? worstSat : 1.0;

    if (!foundResident) return false;
    return worstSat <= m_harmThreshold;
}

inline uint8_t
QosWeightedMloScheduler::DecideLinkMigration(AcIndex ac, const Mac48Address& sta,
                                              const std::list<uint8_t>& eligibleLinks)
{
    if (eligibleLinks.empty()) return m_slowLinkId;
    if (eligibleLinks.size() == 1) {
        uint8_t onlyLink = eligibleLinks.front();
        m_lastSelectedLink[{sta, static_cast<uint8_t>(ac)}] = onlyLink;
        m_metrics[onlyLink][static_cast<uint8_t>(ac)].enqueueCount++;
        return onlyLink;
    }

    uint8_t acIdx = static_cast<uint8_t>(ac);
    StaAcKey staAcKey = {sta, acIdx};

    // Determine current link (anchor)
    auto lastIt = m_lastSelectedLink.find(staAcKey);
    bool hasAnchor = (lastIt != m_lastSelectedLink.end())
                  && (std::find(eligibleLinks.begin(), eligibleLinks.end(),
                                lastIt->second) != eligibleLinks.end());
    uint8_t currentLink = hasAnchor ? lastIt->second : eligibleLinks.front();

    // ---- Phase 5: check if we can stay ----
    double currentSat = m_currentSatisfaction.count(staAcKey)
                            ? m_currentSatisfaction.at(staAcKey).index : 0.0;

    std::string decision = "STAY_SATISFIED";
    uint8_t selectedLink = currentLink;
    double  selectedExpSat = currentSat;

    // ---- Priority bootstrap, held until first measured satisfaction ----
    // Até existir uma amostra de satisfação MEDIDA para este (STA,AC),
    // mantém a alocação por prioridade EDCA (VO/VI -> fast, BE/BK -> slow).
    // Isto impede que o transiente de cold-start (effCap=capacidade total nos
    // links vazios) puxe ACs de baixa prioridade para o fast e contamine a
    // primeira medição dos residentes de alta prioridade.
    bool useBootstrap = false;
    if (m_hasMeasuredSat.count(staAcKey) == 0) {
        uint8_t bootLink = (acIdx >= static_cast<uint8_t>(AC_VI))
                           ? m_fastLinkId : m_slowLinkId;
        if (std::find(eligibleLinks.begin(), eligibleLinks.end(), bootLink)
                != eligibleLinks.end()) {
            selectedLink = bootLink;
            decision = "BOOTSTRAP_PRIORITY";
            useBootstrap = true;
        }
    }

    if (!useBootstrap) {
    if (currentSat >= m_stayThreshold && hasAnchor) {
        decision = "STAY_SATISFIED";
    } else {
        uint8_t bestCat1Link = 0; double bestCat1Score = -1.0; bool hasCat1 = false;
        uint8_t bestCat2Link = 0; double bestCat2Score = -1.0; bool hasCat2 = false;
        uint8_t bestCat3Link = currentLink; double bestCat3Score = ComputeBestAchievableScore(ac, sta, currentLink);

        for (uint8_t linkId : eligibleLinks) {
            bool meetsOwn = MeetsOwnGoals(ac, sta, linkId);
            bool harms    = WouldHarmResident(ac, sta, linkId);
            double score  = ComputeExpectedQosSatisfaction(ac, sta, linkId);

            if (meetsOwn && !harms) {
                if (!hasCat1 || score > bestCat1Score) { bestCat1Score = score; bestCat1Link = linkId; hasCat1 = true; }
            } else if (meetsOwn && harms) {
                if (!hasCat2 || score > bestCat2Score) { bestCat2Score = score; bestCat2Link = linkId; hasCat2 = true; }
            }
            double bestScoreSoFar = ComputeBestAchievableScore(ac, sta, linkId);
            if (bestScoreSoFar > bestCat3Score) { bestCat3Score = bestScoreSoFar; bestCat3Link = linkId; }
        }

        uint8_t bestLink; double bestScore; std::string categoryTag;
        if (hasCat1)      { bestLink = bestCat1Link; bestScore = bestCat1Score; categoryTag = "CAT1_CLEAN"; }
        else if (hasCat2) { bestLink = bestCat2Link; bestScore = bestCat2Score; categoryTag = "CAT2_PRIORITY_OVERRIDE"; }
        else              { bestLink = bestCat3Link; bestScore = bestCat3Score; categoryTag = "CAT3_BEST_EFFORT"; }

        selectedExpSat = bestScore;

        // Anti-downgrade guard para VO/VI. Um AC de alta prioridade no fast
        // não deve descer para o slow (a) por ruído de cold-start se for o
        // único do seu AC no fast, ou (b) para um link onde resida um BE/BK
        // (esfomeá-los-ia por EDCA — regra: BE/BK nunca partilham com VO/VI).
        // EXCEÇÃO: se o VO/VI estiver genuinamente a não cumprir os objetivos
        // no fast (esfomeado), cede e pode descer — tem prioridade sobre o BE.
        // O sinal de fome usa a satisfação PROJETADA no fast (estável), não o
        // curSat medido (ruidoso no cold-start).
        bool isHighPrio = (acIdx >= static_cast<uint8_t>(AC_VI));
        if (isHighPrio && bestLink == m_slowLinkId && currentLink == m_fastLinkId) {
            // Sinal de fome = satisfação MEDIDA no fast (currentSat, fiável após
            // o warmup de 2 janelas), não a projecção (instável no 6GHz).
            constexpr double kStarvationFloor = 0.60; // tunável
            bool starvingOnFast = (currentSat < kStarvationFloor);

            bool soleOccupant = CountCoResidents(ac, sta, m_fastLinkId) == 0;
            bool wouldStarveLowPrio = false;
            for (const auto& [key, resLink] : m_lastSelectedLink) {
                if (resLink == bestLink && key.second < static_cast<uint8_t>(AC_VI)) {
                    wouldStarveLowPrio = true;
                    break;
                }
            }
            if ((soleOccupant || wouldStarveLowPrio) && !starvingOnFast) {
                bestLink = currentLink; // veto: mantém no fast
            }
        }

        if (bestLink != currentLink) {
            double improvement = bestScore - currentSat;
            if (improvement > m_migrationThreshold) {
                selectedLink = bestLink;
                decision     = "MIGRATE_" + categoryTag;
            } else {
                selectedLink = currentLink;
                decision     = "STAY_HYSTERESIS";
            }
        } else {
            decision = hasAnchor ? "STAY_HYSTERESIS" : "STAY_BEST";
        }
    }
    } // end if (!useBootstrap)

    m_lastSelectedLink[staAcKey] = selectedLink;
    m_metrics[selectedLink][acIdx].enqueueCount++;

    // ---- CSV logging ----
    if (m_csvEnabled && m_decisionCsv.is_open()) {
        double now = Simulator::Now().GetSeconds();
        auto& lm   = m_metrics[selectedLink][acIdx];
        auto& edca = m_edca[selectedLink][acIdx];
        auto& cap  = m_linkCapability[selectedLink];

        // Métricas REAIS por-STA (as que alimentam a satisfação); fallback aos
        // proxies per-link quando ainda não há amostra real.
        double logDelay = lm.avgQueueDelayMs, logJitter = lm.avgJitterMs;
        double logPer   = lm.packetErrorRate, logTp = lm.achievedThroughputMbps;
        auto qIt = m_staQos.find(staAcKey);
        if (qIt != m_staQos.end() && qIt->second.valid) {
            logDelay  = qIt->second.delayMs;
            logJitter = qIt->second.jitterMs;
            logPer    = qIt->second.lossRate;
            logTp     = qIt->second.tpMbps;
        }

        m_decisionCsv << now << ',' << m_nodeContext << ',' << +ac << ','
                      << +selectedLink << ',' << decision << ','
                      << currentSat << ',' << selectedExpSat << ','
                      << edca.competitionPressure << ','
                      << edca.effectiveAvailableCapacityMbps << ','
                      << cap.capabilityScore << ','
                      << logDelay << ',' << logJitter << ','
                      << logPer << ',' << lm.channelUtilization << ','
                      << logTp << '\n';
    }

    return selectedLink;
}

// ===========================================================================
// Periodic metrics update (all engines)
// ===========================================================================
inline void
QosWeightedMloScheduler::UpdatePeriodicMetrics()
{
    double now = Simulator::Now().GetSeconds();
    double dt  = now - m_lastPeriodicUpdateTime;
    if (dt <= 0.0) dt = m_metricsIntervalSec;

    // ---- Update global AC throughput ----
    for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
        uint8_t acIdx = static_cast<uint8_t>(ac);
        auto&   acSt  = m_acStates[acIdx];
        uint64_t bytes = acSt.bytesSent - m_lastAcBytesSent[acIdx];
        acSt.throughputMbps = (bytes * 8.0) / (dt * 1e6);
        m_lastAcBytesSent[acIdx] = acSt.bytesSent;
    }

    for (auto& [linkId, acMap] : m_metrics) {
        // ---- Channel utilization ----
        double util = 0.0;
        if (dt > 0.0) util = std::min(1.0, m_linkBusyTime[linkId] / dt);
        // (busy time is reset inside UpdateLinkCapability after use)

        for (auto& [acIdxRaw, ls] : acMap) {
            // Throughput per (link, AC)
            ls.achievedThroughputMbps = (ls.txBytes * 8.0) / (dt * 1e6);
            ls.txBytes = 0;

            // PER — use EWMA instead of a hard per-window reset.
            //
            // BUG THIS FIXES: txAttempts is incremented at PHY TX START
            // (FeedLinkTxAttempt), while txSuccess is incremented only when
            // the ACK/BlockAck confirms success (FeedLinkMetrics) — which
            // can arrive AFTER this 0.5s window has already reset both
            // counters to zero, especially under high delay/contention
            // (exactly when an accurate PER matters most). A hard reset
            // every window systematically inflates PER under load, because
            // a TX attempted near the end of a window whose confirmation
            // lands in the NEXT window is counted as 100% loss in the
            // window it started in, and contributes nothing to the window
            // it actually succeeded in.
            //
            // Fix: keep a running (EWMA) estimate instead. If a window has
            // zero attempts, leave the estimate unchanged rather than
            // snapping it to 0 (no evidence either way).
            if (ls.txAttempts > 0) {
                // PER a partir de drops REAIS (FeedLinkDrop) sobre tentativas.
                // NÃO usar txSuccess: vem do trace AckedMpdu, que subconta ~5%
                // dos MPDUs entregues, inflando o PER para ~95% falsamente.
                double windowPer = std::min(1.0,
                    static_cast<double>(ls.dropFrames) / ls.txAttempts);
                constexpr double perAlpha = 0.3; // smoothing factor
                if (!ls.perInitialized) {
                    ls.packetErrorRate = windowPer; // first-ever sample
                    ls.perInitialized  = true;
                } else {
                    ls.packetErrorRate = (1.0 - perAlpha) * ls.packetErrorRate + perAlpha * windowPer;
                }
            }
            ls.txAttempts = 0;
            ls.txSuccess  = 0;
            ls.dropFrames = 0;
            ls.enqueueCount = 0;

            ls.channelUtilization = util;

            // HOL delay estimate
            if (ls.queueLength > 0) {
                double nowMs  = Simulator::Now().GetMilliSeconds();
                double waitMs = nowMs - ls.lastEnqueueTimeMs;
                ls.holDelay   = std::min(waitMs, MAX_DELAY_MS);
            } else {
                ls.holDelay = 0.0;
            }
        }

        // Phase 3 — traffic composition
        UpdateTrafficComposition(linkId, dt);

        // Phase 2 — link capability (uses updated traffic composition)
        UpdateLinkCapability(linkId, dt);

        // Reset busy time for next window
        m_linkBusyTime[linkId] = 0.0;

        // Phase 4 — EDCA competition
        UpdateEdcaCompetition(linkId);
    }

    // Phase 1 — update current satisfaction for each (STA, AC) on its current link
    for (const auto& [key, linkId] : m_lastSelectedLink) {
        AcIndex ac = static_cast<AcIndex>(key.second);
        m_currentSatisfaction[key] = ComputeQosSatisfaction(ac, key.first, linkId);
        // "Medido" = já existem >=2 amostras reais por-STA (FeedStaQos). Saltar
        // a 1ª janela (contaminada pelo ramp-up) evita migrações prematuras de
        // cold-start; o bootstrap sticky mantém a alocação por prioridade até lá.
        constexpr uint32_t kWarmupSamples = 2;
        if (m_staQos.count(key) && m_staQos.at(key).samples >= kWarmupSamples) {
            m_hasMeasuredSat.insert(key);
        }
    }

    // Update queue lengths from MAC
    Ptr<WifiMac> mac = GetMac();
    if (mac) {
        for (AcIndex ac : {AC_BE, AC_BK, AC_VI, AC_VO}) {
            Ptr<QosTxop> txop = mac->GetQosTxop(ac);
            if (!txop) continue;
            Ptr<WifiMacQueue> q = txop->GetWifiMacQueue();
            if (!q) continue;
            uint32_t nPkts  = q->GetNPackets();
            uint32_t nBytes = q->GetNBytes();
            for (auto& [linkId, acMap] : m_metrics) {
                auto aIt = acMap.find(static_cast<uint8_t>(ac));
                if (aIt != acMap.end()) {
                    aIt->second.queueLength = nPkts;
                    aIt->second.queueBytes  = nBytes;
                }
            }
        }
    }

    m_lastPeriodicUpdateTime = now;
    m_updateEvent = Simulator::Schedule(
        Seconds(m_metricsIntervalSec),
        &QosWeightedMloScheduler::UpdatePeriodicMetrics, this);
}

// ===========================================================================
// ns-3 scheduler interface — delegations
// ===========================================================================
inline std::optional<WifiContainerQueueId>
QosWeightedMloScheduler::GetNext(AcIndex ac, std::optional<uint8_t> linkId,
                                  bool skipBlockedQueues)
{
    EnsureDelegate();
    return m_delegate->GetNext(ac, linkId, skipBlockedQueues);
}

inline std::optional<WifiContainerQueueId>
QosWeightedMloScheduler::GetNext(AcIndex ac, std::optional<uint8_t> linkId,
                                  const WifiContainerQueueId& prevQueueId,
                                  bool skipBlockedQueues)
{
    EnsureDelegate();
    return m_delegate->GetNext(ac, linkId, prevQueueId, skipBlockedQueues);
}

inline std::list<uint8_t>
QosWeightedMloScheduler::GetLinkIds(AcIndex ac, Ptr<const WifiMpdu> mpdu,
                                     const std::list<WifiQueueBlockedReason>& ignoredReasons)
{
    EnsureDelegate();
    const auto eligible = m_delegate->GetLinkIds(ac, mpdu, ignoredReasons);
    if (eligible.empty()) return {};
    Mac48Address dest = (mpdu && !mpdu->GetHeader().GetAddr1().IsBroadcast())
                          ? mpdu->GetHeader().GetAddr1()
                          : Mac48Address("ff:ff:ff:ff:ff:ff");
    return {DecideLinkMigration(ac, dest, eligible)};
}

inline void
QosWeightedMloScheduler::BlockQueues(WifiQueueBlockedReason reason, AcIndex ac,
                                      const std::list<WifiContainerQueueType>& types,
                                      const Mac48Address& rxAddress, const Mac48Address& txAddress,
                                      const std::set<uint8_t>& tids, const std::set<uint8_t>& linkIds)
{
    EnsureDelegate();
    m_delegate->BlockQueues(reason, ac, types, rxAddress, txAddress, tids, linkIds);
}

inline void
QosWeightedMloScheduler::UnblockQueues(WifiQueueBlockedReason reason, AcIndex ac,
                                        const std::list<WifiContainerQueueType>& types,
                                        const Mac48Address& rxAddress, const Mac48Address& txAddress,
                                        const std::set<uint8_t>& tids, const std::set<uint8_t>& linkIds)
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
QosWeightedMloScheduler::GetAllQueuesBlockedOnLink(uint8_t linkId,
                                                    WifiQueueBlockedReason reason)
{
    EnsureDelegate();
    return m_delegate->GetAllQueuesBlockedOnLink(linkId, reason);
}

inline std::optional<WifiMacQueueScheduler::Mask>
QosWeightedMloScheduler::GetQueueLinkMask(AcIndex ac, const WifiContainerQueueId& queueId,
                                           uint8_t linkId)
{
    EnsureDelegate();
    return m_delegate->GetQueueLinkMask(ac, queueId, linkId);
}

// ===========================================================================
// Queue notification hooks
// ===========================================================================
inline Ptr<WifiMpdu>
QosWeightedMloScheduler::HasToDropBeforeEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu)
{
    EnsureDelegate();
    Ptr<WifiMpdu> dropped = m_delegate->HasToDropBeforeEnqueue(ac, mpdu);
    if (dropped) {
        for (auto& [linkId, acMap] : m_metrics)
            acMap[static_cast<uint8_t>(ac)].dropFrames++;
    }
    return dropped;
}

inline void
QosWeightedMloScheduler::NotifyEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu)
{
    EnsureDelegate();
    m_delegate->NotifyEnqueue(ac, mpdu);

    double nowMs = Simulator::Now().GetMilliSeconds();
    if (mpdu && mpdu->GetPacket())
        m_packetEnqueueTimes[mpdu->GetPacket()->GetUid()] = nowMs;

    // Update queue snapshot on all links (per-AC)
    Ptr<WifiMac> mac = GetMac();
    if (mac) {
        Ptr<QosTxop> txop = mac->GetQosTxop(ac);
        if (txop) {
            Ptr<WifiMacQueue> q = txop->GetWifiMacQueue();
            if (q) {
                uint32_t nPkts  = q->GetNPackets();
                uint32_t nBytes = q->GetNBytes();
                for (auto& [linkId, acMap] : m_metrics) {
                    auto& ls = acMap[static_cast<uint8_t>(ac)];
                    ls.queueLength       = nPkts;
                    ls.queueBytes        = nBytes;
                    ls.enqueueCount++;
                    ls.lastEnqueueTimeMs = nowMs;
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

    uint8_t  acIdx      = static_cast<uint8_t>(ac);
    double   nowMs      = Simulator::Now().GetMilliSeconds();
    auto&    acSt       = m_acStates[acIdx];

    for (const auto& mpdu : mpdus) {
        if (!mpdu || !mpdu->GetPacket()) continue;

        acSt.bytesSent   += mpdu->GetSize();
        acSt.packetsSent += 1;

        // Per-STA link lookup for delay/jitter attribution
        Mac48Address destMac = mpdu->GetHeader().GetAddr1();
        StaAcKey key = {destMac, acIdx};
        uint8_t txLinkId = m_slowLinkId; // fallback
        {
            auto lastIt2 = m_lastSelectedLink.find(key);
            if (lastIt2 != m_lastSelectedLink.end()) txLinkId = lastIt2->second;
        }
        auto lIt = m_metrics.find(txLinkId);

        uint64_t uid = mpdu->GetPacket()->GetUid();
        auto eqIt    = m_packetEnqueueTimes.find(uid);
        if (eqIt == m_packetEnqueueTimes.end()) continue;

        double delayMs = nowMs - eqIt->second;
        m_packetEnqueueTimes.erase(eqIt);

        // Update per-link delay and jitter (EWMA) — only on the TX link
        if (lIt != m_metrics.end()) {
            auto aIt = lIt->second.find(acIdx);
            if (aIt != lIt->second.end()) {
                auto& ls = aIt->second;
                // Delay EWMA
                ls.avgQueueDelayMs = (ls.avgQueueDelayMs <= 0.0)
                                       ? delayMs
                                       : (0.8 * ls.avgQueueDelayMs + 0.2 * delayMs);
                // Jitter EWMA
                if (ls.lastDelayMs >= 0.0) {
                    double jitter = std::fabs(delayMs - ls.lastDelayMs);
                    ls.avgJitterMs = (ls.avgJitterMs <= 0.0)
                                       ? jitter
                                       : (0.8 * ls.avgJitterMs + 0.2 * jitter);
                }
                ls.lastDelayMs = delayMs;
            }
        }
    }
}

inline void
QosWeightedMloScheduler::NotifyRemove(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus)
{
    EnsureDelegate();
    m_delegate->NotifyRemove(ac, mpdus);
    for (const auto& mpdu : mpdus) {
        if (!mpdu) continue;
        for (auto& [linkId, acMap] : m_metrics)
            acMap[static_cast<uint8_t>(ac)].dropFrames++;
    }
}

// ===========================================================================
// DoDispose
// ===========================================================================
inline void
QosWeightedMloScheduler::DoDispose()
{
    if (m_updateEvent.IsPending()) m_updateEvent.Cancel();
    m_metrics.clear();
    m_delegate = nullptr;
    if (m_decisionCsv.is_open()) m_decisionCsv.close();
    WifiMacQueueScheduler::DoDispose();
}

// ===========================================================================
// Helpers
// ===========================================================================
inline int
QosWeightedMloScheduler::GetFrequencyRank(int frequency)
{
    switch (frequency) {
    case 6: return 2;
    case 5: return 1;
    case 2: return 0;
    default: return 0;
    }
}

inline void
QosWeightedMloScheduler::PrintFinalScores()
{
    std::cout << "\n=== FINAL MLO SCHEDULER SATISFACTION (" << m_nodeContext << ") ===\n";
    for (const auto& [key, sat] : m_currentSatisfaction) {
        const Mac48Address& mac = key.first;
        uint8_t acIdx = key.second;
        auto lastIt = m_lastSelectedLink.find(key);
        uint8_t curLink = (lastIt != m_lastSelectedLink.end()) ? lastIt->second : 255;
        std::cout << "STA " << mac << " AC " << +acIdx
                  << " Link" << +curLink << " CurrentSat=" << sat.index << "\n";
    }
}

// ===========================================================================
// Installation helper
// ===========================================================================
inline Ptr<QosWeightedMloScheduler>
InstallQosWeightedScheduler(Ptr<WifiMac> mac, int freq1, int freq2)
{
    if (!mac) return nullptr;
    auto sched = CreateObject<QosWeightedMloScheduler>();
    sched->ConfigureForPair(freq1, freq2);
    mac->SetMacQueueScheduler(sched);
    return sched;
}

} // namespace ns3

#endif /* MLO_QOS_WEIGHTED_SCHEDULER_H */