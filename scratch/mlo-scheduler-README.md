# MLO Custom Traffic-Aware Scheduler — Versão Estática Simples

## Resumo

O **TrafficAwareMloQueueScheduler** é um scheduler simples que implementa uma política **fixa e baseada em Access Category (AC)** para seleção de links em WiFi 7 Multi-Link Operation (MLO):

- **VO/VI (alta prioridade)** → sempre o **link mais rápido** (6GHz > 5GHz > 2.4GHz)
- **BE/BK (baixa prioridade)** → sempre o **link mais lento** (2.4GHz < 5GHz < 6GHz)

**Sem:** scoring dinâmico, pesos, histerese, métricas em tempo real, ou proteção de headroom.

---

## Por Que Existe

### Problema no ns-3 WiFi Padrão

O ns-3 WiFi usa **FCFS (First-Come-First-Serve)** sem considerar:
- Se o traffic é VO/VI (alta prioridade) ou BE/BK (baixa prioridade)
- Se o link é rápido (6GHz @ 320MHz) ou lento (2.4GHz @ 20MHz)

**Resultado:** Packets de voz podem ser enviados pelo link lento, causando latência e jitter desnecessários.

### Solução: Política Estática AC-Based

```
Sem scheduler custom:
  Voice packet 1 → 5GHz (aleatório)
  Voice packet 2 → 6GHz (aleatório)
  Video packet  → 5GHz (aleatório)  ← Problema!
  Web packet    → 6GHz

Com scheduler custom:
  Voice packet 1 → 6GHz (sempre)    ← Latência baixa!
  Voice packet 2 → 6GHz (sempre)    ← Latência baixa!
  Video packet  → 6GHz (sempre)     ← Throughput alto!
  Web packet    → 5GHz (sempre)     ← BE usa link lento
```

---

## Como Funciona

### 1. Classificação de Tráfego

Cada packet tem uma **Access Category (AC)** baseado no tipo de aplicação:

| AC   | Prioridade | Exemplos | Ação |
|------|-----------|----------|------|
| VO   | Máxima    | VoIP, chamadas | Link **rápido** (6 > 5 > 2.4 GHz) |
| VI   | Alta      | Vídeo, streaming | Link **rápido** (6 > 5 > 2.4 GHz) |
| BE   | Média     | Web, downloads | Link **lento** (2.4 < 5 < 6 GHz) |
| BK   | Baixa     | Atualizações bg, P2P | Link **lento** (2.4 < 5 < 6 GHz) |

### 2. Ranking de Velocidade de Link

Links são classificados por frequência e largura de banda:

| Link    | Bandwidth | Rank | Nota |
|---------|-----------|------|------|
| 6 GHz   | 320 MHz   | 2 (rápido) | WiFi 7 |
| 5 GHz   | 160 MHz   | 1 (médio) | WiFi 6E |
| 2.4 GHz | 20 MHz    | 0 (lento) | WiFi 5 |

### 3. Seleção de Link (por MPDU)

Para cada packet que precisa ser transmitido:

```cpp
if (AC == VO || AC == VI) {
    // Voz/Vídeo sempre no link mais rápido
    if (fastLink disponível)
        use fastLink
    else
        use slowLink
} else {
    // Best-Effort/Background sempre no link mais lento
    if (slowLink disponível)
        use slowLink
    else
        use fastLink
}
```

---

## Implementação

### Código: `TrafficAwareMloQueueScheduler`

**Header:** `mlo-traffic-aware-queue-scheduler.h`

```cpp
class TrafficAwareMloQueueScheduler : public WifiMacQueueScheduler {
  public:
    void ConfigureForPair(int freq1, int freq2);
    // ... métodos FCFS delegados
    
  private:
    uint8_t SelectPreferredLink(AcIndex ac, 
                               const std::list<uint8_t>& eligibleLinks);
    Ptr<WifiMacQueueScheduler> m_delegate;  // FcfsWifiQueueScheduler
    uint8_t m_fastLinkId;   // 6GHz / 5GHz (rápido)
    uint8_t m_slowLinkId;   // 2.4GHz / 5GHz (lento)
};
```

