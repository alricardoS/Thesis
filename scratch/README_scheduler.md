# QosWeightedMloScheduler — Technical Documentation

## Overview

`QosWeightedMloScheduler` is a link-selection scheduler for Multi-Link Operation (MLO) in Wi-Fi 7, implemented as an extension of the ns-3 `WifiMacQueueScheduler`. It operates **at the AP only** (downlink traffic); the STAs continue to use the default ns-3 `FcfsWifiQueueScheduler`.

For every packet (MPDU) the AP needs to transmit, the scheduler decides **which physical link** (e.g. 2.4 GHz, 5 GHz, or 6 GHz) that packet should be routed to, with the goal that each flow meets its own Quality-of-Service (QoS) objectives while respecting the real EDCA (802.11e) priority hierarchy: **VO > VI > BE > BK**.

The scheduler does not decide *when* to transmit (that remains managed by the real ns-3 EDCA/CSMA-CA) — it only decides **which link** each packet is placed on, in the appropriate MAC queue.

### N-link support (dual-band and tri-band) — Phase B

The scheduler is **agnostic to the number of links**. Internally it maintains `m_linksByRank` — the list of link IDs ordered by band quality (**best first**, worst last), built in `ConfigureForLinks(freqs)` from `GetFrequencyRank` (6 GHz > 5 GHz > 2.4 GHz). The aliases `m_fastLinkId` (=`m_linksByRank.front()`, the best) and `m_slowLinkId` (=`m_linksByRank.back()`, the worst) are derived from it. `ConfigureForPair(f1, f2)` is merely a wrapper around `ConfigureForLinks({f1, f2})`, so the two-link behaviour is **identical** to before.

Every mechanism that used to iterate explicitly over "fast/slow" now iterates over `m_linksByRank` (bootstrap, balancer, cascade, per-frequency cold-start), and the downgrade vetoes are now **rank-based** via `LinkRankPos(linkId)` (0 = best). Adding a 3rd link (tri-band) therefore requires no changes to the decision logic.

- **2 links (dual-band)**: run with `--freq1`/`--freq2` (e.g. `5`+`6`). `--freq3=0` (default) keeps two-link mode.
- **3 links (tri-band)**: pass `--freq3` (e.g. `--freq1=2 --freq2=5 --freq3=6`). The `.cc` creates the 3rd PHY/channel and calls the overload `InstallQosWeightedScheduler(mac, {f1,f2,f3})`. In the runner/suite, the `tri` band enables this mode.

> **Per-frequency cold-start** (before any measurements exist): each link's initial capacity is estimated from its band — 2.4 GHz→150, 5 GHz→400, 6 GHz→500 Mbps — instead of the old fast/slow binary.

### Granularity: per (STA, AC), not per AC

The decision key is `StaAcKey = std::pair<Mac48Address, uint8_t>` — the **destination MAC** plus the AC index.

> **Why**: the previous version routed per AC only. That meant two STAs with the same AC (e.g. 2 voice STAs) always received **the same link decision** — it was impossible to distribute them across different links. With a per-(STA, AC) key, each flow is routed independently.

All decision state is keyed this way:

| Member | Type | Role |
|---|---|---|
| `m_lastSelectedLink` | `map<StaAcKey, uint8_t>` | Current link (anchor) of each flow |
| `m_currentSatisfaction` | `map<StaAcKey, QosSatisfaction>` | **Measured** satisfaction on the current link |
| `m_staQos` | `map<StaAcKey, StaQos>` | QoS metrics (throughput, delay, jitter, loss) — by default **estimated by the AP itself** (TC layer); optionally from the sinks (`--decisionMetrics=sink`) |
| `m_hasMeasuredSat` | `set<StaAcKey>` | Flows that have already passed warmup |
| `m_pendingMigration` | `map<StaAcKey, pair<uint8_t,double>>` | Pending migration intent (target link, timestamp) — debounce |

---

## Architecture: six engines plus a stability layer

| # | Engine | Responsibility | Main function/structure |
|---|-------|-------------------|------------------------------|
| 1 | **Goal-Awareness Engine** | Computes how satisfied each (STA, AC) is, from the **real metrics** | `ComputeQosSatisfaction`, `QosSatisfaction`, `StaQos` |
| 2 | **Link Capability Engine** | Estimates each link's real (PHY) capacity and how much of it is available, per AC, respecting priority | `UpdateLinkCapability`, `LinkCapability` |
| 3 | **Traffic Composition Engine** | Measures the real traffic (throughput, airtime, number of flows) each AC already generates on each link | `UpdateTrafficComposition`, `LinkTrafficComposition` |
| 4 | **EDCA Competition Engine** | Models contention pressure between ACs on the same link | `UpdateEdcaCompetition`, `EdcaCompetition` |
| 5 | **Projection Engine** | Combines 2–4 into a "how good would this link be for me" score | `ComputeExpectedQosSatisfaction` |
| 6 | **Migration Decision Engine** | Decides, via a priority cascade, vetoes, and hysteresis, whether to migrate or stay | `DecideLinkMigration`, `MeetsOwnGoals`, `WouldHarmResident` |

