inline void
QosWeightedMloScheduler::UpdatePeriodicMetrics()
{
    double now = Simulator::Now().GetSeconds();
    double dt = now - m_lastPeriodicUpdateTime;
    if (dt <= 0.0) dt = m_metricsIntervalSec;
    
    for (auto& [linkId, acMap] : m_metrics) {
        for (auto& [ac, metrics] : acMap) {
            // Recalculate throughput: (bytes * 8) / (dt * 1e6)
            metrics.throughputMbps = (metrics.txBytes * 8.0) / (dt * 1e6);
            metrics.txBytes = 0;
            
            // Recalculate loss rate: drops / (drops + enqueued)
            uint64_t totalPackets = metrics.enqueueCount + metrics.dropFrames;
            if (totalPackets > 0) {
                metrics.lossRate = static_cast<double>(metrics.dropFrames) / totalPackets;
            } else {
                metrics.lossRate = 0.0;
            }
            metrics.enqueueCount = 0;
            metrics.dropFrames = 0;
            
            // Recalculate Delay using Little's Law: Delay = QueueSize / Throughput
            // If throughput is > 0, we can estimate delay based on MAC queue size.
            // If throughput is 0 but queue has packets, delay is growing (we cap at a large value).
            // If queue is empty, delay is 0.
            if (metrics.queueBytes > 0) {
                if (metrics.throughputMbps > 0.001) {
                    // Queue bytes * 8 (bits) / (throughput Mbps * 1e6) = delay in seconds. Multiply by 1000 for ms.
                    double estimatedDelayMs = (metrics.queueBytes * 8.0) / (metrics.throughputMbps * 1000.0);
                    // Smooth with EMA
                    metrics.avgDelayMs = (metrics.avgDelayMs == 0.0) ? estimatedDelayMs : (0.7 * metrics.avgDelayMs + 0.3 * estimatedDelayMs);
                } else {
                    // Link is not transmitting but queue is full -> High delay!
                    metrics.avgDelayMs = 5000.0; // 5 seconds (severely degraded)
                }
            } else {
                // Queue is empty -> No delay
                metrics.avgDelayMs = 0.0;
            }
            
            // For Jitter, we can estimate it as the variation in Delay
            double jitter = std::fabs(metrics.avgDelayMs - (metrics.avgDelayMs > 0 ? metrics.avgDelayMs : 0)); // Actually, compare with previous delay
        }
    }
    
    m_lastPeriodicUpdateTime = now;
    
    // Reschedule next update
    m_updateEvent = Simulator::Schedule(Seconds(m_metricsIntervalSec), 
                                        &QosWeightedMloScheduler::UpdatePeriodicMetrics, this);
}