**Sem métodos complexos:**
- ❌ `SetWeights()`, `SetHysteresis()`, `SetBeHeadroom()` → removidos
- ❌ `UpdateLinkMetrics()`, `UpdateAcQueueOccupancy()` → removidos

### Harness: `wifi7-mlo-multi-sta-priority-sch.cc`

**Instalação:**
```cpp
if (g_useCustomMloScheduler) {
    InstallTrafficAwareScheduler(apMac, freq1, freq2);
    for (cada STA)
        InstallTrafficAwareScheduler(staMac, freq1, freq2);
}
```

**Instrumentação (logs apenas):**
- Amostra tráfego por link → `linkTrafficCsv`
- Amostra ocupação de fila por AC → `queueOccupancyCsv`
- Sem feedback ao scheduler (já é estático)

**CLI:**
```bash
--useCustomMloScheduler=true   # Ativa scheduler estático
```

---

## Exemplos de Uso

### Caso 1: VoIP + Download

**Setup:**
- Link A: 2.4 GHz (20 MHz)
- Link B: 5 GHz (160 MHz)

```bash
./ns3 run "scratch/wifi7-mlo-multi-sta-priority-sch \
  --freq1=2 --freq2=5 \
  --useCustomMloScheduler=true \
  --staTrafficTypes=voice,best-effort,voice,video"
```

**Resultado esperado:**
```
Voice packets:
  → Sempre 5 GHz (160 MHz, mais rápido)
  → Latência baixa ✓
  → Sem jitter ✓

Download packets (BE):
  → Sempre 2.4 GHz (20 MHz, mais lento)
  → Não interfere com VoIP ✓
```

### Caso 2: WiFi 7 Completo

**Setup:**
- Link A: 5 GHz (160 MHz)
- Link B: 6 GHz (320 MHz) ← WiFi 7

```bash
./ns3 run "scratch/wifi7-mlo-multi-sta-priority-sch \
  --freq1=5 --freq2=6 \
  --useCustomMloScheduler=true \
  --simTime=12"
```

**Resultado esperado:**
```
VO/VI (voz, vídeo):
  → Link 6 GHz (320 MHz, máximo throughput)
  → Latência mínima

BE/BK (web, bg):
  → Link 5 GHz (160 MHz, disponível)
  → Sem afetar traffic prioritário
```

---

## Parâmetros de Configuração

### Argumentos CLI

| Parâmetro | Tipo | Default | Descrição |
|-----------|------|---------|-----------|
| `--useCustomMloScheduler` | bool | true | Ativa scheduler |
| `--freq1` | int | 5 | Primeira frequência (2, 5, 6 GHz) |
| `--freq2` | int | 6 | Segunda frequência (2, 5, 6 GHz) |
| `--staTrafficTypes` | string | "voice,video,voice,besteffort" | AC por STA |
| `--simTime` | double | 12.0 | Duração (segundos) |
| `--linkTrafficCsv` | string | "" | CSV de tráfego |
| `--queueOccupancyCsv` | string | "" | CSV de ocupação fila |

### Variáveis Globais

```cpp
bool g_useCustomMloScheduler = true;  // Enable/disable
```

---

## Visualização de Tráfego

