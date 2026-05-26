/*
 * Traffic-aware MLO queue scheduler helper.
 *
 * This wrapper keeps the default FCFS queue ordering and only changes the
 * final link selection for each MPDU.
 */
#ifndef MLO_TRAFFIC_AWARE_QUEUE_SCHEDULER_H
#define MLO_TRAFFIC_AWARE_QUEUE_SCHEDULER_H

#include "ns3/fcfs-wifi-queue-scheduler.h"

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>

namespace ns3
{

class TrafficAwareMloQueueScheduler : public WifiMacQueueScheduler
{
  public:
    static TypeId GetTypeId();

    TrafficAwareMloQueueScheduler();
    ~TrafficAwareMloQueueScheduler() override = default;

    void ConfigureForPair(int freq1, int freq2);

    void SetWifiMac(Ptr<WifiMac> mac) override;

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
    Ptr<WifiMpdu> HasToDropBeforeEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu) override;
    void NotifyEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu) override;
    void NotifyDequeue(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus) override;
    void NotifyRemove(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus) override;

  protected:
    void DoDispose() override;

  private:
    static int GetFrequencyRank(int frequency);
    uint8_t SelectPreferredLink(AcIndex ac, const std::list<uint8_t>& eligibleLinks);
    void EnsureDelegate();

    Ptr<WifiMacQueueScheduler> m_delegate;
    uint8_t m_slowLinkId{0};
    uint8_t m_fastLinkId{1};
};

inline TypeId
TrafficAwareMloQueueScheduler::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TrafficAwareMloQueueScheduler")
                            .SetParent<WifiMacQueueScheduler>()
                            .SetGroupName("Wifi")
                            .AddConstructor<TrafficAwareMloQueueScheduler>();
    return tid;
}

inline TrafficAwareMloQueueScheduler::TrafficAwareMloQueueScheduler()
    : m_delegate(CreateObject<FcfsWifiQueueScheduler>())
{
}

inline void
TrafficAwareMloQueueScheduler::ConfigureForPair(int freq1, int freq2)
{
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
}

inline void
TrafficAwareMloQueueScheduler::EnsureDelegate()
{
    if (!m_delegate)
    {
        m_delegate = CreateObject<FcfsWifiQueueScheduler>();
    }
}

inline void
TrafficAwareMloQueueScheduler::SetWifiMac(Ptr<WifiMac> mac)
{
    WifiMacQueueScheduler::SetWifiMac(mac);
    EnsureDelegate();
    m_delegate->SetWifiMac(mac);
}

inline std::optional<WifiContainerQueueId>
TrafficAwareMloQueueScheduler::GetNext(AcIndex ac,
                                       std::optional<uint8_t> linkId,
                                       bool skipBlockedQueues)
{
    EnsureDelegate();
    return m_delegate->GetNext(ac, linkId, skipBlockedQueues);
}

inline std::optional<WifiContainerQueueId>
TrafficAwareMloQueueScheduler::GetNext(AcIndex ac,
                                       std::optional<uint8_t> linkId,
                                       const WifiContainerQueueId& prevQueueId,
                                       bool skipBlockedQueues)
{
    EnsureDelegate();
    return m_delegate->GetNext(ac, linkId, prevQueueId, skipBlockedQueues);
}

inline std::list<uint8_t>
TrafficAwareMloQueueScheduler::GetLinkIds(AcIndex ac,
                                          Ptr<const WifiMpdu> mpdu,
                                          const std::list<WifiQueueBlockedReason>& ignoredReasons)
{
    EnsureDelegate();

    const auto eligibleLinks = m_delegate->GetLinkIds(ac, mpdu, ignoredReasons);
    if (eligibleLinks.empty())
    {
        return {};
    }

    return {SelectPreferredLink(ac, eligibleLinks)};
}

