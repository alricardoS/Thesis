# QosWeightedMloScheduler — Documentação Técnica

## Visão geral

O `QosWeightedMloScheduler` é um scheduler de seleção de link para Multi-Link Operation (MLO) em Wi-Fi 7, implementado como uma extensão do `WifiMacQueueScheduler` do ns-3. Opera **apenas no AP** (tráfego downlink); as STAs continuam a usar o `FcfsWifiQueueScheduler` por defeito do ns-3.

A cada pacote (MPDU) que o AP precisa de enviar, o scheduler decide **a que link físico** (ex: 2.4 GHz, 5 GHz ou 6 GHz) esse pacote deve ser encaminhado, com o objetivo de que cada fluxo cumpra os seus próprios objetivos de qualidade de serviço (QoS), respeitando ao mesmo tempo a hierarquia de prioridades real do EDCA (802.11e): **VO > VI > BE > BK**.

O scheduler não decide *quando* transmitir (isso continua a ser gerido pelo EDCA/CSMA-CA real do ns-3) — decide apenas **em qual link** colocar cada pacote, na fila MAC apropriada.

### Suporte N-link (dualband e triband) — Fase B

O scheduler é **agnóstico ao número de links**. Internamente mantém `m_linksByRank` — a lista dos linkIds ordenada por qualidade da banda (**melhor primeiro**, pior último), construída em `ConfigureForLinks(freqs)` a partir de `GetFrequencyRank` (6 GHz > 5 GHz > 2.4 GHz). Os aliases `m_fastLinkId` (=`m_linksByRank.front()`, o melhor) e `m_slowLinkId` (=`m_linksByRank.back()`, o pior) são derivados daí. `ConfigureForPair(f1, f2)` é apenas um wrapper de `ConfigureForLinks({f1, f2})`, pelo que o comportamento a 2 links é **idêntico** ao anterior.

Todos os mecanismos que iteravam explicitamente "fast/slow" passaram a iterar `m_linksByRank` (bootstrap, balanceador, cascata, cold-start por frequência) e os vetos de downgrade passaram a ser **baseados em rank** via `LinkRankPos(linkId)` (0 = melhor). Assim, adicionar um 3º link (triband) não requer alterações à lógica de decisão.

- **2 links (dualband)**: correr com `--freq1`/`--freq2` (ex.: `5`+`6`). `--freq3=0` (default) mantém o modo 2-link.
- **3 links (triband)**: passar `--freq3` (ex.: `--freq1=2 --freq2=5 --freq3=6`). O `.cc` cria a 3ª PHY/canal e chama o overload `InstallQosWeightedScheduler(mac, {f1,f2,f3})`. No runner/suite, a banda `tri` ativa este modo.

> **Cold-start por frequência** (antes de haver medições): a capacidade inicial de cada link é estimada pela sua banda — 2.4 GHz→150, 5 GHz→400, 6 GHz→500 Mbps — em vez do antigo binário fast/slow.

### Granularidade: por (STA, AC), não por AC

A chave de decisão é `StaAcKey = std::pair<Mac48Address, uint8_t>` — o **MAC de destino** + o índice do AC.

> **Porquê**: a versão anterior encaminhava por AC apenas. Isso significava que duas STAs com o mesmo AC (ex: 2 STAs de voz) recebiam **sempre a mesma decisão de link** — era impossível distribuí-las por links diferentes. Com a chave por (STA, AC), cada fluxo é encaminhado independentemente.

Todo o estado de decisão é chaveado assim:

| Membro | Tipo | Papel |
|---|---|---|
| `m_lastSelectedLink` | `map<StaAcKey, uint8_t>` | Link atual (âncora) de cada fluxo |
| `m_currentSatisfaction` | `map<StaAcKey, QosSatisfaction>` | Satisfação **medida** no link atual |
| `m_staQos` | `map<StaAcKey, StaQos>` | Métricas de QoS (throughput, delay, jitter, loss) — por defeito **estimadas pelo próprio AP** (camada TC); opcionalmente dos sinks (`--decisionMetrics=sink`) |
| `m_hasMeasuredSat` | `set<StaAcKey>` | Fluxos que já passaram o warmup |
| `m_pendingMigration` | `map<StaAcKey, pair<uint8_t,double>>` | Intenção de migração pendente (link alvo, instante) — debounce |

---

## Arquitetura: seis motores + uma camada de estabilidade

| # | Motor | Responsabilidade | Função/estrutura principal |
|---|-------|-------------------|------------------------------|
| 1 | **Goal-Awareness Engine** | Calcula o quão satisfeito está cada (STA, AC), a partir das **métricas reais** | `ComputeQosSatisfaction`, `QosSatisfaction`, `StaQos` |
| 2 | **Link Capability Engine** | Estima a capacidade real de cada link (PHY) e quanto dela está disponível, por AC, respeitando prioridade | `UpdateLinkCapability`, `LinkCapability` |
| 3 | **Traffic Composition Engine** | Mede o tráfego real (throughput, airtime, nº de fluxos) que cada AC já gera em cada link | `UpdateTrafficComposition`, `LinkTrafficComposition` |
| 4 | **EDCA Competition Engine** | Modela a pressão de contenção entre ACs no mesmo link | `UpdateEdcaCompetition`, `EdcaCompetition` |
| 5 | **Projection Engine** | Combina 2–4 numa pontuação "quão bom seria este link para mim" | `ComputeExpectedQosSatisfaction` |
| 6 | **Migration Decision Engine** | Decide, com cascata de prioridades, vetos e histerese, se migra ou fica | `DecideLinkMigration`, `MeetsOwnGoals`, `WouldHarmResident` |