```
        ┌───────────────────────┐
        │    AP (WiFi 7 MLO)    │
        └────────┬──────────────┘
                 │
           ┌─────┴──────┐
           │            │
       ┌───▼──┐    ┌───▼──┐
       │ 5GHz │    │ 6GHz │
       │ 160M │    │ 320M │
       └───┬──┘    └───┬──┘
           │           │
           │      ┌────┴─────────────┐
           │      │  Roteamento:     │
           │      │  VO/VI → 6 GHz   │
           │      │  BE/BK → 5 GHz   │
           │      └─────────────────┘
       ┌───▼───────────▼──┐
       │   STA (VoIP+Web)  │
       └──────────────────┘

Fluxo de Dados:
  📱 VoIP Call (TID 6,7)  ─────────► 6 GHz (rápido)
  📺 Video (TID 4,5)      ─────────► 6 GHz (rápido)
  🌐 Web (TID 0,3)        ─────────► 5 GHz (lento)
  🔄 Background (TID 1)   ─────────► 5 GHz (lento)
```

---

## Limitações

1. **Política fixa** — sem adaptação a congestionamento em tempo real
2. **Sem métricas dinâmicas** — não rastreia SNR, delay, ou drop rate
3. **Sem fallback inteligente** — se link preferido falhar, usa o outro (pode ser lento)
4. **Sem proteção de headroom** — BE pode usar recurso de VO/VI se não estiverem transmitindo

---

## Próximas Melhorias Possíveis

Se precisar de **adaptação dinâmica**, considere implementar:

1. **Scoring com pesos** — considerar throughput, delay, SNR
2. **Hysteresis** — evitar switching frequente entre links
3. **Proteção AC VO/VI** — forçar BE ao link lento mesmo se VO/VI não está usando
4. **Rastreamento em tempo real** — SNR, delay por packet, drop rate

---

## Onde Procurar

- **Scheduler:** [scratch/mlo-traffic-aware-queue-scheduler.h](../scratch/mlo-traffic-aware-queue-scheduler.h)
- **Harness:** [scratch/wifi7-mlo-multi-sta-priority-sch.cc](../scratch/wifi7-mlo-multi-sta-priority-sch.cc)
- **Scripts de teste:** `run_multi_sta_mlo_priority_experiments_with_scheduler.sh`

3. **No RSSI-based optimization**: Link rank is fixed by frequency, not by signal strength

### Possible Future Enhancements

#### 1. Dynamic Congestion Fallback
```cpp
// Monitor queue depth on fast link periodically
if (GetQueueOccupancy(fastLink) > CONGESTION_THRESHOLD)
{
    // Temporarily unblock slow link for VO/VI
    UnblockQueues(AC_VO, slowLink);
}
```

#### 2. RSSI-Based Link Selection
```cpp
// Rank links by actual signal quality, not just frequency
int fastLink = (rssi[0] > rssi[1]) ? 0 : 1;
// Instead of: int fastLink = (freq[0] < freq[1]) ? 1 : 0;
```

#### 3. Per-STA Traffic Steering
```cpp
// Different traffic types to different STAs
// E.g., STA0 (VoIP) → always 6GHz
//       STA1 (downloads) → always 5GHz
```

#### 4. Throughput-Based Selection
```cpp
// Use past throughput metrics to select "best" link
int fastLink = (avgThroughput[0] > avgThroughput[1]) ? 0 : 1;
```

---

## Monitoring & Debugging

### Enable Logging

To see detailed scheduler logs during execution:

```bash
./ns3 run "scratch/wifi7-mlo-multi-sta-priority --useCustomMloScheduler=true" \
    2>&1 | grep "MLO Scheduler"
```

### Example Log Output

```
MLO Scheduler: STA 00:00:00:00:00:01 link speeds initialized: Link0[5GHz rank=1] Link1[6GHz rank=2]
MLO Scheduler: STA 00:00:00:00:00:01 AC=VO blocked link 0 (prefer link 1)
MLO Scheduler: STA 00:00:00:00:00:01 AC=VI blocked link 0 (prefer link 1)
MLO Scheduler: STA 00:00:00:00:00:01 AC=BE,BK not blocked (can use all links)

MLO Scheduler: STA 00:00:00:00:00:02 link speeds initialized: Link0[5GHz rank=1] Link1[6GHz rank=2]
MLO Scheduler: STA 00:00:00:00:00:02 AC=VO blocked link 0 (prefer link 1)
...
```

