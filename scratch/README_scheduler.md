# QosWeightedMloScheduler — Documentação Técnica

## Visão geral

O `QosWeightedMloScheduler` é um scheduler de seleção de link para Multi-Link Operation (MLO) em Wi-Fi 7, implementado como uma extensão do `WifiMacQueueScheduler` do ns-3. Opera **apenas no AP** (tráfego downlink); as STAs continuam a usar o `FcfsWifiQueueScheduler` por defeito do ns-3.

A cada pacote (MPDU) que o AP precisa de enviar, o scheduler decide **a que link físico** (ex: 2.4 GHz, 5 GHz ou 6 GHz) esse pacote deve ser encaminhado, com o objetivo de que cada fluxo cumpra os seus próprios objetivos de qualidade de serviço (QoS), respeitando ao mesmo tempo a hierarquia de prioridades real do EDCA (802.11e): **VO > VI > BE > BK**.

O scheduler não decide *quando* transmitir (isso continua a ser gerido pelo EDCA/CSMA-CA real do ns-3) — decide apenas **em qual link** colocar cada pacote, na fila MAC apropriada.

### Granularidade: por (STA, AC), não por AC

A chave de decisão é `StaAcKey = std::pair<Mac48Address, uint8_t>` — o **MAC de destino** + o índice do AC.

> **Porquê**: a versão anterior encaminhava por AC apenas. Isso significava que duas STAs com o mesmo AC (ex: 2 STAs de voz) recebiam **sempre a mesma decisão de link** — era impossível distribuí-las por links diferentes. Com a chave por (STA, AC), cada fluxo é encaminhado independentemente.

Todo o estado de decisão é chaveado assim:

| Membro | Tipo | Papel |
|---|---|---|
| `m_lastSelectedLink` | `map<StaAcKey, uint8_t>` | Link atual (âncora) de cada fluxo |
| `m_currentSatisfaction` | `map<StaAcKey, QosSatisfaction>` | Satisfação **medida** no link atual |
| `m_staQos` | `map<StaAcKey, StaQos>` | Métricas **reais** end-to-end (throughput, delay, jitter, loss) |
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
6. **Bootstrap por prioridade** — se o fluxo ainda não tem 2 amostras medidas (`m_hasMeasuredSat`), força VO/VI → link rápido, BE/BK → link lento, e **termina aqui**.
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

### `estimatedCapacityMbps` — capacidade ALCANÇÁVEL medida

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

> **Porquê medido e não a taxa PHY.** `dataRate_PHY × (1−PER)` é a taxa **nominal** (símbolos de payload no ar) e **ignora todo o overhead MAC** (preâmbulo, AIFS, backoff, SIFS, BlockAck), que no 6 GHz/320 MHz consome a maior parte do tempo. Isso inflacionava a capacidade **~5×** (ex.: 3722 vs ~708 Mbps reais) e fazia a projeção do BE dar 0.97 num link onde ele viria a colapsar. Medir `goodput / airtime ocupado` extrapola a capacidade real e capta o overhead automaticamente. **Fonte crítica**: `m_linkBusyTime` (de `FeedLinkTxStart/End`, tempo real de PHY ocupado) — os proxies de airtime por-AC não servem, porque são tempo de payload puro (sem overhead) e dariam um cálculo circular de volta à taxa PHY.

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

### Passo 2 — `STAY_SATISFIED`

```
se currentSat >= StayThreshold E há âncora:  fica, sem avaliar nada.
```

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

## Métricas reais por-STA

A satisfação **medida** (`currentSat`) não vem do MAC do AP — vem de medições reais end-to-end feitas nos sinks das STAs e alimentadas ao scheduler via `FeedStaQos`, a cada 1 s (em `CalculateStats`, no `.cc`).

| Dimensão | Fonte | Granularidade |
|---|---|---|
| **Throughput** | `sinks[i]->GetTotalRx()` (delta da janela) | por janela de 1 s |
| **Delay / Jitter** | `MonitorPacketSinkRx` via `SeqTsSizeHeader` (`Now − txTime`) | por janela de 1 s |
| **Loss** | **FlowMonitor**, `(txPackets − rxPackets) / txPackets` por fluxo downlink, mapeado ao STA por IP de destino | **acumulada** desde o início |

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
- **Mecanismo**: até um fluxo ter **2 amostras** reais (`StaQos::samples >= 2`), a alocação é forçada por prioridade EDCA (VO/VI → rápido, BE/BK → lento) e a cascata **nem corre**. Salta-se a janela contaminada; quando a cascata arranca, o `curSat` já reflete a realidade e o `STAY_SATISFIED` trava todos no sítio certo.

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