inline void
TrafficAwareMloQueueScheduler::BlockQueues(WifiQueueBlockedReason reason,
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
TrafficAwareMloQueueScheduler::UnblockQueues(WifiQueueBlockedReason reason,
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
TrafficAwareMloQueueScheduler::BlockAllQueues(WifiQueueBlockedReason reason,
                                              const std::set<uint8_t>& linkIds)
{
    EnsureDelegate();
    m_delegate->BlockAllQueues(reason, linkIds);
}

inline void
TrafficAwareMloQueueScheduler::UnblockAllQueues(WifiQueueBlockedReason reason,
                                                const std::set<uint8_t>& linkIds)
{
    EnsureDelegate();
    m_delegate->UnblockAllQueues(reason, linkIds);
}

inline bool
TrafficAwareMloQueueScheduler::GetAllQueuesBlockedOnLink(
    uint8_t linkId,
    WifiQueueBlockedReason reason)
{
    EnsureDelegate();
    return m_delegate->GetAllQueuesBlockedOnLink(linkId, reason);
}

inline std::optional<WifiMacQueueScheduler::Mask>
TrafficAwareMloQueueScheduler::GetQueueLinkMask(AcIndex ac,
                                                const WifiContainerQueueId& queueId,
                                                uint8_t linkId)
{
    EnsureDelegate();
    return m_delegate->GetQueueLinkMask(ac, queueId, linkId);
}

inline Ptr<WifiMpdu>
TrafficAwareMloQueueScheduler::HasToDropBeforeEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu)
{
    EnsureDelegate();
    return m_delegate->HasToDropBeforeEnqueue(ac, mpdu);
}

inline void
TrafficAwareMloQueueScheduler::NotifyEnqueue(AcIndex ac, Ptr<WifiMpdu> mpdu)
{
    EnsureDelegate();
    m_delegate->NotifyEnqueue(ac, mpdu);
}

inline void
TrafficAwareMloQueueScheduler::NotifyDequeue(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus)
{
    EnsureDelegate();
    m_delegate->NotifyDequeue(ac, mpdus);
}

inline void
TrafficAwareMloQueueScheduler::NotifyRemove(AcIndex ac, const std::list<Ptr<WifiMpdu>>& mpdus)
{
    EnsureDelegate();
    m_delegate->NotifyRemove(ac, mpdus);
}

inline void
TrafficAwareMloQueueScheduler::DoDispose()
{
    m_delegate = nullptr;
    WifiMacQueueScheduler::DoDispose();
}

inline int
TrafficAwareMloQueueScheduler::GetFrequencyRank(int frequency)
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

inline uint8_t
TrafficAwareMloQueueScheduler::SelectPreferredLink(AcIndex ac, const std::list<uint8_t>& eligibleLinks)
{
    // Normalize non-QoS ACs to BE to avoid invalid comparisons
    if (ac != AC_BE && ac != AC_BK && ac != AC_VI && ac != AC_VO)
    {
        ac = AC_BE;
    }

    // Static policy: VO/VI prefer fast link (6GHz > 5GHz > 2.4GHz)
    //              BE/BK prefer slow link (2.4GHz < 5GHz < 6GHz)
    const bool preferFastLink = (ac == AC_VO || ac == AC_VI);
    const uint8_t preferredLink = preferFastLink ? m_fastLinkId : m_slowLinkId;
    const uint8_t fallbackLink = preferFastLink ? m_slowLinkId : m_fastLinkId;

    if (std::find(eligibleLinks.begin(), eligibleLinks.end(), preferredLink) != eligibleLinks.end())
    {
        return preferredLink;
    }

    if (std::find(eligibleLinks.begin(), eligibleLinks.end(), fallbackLink) != eligibleLinks.end())
    {
        return fallbackLink;
    }

    return eligibleLinks.front();
}

inline Ptr<TrafficAwareMloQueueScheduler>
InstallTrafficAwareScheduler(Ptr<WifiMac> mac, int freq1, int freq2)
{
    if (!mac)
    {
        return nullptr;
    }

    Ptr<TrafficAwareMloQueueScheduler> scheduler = CreateObject<TrafficAwareMloQueueScheduler>();
    scheduler->ConfigureForPair(freq1, freq2);
    mac->SetMacQueueScheduler(scheduler);
    return scheduler;
}

} // namespace ns3

#endif /* MLO_TRAFFIC_AWARE_QUEUE_SCHEDULER_H */