### Analyzing Results

To verify the scheduler is working:

1. **Check link traffic distribution** (if `--linkTrafficCsv` provided):
   - VO/VI packets should concentrate on fast link
   - BE/BK packets should be distributed

2. **Compare latency metrics**:
   - With scheduler: VO/VI should have lower latency
   - Without scheduler: VO/VI latency should be higher/more variable

3. **Monitor per-link packet counts**:
   - With scheduler: FastLink should have more VO/VI packets

---

## Code Location Reference

| Component | Location | Purpose |
|-----------|----------|---------|
| Globals | `scratch/wifi7-mlo-multi-sta-priority.cc` line ~135 | Scheduler configuration |
| Speed ranking | `scratch/wifi7-mlo-multi-sta-priority.cc` line ~990 | Frequency → rank mapping |
| Link initialization | `scratch/wifi7-mlo-multi-sta-priority.cc` line ~1000 | Per-STA setup |
| Queue blocking | `scratch/wifi7-mlo-multi-sta-priority.cc` line ~1100 | Apply blocking policy |
| Main integration | `scratch/wifi7-mlo-multi-sta-priority.cc` line ~1520 | Setup scheduler after static association |
| Command-line flag | `scratch/wifi7-mlo-multi-sta-priority.cc` line ~1220 | User toggle |

---

## Troubleshooting

### Issue: Scheduler Not Activating

**Symptoms**: Log shows "MLO CUSTOM SCHEDULER DISABLED" when it should be enabled

**Solution**:
1. Check command-line: `--useCustomMloScheduler=true` (default is true)
2. Verify `useStaticSetup=true` (scheduler only activates with static setup)

```bash
./ns3 run "scratch/wifi7-mlo-multi-sta-priority \
    --useCustomMloScheduler=true \
    --staticSetup=true"
```

### Issue: No Difference in Results

**Symptoms**: With/without scheduler shows similar link usage

**Solution**:
1. Use `--linkTrafficCsv` to get detailed per-link statistics
2. Check that `staTrafficTypes` includes VO/VI (high-priority) traffic
3. Verify links have different speeds (e.g., 5GHz vs 6GHz, not 5GHz vs 5GHz)

```bash
./ns3 run "scratch/wifi7-mlo-multi-sta-priority \
    --freq1=5 --freq2=6 \
    --staTrafficTypes=voice,video,voice,best-effort \
    --linkTrafficCsv=traffic.csv"
```

---

## Frequently Asked Questions

**Q: Can I use this with other MLO schedulers (like RrMultiUserScheduler)?**
A: The custom scheduler modifies queue blocking, not the MAC scheduler. It can coexist with other schedulers, but was designed for FcfsWifiQueueScheduler (default).

**Q: What happens if both links have the same speed?**
A: The scheduler still works but has no benefit. Link 0 is treated as "slower" in that case.

**Q: Does this work with EMLSR (Enhanced Multi-Link Single Radio)?**
A: Not directly configured yet. EMLSR requires additional logic for link switching timing.

**Q: Can I customize which ACs are prioritized?**
A: Yes, modify `ApplyMloSchedulerLogicForSta()` to change which ACs are blocked on slow links (currently AC_VO and AC_VI).

---

## Summary

The Custom MLO Scheduler intelligently routes high-priority traffic (voice, video) to faster links while allowing best-effort traffic flexibility. This reduces jitter, improves latency consistency, and maximizes WiFi 7's potential for mixed-workload environments.

**Toggle the scheduler easily**:
- Enable: `--useCustomMloScheduler=true` (default)
- Disable: `--useCustomMloScheduler=false`

**Key benefit**: Voice calls and video streams prioritize 6 GHz (320 MHz) over slower alternatives, improving user experience.