On top of these engines sits a **stability layer**, built from real bugs observed in simulation (each mechanism is explained in detail in the [Prevention mechanisms](#prevention-mechanisms) section):

- **Priority bootstrap + warmup** (2 windows)
- **Migration debounce** (1 s dwell)
- **VO/VI anti-downgrade veto** (with a starvation exception)
- **Symmetric BE/BK veto**
- **Exclusion of broadcast/beacon frames** from residency checks

Engines 1–4 are recomputed every `MetricsInterval` (0.5 s). Engines 5–6 run **per packet** (in `GetLinkIds`), but use the values already computed in that window — they do not recompute PHY/traffic for every packet.

---

## Decision flow, step by step

This is the exact order of the code in `DecideLinkMigration`:

1. ns-3 calls `GetLinkIds(ac, mpdu, ...)` when it has an MPDU ready to enqueue.
2. The scheduler obtains the eligible links via the delegated `FcfsWifiQueueScheduler`. If the list comes back empty, it returns empty.
3. It extracts the **destination MAC** (`mpdu->GetHeader().GetAddr1()`); if it is broadcast, it uses the key `ff:ff:ff:ff:ff:ff`. It calls `DecideLinkMigration(ac, dest, eligible)`.
4. **Shortcuts**: if there is only 1 eligible link, it records the anchor and returns it (this is the path beacons take — see [beacon exclusion](#4-beacon-exclusion-isroutablesta)).
5. It determines the **current link** (anchor) and `currentSat` (the **measured** satisfaction, from `m_staQos`).
6. **Priority bootstrap** — if the flow does not yet have 2 measured samples (`m_hasMeasuredSat`), it forces VO/VI → best link; BE/BK → in **tri-band** (≥3 links) the **2nd-best** link (e.g. 5 GHz, leaving 2.4 GHz free), in **dual-band** the worst link. And it **stops here**.
7. **`STAY_SATISFIED`** — if `currentSat >= StayThreshold` and there is an anchor, it stays without evaluating anything else.
8. **Cat1/Cat2/Cat3 cascade** — it evaluates all eligible links and picks the best link of the best category.
9. **VO/VI anti-downgrade veto** — may revert `bestLink` to the current link.
10. **Symmetric BE/BK veto** — likewise, for low-priority ACs.
11. **Hysteresis + debounce** — if `bestLink != currentLink` and the gain exceeds `MigrationThreshold`, the intent must persist for ≥ 1 s (dwell) before executing.
12. It records the anchor (`m_lastSelectedLink`), logs to the CSV, and returns the link.

Every 0.5 s, `UpdatePeriodicMetrics` recomputes engines 1–4 and updates `m_currentSatisfaction`.

---

## Engine 1 — Goal-Awareness Engine

### What it does

For each **(STA, AC)**, it computes a **composite satisfaction index** between 0 and 1, from the **real end-to-end metrics** stored in `m_staQos` (see [Per-STA metrics](#per-sta-metrics--estimated-by-the-ap-itself-no-sta-feedback)):

```cpp
sat.throughput = UtilityFunction(q.tpMbps,   g.targetThroughputMbps, /*lowerIsBetter=*/false)
sat.delay      = UtilityFunction(q.delayMs,  g.maxDelayMs,           true)
sat.jitter     = UtilityFunction(q.jitterMs, g.maxJitterMs,          true)
sat.loss       = UtilityFunction(q.lossRate, g.maxPacketLoss,        true)

sat.index = (throughputWeight·sat.throughput + delayWeight·sat.delay
           + jitterWeight·sat.jitter + lossWeight·sat.loss) / sum_of_weights
```

If there is not yet a valid sample in `m_staQos` (startup), it returns zeros — the bootstrap handles that phase.

> **Important note**: the `linkId` parameter exists in the signature but is **not used** — the real metrics are end-to-end and refer to the link where the flow *currently is*. This is deliberate: for the current link there is no need to project, the real value is known. For the *other* links, Engine 5 is used.

### The sigmoid function (`UtilityFunction`)

```cpp
ratio = value / target
lowerIsBetter (tolerable maximum):  utility = 1 / (1 + e^(5·(ratio − 1)))    → 0.5 at the limit
higherIsBetter (desired target):    utility = 1 / (1 + e^(10·(0.5 − ratio))) → 0.99 when met
```

- **`lowerIsBetter`** (delay, jitter, loss): the target is a *tolerable maximum*, so being at the limit = **0.5** is the correct semantics.
- **`higherIsBetter`** (throughput): the target is *what is needed*. The inflection point is at **50% of the target** → meeting the target ≈ **0.99**, half the target = 0.5. *(Previously the inflection was at the target, which gave only 0.5 when met and required ~2× the target to saturate — see limitation #4.)*
- If `target <= 0`, it returns `0.5`.
- Being asymptotic, it never reaches exactly 0 or 1.

### `AcGoals` — per-AC objectives (constructor; `SetGoals` is **not** called by the script)

| AC | targetThroughputMbps | maxDelayMs | maxJitterMs | maxPacketLoss |
|----|----------------------|------------|-------------|----------------|
| VO | 150.0 | 15.0 | **5.0** | 0.01 (1%) |
| VI | 150.0 | 30.0 | **10.0** | 0.01 (1%) |
| BE | 150.0 | 200.0 | 1.0 | 0.10 (10%) |
| BK | 150.0 | 300.0 | 100.0 | 0.10 (10%) |

> The `maxJitterMs` of VO/VI was raised from 0.1 ms (100 µs, physically unattainable on a shared network) to 5/10 ms — previously it guaranteed the VO never scored well on jitter, costing it ~0.22 of permanent satisfaction.

### `AcWeights` — per-AC weights (set by the `.cc` via `SetWeights`)

| AC | delayWeight | jitterWeight | lossWeight | throughputWeight |
|----|-------------|--------------|------------|-------------------|
| VO | 0.40 | 0.30 | 0.25 | 0.05 |
| VI | 0.25 | 0.15 | 0.20 | 0.40 |
| BE | 0.20 | 0.05 | 0.15 | 0.60 |
| BK | 0.05 | 0.05 | 0.10 | 0.80 |

They need not sum to 1 — the code normalises by the sum. VO prioritises delay+jitter (voice does not tolerate latency, tolerates less throughput). BK almost exclusively wants throughput.

---

## Engine 2 — Link Capability Engine

### `estimatedCapacityMbps` — effective rate at the current load (measured)

```cpp
busyFrac = min(1, m_linkBusyTime[link] / dt)          // real occupied airtime (with overhead)
occupied = Σ throughput of all ACs                    // aggregate goodput
if busyFrac > 0.05 and occupied > 0:
    estimatedCapacityMbps = occupied / busyFrac        // MEASURED
else if PHY data exists:
    estimatedCapacityMbps = dataRate_PHY × (1 − PER)   // fallback
else:
    estimatedCapacityMbps = 400 (fast) or 150 (slow)   // cold-start
```

> **What this value IS (and what it is NOT).** `occupied / busyFrac` measures the **effective rate at the current operating point** — how much goodput the link delivers per unit of airtime, at the load it is seeing *now*. It is **not** the link's saturated maximum.

> **Why measure instead of using the PHY rate.** `dataRate_PHY × (1−PER)` is the **nominal** rate (payload symbols on the air) and **ignores all MAC overhead** (preamble, AIFS, backoff, SIFS, BlockAck). That **overestimated** capacity by ~5× (e.g. 3722 Mbps at 6 GHz). The fix replaced that **gross overestimate** with a **conservative underestimate** — much safer, because the scheduler never comes to believe a link has more headroom than it does.

> **Why it underestimates (and rises with load).** Wi-Fi efficiency depends on **A-MPDU aggregation**, which grows with queue depth: more load → larger A-MPDUs → less overhead per byte → higher effective rate. Measured at 6 GHz:
> | Load on the link | Measured `estimatedCapacity` |
> |---|---|
> | ~300 Mbps (1VO+1VI) | ~655 Mbps |
> | ~450 Mbps (2VO+1VI) | ~982 Mbps |
> | single-link saturation | ~1600 Mbps (real maximum) |
>
> The value **rises monotonically with load** and **never overestimates**. It is the rate relevant to the load the scheduler actually sees — not the theoretical maximum. Consequence (see limitation #2): the "how much would fit here" projection is **pessimistic**.

> **Critical source**: `m_linkBusyTime` (from `FeedLinkTxStart/End`, real occupied PHY time) — the per-AC airtime proxies do not serve here, because they are pure payload time (no overhead) and would give a circular calculation back to the PHY rate.

> **⚠️ Per-AC A-MPDU aggregation (`.cc` config).** All four ACs now use `MaxAmpduSize = 65535` — including **BK** (`bkMaxAmpduBytes = 65535`; previously `0`/disabled). With aggregation disabled, BK sent frame by frame and was limited to **~50 Mbps even when alone on a link** (each frame pays the full MAC overhead: preamble, AIFS, backoff, SIFS, BlockAck), saturating the TC queue → multi-second delay and high loss. With A-MPDU enabled, BK keeps up with the other ACs up to the offered rate. Tunable via CLI: `--bkMaxAmpdu=<bytes>` (`0` disables it again). This aggregation ceiling is a **MAC** characteristic, independent of the scheduler's link decision.

`ConfigureForPair` orders the two links by `GetFrequencyRank` (6 GHz > 5 GHz > 2.4 GHz) and sets `m_fastLinkId` / `m_slowLinkId`.

### `availableCapacityPerAcMbps` — priority-aware

Each AC discounts **only** the consumption of ACs of equal or higher priority:

```
availableCapacityPerAcMbps[VO] = estimatedCapacity − consumption(VO)
availableCapacityPerAcMbps[VI] = estimatedCapacity − consumption(VO) − consumption(VI)
availableCapacityPerAcMbps[BE] = estimatedCapacity − consumption(VO) − consumption(VI) − consumption(BE)
availableCapacityPerAcMbps[BK] = ... − consumption(BK)
```

This models real 802.11: VO, with shorter AIFS/CW, always wins the medium first — it does not "lose" capacity because BE transmits a lot. BE suffers everything above it.

### `freeAirtime` and `capabilityScore`

```
freeAirtime     = 1 − (link occupied time / window duration)
capabilityScore = min(1, availableCapacityMbps / estimatedCapacityMbps)
```

---

## Engine 3 — Traffic Composition Engine

Per 0.5 s window, it measures each AC's real traffic on each link:

- `voThroughputMbps`, `viThroughputMbps`, `beThroughputMbps`, `bkThroughputMbps`
- `voAirtimeFrac`, etc. — **real utilisation** of the link per AC, in [0,1] (load proxy for Engine 4)
- `voFlows`, etc. — number of distinct active flows (STA+TID)

> **`*AirtimeFrac` = real utilisation, not a mixture.** The per-AC payload airtime (from `FeedPacketTransmitted`) captures the *ratio* between ACs well, but it is pure payload time. It is scaled to the real PHY `busyFrac` (`m_linkBusyTime/dt`) → each fraction lands in [0,1] and together they sum to the link's utilisation. *(Previously they were normalised by the used airtime, which made the fractions always sum to 1.0: an AC on an idle link "weighed" the same as on a saturated one, and `pressure` could not tell the two apart — see limitation #1.)*

It feeds Engine 2 (absolute consumption) and Engine 4 (normalised load).

---

## Engine 4 — EDCA Competition Engine

### `m_edcaWeights` — contention-pressure matrix

```cpp
// rows = candidate AC; columns = other AC present on the link
// order: BK, BE, VI, VO
{0.0,  0.5,  7.0,  9.0}   // candidate BK  — feels VO (9.0) and VI (7.0) STRONGLY
{0.5,  1.0,  6.0,  8.0}   // candidate BE  — feels VO (8.0) and VI (6.0) STRONGLY
{0.25, 0.5,  1.0,  2.0}   // candidate VI  — feels VO (2.0) moderately
{0.0,  0.05, 0.25, 1.0}   // candidate VO  — barely feels anything below it
```

`m_edcaWeights[X][Y]` = **"how much AC X feels the presence of AC Y"**. It is not symmetric, because real EDCA contention is not.

### Computation chain

```
pressure[cand]   = Σ (m_edcaWeights[cand][other] × NormalisedLoad(other, link))
opportunity      = freeAirtime / (1 + pressure)        (clamped to [0,1])
effectiveAvailableCapacityMbps[ac] = availableCapacityPerAcMbps[ac] × opportunity
```

`effectiveAvailableCapacityMbps` is the final estimate of "how many Mbps I could get here right now".

---

## Engine 5 — Projection Engine (`ComputeExpectedQosSatisfaction`)

Used to evaluate links (including those where the flow is **not**).

```
coRes         = number of OTHER STAs of the same AC already on this link
effCapShared  = effectiveAvailableCapacityMbps / (coRes + 1)      ← co-occupancy penalty

expTp     = UtilityFunction(effCapShared, targetThroughputMbps, false)
expDelay  = UtilityFunction(delay_proxy × (1 + pressure×0.3), maxDelayMs, true)
expJitter = UtilityFunction(jitter_proxy × (1 + pressure×0.2), maxJitterMs, true)
expLoss   = UtilityFunction(PER_proxy, maxPacketLoss, true)

baseSat = (throughputWeight·expTp + delayWeight·expDelay
         + jitterWeight·expJitter + lossWeight·expLoss) / sum_of_weights

expectedScore = baseSat × 0.65 + capabilityScore × 0.25 + (1 − PER_proxy) × 0.10
```

> **Caution**: `delay_proxy` / `jitter_proxy` / `PER_proxy` come from `m_metrics[link][ac]` (per-link, **not** the real per-STA metrics). This asymmetry is a known limitation — see [Known limitations](#known-limitations).

### Co-occupancy penalty

It divides the projected capacity by the number of same-AC STAs that would share the link (`coRes + 1`). If the flow were the only one of its AC there, the denominator is 1 and nothing changes.

> **Why**: without this, in a 2VO+2VI scenario all flows would see the fast link as equally good and pile up there. The penalty makes the 2nd VI overflow when the link cannot hold both.

### Altruistic penalty

Applied after `expectedScore`, to protect already-resident ACs:

- **(A) Reactive** — if a resident AC is already starved (`currentSat < 0.45`), it adds `(0.6 − currentSat)`.
- **(B) Preventive** — even without being starved, if the resident's `headroomRatio` (`effectiveAvailableCapacity / targetThroughput`) is < 1.0 **and** `currentSat < 0.75`, it applies a penalty proportional to the load the candidate would bring (`min(0.5, tightness × candidateLoad × 2)`).

The final penalty is the `max` of (A) and (B) per resident (not the sum), subtracted from the score, never below 0.

---

## Engine 6 — Migration Decision Engine (`DecideLinkMigration`)

### Step 1 — Priority bootstrap (while there are no measurements)

```cpp
if (m_hasMeasuredSat.count(staAcKey) == 0) {
    bootLink = (acIdx >= AC_VI) ? m_fastLinkId : m_slowLinkId;   // VO/VI → fast; BE/BK → slow
    decision = "BOOTSTRAP_PRIORITY";
    // stops here — the cascade does not run
}
```

### Step 2 — `STAY_SATISFIED` and considerate eviction

```
if currentSat >= StayThreshold AND there is an anchor:
    if (VO/VI) AND there is a BE/BK on the current link AND a CLEAN link with capacity exists:
        MIGRATE_CONSIDERATE_EVICT  → migrate to that link (leaving the current one to the BE/BK)
    else:
        STAY_SATISFIED             → stay, without evaluating anything else.
```

**Considerate eviction.** A **satisfied** VO/VI that shares its current link with **BE/BK** is starving them by EDCA (rule: BE/BK never coexist well with VO/VI). If a **clean** link exists (no resident BE/BK) with **capacity** for all the high-priority demand that would end up there, the VO/VI **migrates to it** — even without improving its own satisfaction — freeing the current link for the BE/BK. Implemented by `FindConsiderateEvictTarget`, which uses `estimatedCapacity ≥ Σ(VO/VI demand)` (**measured capacity**, *not* the `MeetsOwnGoals` projection, which is pessimistic due to the co-occupancy penalty and would misclassify the target link). The migration is **immediate** (no dwell): the condition is stable, and `EnforceRouting` (binding) makes the decision **effective** in the physical distribution — this is the only reason this mechanism works now (see limitation #14).

- **No ping-pong / safe**: it only fires for VO/VI with a BE/BK on the current link AND a clean target with room. After moving, the new link has no BE/BK → it does not fire again. If no clean target fits, it stays in `STAY_SATISFIED` (preserving the legitimate case where the VO/VI **must** share with the BE — `CAT2_PRIORITY_OVERRIDE`).

### Step 3 — Explicit three-category cascade

| Category | Condition | Meaning |
|-----------|----------|-------------|
| **1 — CAT1_CLEAN** | `MeetsOwnGoals` **and** `!WouldHarmResident` | I meet my objectives without harming anyone |
| **2 — CAT2_PRIORITY_OVERRIDE** | `MeetsOwnGoals` **but** `WouldHarmResident` | I meet my objectives at another AC's expense — acceptable because EDCA priority is legitimate |
| **3 — CAT3_BEST_EFFORT** | I meet them nowhere | Last resort: best achievable score |

It always picks the best category (1 > 2 > 3) and, within it, the link with the best `ComputeExpectedQosSatisfaction`.

- **`MeetsOwnGoals(ac, sta, link)`** → `ComputeExpectedQosSatisfaction(...) >= OwnGoalsThreshold` (0.90).
- **`WouldHarmResident(ac, sta, link)`** → searches, among residents of **other ACs** (same-AC residents are skipped — co-occupancy handles them), for the one with the worst `currentSat`; if it is `<= HarmThreshold` (0.70), it deems that harm would occur.

### Step 4 — Priority vetoes

See [Prevention mechanisms](#2-priority-vetoes-bebk-never-share-a-link-with-vovi).

### Step 5 — Hysteresis + debounce

```
if bestLink != currentLink:
    improvement = bestScore − currentSat
    if improvement > MigrationThreshold:
        → debounce (dwell): the intent must persist for ≥ 1 s
    else:
        STAY_HYSTERESIS  (clears the pending intent)
else:
    STAY_HYSTERESIS / STAY_BEST  (clears the pending intent)
```

---

## Per-STA metrics — estimated by the AP itself (no STA feedback)

The **measured** satisfaction (`currentSat`) is estimated **using AP information only**, at the AP's own
**traffic-control layer** (the AP's queue-disc), and fed to the scheduler via `FeedStaQos` every
1 s (in `CalculateStats`, in the `.cc`). **It needs no STA feedback.** This removes the old
dependency on the sinks (see item #5 in [Known limitations](#known-limitations)).

Each packet is tracked from the **top of the AP's stack** (traffic-control enqueue) until it is **acknowledged
(ACK)** at the MAC — capturing all the waiting (TC queue **+** MAC queue + channel access) and the drops.

| Dimension | Source (AP-only, downlink) | How |
|---|---|---|
| **Throughput** | MAC `Dequeue` (delivered) × `payloadSize` | exact goodput = `nDelivered × payload × 8 / dt` (the AP knows its application's payload → no header overhead) |
| **Delay / Jitter** | TC `Enqueue` → MAC `Dequeue` (ACK) | per packet, `t_ACK − t_TCenqueue` (the packet `uid` matches across the two layers → 100% match) |
| **Loss** | TC `Enqueue`/`DropBeforeEnqueue` + MAC `Dequeue` | `1 − delivered / offered`, `offered = TC-enqueued + pre-queue drops` (TC overlimit + MAC pre-enqueue) |

> **Validation vs the sinks (all-BE, N=192).** Compared with the sinks' end-to-end measurements (ground truth):
> **loss** mean error 0.0001; **delay** error 0.19 ms (even with the ~200 ms of startup-saturation waiting);
> **throughput** exact after the goodput fix; **jitter** same order of magnitude. The flag
> `--decisionMetrics=ap|sink` switches the decision source (`ap` by default); the
> `metric_comparison` CSV always records both (sink vs AP) for auditing.

> ### Historical note: why measure at the TC layer (and not the MAC)?
>
> Measuring at the **MAC queue** only underestimated delay and loss **under congestion**: in the all-BE case, 100% of the drops and
> ~90% of the waiting occur **above** the MAC, in the traffic-control queue (overlimit). Measuring at the **top**
> (TC enqueue) through to the ACK captures everything → it matches the sink. The sink metrics were the **reference**
> that validated this AP-side estimate.

> ### Why real metrics? (bug found)
>
> Originally the scheduler used metrics from its own MAC:
> - The **PER** came from `1 − txSuccess/txAttempts`. `txAttempts` counted **all** the MPDUs aggregated at the start of the TX, but `txSuccess` was fed by the `AckedMpdu` trace, which captured only **~5%** of the delivered MPDUs → a **false PER of ~95%** and throughput measured at ~5% of the real value.
> - With the loss+tp weights, this **capped VO satisfaction at ~0.70** (the ceiling given by delay+jitter). Since `StayThreshold` is 0.75, **nothing was pinned by `STAY_SATISFIED`** — every AC was permanently at the mercy of the noisy cascade. This was the root of nearly all the initial instabilities.
>
> ### Why the FlowMonitor cumulative loss? (second bug)
>
> The first attempt to fix loss used `1 − throughput/offered`. That **is not loss** — it is the *throughput deficit*. A normal 1-window fluctuation (the VI receiving 144 instead of 150 Mbps, from queueing variance) appeared as "3.7% loss"; with `maxPacketLoss = 0.01` for the VI, the sigmoid dragged `curSat` from 0.665 → 0.465 and triggered a spurious migration.
>
> The FlowMonitor **cumulative** loss has no such noise: packets in flight at the window boundary are insignificant against the whole history, and the value converges to the real loss (~0 on a healthy link, high on a congested one).

---

## Prevention mechanisms

Each of these exists because of a bug **observed in simulation**. They are documented as *symptom → cause → mechanism*.

### 1. Priority bootstrap + warmup (`kWarmupSamples = 2`)

- **Symptom**: at cold-start (t≈2.5 s) BE and VO **swapped links at the same time** — BE fled to the fast link, VO to the slow one — and got stuck in the wrong configuration.
- **Cause**: the **1st measurement window is contaminated by the ramp-up**. The VO measured `curSat = 0.469` instead of its real value (~0.87). With everyone looking bad on the current link and the other (empty) link projecting well, they all migrated at once.
- **Mechanism**: until a flow has **2 real samples** (`StaQos::samples >= 2`), the allocation is forced by EDCA priority (VO/VI → best link; BE/BK → 2nd-best link in tri-band, worst link in dual-band) and the cascade **does not even run**. The contaminated window is skipped; when the cascade starts, `curSat` already reflects reality and `STAY_SATISFIED` pins everyone in the right place.
  - **Tri-band — why BE/BK on the 2nd-best link (5 GHz), not the worst (2.4 GHz)**: the 2.4 GHz link is the lowest-capacity one; starting the low-priority ACs there immediately under-utilised 5 GHz and overloaded 2.4 GHz. With the bootstrap on `m_linksByRank[1]` (=5 GHz), BE/BK start from a much higher-capacity link and 2.4 GHz is left available for the balancer/cascade to fill according to load.

### 2. Priority vetoes (BE/BK never share a link with VO/VI)

> **These vetoes are not a temporary workaround — they are the deliberate modelling of a physical effect that the analytic model cannot express.** EDCA starvation is a **channel-access** phenomenon, not a bandwidth one: a BE competing with a VO on the same link **gets no transmission opportunities** (the VO, with AIFSN=2/CWmin=3, reacquires the channel almost immediately; the BE's AIFS+backoff almost never expires), even if there is free airtime in aggregate. A BE collapsing to 4.95 Mbps on a link with ~57% "free" airtime proves it. No model based on **capacity or airtime** (which is what engines 2–5 measure) would capture this — only a Bianchi-style EDCA saturation model, which is out of scope. The vetoes encode that physics directly and are **permanent**.

The physical rule: **BE/BK on a link with VO/VI never get a transmission opportunity** (EDCA). Both sides are vetoed, but with different strictness.

**(a) VO/VI anti-downgrade veto** — a VO/VI on the fast link does not go down to the slow one if:
- it is the **only one of its AC** on the fast link (`CountCoResidents == 0`), **or**
- the destination link has a **resident BE/BK** (`acIdx < AC_VI`),

**except** if it is genuinely starved (`currentSat < kStarvationFloor = 0.60`) — then it yields and may go down, because VO has priority over BE.

- **Symptom**: one of the 2 VOs drifted to the slow link and stayed there, starving the BE that was there.
- **Cause**: both links projected almost **equal** for the VO (`0.963223` vs `0.963183` — a difference of 0.00004!). The VO is dominated by delay (tp weight = 0.05), so capacity is almost irrelevant and the decision became a coin flip.
- **Note**: the starvation signal uses the **measured** `currentSat`, not the projection. The projection was tried and failed — it collapsed at 6 GHz and opened the exception through noise.

**(b) Symmetric BE/BK veto** — a BE/BK **never** migrates to a link where VO/VI reside. **No exception.**

- **Symptom**: on the 2.4+6 GHz pair, the BE was perfectly fine on 2.4 GHz (`tp = 150`, `delay = 0.66 ms`) and yet migrated to 6 GHz — and collapsed: **`tp` 150 → 4.95 Mbps, `delay` 647 ms, `loss` 22%**.
- **Cause**: the projection gave **0.9696** to the BE on 6 GHz. The model sees the free raw capacity ("~700 Mbps available") and **ignores that the BE will be starved by EDCA** by the resident VO/VI. At 5 GHz the raw capacity is lower, the projection gave 0.651 and the BE did not move — **that was luck, not logic**.
- **Why no exception**: for a BE/BK, going to the VO/VI link **never helps** — it would be more starved, not less.

### 3. Migration debounce (`kMigrationDwellSec = 1.0`)

- **Symptom**: midway through a stable simulation (t≈11 s), the VI jumped to the slow link and disturbed the BE, recovering 1–2 s later.
- **Cause**: **normal EDCA airtime-sharing variance**. In a 1 s window the VOs grabbed slightly more airtime and the VI measured 144 instead of 150 Mbps (the VOs, on the same link, did not drop — proof that it was not a physical link event). The resulting migration **self-disturbed** and sustained the bad state in the following window.
- **Mechanism**: when the cascade wants to migrate, the intent is **recorded** (`MIGRATE_PENDING`) but not executed. It only migrates if the same intent (same target link) persists for ≥ 1 s — that is, if it survives at least one **new measurement window**. If the intent disappears, it is cleared. This breaks the loop: by not migrating in the noisy window, the next one is clean and the intent evaporates.

### 4. Beacon exclusion (`IsRoutableSta`)

This is the subtlest mechanism, and worth understanding in detail.

- **Symptom**: in the all-BE scenario, the 4 STAs got **stuck on the slow link** (`delay = 217 ms`, `loss = 58%`, `tp = 62 Mbps`) with the fast link **empty**. The metrics were correct (`curSat = 0.157`) and `improvement` was 0.45 (well above the threshold) — the BEs **wanted** to migrate, but alternated endlessly between `MIGRATE_PENDING` and `STAY_HYSTERESIS`, with **zero migrations**.

- **The clue**: the `MIGRATE_PENDING` timestamps were spaced **~102.4 ms** apart — exactly the **ns-3 beacon interval**.

- **Cause**:
  1. Beacons are **broadcast** frames. In `GetLinkIds`, any broadcast is keyed as `(ff:ff:ff:ff:ff:ff, AC)` — with a high-priority AC.
  2. Since each beacon has only **1 eligible link**, it takes the *early return* of `DecideLinkMigration`, which **writes to `m_lastSelectedLink` but does not write to the CSV** — hence only AC=0 is seen in the file, despite a high-priority phantom entry existing.
  3. That entry jumped from link to link with every beacon. When it pointed at the target link, the **symmetric BE/BK veto** saw "there is a VO there" → `bestLink = currentLink` → `STAY_HYSTERESIS`.
  4. The `STAY_HYSTERESIS` branch **erases `m_pendingMigration`** → the dwell restarted. With the beacon repeating **10×/second** and the dwell needing 1 s, it **never completed**.

- **Mechanism**:
  ```cpp
  static bool IsRoutableSta(const Mac48Address& a) { return !a.IsGroup(); }
  ```
  `IsGroup()` covers broadcast and multicast. The filter `if (!IsRoutableSta(key.first)) continue;` is applied in the **5 residency scans**:
  1. altruistic penalty (`ComputeExpectedQosSatisfaction`)
  2. `CountCoResidents` (co-occupancy)
  3. `WouldHarmResident`
  4. VO/VI anti-downgrade veto (`wouldStarveLowPrio`)
  5. symmetric BE/BK veto (`highPrioOnTarget`) ← the one that fixed the bug

  The **routing** of the beacons does not change — they simply stop counting as "residents" of a link.

### 5. Idle-link balancing (`RebalanceIdleLinks`)

> **Philosophy note**: this mechanism adds a **second objective** — until now the scheduler only *satisfied QoS*; now it also *makes use of idle links*. It is a deliberate choice.

- **Symptom**: in homogeneous scenarios (all-BE), or where several flows fit on the best link (2VO+2VI), **all STAs end up on the same link and the other one sits idle**. Spreading the load improves the metrics (less contention, less delay).
- **Cause**: after `UtilityFunction` gives ~0.99 to a well-served flow, `STAY_SATISFIED` pins it and it stops exploring the empty link. Balancing has to be **explicit**.
- **Mechanism** (in `UpdatePeriodicMetrics`, one move per window):
  1. **Gate**: **all** routable flows satisfied (`curSat ≥ StayThreshold`). If any is not, the cascade is rescuing it → do not balance.
  2. **Homogeneous target**: a flow is only moved to a link that is **empty** or that **contains only flows of the same AC** as the candidate.
  3. It moves the **lowest EDCA-priority** flow (non-VO — voice is pinned) from a link with ≥2 flows, **and only if it reduces the imbalance** (`flows(source) > flows(target)+1`).
  4. **Target order: best → worst.** The **highest-quality link is preferred first** (e.g. 5 GHz before 2.4 GHz); only when it fills up is the next one considered. (It used to be worst → best.)
  5. **Headroom test (fill up to real capacity):** `headroom ≥ targetThroughput` is required for **any** candidate, using a **reliable capacity** per link — the **measured** one when the target already carries traffic, and the **per-band nominal** (`NominalCapByFreq`: 2.4→150, 5→400, 6→500 Mbps) when it is **idle** (an empty link's capacity is not measurable). Since the nominal ≥ demand in every band, the **1st flow always enters**; only the **excess** is limited — 2.4 GHz holds 1 VI, 5 GHz holds 2, etc.

  **The homogeneous-target rule is the safety core** — it replaces a "bidirectional veto" with something stronger: **the balancer never mixes ACs**. Consequences:
  - It never creates BE/BK together with VO/VI (in either direction). *(This was a bug in an earlier version: a VI was moved on top of a BE. It cannot happen here — the BE's link is neither empty nor "VI-only".)*
  - A new group **only starts on an empty link**; it then fills up with the same AC while there is headroom → **iterative**, the "how many migrate" emerges from capacity (e.g. tri-band 2VO+2VI → both VIs first go to 5 GHz; with 2.4 GHz still idle, one of them then migrates there — 5 GHz holds 2, 2.4 GHz holds 1).
  - It prevents **reverse consolidation** (moving a VI back to the VOs' link) → no ping-pong.

  **Anti-oscillation**: global cooldown (`kRebalanceCooldownSec = 2 s`) + per-flow (`kFlowBalanceCooldownSec = 10 s`). If a move degrades a flow, the cascade pulls it back and the cooldown prevents the re-push.

  > **Important — the balancer's decision is *advisory*, not *binding*.** Since `RebalanceIdleLinks` only alters `m_lastSelectedLink` (which biases the channel-access request at enqueue), the observed physical traffic may differ from the decided assignment. See [Advice vs. execution](#advice-enqueue-vs-execution-dequeue-why-all-be-uses-both-links).


### 6. Pruning of inactive flows (apps that stop, e.g. before an AC *switch*)

- **Symptom**: in a *switch* scenario (e.g. `be>vo`), the BE apps stop midway. Their entries lingered forever in `m_lastSelectedLink` with **frozen phase-1 satisfaction** (`ComputeQosSatisfaction` uses `m_staQos`, which stops being fed). Since that value is usually `< StayThreshold`, it poisoned the **balancer's gate** (it never spread again) and the stale flows "resided" as **phantoms** in the cascade (`WouldHarmResident`/co-occupancy), preventing the active flows from migrating to idle links.
- **Mechanism** (start of `UpdatePeriodicMetrics`): each `(STA,AC)` records the timestamp of its last enqueue (`m_lastActivitySec`, updated in `DecideLinkMigration`). A flow with no enqueues for `> kInactivityTimeoutSec` (1 s ≈ 2 windows) is **pruned**: its binding masks are cleared (`UnblockQueues` on all links) and it is erased from `m_lastSelectedLink`, `m_currentSatisfaction`, `m_boundLink`, `m_hasMeasuredSat`, `m_pendingMigration`, `m_lastFlowBalanceSec`, `m_staQos`.
- **Reliable**: a *starved-but-active* app keeps enqueuing → it is not pruned; only genuinely stopped apps are.

---

## Advice (enqueue) vs. execution (dequeue): why all-BE uses both links

This section explains a behaviour that **surprises**: although the scheduler decides **one link per (STA, AC)**, in the **all-BE** scenario each STA ends up transmitting on **both** links simultaneously (~28% on 2.4 GHz / ~72% on 5 GHz in 2+5; ~50/50 in 5+6). **This is not a scheduler decision** — it is a consequence of the ns-3 architecture underneath it.

### The scheduler's decision only acts at *enqueue*

`GetLinkIds(ac, mpdu)` is called when the MAC has an MPDU ready to **enqueue** (`Txop::Queue`). It returns **1 link** (via `DecideLinkMigration`), but that value only decides **which link the channel-access request is made on**. **It does not pin the frame to that link.**

### The *dequeue* is done by the Fcfs delegate, which ignores the decision

The critical part is who serves the queue at transmission time. When `m_delegate->SetWifiMac(mac)` is called, it is the **`FcfsWifiQueueScheduler` delegate** that registers as the scheduler of each `WifiMacQueue` (`WifiMacQueueSchedulerImpl::SetWifiMac` → `queue->SetScheduler(this)`). `QosWeightedMloScheduler` extends `WifiMacQueueScheduler` **directly** and does not do that wiring. Therefore:

- **`WifiMacQueue::m_scheduler == delegate` (Fcfs)**, not our scheduler.
- On the transmission path, `Peek`/`PeekFirstAvailable(linkId)` call the **delegate's** `GetNext` — **pure Fcfs** over the AC's shared queue, which serves **any** STA on **any** link with access. Our `GetNext` override is **never called by the queue**, and `m_lastSelectedLink` is **never** consulted at dequeue.

### Why this spreads the traffic

Channel access is **per-AC** (there is one `Txop` per AC), not per-STA. In all-BE, the balancer advises 1 BE to 2.4 GHz and 3 to 5 GHz → **both** links start requesting access for AC BE → **both** BE `Txop`s win access → each, when transmitting, pulls from the shared BE queue **the next frame of any STA** (Fcfs). Result: each STA's frames go out on both links.

### Measured evidence (*bound-vs-real* proof)

We temporarily instrumented each **acknowledged (ACKed)** frame to compare the **decided** link (`m_lastSelectedLink`) with the **real** link (read from the AP's address in `Addr2`). In **all-BE 2+5**, the scheduler decided **exactly the intended 1-3 split — STA0→2.4 GHz, STA1/2/3→5 GHz** — but physically:

| STA | decided | 2.4 GHz (real) | 5 GHz (real) |
|-----|----------|---------------|-------------|
| 0 | 2.4 GHz | 829 | **732** (≠ decided) |
| 1 | 5 GHz | **807** (≠ decided) | 752 |
| 2 | 5 GHz | **836** (≠ decided) | 807 |
| 3 | 5 GHz | **804** (≠ decided) | 791 |

That is: **intent = clean 1-3; execution = ~50/50**. STA0, decided for 2.4 GHz, sent almost half its frames on 5 GHz; STA1/2/3, decided for 5 GHz, sent almost half on 2.4 GHz.

### Why the priority scenarios (2VO+2VI, …) *look* clean

Spreading only happens when flows of the **same AC** are separated across different links. In the priority scenarios, each AC stays **together** on one link (the 2 VOs together on the fast link, the 2 VIs together on 5 GHz) → the other link **never** contends for that AC → practically **no spreading**. The residual 73/27 of the VIs is just the **bootstrap transient** (the VI starts on the fast link and settles on 5 GHz), not steady-state sharing.

Likewise, the **bootstrap looks "binding"** (fast link at 0% for the BEs) only because **all** flows advise the **same** link — the other one never requests access for that AC. The "rule" comes from the absence of contention, not from binding being enforced.

### Spreading is a natural interaction of MLO in STR mode

Before viewing this as a "defect", it is worth recognising that this spreading is a **natural and important behaviour of MLO in STR mode** (Simultaneous Transmit and Receive). With **per-AC shared queues** and **independent per-link CSMA**, any free link can serve that AC's *head-of-line* frame — naturally distributing a single flow's traffic across the available links. It is, in practice, **legitimate link aggregation**: it satisfies all STAs and exploits the aggregate capacity. The contrast **VO (100% on one link) vs VI (50/50)** is explained solely by this: the VO always advises the same link → the other never activates the VO AC's access loop; the VIs ended up separated across links → **both** VI `Txop`s active → Fcfs feeds both.

### How it became binding (implemented — queue-blocking binding)

For scenarios where we **want** the decision to be an **order** (no spreading), the scheduler physically pins each flow to the decided link via **block masks on the delegate** — the only mechanism the delegate's `GetNext` respects:

- **`EnforceRouting(ac, dest, chosenLink)`** (`mlo-qos-weighted-scheduler.h`), called at the end of `GetLinkIds`: for the (STA, AC), it calls `m_delegate->BlockQueues(WIFI_SCHEDULER_ROUTING, …)` on **all links except** the chosen one and `UnblockQueues` on the chosen one. That (STA, AC)'s queue gets a mask bit set on the wrong links → `GetNext(ac, linkId)` **skips it** there → the frame is only served on the decided link. No spreading.
- It uses a **new reason, `WIFI_SCHEDULER_ROUTING`**, added to the ns-3 **core** `WifiQueueBlockedReason` enum (all existing reasons are managed by the MAC and would be cleared by it). See the [Changes to the ns-3 core](#changes-to-the-ns-3-core) section.
- Details that make this work: the unicast QoS-data container is keyed by `(WIFI_QOSDATA_QUEUE, UNICAST, RA=STA-MLD-address, tid)` — matching the `dest` the scheduler already uses; `BlockQueues` is called with `txAddress = GetMac()->GetAddress()` (the AP's MLD address), **the condition for `InitQueueInfo` to populate the per-link mask**; and both TIDs of the AC are blocked.
- To avoid freezing migration, `GetLinkIds` determines the eligible links **while ignoring `WIFI_SCHEDULER_ROUTING` itself** — the decision still "sees" all links; the binding only restricts the **physical dequeue**, not the decision.
- `m_boundLink[(STA,AC)]` stores the pinned link; it is only re-blocked when the decision **changes** (avoiding per-frame work). On a migration, it re-pins in the same step (blocking the old link, unblocking the new one).

> **Future work — revisiting spreading.** Binding removes spreading by default, but STR spreading is a **desirable** mechanism to explore (exploiting per-flow aggregate capacity). It remains future work to be able to **re-enable it selectively** (e.g. per AC, or when the target link is saturated) — there is a dedicated plan for that interaction.

---

## Table of all configurable variables

### ns-3 attributes (via `SetAttribute`)

| Attribute | C++ variable | Effective value | What it controls |
|----------|--------------|-------------|------------------|
| `StayThreshold` | `m_stayThreshold` | **0.75** | Minimum satisfaction to not even evaluate alternatives. Higher → re-evaluates more; lower → "lazier". |
| `MigrationThreshold` | `m_migrationThreshold` | **0.25** | Minimum gain to justify a migration. Higher → more stable; lower → more reactive, oscillation risk. |
| `MetricsInterval` | `m_metricsIntervalSec` | **0.5 s** | Recomputation frequency of Engines 1–4. |
| `OwnGoalsThreshold` | `m_ownGoalsThreshold` | **0.90** | Minimum score for a link to count as "I meet my objectives" (Cat1/Cat2). |
| `HarmThreshold` | `m_harmThreshold` | **0.70** | Satisfaction below which a resident is considered "harmed". |

> The `StayThreshold` / `MigrationThreshold` values are **imposed by the script** (`g_stayThreshold` / `g_migrationThreshold` in the `.cc`, also exposed on the CLI via `--stayThreshold` / `--migrationThreshold`). The header defaults are the same (0.75 / 0.25).

### Hardcoded constants (not ns-3 attributes)

| Constant | Value | Location | Meaning | Rationale |
|-----------|-------|-------|-------------|--------------|
| `kWarmupSamples` | 2 | `UpdatePeriodicMetrics` | Measurement windows before releasing the bootstrap | **(P)** The 1st window is contaminated by the ramp-up; it must be skipped and the 2nd (clean) one used. 2 is the **minimum** to have one clean window before releasing — 1 would skip nothing, higher would delay convergence for no gain. |
| `kMigrationDwellSec` | 1.0 s | `DecideLinkMigration` | Persistence required of a migration intent | **(P)** Equal to the `FeedStaQos` cadence (1 s). Guarantees the intent survives **≥1 new measurement window** before executing → filters 1-window blips. Smaller would not see fresh measurement; larger would delay genuine migrations. |
| `kStarvationFloor` | 0.60 | `DecideLinkMigration` | `currentSat` below which a VO/VI is "starved" (opens the veto exception) | **(P/E)** Logical bounds: **< `StayThreshold` (0.75)** (otherwise a satisfied flow would open the veto) and **> transient degradation**. 0.60 sits in the "clearly bad, but not catastrophic" band; the exact digit is not critical within it. |
| `kRebalanceCooldownSec` | 2.0 s | `RebalanceIdleLinks` | Minimum interval between balancing moves | **(E)** Lets the metrics settle between moves. Conservative; any few-seconds value works. |
| `kFlowBalanceCooldownSec` | 10.0 s | `RebalanceIdleLinks` | Minimum time before touching the same flow again | **(E)** Anti-ping-pong: if the cascade pulls a flow back, we do not immediately re-push it. Generous value, not critical. |
| `perAlpha` | 0.3 | `UpdatePeriodicMetrics` | EWMA smoothing of the PER proxy | **(E)** Conventional EWMA factor: 30% new sample / 70% history. Balances reactivity and stability. Higher = noisier; lower = slower to react. Not critical. |
| Final blend | 0.65 / 0.25 / 0.10 | `ComputeExpectedQosSatisfaction` | `baseSat` / `capabilityScore` / `perPenalty` | **(E)** Tuned (was 0.8/0.1/0.1). QoS satisfaction **dominates** (0.65, primary signal); link capability is a **strong secondary** (0.25, raised to give more weight to capable/spare links); the PER is a minor nudge (0.10). **Sums to 1.** The *proportion* matters, not the digits. |
| "Starved" threshold (altruistic A) | 0.45 | idem | Resident treated as starved | **(P/E)** Below the 0.5 midpoint → the resident is clearly in the **unsatisfied half**. Marks "this AC already suffers" to trigger the reactive penalty. |
| Base penalty (altruistic A) | 0.6 | idem | `(0.6 − otherSat)`, max 0.6 | **(E)** Sets the magnitude: max penalty 0.6 (resident at 0), min 0.15 (resident at 0.45). Strong enough to steer candidates away from a starved resident, without annihilating the score (in [0,1]). |
| Headroom threshold (altruistic B) | 0.75 | idem | Satisfaction below which the preventive branch may fire | **(P/E)** Coincides with the old `StayThreshold`: **above 0.75** the resident is comfortable and a newcomer is no threat; **below**, it is. The "comfortable vs tight" boundary. |
| Preventive cap (B) | 0.5 | idem | Maximum that (B) subtracts | **(P)** Safeguard: the preventive branch never cuts more than **half** the score → it alone cannot catastrophically decide a migration. |
| Proxy load at cold-start | 0.15 | idem | `NormalisedLoad` assumed without measurement | **(E)** Without measurement, ~15% load is assumed so the preventive branch **is not null just from a lack of data**. Small and conservative. |
| Pressure → delay factor | 0.3 | idem | `delay × (1 + pressure×0.3)` | **(P/E)** Delay is **directly** worsened by contention (more competition → more queue waiting) → larger factor. Raised from 0.2 → 0.3 empirically. |
| Pressure → jitter factor | 0.2 | idem | `jitter × (1 + pressure×0.2)` | **(P/E)** Jitter (delay variation) suffers from contention but **less directly** than the mean delay → **smaller** factor (0.2 < 0.3). |
| `m_edcaWeights[4][4]` | see Engine 4 | constructor | Contention-pressure matrix | **(P/E)** Encodes the real EDCA asymmetry (AIFS/CW): VO (AIFSN=2/CWmin=3) barely feels those below it; BE/BK (long AIFS/CW) feel those above them strongly. The **order of magnitude** (VO/VI ≫ BK/BE in the column) is physical; the exact values (8.0, 6.0…) are tuned. |

> **P = principle** (the value derives from something concrete — a cadence, a logical boundary, a sum = 1; defensible rigorously) · **E = empirical** (tuned by observation; what matters is the *band*, not the digit). The distinction is deliberate: a heuristic scheduler assumes several constants are tuned within a reasonable range, and the behaviour is robust to small variations of them.

Changing any of these requires editing the `.h` and recompiling.

### Per-AC weights and objectives

| Structure | Fields | How to configure |
|-----------|--------|------------------|
| `AcGoals` | `targetThroughputMbps`, `maxDelayMs`, `maxJitterMs`, `maxPacketLoss` | `SetGoals(ac, tp, delay, jitter, loss)` — **not currently called**; the constructor defaults apply |
| `AcWeights` | `delayWeight`, `jitterWeight`, `lossWeight`, `throughputWeight` | `SetWeights(ac, delay, jitter, loss, tp)` — called by the `.cc`; each weight has its own CLI flag (e.g. `--voDelayWeight`) |

---

## External feed APIs

The scheduler does not read directly from the PHY/MAC — the simulation script wires up *traces* and calls:

| Function | When to call | Feeds |
|--------|----------------|----------|
| `FeedStaQos(sta, ac, tp, delay, jitter, loss)` | periodically (1 s, in `CalculateStats`) | **`m_staQos`** — the metrics that define `currentSat` (by default estimated by the AP at the TC layer; sinks only with `--decisionMetrics=sink`) |
| `FeedLinkPhyState(linkId, txVector, per)` | `PhyTxPsduBegin` | EWMA of data rate and PER (Engine 2) |
| `FeedLinkTxAttempt(linkId, ac, nFrames)` | `PhyTxPsduBegin` | `txAttempts` (denominator of the PER proxy) |
| `FeedLinkDrop(linkId, ac)` | queue/MAC drops | `dropFrames` (numerator of the PER proxy) |
| `FeedLinkMetrics(linkId, ac, bytes, frames, drops)` | `AckedMpdu` | `txBytes` (diagnostics) and `dropFrames`. **`frames` is ignored** (see note) |
| `FeedPacketTransmitted(linkId, ac, bytes, airtime, peer, tid)` | `PhyTxPsduBegin` (SU) | Engine 3 (throughput/airtime/flows) |
| `FeedLinkTxStart` / `FeedLinkTxEnd` | `PhyTxBegin` / `PhyTxEnd` | Occupied time (`channelUtilization`, `freeAirtime`) |

> **Note on `FeedLinkMetrics`**: the `txFrames` parameter is no longer used. It fed the old PER `txSuccess`, which under-counted ~5% of the MPDUs. The parameter is kept only so as not to break the public signature.

---

## Logging and diagnostics

`EnableDecisionCsv(filename, nodeContext)` produces a CSV with one row per decision:

```
Timestamp, NodeContext, AC, SelectedLink, Decision, CurrentSat, ExpectedSat,
CompetitionPressure, EffectiveCapMbps, CapabilityScore, AvgDelayMs, AvgJitterMs,
PER, ChannelUtil, Throughput, ProjCurrentLink
```

The `AvgDelayMs`, `AvgJitterMs`, `PER`, and `Throughput` columns show the **real per-STA metrics** (from `m_staQos`) when available, falling back to the per-link proxies.

The **`ProjCurrentLink`** column is the projection (`ComputeExpectedQosSatisfaction`) of the link **where the flow currently is**. It serves as an internal-consistency check: it should be close to `CurrentSat` (the measured satisfaction on the same link). If they diverge greatly, the projection is wrong — and it is the same projection that judges the *other* links. It is the best metric for assessing the correctness of the capacity estimate (limitation #1).

Possible `Decision` values:

| Decision | Meaning |
|---|---|
| `BOOTSTRAP_PRIORITY` | Allocation forced by priority (still in warmup) |
| `STAY_SATISFIED` | `currentSat >= StayThreshold` — did not even evaluate alternatives |
| `MIGRATE_CONSIDERATE_EVICT` | A satisfied VO/VI left a link shared with BE/BK for a clean link with capacity (see [Step 2](#step-2--stay_satisfied-and-considerate-eviction)) |
| `STAY_HYSTERESIS` | Evaluated, but the gain does not pay off (or the best link is the current one) |
| `STAY_BEST` | Stayed on the best link, with no prior anchor |
| `MIGRATE_PENDING` | **Wants** to migrate; awaiting dwell confirmation (1 s) |
| `MIGRATE_CAT1_CLEAN` | Migrated: meets objectives without harming anyone |
| `MIGRATE_CAT2_PRIORITY_OVERRIDE` | Migrated: meets objectives, but harms a resident |
| `MIGRATE_CAT3_BEST_EFFORT` | Migrated: meets them nowhere, went to the least-bad option |

> A `MIGRATE_PENDING` never followed by a `MIGRATE_*` means the dwell filtered noise — **expected** behaviour.

`PrintFinalScores()` prints, at the end, the projected score and current satisfaction of each flow on each link.

---

## Known limitations

An honest analysis of the current state. Nothing here prevents the tested scenarios from converging, but they are real structural weaknesses.

1. **The projection measures capacity/airtime, not *access* — and therefore does not model EDCA starvation.** This is the underlying limit: engines 2–5 estimate *how much of the medium is occupied*, not *who can access the channel*. BE starvation next to VO/VI (collapse to 4.95 Mbps with 57% free airtime) is an access phenomenon that **no capacity model captures** — only a Bianchi-style EDCA saturation model (out of scope).
   > **What prevents the BE collapse is the veto, not the capacity.** Historically (before the symmetric veto existed), the BE projection gave ~0.97 on a link with VO/VI and the BE migrated there and collapsed. Today the **symmetric veto** prevents BE/BK from migrating to a link with VO/VI **regardless of any capacity number** — it is the only viable representation of access starvation and is **permanent** (see §2 of the mechanisms).
   > **What the capacity fix does (separate from the above)**: it makes the projection honest in the *other* decisions (Cat1/2/3 cascade, `MeetsOwnGoals`, co-occupancy), which have **no** veto protecting them. `estimatedCapacityMbps` is no longer the nominal PHY rate (which **overestimated** by ~5×: 3722 Mbps) and is now **measured** (a conservative underestimate, ~655–982 depending on load — see Engine 2). `NormalisedLoad` went from "mixture between ACs" (always summed to 1) to "real utilisation". The current-link projection converged with the measured satisfaction for VO/VI (error ~0.11, see the CSV's `ProjCurrentLink` column).

2. **Measured-vs-projected asymmetry + `effCap` measures *what is left over*, not *what I would receive*.** Two coexisting projection defects:
   - *Asymmetry*: `currentSat` (current link) comes from real per-STA metrics; the projection of the *other* links uses per-(link,AC) proxies. Comparing the two in `improvement = bestScore − currentSat` compares different quantities.
   - *residual effCap*: the projection asks *"how much is left over here?"* instead of *"how much would I receive here?"*. A **well-served** flow, using everything it needs, sees "nothing left over" on its own link and projects poorly there.
   - *underestimated capacity*: `estimatedCapacity` is the effective rate at the current load (limited by A-MPDU aggregation), which **underestimates the saturated maximum** (~655–982 measured vs ~1600 real at 6 GHz — see Engine 2). It is conservative (never overestimates), so the "how much would fit here" projection is **pessimistic** — which is safe, but means the scheduler thinks the links are fuller than they are.
   > **Measured impact — real but harmless**: the BE projection on its own link diverges from the measured satisfaction (projected **0.46** vs measured **0.99**) on **low-capacity** links (2.4 GHz, where the BE consumes almost everything); at 5 GHz, with headroom, it converges (error 0.07). It causes no wrong decision because `STAY_SATISFIED` (0.99) always fires and the bad projection is never used. Fixing it would require an airtime-sharing model — not worth the effort.

3. **The `avgQueueDelayMs` proxy (per-link) is ≈ 0 even under congestion.** It only counts packets that reach the dequeue; those stuck or dropped by a full queue never enter the statistic. Only the per-STA path was fixed — the proxy the projection uses remains blind to congestion.

4. **~~`UtilityFunction` gives 0.5 at the target~~ — FIXED and VALIDATED.** *Original problem*: for throughput, meeting the target exactly gave only 0.5 (~2× the target was needed to saturate), which made VO/VI stabilise at ~0.69–0.75, glued to `StayThreshold`, dependent on the vetoes rather than on `STAY_SATISFIED`. *Fix*: (a) in the *higher-is-better* branch the inflection moved to 50% of the target (`1/(1+e^{10(0.5−ratio)})`) → meeting the target ≈ 0.99; (b) the VO/VI jitter goals (`maxJitterMs`) were raised from 0.1 ms (unattainable) to 5/10 ms. *Measured result*: well-served VO/VI/BE moved to ~**0.99** and `STAY_SATISFIED` dominated again (zero migrations in steady state). `StayThreshold=0.75` **did not** need to change — with the satisfied ones at ~0.99, it still separates well-served from starved.

5. **Metrics estimated by the AP itself** *(RESOLVED — it used to be a limitation)*. The scheduler **no longer depends on STA feedback**: it estimates the 4 metrics at its own **traffic-control layer** (the AP queue-disc's `Enqueue`/`Dequeue`/`DropBeforeEnqueue`), tracking each packet from TC enqueue to MAC ACK. Validated almost perfectly against the sinks (see [Per-STA metrics](#per-sta-metrics--estimated-by-the-ap-itself-no-sta-feedback)): loss error 0.0001, delay error 0.19 ms, throughput exact. Since it uses only information a **real** AP also has (its own stack), it is **portable to hardware**. The sinks remain only as a validation reference (`--decisionMetrics=sink` for A/B; the `metric_comparison` CSV). *Residual caveat:* the estimated jitter is slightly above the real one (same order of magnitude).

6. **Cumulative loss is stable but slow.** A sudden degradation takes time to be reflected, and after a flow improves, the bad history only dilutes gradually. It was a deliberate trade-off (stability > reactivity).

7. **The BE/BK veto is absolute and does not look at the real airtime of the VO/VI link.** The two vetoes are not symmetric in strictness: the VO/VI one has a starvation exception (`currentSat < 0.60`); the BE/BK one **always blocks**. The rule assumes VO/VI **saturate** their link — true in the tested scenarios. But it **never checks whether there is free airtime**. Case where it would fail: 1 VO alone on a 6 GHz link using 150 of ~1000 Mbps (~85% free) and 4 BEs jammed on 2.4 GHz with 58% loss — physically the BEs would benefit from sharing the 6 GHz, but they would be stuck with no escape. It is a **priority heuristic, not a capacity-based decision**.

8. **Many critical constants are hardcoded** and not exposed as ns-3 attributes: the `0.65/0.25/0.10` blend, `kMigrationDwellSec`, `kWarmupSamples`, `kStarvationFloor`, `perAlpha`, `m_edcaWeights`. Tuning requires recompiling.

9. **`WouldHarmResident` remains reactive.** It uses the *current* satisfaction of the residents, not a projection of the candidate's arrival impact. It detects "someone is already doing badly", not "I am going to worsen someone who is doing well". The priority vetoes cover the critical cases; Engine 5's preventive penalty (B) partially covers the rest.

10. **The 1 s dwell is coupled to the measurement interval.** It was chosen to guarantee an intent survives ≥ 1 new window. If `MetricsInterval` or the `FeedStaQos` cadence (1 s) changes, the dwell must be re-evaluated.

11. **Balancing — voice pinned.** `RebalanceIdleLinks` **never moves VO** (voice stays on the best link, being the most delay-sensitive). Consequence: in an all-VO scenario the VOs do not spread to an empty link. A conservative choice; relaxing it is trivial (remove the `acIdx == AC_VO` guard).

12. **Balancing — an empty link receives the 1st flow unconditionally.** An idle link's capacity **is not measurable** (`estimatedCapacity = goodput/airtime` needs traffic → it is 0 if never used, or stale if used early). Hence the 1st flow enters with no capacity test — it takes the whole link — and reliance is placed on the **next window's measurement** (to decide on a 2nd) and on the **cascade's self-correction** (which pulls it back if the link really is weak). Without this, the `headroom ≥ target` test would always block the 1st migration.

13. **Test coverage.** Validated on **4-STA** scenarios (2VO+1VI+1BE, 2VO+2VI, 1VO+1VI+1BE+1BK, all-BE) × **3 frequency pairs** (2.4+5, 2.4+6, 5+6), with saturating 150 Mbps-per-STA UDP traffic and static STAs. Outside this envelope — more links, more STAs, mobility, variable traffic, TCP — there are no guarantees. Several mechanisms (bootstrap, vetoes, balancing) implicitly assume **exactly 2 links**.

14. **~~The binding is advisory, not binding.~~ RESOLVED — binding enforced via queue-blocking.** Historically, the per-(STA, AC) decision only biased the **channel-access request** at enqueue and the **dequeue** (Fcfs delegate) served any STA on any link with access → the physical per-flow distribution **used both links** (STR spreading, measured in all-BE and in the S5 VIs). **Now** `EnforceRouting` pins each (STA, AC) to the decided link, blocking its queue on the other links (reason `WIFI_SCHEDULER_ROUTING`) → decisions are **orders** and there is no spreading. See [Advice vs. execution → How it became binding](#how-it-became-binding-implemented--queue-blocking-binding) and [Changes to the ns-3 core](#changes-to-the-ns-3-core).

    > **Historical note (changing the decision did NOT change the physical distribution).** Before binding, **changing** `m_lastSelectedLink` (via migration, balancer, etc.) did **not** change the physical distribution if the other link was active for that AC. Concrete case: in the *switch* 2VO+2VI→2BE+2VI (phase 2), the "considerate eviction" (moving the VI that shared 2.4 GHz with the BEs to 5 GHz) gave a perfect **decision** but a **physical distribution with the VI ~40% on 2.4 GHz** (BE starved at ~19 Mbps), because 2.4 GHz kept pulling VI from the shared queue. With the current binding this problem no longer exists — the VI queue is blocked on 2.4 GHz and the dequeue only occurs on 5 GHz. **The considerate eviction was therefore reintroduced** (see [Step 2](#step-2--stay_satisfied-and-considerate-eviction)) and **now converges physically**: the VI sharing 2.4 GHz with the BEs migrates to 5 GHz (where it fits with the other VI) and 2.4 GHz is freed for the 2 BEs.
    >
    > **STR spreading remains desirable** as a per-flow aggregation mechanism; re-enabling it selectively remains future work (see the "Future work" note in the binding section).

---

## Changes to the ns-3 core

This project lives almost entirely in `scratch/`, but the **enforced binding** (limitation #14, resolved) required **one** change to the ns-3 core. Every changed line is marked with the comment **`// I CHANGED HERE`** for easy tracking.

| File | What | Why |
|---|---|---|
| `src/wifi/model/wifi-mac-queue-scheduler.h` | New value `WIFI_SCHEDULER_ROUTING` in the `enum class WifiQueueBlockedReason` (before `REASONS_COUNT`) + the corresponding `case` in `operator<<`. | `EnforceRouting` needs its **own block reason** to pin each (STA, AC) to a link. All existing reasons (`WAITING_ADDBA_RESP`, `POWER_SAVE_MODE`, `USING_OTHER_EMLSR_LINK`, `WAITING_EMLSR_TRANSITION_DELAY`, `TID_NOT_MAPPED`) are **managed by the MAC**, which would set/clear them on its own, conflicting with our use. The new reason is managed **exclusively** by the scheduler. |

**Impact**: adding a value to the enum grows each container-queue's `Mask = std::bitset<REASONS_COUNT>` by 1 bit — it requires **recompiling the wifi module** (`./ns3 build`), without changing the behaviour of any existing mechanism.

**How to revert**: remove the 3 lines marked `// I CHANGED HERE` in `src/wifi/model/wifi-mac-queue-scheduler.h` and the `EnforceRouting`/`WIFI_SCHEDULER_ROUTING` in `scratch/mlo-qos-weighted-scheduler.h`. Without the new reason, the scheduler reverts to advisory mode (with STR spreading).

**Locate the changes**: `grep -rn "I CHANGED HERE" src/`.