| Constante | Valor | Local | Significado |
|-----------|-------|-------|-------------|
| `kWarmupSamples` | 2 | `UpdatePeriodicMetrics` | Janelas de medição antes de libertar o bootstrap |
| `kMigrationDwellSec` | 1.0 s | `DecideLinkMigration` | Persistência exigida a uma intenção de migração |
| `kStarvationFloor` | 0.60 | `DecideLinkMigration` | `currentSat` abaixo do qual um VO/VI é "esfomeado" (abre a exceção do veto) |
| `perAlpha` | 0.3 | `UpdatePeriodicMetrics` | Suavização EWMA do PER proxy |
| Blend final | 0.65 / 0.25 / 0.10 | `ComputeExpectedQosSatisfaction` | `baseSat` / `capabilityScore` / `perPenalty` |
| Limiar "esfomeado" (altruísta A) | 0.45 | idem | Residente tratado como esfomeado |
| Penalidade base (altruísta A) | 0.6 | idem | `(0.6 − otherSat)`, máx. 0.6 |
| Limiar headroom (altruísta B) | 0.75 | idem | Satisfação abaixo da qual a preventiva pode disparar |
| Teto da preventiva (B) | 0.5 | idem | Máximo que (B) subtrai |
| Carga proxy em cold-start | 0.15 | idem | `NormalisedLoad` assumida sem medição |
| Fator pressão → delay | 0.3 | idem | `delay × (1 + pressure×0.3)` |
| Fator pressão → jitter | 0.2 | idem | `jitter × (1 + pressure×0.2)` |
| `m_edcaWeights[4][4]` | ver Motor 4 | construtor | Matriz de pressão de contenção |

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
| `FeedStaQos(sta, ac, tp, delay, jitter, loss)` | periodicamente (1 s, em `CalculateStats`) | **`m_staQos`** — as métricas reais que definem `currentSat` |
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

1. **A projeção mede capacidade/airtime, não *acesso* — e por isso não modela a fome do EDCA.** Este é o limite de fundo: os motores 2–5 estimam *quanto do meio está ocupado*, não *quem consegue aceder ao canal*. A fome do BE junto a VO/VI (colapso para 4.95 Mbps com 57% de airtime livre) é um fenómeno de acesso que **nenhum modelo de capacidade capta** — só um modelo de saturação EDCA tipo Bianchi (fora do âmbito). É por isso que os **vetos de prioridade são permanentes** (ver §2 dos mecanismos), não um remendo.
   > **Correção aplicada e validada**: a *inflação* da capacidade — um problema **separado** da fome de acesso — foi corrigida. O `estimatedCapacityMbps` deixou de ser a taxa PHY nominal (que inflacionava ~5×) e passa a ser **medido** (`goodput / airtime ocupado`) — verificado no 6 GHz: de **3722 → ~655 Mbps**. O `NormalisedLoad` passou de "mistura entre ACs" (somava sempre 1) para "utilização real". A projeção do link atual convergiu com a satisfação medida para VO/VI (erro ~0.11, ver coluna `ProjCurrentLink` do CSV). **Não** dispensa os vetos (a fome de acesso mantém-se).

2. **Assimetria medido-vs-projetado + `effCap` mede *o que sobra*, não *o que eu receberia*.** Dois defeitos da projeção que coexistem:
   - *Assimetria*: o `currentSat` (link atual) vem de métricas reais por-STA; a projeção dos *outros* links usa proxies por-(link,AC). Comparar os dois em `improvement = bestScore − currentSat` é comparar grandezas diferentes.
   - *effCap residual*: a projeção pergunta *"quanto sobra aqui?"* em vez de *"quanto é que EU receberia aqui?"*. Um fluxo **bem servido**, que usa tudo o que precisa, vê "nada a sobrar" no seu próprio link e projeta-se mal lá.
   > **Impacto medido — real mas inofensivo**: a projeção do BE no seu próprio link diverge da satisfação medida (projetado **0.46** vs medido **0.99**) em links de **baixa capacidade** (2.4GHz, onde o BE consome quase tudo); no 5GHz, com folga, converge (erro 0.07). Não causa decisão errada porque o `STAY_SATISFIED` (0.99) dispara sempre e a projeção má nunca chega a ser usada. Corrigi-lo exigiria um modelo de partilha de airtime — não vale o esforço.

