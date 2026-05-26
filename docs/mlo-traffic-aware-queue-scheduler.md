# TrafficAwareMloQueueScheduler — Scheduler Estático Simples

## Resumo

O `TrafficAwareMloQueueScheduler` é um wrapper simples que:

1. **Delega ordenação** a `FcfsWifiQueueScheduler` (mantém FIFO dentro de cada fila)
2. **Aplica política estática de seleção de link** por Access Category (AC):
   - **VO/VI (alta prioridade)** → sempre o **link mais rápido** (6GHz > 5GHz > 2.4GHz)
   - **BE/BK (baixa prioridade)** → sempre o **link mais lento** (2.4GHz < 5GHz < 6GHz)

**Sem complexidade de scoring, pesos, histerese ou métricas** — apenas política fixa AC-based.

---

## Como Funciona

### 1. Inicialização
```cpp
Ptr<TrafficAwareMloQueueScheduler> scheduler = 
    InstallTrafficAwareScheduler(mac, freq1, freq2);
scheduler->ConfigureForPair(freq1, freq2);
```

O scheduler identifica os dois links (freq1, freq2) e classifica-os por velocidade:
- Link mais rápido → `m_fastLinkId`
- Link mais lento → `m_slowLinkId`

### 2. Seleção de Link (por MPDU)

Quando o ns-3 WiFi stack precisa de enviar um MPDU:

1. Chama `GetLinkIds(ac, mpdu)` no scheduler
2. O scheduler invoca `SelectPreferredLink(ac, eligibleLinks)` 
3. **Política estática aplicada:**

```cpp
if (ac == AC_VO || ac == AC_VI) {
    // Voz/Vídeo → link rápido
    return (fastLink em eligibleLinks) ? fastLink : slowLink;
} else {
    // BE/BK → link lento
    return (slowLink em eligibleLinks) ? slowLink : fastLink;
}
```

4. Devolve lista com um único link → `[selectedLink]`
5. WiFi stack usa esse link para transmitir o MPDU

### 3. Prioridade das ACs

| AC   | Priority | Política |
|------|----------|---------|
| VO   | 0 (máx)  | Link rápido (6GHz > 5GHz > 2.4GHz) |
| VI   | 1        | Link rápido (6GHz > 5GHz > 2.4GHz) |
| BE   | 2        | Link lento (2.4GHz < 5GHz < 6GHz) |
| BK   | 3 (mín)  | Link lento (2.4GHz < 5GHz < 6GHz) |

---

## Estrutura do Código

### Header: `mlo-traffic-aware-queue-scheduler.h`

**APIs principais:**
```cpp
class TrafficAwareMloQueueScheduler : public WifiMacQueueScheduler {
  public:
    void ConfigureForPair(int freq1, int freq2);
    // ... métodos virtuais delegados a FcfsWifiQueueScheduler
    
  private:
    uint8_t SelectPreferredLink(AcIndex ac, 
                               const std::list<uint8_t>& eligibleLinks);
    Ptr<WifiMacQueueScheduler> m_delegate;  // FcfsWifiQueueScheduler
    uint8_t m_fastLinkId;   // 6GHz, 5GHz, etc.
    uint8_t m_slowLinkId;   // 2.4GHz, etc.
};
```

**Sem métodos de configuração de pesos:**
- ❌ `SetWeights()` removido
- ❌ `SetHysteresis()` removido
- ❌ `SetBeHeadroom()` removido
- ❌ `UpdateLinkMetrics()` removido
- ❌ `UpdateAcQueueOccupancy()` removido

### Harness: `scratch/wifi7-mlo-multi-sta-priority-sch.cc`

**Instrumentação:**
- Amostra tráfego por link (0.1s interval) → `g_linkTrafficData`
- Amostra ocupação de fila por AC (0.1s interval) → `g_queueOccupancyStream`
- Escreve CSVs: `linkTrafficCsv`, `queueOccupancyCsv`
- **Sem chamadas a `UpdateLinkMetrics()` ou `UpdateAcQueueOccupancy()`**

**Instalação do scheduler:**
```cpp
if (g_useCustomMloScheduler) {
    InstallTrafficAwareScheduler(apWifiDev->GetMac(), freq1, freq2);
    for (cada STA)
        InstallTrafficAwareScheduler(staWifiDev->GetMac(), freq1, freq2);
}
```

**CLI flag:**
```bash
--useCustomMloScheduler=true   # Ativa o scheduler estático
```

---

## Exemplo de Execução

```bash
./ns3 run "scratch/wifi7-mlo-multi-sta-priority-sch \
  --simTime=12 \
  --useCustomMloScheduler=true \
  --freq1=2 --freq2=5 \
  --linkTrafficCsv=/tmp/link_traffic.csv \
  --queueOccupancyCsv=/tmp/queue_occ.csv"
```

**Resultado esperado:**
- VO/VI packets → sempre no link 5GHz (mais rápido que 2.4GHz)
- BE/BK packets → sempre no link 2.4GHz (mais lento)
- Latência VO/VI reduzida
- Utilização mais previsível do link

---

## Onde Procurar no Código

- **Implementação do scheduler:** [scratch/mlo-traffic-aware-queue-scheduler.h](../scratch/mlo-traffic-aware-queue-scheduler.h)
- **Harness e instrumentação:** [scratch/wifi7-mlo-multi-sta-priority-sch.cc](../scratch/wifi7-mlo-multi-sta-priority-sch.cc)
- **Scripts de teste:** `run_multi_sta_mlo_priority_experiments_with_scheduler.sh`

---

## Limitações Conhecidas

1. **Política fixa** — não se adapta ao congestionamento em tempo real
2. **Sem fallback inteligente** — se o link preferido falhar, usa o outro
3. **Sem métricas de SNR/delay** — decisão baseada apenas em AC + velocidade
4. **Sem proteção de headroom** — BE pode usar todo o link se VO/VI não estiverem usando

---

## Próximas Melhorias Possíveis

Se precisar de adaptação dinâmica, considere:
1. Implementar scoring com pesos configuráveis
2. Adicionar hysteresis e minSwitchInterval
3. Rastrear SNR e delay em tempo real
4. Proteção dinâmica de AC VO/VI contra congestionamento de BE/BK