Sobre estes motores existe uma **camada de estabilidade**, construída a partir de bugs reais observados em simulação (cada mecanismo é explicado em detalhe na secção [Mecanismos de prevenção](#mecanismos-de-prevenção)):

- **Bootstrap por prioridade + warmup** (2 janelas)
- **Debounce de migração** (dwell de 1 s)
- **Veto anti-downgrade VO/VI** (com exceção de fome)
- **Veto simétrico BE/BK**
- **Exclusão de frames broadcast/beacons** das verificações de residência

Os motores 1–4 são recalculados a cada `MetricsInterval` (0.5 s). Os motores 5–6 correm **por pacote** (em `GetLinkIds`), mas usam os valores já calculados nessa janela — não recalculam PHY/tráfego a cada pacote.

---

## Fluxo de uma decisão, passo a passo

Esta é a ordem exata do código em `DecideLinkMigration`:

1. O ns-3 chama `GetLinkIds(ac, mpdu, ...)` quando tem um MPDU pronto a enfileirar.
2. O scheduler obtém os links elegíveis via `FcfsWifiQueueScheduler` delegado. Se a lista vier vazia, devolve vazio.
3. Extrai o **MAC de destino** (`mpdu->GetHeader().GetAddr1()`); se for broadcast, usa a chave `ff:ff:ff:ff:ff:ff`. Chama `DecideLinkMigration(ac, dest, eligible)`.
4. **Atalhos**: se só houver 1 link elegível, grava a âncora e devolve-o (é por aqui que os beacons passam — ver [exclusão de beacons](#4-exclusão-de-beacons-isroutablesta)).
5. Determina o **link atual** (âncora) e o `currentSat` (satisfação **medida**, de `m_staQos`).
6. **Bootstrap por prioridade** — se o fluxo ainda não tem 2 amostras medidas (`m_hasMeasuredSat`), força VO/VI → melhor link; BE/BK → em **triband** (≥3 links) o **2º melhor** link (ex.: 5 GHz, deixando o 2.4 GHz livre), em **dualband** o pior link. E **termina aqui**.
7. **`STAY_SATISFIED`** — se `currentSat >= StayThreshold` e há âncora, fica sem avaliar mais nada.
8. **Cascata Cat1/Cat2/Cat3** — avalia todos os links elegíveis e escolhe o melhor da melhor categoria.
9. **Veto anti-downgrade VO/VI** — pode reverter `bestLink` para o link atual.
10. **Veto simétrico BE/BK** — idem, para ACs de baixa prioridade.
11. **Histerese + debounce** — se `bestLink != currentLink` e o ganho passar `MigrationThreshold`, a intenção tem de persistir ≥ 1 s (dwell) antes de executar.
12. Grava a âncora (`m_lastSelectedLink`), regista no CSV e devolve o link.

A cada 0.5 s, `UpdatePeriodicMetrics` recalcula os motores 1–4 e atualiza `m_currentSatisfaction`.

---

## Motor 1 — Goal-Awareness Engine

### O que faz

Para cada **(STA, AC)**, calcula um **índice de satisfação composto** entre 0 e 1, a partir das **métricas reais end-to-end** guardadas em `m_staQos` (ver [Métricas reais por-STA](#métricas-reais-por-sta)):

```cpp
sat.throughput = UtilityFunction(q.tpMbps,   g.targetThroughputMbps, /*lowerIsBetter=*/false)
sat.delay      = UtilityFunction(q.delayMs,  g.maxDelayMs,           true)
sat.jitter     = UtilityFunction(q.jitterMs, g.maxJitterMs,          true)
sat.loss       = UtilityFunction(q.lossRate, g.maxPacketLoss,        true)

sat.index = (throughputWeight·sat.throughput + delayWeight·sat.delay
           + jitterWeight·sat.jitter + lossWeight·sat.loss) / soma_dos_pesos
```

Se ainda não houver uma amostra válida em `m_staQos` (arranque), devolve zeros — o bootstrap trata dessa fase.

> **Nota importante**: o parâmetro `linkId` existe na assinatura mas **não é usado** — as métricas reais são end-to-end e referem-se ao link onde o fluxo *está*. Isto é deliberado: para o link atual não é preciso projetar, sabe-se o valor real. Para os *outros* links usa-se o Motor 5.

### A função sigmoide (`UtilityFunction`)

```cpp
ratio = valor / alvo
lowerIsBetter (máximo tolerável):   utilidade = 1 / (1 + e^(5·(ratio − 1)))    → 0.5 no limite
higherIsBetter (alvo desejado):     utilidade = 1 / (1 + e^(10·(0.5 − ratio))) → 0.99 ao cumprir
```

- **`lowerIsBetter`** (delay, jitter, loss): o alvo é um *máximo tolerável*, logo estar no limite = **0.5** é semântica correta.
- **`higherIsBetter`** (throughput): o alvo é *o que se precisa*. A inflexão está a **50% do alvo** → cumprir o alvo ≈ **0.99**, metade do alvo = 0.5. *(Antes a inflexão estava no alvo, o que dava só 0.5 ao cumprir e exigia ~2× o alvo para saturar — ver limitação #4.)*
- Se `alvo <= 0`, devolve `0.5`.
- Por ser assintótica, nunca atinge exatamente 0 ou 1.

### `AcGoals` — objetivos por AC (construtor; `SetGoals` **não** é chamado pelo script)

| AC | targetThroughputMbps | maxDelayMs | maxJitterMs | maxPacketLoss |
|----|----------------------|------------|-------------|----------------|
| VO | 150.0 | 15.0 | **5.0** | 0.01 (1%) |
| VI | 150.0 | 30.0 | **10.0** | 0.01 (1%) |
| BE | 150.0 | 200.0 | 1.0 | 0.10 (10%) |
| BK | 150.0 | 300.0 | 100.0 | 0.10 (10%) |

> `maxJitterMs` de VO/VI foi subido de 0.1 ms (100 µs, fisicamente inatingível numa rede partilhada) para 5/10 ms — antes garantia que o VO nunca pontuava bem em jitter, custando-lhe ~0.22 de satisfação permanentes.

### `AcWeights` — pesos por AC (definidos pelo `.cc` via `SetWeights`)

| AC | delayWeight | jitterWeight | lossWeight | throughputWeight |
|----|-------------|--------------|------------|-------------------|
| VO | 0.40 | 0.30 | 0.25 | 0.05 |
| VI | 0.25 | 0.15 | 0.20 | 0.40 |
| BE | 0.20 | 0.05 | 0.15 | 0.60 |
| BK | 0.05 | 0.05 | 0.10 | 0.80 |

Não precisam de somar 1 — o código normaliza pela soma. VO prioriza delay+jitter (voz não tolera atraso, tolera menos throughput). BK quase só quer throughput.

---

## Motor 2 — Link Capability Engine

### `estimatedCapacityMbps` — rate efetivo à carga atual (medido)

```cpp
busyFrac = min(1, m_linkBusyTime[link] / dt)          // airtime ocupado real (com overhead)
occupied = Σ throughput de todos os ACs               // goodput agregado
se busyFrac > 0.05 e occupied > 0:
    estimatedCapacityMbps = occupied / busyFrac        // MEDIDO
senão se houver dados PHY:
    estimatedCapacityMbps = dataRate_PHY × (1 − PER)   // fallback
senão:
    estimatedCapacityMbps = 400 (rápido) ou 150 (lento) // cold-start
```

> **O que este valor É (e o que NÃO é).** `occupied / busyFrac` mede o **rate efetivo no ponto de operação atual** — quanto goodput o link entrega por unidade de airtime, à carga que está a ver *agora*. **Não** é o máximo saturado do link.
>
> **Porquê medir em vez de usar a taxa PHY.** `dataRate_PHY × (1−PER)` é a taxa **nominal** (símbolos de payload no ar) e **ignora todo o overhead MAC** (preâmbulo, AIFS, backoff, SIFS, BlockAck). Isso **sobrestimava** a capacidade ~5× (ex.: 3722 Mbps no 6 GHz). A correção substituiu essa **sobrestimação grosseira** por uma **subestimação conservadora** — muito mais seguro, porque o scheduler nunca passa a achar que um link tem mais folga do que tem.
>
> **Porquê subestima (e sobe com a carga).** A eficiência do WiFi depende da **agregação A-MPDU**, que cresce com a profundidade das filas: mais carga → A-MPDUs maiores → menos overhead por byte → rate efetivo maior. Medido no 6 GHz:
> | Carga no link | `estimatedCapacity` medida |
> |---|---|
> | ~300 Mbps (1VO+1VI) | ~655 Mbps |
> | ~450 Mbps (2VO+1VI) | ~982 Mbps |
> | saturação de link único | ~1600 Mbps (máximo real) |
>
> O valor **sobe monotonamente com a carga** e **nunca sobrestima**. É o rate relevante para a carga que o scheduler realmente vê — não o máximo teórico. Consequência (ver limitação #2): a projeção de "quanto caberia aqui" é **pessimista**.
>
> **Fonte crítica**: `m_linkBusyTime` (de `FeedLinkTxStart/End`, tempo real de PHY ocupado) — os proxies de airtime por-AC não servem, porque são tempo de payload puro (sem overhead) e dariam um cálculo circular de volta à taxa PHY.

> **⚠️ Agregação A-MPDU por AC (config. do `.cc`).** Todos os quatro ACs usam agora `MaxAmpduSize = 65535` — incluindo o **BK** (`bkMaxAmpduBytes = 65535`; anteriormente era `0`/desligado). Com a agregação desligada, o BK enviava frame-a-frame e ficava limitado a **~50 Mbps mesmo estando sozinho num link** (cada frame paga todo o overhead MAC: preâmbulo, AIFS, backoff, SIFS, BlockAck), saturando a fila TC → delay de segundos e loss elevada. Com A-MPDU ativo o BK acompanha os restantes ACs até ao rate oferecido. Ajustável por CLI: `--bkMaxAmpdu=<bytes>` (`0` volta a desligar). Este teto de agregação é uma característica de **MAC**, independente da decisão de link do scheduler.

`ConfigureForPair` ordena os dois links por `GetFrequencyRank` (6 GHz > 5 GHz > 2.4 GHz) e define `m_fastLinkId` / `m_slowLinkId`.

### `availableCapacityPerAcMbps` — priority-aware

Cada AC desconta **apenas** o consumo de ACs com prioridade igual ou superior:

```
availableCapacityPerAcMbps[VO] = estimatedCapacity − consumo(VO)
availableCapacityPerAcMbps[VI] = estimatedCapacity − consumo(VO) − consumo(VI)
availableCapacityPerAcMbps[BE] = estimatedCapacity − consumo(VO) − consumo(VI) − consumo(BE)
availableCapacityPerAcMbps[BK] = ... − consumo(BK)
```

Modela o 802.11 real: o VO, com AIFS/CW mais curtos, ganha sempre o meio primeiro — não "perde" capacidade porque o BE transmite muito. O BE sofre tudo o que está acima dele.

### `freeAirtime` e `capabilityScore`

```
freeAirtime     = 1 − (tempo ocupado do link / duração da janela)
capabilityScore = min(1, availableCapacityMbps / estimatedCapacityMbps)
```

---

## Motor 3 — Traffic Composition Engine

Mede, por janela de 0.5 s, o tráfego real de cada AC em cada link:

- `voThroughputMbps`, `viThroughputMbps`, `beThroughputMbps`, `bkThroughputMbps`
- `voAirtimeFrac`, etc. — **utilização real** do link por AC, em [0,1] (proxy de carga para o Motor 4)
- `voFlows`, etc. — nº de fluxos distintos (STA+TID) ativos

> **`*AirtimeFrac` = utilização real, não mistura.** O airtime de payload por-AC (de `FeedPacketTransmitted`) capta bem o *rácio* entre ACs, mas é tempo de payload puro. É escalado para o `busyFrac` real do PHY (`m_linkBusyTime/dt`) → cada fração fica em [0,1] e todas juntas somam a utilização do link. *(Antes normalizava-se pelo airtime usado, o que fazia as frações somarem sempre 1.0: um AC num link ocioso "pesava" o mesmo que num saturado, e a `pressure` não distinguia os dois — ver limitação #1.)*

Alimenta o Motor 2 (consumo absoluto) e o Motor 4 (carga normalizada).

---

## Motor 4 — EDCA Competition Engine

### `m_edcaWeights` — matriz de pressão de contenção

```cpp
// linhas = AC candidata; colunas = outra AC presente no link
// ordem: BK, BE, VI, VO
{0.0,  0.5,  7.0,  9.0}   // candidata BK  — sente MUITO o VO (9.0) e o VI (7.0)
{0.5,  1.0,  6.0,  8.0}   // candidata BE  — sente MUITO o VO (8.0) e o VI (6.0)
{0.25, 0.5,  1.0,  2.0}   // candidata VI  — sente moderadamente o VO (2.0)
{0.0,  0.05, 0.25, 1.0}   // candidata VO  — quase não sente ninguém abaixo
```

`m_edcaWeights[X][Y]` = **"o quanto a AC X sente a presença da AC Y"**. Não é simétrica, porque a contenção real do EDCA não é.

### Cadeia de cálculo

```
pressure[cand]   = Σ (m_edcaWeights[cand][other] × NormalisedLoad(other, link))
opportunity      = freeAirtime / (1 + pressure)        (limitado a [0,1])
effectiveAvailableCapacityMbps[ac] = availableCapacityPerAcMbps[ac] × opportunity
```

`effectiveAvailableCapacityMbps` é a estimativa final de "quantos Mbps eu conseguiria aqui agora".

---

## Motor 5 — Projection Engine (`ComputeExpectedQosSatisfaction`)

Usado para avaliar links (incluindo aqueles onde o fluxo **não** está).

```
coRes         = nº de OUTRAS STAs do mesmo AC já neste link
effCapShared  = effectiveAvailableCapacityMbps / (coRes + 1)      ← co-occupancy penalty

expTp     = UtilityFunction(effCapShared, targetThroughputMbps, false)
expDelay  = UtilityFunction(delay_proxy × (1 + pressure×0.3), maxDelayMs, true)
expJitter = UtilityFunction(jitter_proxy × (1 + pressure×0.2), maxJitterMs, true)
expLoss   = UtilityFunction(PER_proxy, maxPacketLoss, true)

baseSat = (throughputWeight·expTp + delayWeight·expDelay
         + jitterWeight·expJitter + lossWeight·expLoss) / soma_dos_pesos

expectedScore = baseSat × 0.65 + capabilityScore × 0.25 + (1 − PER_proxy) × 0.10
```

> **Atenção**: `delay_proxy` / `jitter_proxy` / `PER_proxy` vêm de `m_metrics[link][ac]` (por-link, **não** as métricas reais por-STA). Esta assimetria é uma limitação conhecida — ver [Limitações](#limitações-conhecidas).

### Co-occupancy penalty

Divide a capacidade projetada pelo nº de STAs do mesmo AC que partilhariam o link (`coRes + 1`). Se o fluxo fosse o único do seu AC ali, o denominador é 1 e nada muda.

> **Porquê**: sem isto, num cenário 2VO+2VI todos os fluxos veriam o link rápido como igualmente bom e amontoavam-se lá. A penalidade faz o 2.º VI transbordar quando o link não os comporta aos dois.

### Penalidade altruísta

Aplicada depois do `expectedScore`, para proteger ACs já residentes:

- **(A) Reativa** — se uma AC residente já está esfomeada (`currentSat < 0.45`), soma `(0.6 − currentSat)`.
- **(B) Preventiva** — mesmo sem estar esfomeada, se o `headroomRatio` (`effectiveAvailableCapacity / targetThroughput`) do residente for < 1.0 **e** `currentSat < 0.75`, aplica penalidade proporcional à carga que o candidato traria (`min(0.5, tightness × candidateLoad × 2)`).

A penalidade final é o `max` entre (A) e (B) por residente (não a soma), subtraída ao score, nunca abaixo de 0.

---

## Motor 6 — Migration Decision Engine (`DecideLinkMigration`)

### Passo 1 — Bootstrap por prioridade (enquanto não há medições)

```cpp
if (m_hasMeasuredSat.count(staAcKey) == 0) {
    bootLink = (acIdx >= AC_VI) ? m_fastLinkId : m_slowLinkId;   // VO/VI → rápido; BE/BK → lento
    decision = "BOOTSTRAP_PRIORITY";
    // termina aqui — não corre a cascata
}
```

### Passo 2 — `STAY_SATISFIED` (+ expulsão considerada)

```
se currentSat >= StayThreshold E há âncora:
    se (VO/VI) E há BE/BK no link atual E existe link LIMPO com capacidade:
        MIGRATE_CONSIDERATE_EVICT  → migra p/ esse link (deixa o atual aos BE/BK)
    senão:
        STAY_SATISFIED             → fica, sem avaliar mais nada.
```

**Expulsão considerada.** Um VO/VI **satisfeito** que partilha o link atual com **BE/BK** está a esfomeá-los por EDCA (regra: BE/BK nunca coexistem bem com VO/VI). Se existir um link **limpo** (sem BE/BK residente) com **capacidade** para toda a procura de alta-prioridade que lá ficaria, o VO/VI **migra para lá** — mesmo sem melhorar a sua própria satisfação — libertando o link atual para os BE/BK. Implementado por `FindConsiderateEvictTarget`, que usa `estimatedCapacity ≥ Σ(procura VO/VI)` (**capacidade medida**, *não* a projeção `MeetsOwnGoals`, que é pessimista por causa da penalidade de co-ocupação e classificaria mal o link-alvo). Migração **imediata** (sem dwell): a condição é estável e o `EnforceRouting` (binding) torna a decisão **efetiva** no físico — só por isso este mecanismo funciona agora (ver limitação #14).

- **Sem ping-pong / seguro**: só dispara para VO/VI com BE/BK no link atual E um alvo limpo com espaço. Depois de mover, o novo link não tem BE/BK → não volta a disparar. Se nenhum alvo limpo couber, fica em `STAY_SATISFIED` (preserva o caso legítimo em que o VO/VI **tem** de partilhar com o BE — `CAT2_PRIORITY_OVERRIDE`).

### Passo 3 — Cascata explícita de três categorias

| Categoria | Condição | Significado |
|-----------|----------|-------------|
| **1 — CAT1_CLEAN** | `MeetsOwnGoals` **e** `!WouldHarmResident` | Cumpro os meus objetivos sem prejudicar ninguém |
| **2 — CAT2_PRIORITY_OVERRIDE** | `MeetsOwnGoals` **mas** `WouldHarmResident` | Cumpro os meus objetivos à custa de outro AC — aceitável porque a prioridade EDCA é legítima |
| **3 — CAT3_BEST_EFFORT** | Não cumpro em lado nenhum | Último recurso: melhor pontuação possível |

Escolhe sempre a melhor categoria (1 > 2 > 3) e, dentro dela, o link com melhor `ComputeExpectedQosSatisfaction`.

- **`MeetsOwnGoals(ac, sta, link)`** → `ComputeExpectedQosSatisfaction(...) >= OwnGoalsThreshold` (0.90).
- **`WouldHarmResident(ac, sta, link)`** → procura, entre os residentes de **outros ACs** (os do mesmo AC são saltados — a co-occupancy trata deles), o de pior `currentSat`; se for `<= HarmThreshold` (0.70), considera que haveria dano.

### Passo 4 — Vetos de prioridade

Ver [Mecanismos de prevenção](#2-vetos-de-prioridade-bebk-nunca-partilham-link-com-vovi).

### Passo 5 — Histerese + debounce

```
se bestLink != currentLink:
    improvement = bestScore − currentSat
    se improvement > MigrationThreshold:
        → debounce (dwell): a intenção tem de persistir ≥ 1 s
    senão:
        STAY_HYSTERESIS  (limpa a intenção pendente)
senão:
    STAY_HYSTERESIS / STAY_BEST  (limpa a intenção pendente)
```

---

## Métricas por-STA — estimadas pelo próprio AP (sem feedback dos STAs)

A satisfação **medida** (`currentSat`) é estimada **só com informação do AP**, na sua própria
**camada de traffic-control** (queue-disc do AP), e alimentada ao scheduler via `FeedStaQos` a
cada 1 s (em `CalculateStats`, no `.cc`). **Não precisa de feedback dos STAs.** Isto remove a
antiga dependência dos sinks (ver o ponto #5 em [Limitações conhecidas](#limitações-conhecidas)).

Cada pacote é seguido do **topo da pilha do AP** (enqueue no traffic-control) até ser **confirmado
(ACK)** no MAC — captando toda a espera (fila TC **+** fila MAC + acesso ao canal) e os drops.

| Dimensão | Fonte (só-AP, downlink) | Como |
|---|---|---|
| **Throughput** | `Dequeue` do MAC (entregues) × `payloadSize` | goodput exato = `nEntregues × payload × 8 / dt` (o AP conhece o payload da sua app → sem overhead de cabeçalhos) |
| **Delay / Jitter** | `Enqueue` do TC → `Dequeue` do MAC (ACK) | por pacote, `t_ACK − t_TCenqueue` (o `uid` do pacote casa entre as duas camadas → 100% match) |
| **Loss** | `Enqueue`/`DropBeforeEnqueue` do TC + `Dequeue` do MAC | `1 − entregues / oferecidos`, `oferecidos = TC-enfileirados + drops pré-fila` (overlimit do TC + pré-enqueue do MAC) |

> **Validação vs sinks (all-BE, N=192).** Comparado com as medições end-to-end dos sinks (verdade):
> **loss** erro médio 0.0001; **delay** erro 0.19 ms (mesmo com os ~200 ms de espera na saturação
> do arranque); **throughput** exato após a correção de goodput; **jitter** mesma ordem. A flag
> `--decisionMetrics=ap|sink` alterna a fonte da decisão (`ap` por defeito); o CSV
> `metric_comparison` regista sempre ambas (sink vs AP) para auditoria.

> ### Nota histórica: porquê medir na camada TC (e não no MAC)?
>
> Medir só na **fila MAC** subestimava delay e loss **sob congestão**: no all-BE, 100% dos drops e
> ~90% da espera estão **acima** do MAC, na fila do traffic-control (overlimit). Medir no **topo**
> (TC-enqueue) até ao ACK capta tudo → bate com o sink. As métricas dos sinks foram a **referência**
> que validou esta estimativa-AP.

> ### Porquê métricas reais? (bug encontrado)
>
> Originalmente o scheduler usava métricas do próprio MAC:
> - O **PER** vinha de `1 − txSuccess/txAttempts`. O `txAttempts` contava **todos** os MPDUs agregados no início da TX, mas o `txSuccess` era alimentado pelo trace `AckedMpdu`, que captava apenas **~5%** dos MPDUs entregues → **PER falso de ~95%** e throughput medido a ~5% do real.
> - Com os pesos de loss+tp, isto **capava a satisfação do VO em ~0.70** (o teto dado por delay+jitter). Como `StayThreshold` é 0.75, **nada fixava por `STAY_SATISFIED`** — todos os ACs ficavam eternamente à mercê da cascata ruidosa. Foi a raiz de quase todas as instabilidades iniciais.
>
> ### Porquê a loss acumulada do FlowMonitor? (segundo bug)
>
> A primeira tentativa de corrigir a loss usou `1 − throughput/oferecido`. Isso **não é perda** — é o *défice de throughput*. Uma flutuação normal de 1 janela (o VI receber 144 em vez de 150 Mbps, por variação de enfileiramento) aparecia como "3.7% de loss"; com `maxPacketLoss = 0.01` para o VI, a sigmoide derrubava o `curSat` de 0.665 → 0.465 e disparava uma migração espúria.
>
> A loss **acumulada** do FlowMonitor não tem esse ruído: pacotes em voo na fronteira da janela são insignificantes face a todo o histórico, e o valor converge para a perda real (~0 num link saudável, elevada num congestionado).

---

## Mecanismos de prevenção

Cada um destes existe por causa de um bug **observado em simulação**. Estão documentados como *sintoma → causa → mecanismo*.

### 1. Bootstrap por prioridade + warmup (`kWarmupSamples = 2`)

- **Sintoma**: no cold-start (t≈2.5 s) o BE e o VO **trocavam de link ao mesmo tempo** — o BE fugia para o rápido, o VO para o lento — e ficavam presos na configuração errada.
- **Causa**: a **1.ª janela de medição está contaminada pelo ramp-up**. O VO media `curSat = 0.469` em vez do seu valor real (~0.87). Com todos a parecerem mal no link atual e o outro link (vazio) a projetar bem, todos migravam ao mesmo tempo.
- **Mecanismo**: até um fluxo ter **2 amostras** reais (`StaQos::samples >= 2`), a alocação é forçada por prioridade EDCA (VO/VI → melhor link; BE/BK → 2º melhor link em triband, pior link em dualband) e a cascata **nem corre**. Salta-se a janela contaminada; quando a cascata arranca, o `curSat` já reflete a realidade e o `STAY_SATISFIED` trava todos no sítio certo.
  - **Triband — porquê BE/BK no 2º melhor link (5 GHz), não no pior (2.4 GHz)**: o link de 2.4 GHz é o de menor capacidade; arrancar aí os ACs de baixa prioridade sub-utilizava logo à partida o 5 GHz e sobrecarregava o 2.4 GHz. Com o bootstrap em `m_linksByRank[1]` (=5 GHz), o BE/BK partem de um link com muito mais capacidade e o 2.4 GHz fica disponível para o balanceador/cascata o preencher conforme a carga.

### 2. Vetos de prioridade (BE/BK nunca partilham link com VO/VI)

> **Estes vetos não são um workaround temporário — são a modelação deliberada de uma física que o modelo analítico não consegue exprimir.** A fome do EDCA é um fenómeno de **acesso ao canal**, não de largura de banda: um BE que compete com um VO no mesmo link **não obtém oportunidades de transmissão** (o VO, com AIFSN=2/CWmin=3, reapanha o canal quase imediatamente; o AIFS+backoff do BE quase nunca expira), mesmo que haja airtime livre no agregado. Um BE que colapsa para 4.95 Mbps num link com ~57% de airtime "livre" prova-o. Nenhum modelo baseado em **capacidade ou airtime** (que é o que os motores 2–5 medem) captaria isto — só um modelo de saturação EDCA tipo Bianchi, que está fora do âmbito. Os vetos codificam essa física diretamente e são **permanentes**.

A regra física: **BE/BK num link com VO/VI nunca têm oportunidade de transmissão** (EDCA). Ambos os lados são vetados, mas com rigor diferente.

**(a) Veto anti-downgrade VO/VI** — um VO/VI no link rápido não desce para o lento se:
- for o **único do seu AC** no rápido (`CountCoResidents == 0`), **ou**
- o link de destino tiver um **BE/BK residente** (`acIdx < AC_VI`),

**exceto** se estiver genuinamente esfomeado (`currentSat < kStarvationFloor = 0.60`) — aí cede e pode descer, porque o VO tem prioridade sobre o BE.

- **Sintoma**: um dos 2 VOs derivava para o link lento e ficava lá, esfomeando o BE que lá estava.
- **Causa**: os dois links projetavam praticamente **igual** para o VO (`0.963223` vs `0.963183` — diferença de 0.00004!). O VO é dominado por delay (peso tp = 0.05), logo a capacidade é quase irrelevante e a decisão virava uma moeda ao ar.
- **Nota**: o sinal de fome usa o `currentSat` **medido**, não a projeção. A projeção foi testada e falhava — colapsava no 6 GHz e abria a exceção por ruído.

**(b) Veto simétrico BE/BK** — um BE/BK **nunca** migra para um link onde residam VO/VI. **Sem exceção**.

- **Sintoma**: no par 2.4+6 GHz, o BE estava perfeitamente bem no 2.4 GHz (`tp = 150`, `delay = 0.66 ms`) e mesmo assim migrou para o 6 GHz — e colapsou: **`tp` 150 → 4.95 Mbps, `delay` 647 ms, `loss` 22%**.
- **Causa**: a projeção deu **0.9696** ao BE no 6 GHz. O modelo vê a capacidade bruta livre ("~700 Mbps disponíveis") e **ignora que o BE será esfomeado por EDCA** pelos VO/VI residentes. No 5 GHz a capacidade bruta é menor, a projeção deu 0.651 e o BE não se mexeu — **foi sorte, não lógica**.
- **Porquê sem exceção**: para um BE/BK, ir para o link dos VO/VI **nunca ajuda** — seria mais esfomeado, não menos.

### 3. Debounce de migração (`kMigrationDwellSec = 1.0`)

- **Sintoma**: a meio de uma simulação estável (t≈11 s), o VI saltava para o link lento e perturbava o BE, recuperando 1–2 s depois.
- **Causa**: **variância normal de partilha de airtime EDCA**. Numa janela de 1 s os VOs apanharam ligeiramente mais airtime e o VI mediu 144 em vez de 150 Mbps (os VOs, no mesmo link, não caíram — prova de que não era um evento físico do link). A migração daí resultante **auto-perturbava-se** e sustentava o estado mau na janela seguinte.
- **Mecanismo**: quando a cascata quer migrar, a intenção é **registada** (`MIGRATE_PENDING`) mas não executada. Só migra se a mesma intenção (mesmo link alvo) persistir ≥ 1 s — ou seja, se sobreviver a pelo menos uma **nova janela de medição**. Se a intenção desaparecer, é limpa. Isto quebra o ciclo: ao não migrar na janela ruidosa, a seguinte fica limpa e a intenção evapora-se.

### 4. Exclusão de beacons (`IsRoutableSta`)

Este é o mecanismo mais subtil, e vale a pena perceber em detalhe.

- **Sintoma**: no cenário all-BE, os 4 STAs ficavam **presos no link lento** (`delay = 217 ms`, `loss = 58%`, `tp = 62 Mbps`) com o link rápido **vazio**. As métricas estavam corretas (`curSat = 0.157`) e o `improvement` era 0.45 (muito acima do threshold) — os BEs **queriam** migrar, mas alternavam eternamente entre `MIGRATE_PENDING` e `STAY_HYSTERESIS`, com **zero migrações**.

- **A pista**: os instantes dos `MIGRATE_PENDING` estavam espaçados **~102.4 ms** — exatamente o **intervalo de beacon do ns-3**.

- **Causa**:
  1. Beacons são frames **broadcast**. Em `GetLinkIds`, qualquer broadcast é chaveado como `(ff:ff:ff:ff:ff:ff, AC)` — com AC de alta prioridade.
  2. Como cada beacon tem só **1 link elegível**, entra pelo *early return* de `DecideLinkMigration`, que **grava em `m_lastSelectedLink` mas não escreve no CSV** — daí só se ver AC=0 no ficheiro, apesar de existir uma entrada fantasma de alta prioridade.
  3. Essa entrada saltava de link a cada beacon. Quando apontava para o link alvo, o **veto simétrico BE/BK** via "há um VO ali" → `bestLink = currentLink` → `STAY_HYSTERESIS`.
  4. O ramo `STAY_HYSTERESIS` **apaga o `m_pendingMigration`** → o dwell reiniciava. Com o beacon a repetir **10×/segundo** e o dwell a precisar de 1 s, **nunca completava**.

- **Mecanismo**:
  ```cpp
  static bool IsRoutableSta(const Mac48Address& a) { return !a.IsGroup(); }
  ```
  `IsGroup()` cobre broadcast e multicast. O filtro `if (!IsRoutableSta(key.first)) continue;` é aplicado nos **5 scans de residência**:
  1. penalidade altruísta (`ComputeExpectedQosSatisfaction`)
  2. `CountCoResidents` (co-occupancy)
  3. `WouldHarmResident`
  4. veto anti-downgrade VO/VI (`wouldStarveLowPrio`)
  5. veto simétrico BE/BK (`highPrioOnTarget`) ← o que corrigia o bug

  O **encaminhamento** dos beacons não muda — apenas deixam de contar como "residentes" de um link.

### 5. Balanceamento de links ociosos (`RebalanceIdleLinks`)

> **Nota de filosofia**: este mecanismo adiciona um **segundo objetivo** — até aqui o scheduler só *satisfazia QoS*; agora também *aproveita links ociosos*. É uma escolha deliberada.

- **Sintoma**: em cenários homogéneos (all-BE) ou onde vários fluxos cabem no melhor link (2VO+2VI), **todos os STAs acabam no mesmo link e o outro fica parado**. Espalhar a carga melhora as métricas (menos contenção, menos delay).
- **Causa**: depois de a `UtilityFunction` dar ~0.99 a um fluxo bem servido, o `STAY_SATISFIED` trava-o e ele deixa de explorar o link vazio. O balanceamento tem de ser **explícito**.
- **Mecanismo** (em `UpdatePeriodicMetrics`, um movimento por janela):
  1. **Gate**: **todos** os fluxos routáveis satisfeitos (`curSat ≥ StayThreshold`). Se algum não está, o cascade está a resgatá-lo → não se balanceia.
  2. **Alvo homogéneo**: só se move para um link **vazio** ou que **só contenha fluxos da mesma AC** do candidato.
  3. Move o fluxo de **menor prioridade EDCA** (não-VO — voz pinada) de um link com ≥2 fluxos, **e apenas se reduzir o desequilíbrio** (`flows(source) > flows(target)+1`).
  4. **Teste de folga**: se o alvo estiver **vazio**, o 1º fluxo entra sempre (fica com o link todo — um link ocioso não tem capacidade medível). Se já tiver fluxos, exige `folga medida ≥ targetThroughput`.

  **A regra do alvo homogéneo é o núcleo de segurança** — substitui um "veto bidirecional" por algo mais forte: **o balanceador nunca mistura ACs**. Consequências:
  - Nunca cria BE/BK com VO/VI (em nenhum sentido). *(Foi o bug de uma versão anterior: um VI era movido para cima de um BE. Aqui não acontece — o link do BE não é vazio nem "só-VI".)*
  - Um grupo novo **só arranca num link vazio**; depois enche-se com a mesma AC enquanto houver folga → **iterativo**, o "quantos migram" emerge da capacidade medida (ex.: 2VO+2VI → 2 VIs no 5GHz, mas só 1 no 2.4GHz).
  - Impede a **consolidação inversa** (mover um VI de volta para o link dos VOs) → sem ping-pong.

  **Anti-oscilação**: cooldown global (`kRebalanceCooldownSec = 2 s`) + por-fluxo (`kFlowBalanceCooldownSec = 10 s`). Se um movimento degradar um fluxo, o cascade puxa-o de volta e o cooldown impede o re-empurrão.

  > **Importante — a decisão do balanceador é *consultiva*, não *vinculativa*.** Como o `RebalanceIdleLinks` só altera o `m_lastSelectedLink` (que enviesa o pedido de acesso ao canal no enqueue), o resultado observado no tráfego físico pode diferir da atribuição decidida. Ver [Conselho vs. execução](#conselho-enqueue-vs-execução-dequeue-porque-o-all-be-usa-os-dois-links).

---

## Conselho (enqueue) vs. execução (dequeue): porque o all-BE usa os dois links

Esta secção explica um comportamento que **surpreende**: apesar de o scheduler decidir **um link por (STA, AC)**, no cenário **all-BE** cada STA acaba a transmitir nos **dois** links em simultâneo (~28% no 2.4GHz / ~72% no 5GHz em 2+5; ~50/50 em 5+6). **Isto não é uma decisão do scheduler** — é uma consequência da arquitetura do ns-3 por baixo dele.

### A decisão do scheduler só actua no *enqueue*

`GetLinkIds(ac, mpdu)` é chamado quando o MAC tem um MPDU pronto a **enfileirar** (`Txop::Queue`). Devolve **1 link** (via `DecideLinkMigration`), mas esse valor só serve para decidir **em que link se pede acesso ao canal**. **Não fixa o frame a esse link.**

### O *dequeue* é feito pelo delegate Fcfs, que ignora a decisão

A parte crítica está em quem serve a fila no momento da transmissão. Ao fazer `m_delegate->SetWifiMac(mac)`, é o **delegate `FcfsWifiQueueScheduler`** que se regista como scheduler de cada `WifiMacQueue` (`WifiMacQueueSchedulerImpl::SetWifiMac` → `queue->SetScheduler(this)`). O `QosWeightedMloScheduler` estende `WifiMacQueueScheduler` **diretamente** e não faz esse wiring. Logo:

- **`WifiMacQueue::m_scheduler == delegate` (Fcfs)**, não o nosso scheduler.
- No caminho de transmissão, `Peek`/`PeekFirstAvailable(linkId)` chamam `GetNext` do **delegate** — **Fcfs puro** sobre a fila partilhada da AC, que serve **qualquer** STA a **qualquer** link com acesso. O nosso override de `GetNext` **nunca é chamado pela fila**, e o `m_lastSelectedLink` **nunca** é consultado no dequeue.

### Porque isto espalha o tráfego

O acesso ao canal é **por-AC** (há um `Txop` por AC), não por-STA. No all-BE, o balanceador aconselha 1 BE ao 2.4GHz e 3 ao 5GHz → **ambos** os links passam a pedir acesso para a AC BE → **ambos** os `Txop`-BE ganham acesso → cada um, ao transmitir, puxa da fila BE partilhada **o próximo frame de qualquer STA** (Fcfs). Resultado: os frames de cada STA saem pelos dois links.

### Evidência medida (prova *bound-vs-real*)

Instrumentámos temporariamente cada frame **confirmado (ACKed)** a comparar o link **decidido** (`m_lastSelectedLink`) com o link **real** (lido do endereço do AP em `Addr2`). No **all-BE 2+5**, o scheduler decidiu **exatamente o 1-3 pretendido — STA0→2.4GHz, STA1/2/3→5GHz** — mas fisicamente:

| STA | decidido | 2.4GHz (real) | 5GHz (real) |
|-----|----------|---------------|-------------|
| 0 | 2.4GHz | 829 | **732** (≠ decidido) |
| 1 | 5GHz | **807** (≠ decidido) | 752 |
| 2 | 5GHz | **836** (≠ decidido) | 807 |
| 3 | 5GHz | **804** (≠ decidido) | 791 |

Ou seja: **intenção = 1-3 limpo; execução = ~50/50**. O STA0, decidido para o 2.4GHz, enviou quase metade dos frames no 5GHz; os STA1/2/3, decididos para o 5GHz, enviaram quase metade no 2.4GHz.

### Porque os cenários de prioridade (2VO+2VI, …) *parecem* limpos

O espalhamento só acontece quando fluxos da **mesma AC** estão separados por links diferentes. Nos cenários de prioridade, cada AC fica **junta** num link (os 2 VO juntos no rápido, os 2 VI juntos no 5GHz) → o outro link **nunca** contende por essa AC → praticamente **sem espalhamento**. O 73/27 residual dos VI é apenas o **transiente de bootstrap** (o VI arranca no link rápido e assenta no 5GHz), não partilha em regime.

Da mesma forma, o **bootstrap parece "vinculativo"** (link rápido a 0% para os BE) só porque **todos** os fluxos aconselham o **mesmo** link — o outro nunca pede acesso a essa AC. A "regra" vem da ausência de contenção, não de o binding ser aplicado.

### O espalhamento é uma interação natural do MLO em modo STR

Antes de o ver como um "defeito", vale a pena reconhecer que este espalhamento é um **comportamento natural e importante do MLO em modo STR** (Simultaneous Transmit and Receive). Com **filas partilhadas por-AC** e **CSMA independente por link**, qualquer link livre pode servir o frame *head-of-line* dessa AC — distribuindo naturalmente o tráfego de um mesmo fluxo pelos vários links disponíveis. É, na prática, **agregação de links legítima**: satisfaz todos os STAs e aproveita a capacidade agregada. O contraste **VO (100% num link) vs VI (50/50)** explica-se só por isto: o VO aconselha sempre o mesmo link → o outro nunca ativa o loop de acesso à AC VO; os VI ficaram separados por links → **ambos** os `Txop`-VI ativos → o Fcfs alimenta os dois.

### Como se tornou vinculativo (implementado — binding por queue-blocking)

Para os cenários em que **queremos** que a decisão seja uma **ordem** (sem espalhamento), o scheduler amarra fisicamente cada fluxo ao link decidido através de **máscaras de bloqueio no delegate** — o único mecanismo que o `GetNext` do delegate respeita:

- **`EnforceRouting(ac, dest, chosenLink)`** (`mlo-qos-weighted-scheduler.h`), chamado no fim de `GetLinkIds`: para o (STA, AC), faz `m_delegate->BlockQueues(WIFI_SCHEDULER_ROUTING, …)` em **todos os links exceto** o escolhido e `UnblockQueues` no escolhido. A fila desse (STA, AC) fica com um bit de máscara ativo nos links errados → `GetNext(ac, linkId)` **salta-a** aí → o frame só é servido no link decidido. Sem espalhamento.
- Usa um **reason novo, `WIFI_SCHEDULER_ROUTING`**, adicionado ao enum `WifiQueueBlockedReason` do **core** do ns-3 (todas as razões existentes são geridas pela MAC e seriam limpas por ela). Ver a secção [Alterações ao core do ns-3](#alterações-ao-core-do-ns-3).
- Detalhes que fazem isto funcionar: o container de QoS data unicast é chaveado por `(WIFI_QOSDATA_QUEUE, UNICAST, RA=endereço-MLD-do-STA, tid)` — coincide com o `dest` que o scheduler já usa; o `BlockQueues` é chamado com `txAddress = GetMac()->GetAddress()` (endereço MLD do AP), **condição para o `InitQueueInfo` popular a máscara por-link**; e bloqueiam-se os 2 TIDs da AC.
- Para não congelar a migração, o `GetLinkIds` apura os links elegíveis **ignorando o próprio `WIFI_SCHEDULER_ROUTING`** — a decisão continua a "ver" todos os links; o binding só restringe o **dequeue físico**, não a decisão.
- `m_boundLink[(STA,AC)]` guarda o link amarrado; só se re-bloqueia quando a decisão **muda** (evita trabalho por-frame). Numa migração, reamarra no mesmo passo (bloqueia o antigo, desbloqueia o novo).

> **Trabalho futuro — reexplorar o espalhamento.** O binding elimina o espalhamento por defeito, mas o espalhamento STR é um mecanismo **desejável** a explorar (aproveitamento de capacidade agregada por-fluxo). Fica como trabalho futuro poder **reativá-lo seletivamente** (ex.: por AC, ou quando o link-alvo está saturado) — existe um plano próprio para essa interação.

---

## Tabela de todas as variáveis configuráveis

### Atributos ns-3 (via `SetAttribute`)

| Atributo | Variável C++ | Valor efetivo | O que controla |
|----------|--------------|-------------|------------------|
| `StayThreshold` | `m_stayThreshold` | **0.75** | Satisfação mínima para nem avaliar alternativas. Mais alto → reavalia mais; mais baixo → mais "preguiçoso". |
| `MigrationThreshold` | `m_migrationThreshold` | **0.25** | Ganho mínimo para justificar migração. Mais alto → mais estável; mais baixo → mais reativo, risco de oscilação. |
| `MetricsInterval` | `m_metricsIntervalSec` | **0.5 s** | Frequência de recálculo dos Motores 1–4. |
| `OwnGoalsThreshold` | `m_ownGoalsThreshold` | **0.90** | Score mínimo para um link contar como "cumpro os meus objetivos" (Cat1/Cat2). |
| `HarmThreshold` | `m_harmThreshold` | **0.70** | Satisfação abaixo da qual um residente é considerado "prejudicado". |

> Os valores de `StayThreshold` / `MigrationThreshold` são **impostos pelo script** (`g_stayThreshold` / `g_migrationThreshold` no `.cc`, também expostos na CLI via `--stayThreshold` / `--migrationThreshold`). Os defaults do header são iguais (0.75 / 0.25).

### Constantes hardcoded (não são atributos ns-3)

| Constante | Valor | Local | Significado | Justificação |
|-----------|-------|-------|-------------|--------------|
| `kWarmupSamples` | 2 | `UpdatePeriodicMetrics` | Janelas de medição antes de libertar o bootstrap | **(P)** A 1ª janela vem contaminada pelo ramp-up; é preciso saltá-la e usar a 2ª (limpa). 2 é o **mínimo** para haver uma janela limpa antes de libertar — 1 não saltaria nada, mais alto atrasaria a convergência sem ganho. |
| `kMigrationDwellSec` | 1.0 s | `DecideLinkMigration` | Persistência exigida a uma intenção de migração | **(P)** Igual à cadência do `FeedStaQos` (1 s). Garante que a intenção sobrevive a **≥1 janela de medição nova** antes de executar → filtra blips de 1 janela. Menor não veria medição fresca; maior atrasaria migrações genuínas. |
| `kStarvationFloor` | 0.60 | `DecideLinkMigration` | `currentSat` abaixo do qual um VO/VI é "esfomeado" (abre a exceção do veto) | **(P/E)** Fronteiras lógicas: **< `StayThreshold` (0.75)** (senão um fluxo satisfeito abriria o veto) e **> degradação transitória**. 0.60 fica na banda "claramente mal, mas não catastrófico"; o dígito exato não é crítico dentro dela. |
| `kRebalanceCooldownSec` | 2.0 s | `RebalanceIdleLinks` | Intervalo mínimo entre movimentos de balanceamento | **(E)** Deixa as métricas estabilizar entre movimentos. Conservador; qualquer valor de alguns segundos serve. |
| `kFlowBalanceCooldownSec` | 10.0 s | `RebalanceIdleLinks` | Tempo mínimo antes de re-mexer no mesmo fluxo | **(E)** Anti-ping-pong: se o cascade puxar um fluxo de volta, não o re-empurramos logo. Valor folgado, não crítico. |
| `perAlpha` | 0.3 | `UpdatePeriodicMetrics` | Suavização EWMA do PER proxy | **(E)** Fator EWMA convencional: 30% amostra nova / 70% histórico. Equilibra reatividade e estabilidade. Mais alto = mais ruidoso; mais baixo = mais lento a reagir. Não crítico. |
| Blend final | 0.65 / 0.25 / 0.10 | `ComputeExpectedQosSatisfaction` | `baseSat` / `capabilityScore` / `perPenalty` | **(E)** Afinado (era 0.8/0.1/0.1). A satisfação QoS **domina** (0.65, sinal primário); a capacidade do link é um **secundário forte** (0.25, subido para dar mais peso a links capazes/com folga); o PER é um empurrão menor (0.10). **Soma 1.** Importa a *proporção*, não os dígitos. |
| Limiar "esfomeado" (altruísta A) | 0.45 | idem | Residente tratado como esfomeado | **(P/E)** Abaixo do ponto médio 0.5 → o residente está claramente na **metade insatisfeita**. Marca "este AC já sofre" para disparar a penalidade reativa. |
| Penalidade base (altruísta A) | 0.6 | idem | `(0.6 − otherSat)`, máx. 0.6 | **(E)** Define a magnitude: penalidade máx. 0.6 (residente a 0), mín. 0.15 (residente a 0.45). Forte para afastar candidatos de um residente esfomeado, sem aniquilar o score (em [0,1]). |
| Limiar headroom (altruísta B) | 0.75 | idem | Satisfação abaixo da qual a preventiva pode disparar | **(P/E)** Coincide com o antigo `StayThreshold`: **acima de 0.75** o residente está confortável e um recém-chegado não é ameaça; **abaixo**, é. Fronteira "confortável vs apertado". |
| Teto da preventiva (B) | 0.5 | idem | Máximo que (B) subtrai | **(P)** Salvaguarda: a preventiva nunca corta mais de **metade** do score → sozinha não decide uma migração de forma catastrófica. |
| Carga proxy em cold-start | 0.15 | idem | `NormalisedLoad` assumida sem medição | **(E)** Sem medição, assume-se ~15% de carga para a preventiva **não ser nula só por falta de dados**. Pequeno e conservador. |
| Fator pressão → delay | 0.3 | idem | `delay × (1 + pressure×0.3)` | **(P/E)** O delay é **diretamente** agravado pela contenção (mais competição → mais espera na fila) → fator maior. Subido de 0.2 → 0.3 empiricamente. |
| Fator pressão → jitter | 0.2 | idem | `jitter × (1 + pressure×0.2)` | **(P/E)** O jitter (variação do delay) sofre com a contenção mas de forma **menos direta** que o delay médio → fator **menor** (0.2 < 0.3). |
| `m_edcaWeights[4][4]` | ver Motor 4 | construtor | Matriz de pressão de contenção | **(P/E)** Codifica a assimetria real do EDCA (AIFS/CW): VO (AIFSN=2/CWmin=3) sente pouco os de baixo; BE/BK (AIFS/CW longos) sentem muito os de cima. A **ordem de grandeza** (VO/VI ≫ BK/BE na coluna) é física; os valores exatos (8.0, 6.0…) são afinados. |

> **P = princípio** (o valor deriva de algo concreto — cadência, fronteira lógica, soma = 1; defensável com rigor) · **E = empírico** (afinado por observação; o que importa é a *banda*, não o dígito). A distinção é deliberada: um scheduler heurístico assume que várias constantes são afinadas dentro de uma gama razoável, e o comportamento é robusto a pequenas variações delas.

Para alterar qualquer uma destas é preciso editar o `.h` e recompilar.

### Pesos e objetivos por AC

| Estrutura | Campos | Como configurar |
|-----------|--------|------------------|
| `AcGoals` | `targetThroughputMbps`, `maxDelayMs`, `maxJitterMs`, `maxPacketLoss` | `SetGoals(ac, tp, delay, jitter, loss)` — **não é chamado atualmente**; valem os defaults do construtor |
| `AcWeights` | `delayWeight`, `jitterWeight`, `lossWeight`, `throughputWeight` | `SetWeights(ac, delay, jitter, loss, tp)` — chamado pelo `.cc`; cada peso tem CLI própria (ex: `--voDelayWeight`) |

---

## APIs de alimentação externa

O scheduler não lê diretamente do PHY/MAC — o script de simulação liga *traces* e chama:

| Função | Quando chamar | Alimenta |
|--------|----------------|----------|
| `FeedStaQos(sta, ac, tp, delay, jitter, loss)` | periodicamente (1 s, em `CalculateStats`) | **`m_staQos`** — as métricas que definem `currentSat` (por defeito estimadas pelo AP na camada TC; sinks só com `--decisionMetrics=sink`) |
| `FeedLinkPhyState(linkId, txVector, per)` | `PhyTxPsduBegin` | EWMA de data rate e PER (Motor 2) |
| `FeedLinkTxAttempt(linkId, ac, nFrames)` | `PhyTxPsduBegin` | `txAttempts` (denominador do PER proxy) |
| `FeedLinkDrop(linkId, ac)` | drops de fila/MAC | `dropFrames` (numerador do PER proxy) |
| `FeedLinkMetrics(linkId, ac, bytes, frames, drops)` | `AckedMpdu` | `txBytes` (diagnóstico) e `dropFrames`. **`frames` é ignorado** (ver nota) |
| `FeedPacketTransmitted(linkId, ac, bytes, airtime, peer, tid)` | `PhyTxPsduBegin` (SU) | Motor 3 (throughput/airtime/flows) |
| `FeedLinkTxStart` / `FeedLinkTxEnd` | `PhyTxBegin` / `PhyTxEnd` | Tempo ocupado (`channelUtilization`, `freeAirtime`) |

> **Nota sobre `FeedLinkMetrics`**: o parâmetro `txFrames` deixou de ser usado. Alimentava o antigo `txSuccess` do PER, que subcontava ~5% dos MPDUs. O parâmetro mantém-se apenas para não quebrar a assinatura pública.

---

## Logging e diagnóstico

`EnableDecisionCsv(filename, nodeContext)` gera um CSV com uma linha por decisão:

```
Timestamp, NodeContext, AC, SelectedLink, Decision, CurrentSat, ExpectedSat,
CompetitionPressure, EffectiveCapMbps, CapabilityScore, AvgDelayMs, AvgJitterMs,
PER, ChannelUtil, Throughput, ProjCurrentLink
```

As colunas `AvgDelayMs`, `AvgJitterMs`, `PER` e `Throughput` mostram as **métricas reais por-STA** (de `m_staQos`) quando disponíveis, com fallback aos proxies por-link.

A coluna **`ProjCurrentLink`** é a projeção (`ComputeExpectedQosSatisfaction`) do link **onde o fluxo está**. Serve de teste de consistência interna: deve aproximar-se de `CurrentSat` (a satisfação medida no mesmo link). Se divergirem muito, a projeção está errada — e é a mesma projeção que julga os *outros* links. É a melhor métrica para avaliar a correção da capacidade (limitação #1).

Valores possíveis de `Decision`:

| Decisão | Significado |
|---|---|
| `BOOTSTRAP_PRIORITY` | Alocação forçada por prioridade (ainda em warmup) |
| `STAY_SATISFIED` | `currentSat >= StayThreshold` — nem avaliou alternativas |
| `MIGRATE_CONSIDERATE_EVICT` | VO/VI satisfeito saiu de um link partilhado com BE/BK para um link limpo com capacidade (ver [Passo 2](#passo-2--stay_satisfied--expulsão-considerada)) |
| `STAY_HYSTERESIS` | Avaliou, mas o ganho não compensa (ou o melhor link é o atual) |
| `STAY_BEST` | Ficou no melhor link, sem âncora prévia |
| `MIGRATE_PENDING` | **Quer** migrar; a aguardar confirmação do dwell (1 s) |
| `MIGRATE_CAT1_CLEAN` | Migrou: cumpre objetivos sem prejudicar ninguém |
| `MIGRATE_CAT2_PRIORITY_OVERRIDE` | Migrou: cumpre objetivos, mas prejudica um residente |
| `MIGRATE_CAT3_BEST_EFFORT` | Migrou: não cumpre em lado nenhum, foi para o menos mau |

> Um `MIGRATE_PENDING` que nunca é seguido de um `MIGRATE_*` significa que o dwell filtrou ruído — comportamento **esperado**.

`PrintFinalScores()` imprime, no fim, a pontuação projetada e a satisfação atual de cada fluxo em cada link.

---

## Limitações conhecidas

Análise honesta do estado atual. Nada aqui impede os cenários testados de convergir, mas são fraquezas estruturais reais.

1. **A projeção mede capacidade/airtime, não *acesso* — e por isso não modela a fome do EDCA.** Este é o limite de fundo: os motores 2–5 estimam *quanto do meio está ocupado*, não *quem consegue aceder ao canal*. A fome do BE junto a VO/VI (colapso para 4.95 Mbps com 57% de airtime livre) é um fenómeno de acesso que **nenhum modelo de capacidade capta** — só um modelo de saturação EDCA tipo Bianchi (fora do âmbito).
   > **O que impede o colapso do BE é o veto, não a capacidade.** Historicamente (antes do veto simétrico existir), a projeção do BE dava ~0.97 num link com VO/VI e o BE migrava para lá e colapsava. Hoje o **veto simétrico** impede o BE/BK de migrar para um link com VO/VI **independentemente de qualquer número de capacidade** — é a única representação viável da fome de acesso e é **permanente** (ver §2 dos mecanismos).
   > **O que a correção da capacidade faz (separado do acima)**: torna a projeção honesta nas *outras* decisões (cascata Cat1/2/3, `MeetsOwnGoals`, co-occupancy), que **não** têm um veto a protegê-las. O `estimatedCapacityMbps` deixou de ser a taxa PHY nominal (que **sobrestimava** ~5×: 3722 Mbps) e passa a ser **medido** (subestimação conservadora, ~655–982 conforme a carga — ver Motor 2). O `NormalisedLoad` passou de "mistura entre ACs" (somava sempre 1) para "utilização real". A projeção do link atual convergiu com a satisfação medida para VO/VI (erro ~0.11, ver coluna `ProjCurrentLink` do CSV).

2. **Assimetria medido-vs-projetado + `effCap` mede *o que sobra*, não *o que eu receberia*.** Dois defeitos da projeção que coexistem:
   - *Assimetria*: o `currentSat` (link atual) vem de métricas reais por-STA; a projeção dos *outros* links usa proxies por-(link,AC). Comparar os dois em `improvement = bestScore − currentSat` é comparar grandezas diferentes.
   - *effCap residual*: a projeção pergunta *"quanto sobra aqui?"* em vez de *"quanto é que EU receberia aqui?"*. Um fluxo **bem servido**, que usa tudo o que precisa, vê "nada a sobrar" no seu próprio link e projeta-se mal lá.
   - *capacidade subestimada*: `estimatedCapacity` é o rate efetivo à carga atual (limitado pela agregação A-MPDU), que **subestima o máximo saturado** (~655–982 medidos vs ~1600 reais no 6 GHz — ver Motor 2). É conservador (nunca sobrestima), logo a projeção de "quanto caberia aqui" é **pessimista** — o que é seguro, mas significa que o scheduler acha os links mais cheios do que estão.
   > **Impacto medido — real mas inofensivo**: a projeção do BE no seu próprio link diverge da satisfação medida (projetado **0.46** vs medido **0.99**) em links de **baixa capacidade** (2.4GHz, onde o BE consome quase tudo); no 5GHz, com folga, converge (erro 0.07). Não causa decisão errada porque o `STAY_SATISFIED` (0.99) dispara sempre e a projeção má nunca chega a ser usada. Corrigi-lo exigiria um modelo de partilha de airtime — não vale o esforço.

3. **O `avgQueueDelayMs` proxy (por-link) é ≈ 0 mesmo sob congestão.** Só conta pacotes que chegam ao dequeue; os que ficam presos ou são dropados por fila cheia nunca entram na estatística. Só o caminho per-STA foi corrigido — o proxy que a projeção usa continua cego à congestão.

4. **~~`UtilityFunction` dá 0.5 no alvo~~ — CORRIGIDO e VALIDADO.** *Problema original*: para throughput, cumprir exatamente o alvo dava só 0.5 (era preciso ~2× o alvo para saturar), o que fazia VO/VI estabilizarem em ~0.69–0.75, colados ao `StayThreshold`, dependentes dos vetos em vez do `STAY_SATISFIED`. *Correção*: (a) no ramo *higher-is-better* a inflexão passou para 50% do alvo (`1/(1+e^{10(0.5−ratio)})`) → cumprir o alvo ≈ 0.99; (b) as metas de jitter de VO/VI (`maxJitterMs`) subiram de 0.1 ms (inatingível) para 5/10 ms. *Resultado medido*: VO/VI/BE bem servidos passaram a ~**0.99** e o `STAY_SATISFIED` voltou a dominar (zero migrações no steady state). O `StayThreshold=0.75` **não** precisou de mudar — com os satisfeitos a ~0.99, continua a separar bem servido de esfomeado.

5. **Métricas estimadas pelo próprio AP** *(RESOLVIDO — antes era uma limitação)*. O scheduler já **não depende do feedback dos STAs**: estima as 4 métricas na sua própria **camada de traffic-control** (`Enqueue`/`Dequeue`/`DropBeforeEnqueue` do queue-disc do AP), seguindo cada pacote do TC-enqueue até ao ACK no MAC. Validado quase na perfeição vs os sinks (ver [Métricas por-STA](#métricas-por-sta--estimadas-pelo-próprio-ap-sem-feedback-dos-stas)): loss erro 0.0001, delay erro 0.19 ms, throughput exato. Como usa apenas informação que um AP **real também tem** (a sua própria pilha), é **transponível para hardware**. Os sinks ficam só como referência de validação (`--decisionMetrics=sink` para A/B; CSV `metric_comparison`). *Caveat residual:* o jitter estimado fica ligeiramente acima do real (mesma ordem de grandeza).

6. **A loss acumulada é estável mas lenta.** Uma degradação súbita demora a refletir-se, e depois de um fluxo melhorar o histórico mau só se dilui gradualmente. Foi uma troca deliberada (estabilidade > reatividade).

7. **O veto BE/BK é absoluto e não olha para o airtime real do link dos VO/VI.** Os dois vetos não são simétricos no rigor: o do VO/VI tem exceção de fome (`currentSat < 0.60`); o do BE/BK **bloqueia sempre**. A regra assume que os VO/VI **saturam** o seu link — verdade nos cenários testados. Mas **nunca verifica se há airtime livre**. Caso onde falharia: 1 VO sozinho num 6 GHz a usar 150 de ~1000 Mbps (~85% livre) e 4 BEs entupidos no 2.4 GHz com 58% de perdas — fisicamente os BEs beneficiariam de partilhar o 6 GHz, mas ficariam presos sem escape. É uma **heurística de prioridade, não uma decisão baseada em capacidade**.

8. **Muitas constantes críticas estão hardcoded** e não expostas como atributos ns-3: blend `0.65/0.25/0.10`, `kMigrationDwellSec`, `kWarmupSamples`, `kStarvationFloor`, `perAlpha`, `m_edcaWeights`. Afinar exige recompilar.

9. **`WouldHarmResident` continua reativo.** Usa a satisfação *atual* dos residentes, não projeta o impacto da chegada do candidato. Deteta "alguém já está mal", não "vou piorar alguém que está bem". Os vetos de prioridade cobrem os casos críticos; a penalidade preventiva (B) do Motor 5 cobre parcialmente o resto.

10. **O dwell de 1 s está acoplado ao intervalo de medição.** Foi escolhido para garantir que uma intenção sobrevive a ≥ 1 janela nova. Se o `MetricsInterval` ou a cadência do `FeedStaQos` (1 s) mudarem, o dwell tem de ser reavaliado.

11. **Balanceamento — voz pinada.** O `RebalanceIdleLinks` **nunca move VO** (voz fica no melhor link, por ser a mais sensível a delay). Consequência: num cenário all-VO os VOs não se espalham por um link vazio. Escolha conservadora; relaxá-la é trivial (remover o guard `acIdx == AC_VO`).

12. **Balanceamento — link vazio recebe o 1º fluxo incondicionalmente.** A capacidade de um link ocioso **não é medível** (`estimatedCapacity = goodput/airtime` precisa de tráfego → fica 0 se nunca foi usado, ou estagnada se o foi cedo). Por isso o 1º fluxo entra sem teste de capacidade — fica com o link todo — e confia-se na **medição da janela seguinte** (para decidir sobre um 2º) e na **auto-correção do cascade** (que o puxa de volta se o link for mesmo fraco). Sem isto, o teste `folga ≥ target` bloqueava sempre a 1ª migração.

13. **Cobertura de teste.** Validado em cenários de **4 STAs** (2VO+1VI+1BE, 2VO+2VI, 1VO+1VI+1BE+1BK, all-BE) × **3 pares de frequências** (2.4+5, 2.4+6, 5+6), com tráfego UDP saturante de 150 Mbps por STA e STAs estáticos. Fora deste envelope — mais links, mais STAs, mobilidade, tráfego variável, TCP — não há garantias. Vários mecanismos (bootstrap, vetos, balanceamento) assumem implicitamente **exatamente 2 links**.

14. **~~O binding é consultivo, não vinculativo.~~ RESOLVIDO — binding vinculativo via queue-blocking.** Historicamente, a decisão por (STA, AC) só enviesava o **pedido de acesso ao canal** no enqueue e o **dequeue** (delegate Fcfs) servia qualquer STA em qualquer link com acesso → a distribuição física por-fluxo **usava ambos os links** (o espalhamento STR, medido no all-BE e nos VI do S5). **Agora** o `EnforceRouting` amarra cada (STA, AC) ao link decidido, bloqueando a sua fila nos outros links (reason `WIFI_SCHEDULER_ROUTING`) → as decisões são **ordens** e não há espalhamento. Ver [Conselho vs. execução → Como se tornou vinculativo](#como-se-tornou-vinculativo-implementado--binding-por-queue-blocking) e [Alterações ao core do ns-3](#alterações-ao-core-do-ns-3).

    > **Nota histórica (mudar a decisão NÃO mudava o físico).** Antes do binding, **alterar** o `m_lastSelectedLink` (via migração, balanceador, etc.) **não** alterava a distribuição física se o outro link estivesse ativo para essa AC. Caso concreto: no *switch* 2VO+2VI→2BE+2VI (fase 2), a "expulsão considerada" (mover o VI que partilhava o 2.4GHz com os BEs para o 5GHz) deu **decisão** perfeita mas **físico com VI ~40% no 2.4GHz** (BE esfomeado a ~19 Mbps), porque o 2.4GHz continuava a puxar VI da fila partilhada. Com o binding atual este problema deixa de existir — a fila de VI fica bloqueada no 2.4GHz e o dequeue só ocorre no 5GHz. **A expulsão considerada foi por isso reintroduzida** (ver [Passo 2](#passo-2--stay_satisfied--expulsão-considerada)) e **agora converge fisicamente**: o VI que partilha o 2.4GHz com os BEs migra para o 5GHz (onde cabe com o outro VI) e o 2.4GHz fica livre para os 2 BEs.
    >
    > **O espalhamento STR continua a ser desejável** enquanto mecanismo de agregação por-fluxo; fica como trabalho futuro poder reativá-lo seletivamente (ver a nota "Trabalho futuro" na secção de binding).

---

## Alterações ao core do ns-3

Este projeto vive quase inteiramente em `scratch/`, mas o **binding vinculativo** (limitação #14, resolvida) obrigou a **uma** alteração ao core do ns-3. Todas as linhas alteradas estão marcadas com o comentário **`// I CHANGED HERE`** para fácil rastreio.

| Ficheiro | O quê | Porquê |
|---|---|---|
| `src/wifi/model/wifi-mac-queue-scheduler.h` | Novo valor `WIFI_SCHEDULER_ROUTING` no `enum class WifiQueueBlockedReason` (antes de `REASONS_COUNT`) + o `case` correspondente no `operator<<`. | O `EnforceRouting` precisa de uma **razão de bloqueio própria** para amarrar cada (STA, AC) a um link. Todas as razões existentes (`WAITING_ADDBA_RESP`, `POWER_SAVE_MODE`, `USING_OTHER_EMLSR_LINK`, `WAITING_EMLSR_TRANSITION_DELAY`, `TID_NOT_MAPPED`) são **geridas pela MAC**, que as poria/limparia por conta própria, entrando em conflito com o nosso uso. A razão nova é gerida **exclusivamente** pelo scheduler. |

**Impacto**: acrescentar um valor ao enum aumenta em 1 bit a `Mask = std::bitset<REASONS_COUNT>` de cada container-queue — obriga a **recompilar o módulo wifi** (`./ns3 build`), sem alterar comportamento de nenhum mecanismo existente.

**Como reverter**: remover as 3 linhas marcadas com `// I CHANGED HERE` em `src/wifi/model/wifi-mac-queue-scheduler.h` e o `EnforceRouting`/`WIFI_SCHEDULER_ROUTING` no `scratch/mlo-qos-weighted-scheduler.h`. Sem a razão nova, o scheduler volta ao modo consultivo (com espalhamento STR).

**Localizar as alterações**: `grep -rn "I CHANGED HERE" src/`.