3. **O `avgQueueDelayMs` proxy (por-link) é ≈ 0 mesmo sob congestão.** Só conta pacotes que chegam ao dequeue; os que ficam presos ou são dropados por fila cheia nunca entram na estatística. Só o caminho per-STA foi corrigido — o proxy que a projeção usa continua cego à congestão.

4. **~~`UtilityFunction` dá 0.5 no alvo~~ — CORRIGIDO e VALIDADO.** *Problema original*: para throughput, cumprir exatamente o alvo dava só 0.5 (era preciso ~2× o alvo para saturar), o que fazia VO/VI estabilizarem em ~0.69–0.75, colados ao `StayThreshold`, dependentes dos vetos em vez do `STAY_SATISFIED`. *Correção*: (a) no ramo *higher-is-better* a inflexão passou para 50% do alvo (`1/(1+e^{10(0.5−ratio)})`) → cumprir o alvo ≈ 0.99; (b) as metas de jitter de VO/VI (`maxJitterMs`) subiram de 0.1 ms (inatingível) para 5/10 ms. *Resultado medido*: VO/VI/BE bem servidos passaram a ~**0.99** e o `STAY_SATISFIED` voltou a dominar (zero migrações no steady state). O `StayThreshold=0.75` **não** precisou de mudar — com os satisfeitos a ~0.99, continua a separar bem servido de esfomeado.

5. **As métricas end-to-end são medidas nos sinks das STAs** — um AP real não teria acesso a elas. É legítimo em simulação e foi a forma de obter valores fiáveis, mas o scheduler **não é diretamente transponível para hardware** sem uma fonte equivalente do lado do AP.

6. **A loss acumulada é estável mas lenta.** Uma degradação súbita demora a refletir-se, e depois de um fluxo melhorar o histórico mau só se dilui gradualmente. Foi uma troca deliberada (estabilidade > reatividade).

7. **O veto BE/BK é absoluto e não olha para o airtime real do link dos VO/VI.** Os dois vetos não são simétricos no rigor: o do VO/VI tem exceção de fome (`currentSat < 0.60`); o do BE/BK **bloqueia sempre**. A regra assume que os VO/VI **saturam** o seu link — verdade nos cenários testados. Mas **nunca verifica se há airtime livre**. Caso onde falharia: 1 VO sozinho num 6 GHz a usar 150 de ~1000 Mbps (~85% livre) e 4 BEs entupidos no 2.4 GHz com 58% de perdas — fisicamente os BEs beneficiariam de partilhar o 6 GHz, mas ficariam presos sem escape. É uma **heurística de prioridade, não uma decisão baseada em capacidade**.

8. **Muitas constantes críticas estão hardcoded** e não expostas como atributos ns-3: blend `0.65/0.25/0.10`, `kMigrationDwellSec`, `kWarmupSamples`, `kStarvationFloor`, `perAlpha`, `m_edcaWeights`. Afinar exige recompilar.

9. **`WouldHarmResident` continua reativo.** Usa a satisfação *atual* dos residentes, não projeta o impacto da chegada do candidato. Deteta "alguém já está mal", não "vou piorar alguém que está bem". Os vetos de prioridade cobrem os casos críticos; a penalidade preventiva (B) do Motor 5 cobre parcialmente o resto.

10. **O dwell de 1 s está acoplado ao intervalo de medição.** Foi escolhido para garantir que uma intenção sobrevive a ≥ 1 janela nova. Se o `MetricsInterval` ou a cadência do `FeedStaQos` (1 s) mudarem, o dwell tem de ser reavaliado.

11. **Cobertura de teste.** Validado em cenários de **4 STAs** (2VO+1VI+1BE, 2VO+2VI, 1VO+1VI+1BE+1BK, all-BE) × **3 pares de frequências** (2.4+5, 2.4+6, 5+6), com tráfego UDP saturante de 150 Mbps por STA e STAs estáticos. Fora deste envelope — mais links, mais STAs, mobilidade, tráfego variável, TCP — não há garantias. Vários mecanismos (bootstrap, vetos) assumem implicitamente **exatamente 2 links**.
