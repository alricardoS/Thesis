# QosWeightedMloScheduler — Documentação Técnica

## Visão geral

O `QosWeightedMloScheduler` é um scheduler de seleção de link para Multi-Link Operation (MLO) em Wi-Fi 7, implementado como uma extensão do `WifiMacQueueScheduler` do ns-3. Opera **apenas no AP** (tráfego downlink); as STAs continuam a usar o `FcfsWifiQueueScheduler` por defeito do ns-3.

A cada pacote (MPDU) que o AP precisa de enviar, o scheduler decide **a que link físico** (ex: 2.4 GHz, 5 GHz ou 6 GHz) esse pacote deve ser encaminhado, com o objetivo de que cada Access Category (AC — VO, VI, BE, BK) cumpra os seus próprios objetivos de qualidade de serviço (QoS), respeitando ao mesmo tempo a hierarquia de prioridades real do EDCA (802.11e): VO > VI > BE > BK.

O scheduler não decide *quando* transmitir (isso continua a ser geração pelo EDCA/CSMA-CA real do ns-3) — decide apenas **em qual link** colocar cada pacote, na fila MAC apropriada.

---

## Arquitetura: seis motores internos

O scheduler está organizado em seis motores, cada um responsável por uma fatia da decisão final:

| # | Motor | Responsabilidade | Função/estrutura principal |
|---|-------|-------------------|------------------------------|
| 1 | **Goal-Awareness Engine** | Calcula o quão satisfeita está cada AC, num link, face aos seus próprios objetivos | `ComputeQosSatisfaction`, `QosSatisfaction` |
| 2 | **Link Capability Engine** | Estima a capacidade real de cada link (PHY) e quanto dela está disponível, por AC, respeitando prioridade | `UpdateLinkCapability`, `LinkCapability` |
| 3 | **Traffic Composition Engine** | Mede o tráfego real (throughput, airtime, nº de fluxos) que cada AC já está a gerar em cada link | `UpdateTrafficComposition`, `LinkTrafficComposition` |
| 4 | **EDCA Competition Engine** | Modela a pressão de contenção entre ACs no mesmo link, com base nas regras reais do EDCA | `UpdateEdcaCompetition`, `EdcaCompetition` |
| 5 | **Projection Engine** | Combina os motores 1–4 numa pontuação única "quão bom seria este link para mim" | `ComputeExpectedQosSatisfaction` |
| 6 | **Migration Decision Engine** | Decide, com histerese e uma cascata explícita de prioridades, se migra de link ou fica | `DecideLinkMigration`, `MeetsOwnGoals`, `WouldHarmResident` |

Todos os motores 1–4 são recalculados periodicamente (por omissão, a cada 0.5 s, ver `MetricsInterval`). O motor 5 e 6 correm **por pacote** (cada vez que o ns-3 chama `GetLinkIds`), mas usam os valores já calculados pelos motores 1–4 nessa janela — não recalculam PHY/tráfego a cada pacote, por razões de desempenho.

---

## Fluxo de uma decisão, passo a passo

1. O ns-3 chama `GetLinkIds(ac, mpdu, ...)` quando tem um MPDU pronto a enfileirar.
2. O scheduler extrai os links elegíveis (via `FcfsWifiQueueScheduler` delegado) e chama `DecideLinkMigration(ac, eligibleLinks)`.
3. `DecideLinkMigration` verifica primeiro se a AC já está satisfeita no link onde está atualmente (`StayThreshold`). Se sim, fica — sem reavaliar nada.
4. Se não estiver suficientemente satisfeita, avalia **todos os links elegíveis** e classifica-os em três categorias (explicado em detalhe mais abaixo).
5. Escolhe o melhor link dentro da melhor categoria disponível, sujeito a uma margem mínima de melhoria (`MigrationThreshold`) para evitar oscilação.
6. Regista a decisão (opcionalmente em CSV) e devolve o link escolhido.
7. A cada 0.5 s (`UpdatePeriodicMetrics`), os motores 1–4 recalculam tudo com base nas medições reais entretanto recolhidas (PHY, throughput, delay, jitter, PER).

---

## Motor 1 — Goal-Awareness Engine

### O que faz

Para cada AC, em cada link, calcula um **índice de satisfação composto** entre 0 e 1, combinando quatro dimensões de QoS através de uma função sigmoide:

```
sat.throughput = UtilityFunction(throughput_medido, targetThroughputMbps, lowerIsBetter=false)
sat.delay      = UtilityFunction(delay_medido,       maxDelayMs,           lowerIsBetter=true)
sat.jitter     = UtilityFunction(jitter_medido,       maxJitterMs,          lowerIsBetter=true)
sat.loss       = UtilityFunction(PER_medido,          maxPacketLoss,        lowerIsBetter=true)

sat.index = (throughputWeight·sat.throughput + delayWeight·sat.delay
           + jitterWeight·sat.jitter + lossWeight·sat.loss) / soma_dos_pesos
```

### A função sigmoide (`UtilityFunction`)

```cpp
ratio = valor / alvo
lowerIsBetter:     utilidade = 1 / (1 + e^(5·(ratio − 1)))
maior é melhor:    utilidade = 1 / (1 + e^(5·(1 − ratio)))
```

- Quando `valor == alvo` (`ratio = 1`), a utilidade é exatamente **0.5** — o ponto de inflexão.
- A constante `5` controla a inclinação: valores acima/abaixo do alvo saturam rapidamente para perto de 0 ou 1.
- Se `alvo <= 0`, devolve `0.5` (neutro, evita divisão por zero).
- **Importante**: por ser uma sigmoide assintótica, a utilidade nunca atinge exatamente 0 ou 1 — só se aproxima.

### `AcGoals` — objetivos por AC (valores atuais no construtor)

| AC | targetThroughputMbps | maxDelayMs | maxJitterMs | maxPacketLoss |
|----|----------------------|------------|-------------|----------------|
| VO | 150.0 | 15.0 | 0.1 | 0.01 (1%) |
| VI | 150.0 | 30.0 | 0.1 | 0.01 (1%) |
| BE | 150.0 | 200.0 | 1.0 | 0.10 (10%) |
| BK | 150.0 | 300.0 | 100.0 | 0.10 (10%) |

- `targetThroughputMbps`: throughput que a AC idealmente quer alcançar nesse link. **Nota**: está igual (150 Mbps) para todas as ACs neste momento — é um valor de configuração, não uma limitação estrutural; pode ser ajustado por AC via `SetGoals`.
- `maxDelayMs` / `maxJitterMs`: o limiar acima do qual a satisfação dessa dimensão cai abaixo de 0.5. VO e VI têm exigências muito mais apertadas (delay/jitter baixos), refletindo tráfego sensível ao tempo real.
- `maxPacketLoss`: fração de perda tolerável (0–1) antes da satisfação de perda cair abaixo de 0.5.

### `AcWeights` — pesos por AC (valores atuais no construtor)

| AC | delayWeight | jitterWeight | lossWeight | throughputWeight |
|----|-------------|--------------|------------|-------------------|
| VO | 0.40 | 0.30 | 0.25 | 0.05 |
| VI | 0.25 | 0.15 | 0.20 | 0.40 |
| BE | 0.20 | 0.05 | 0.15 | 0.60 |
| BK | 0.05 | 0.05 | 0.10 | 0.80 |

Os pesos determinam **quanto cada dimensão pesa na satisfação composta**. Não precisam de somar 1 — o código normaliza pela soma total. VO prioriza fortemente delay+jitter (tráfego de voz não tolera atraso, mas tolera throughput mais baixo). BK quase só se importa com throughput (tráfego de fundo, sem requisitos de tempo real).

**Configuráveis em runtime** via `SetWeights(ac, delay, jitter, loss, tp)` e `SetGoals(ac, tp, delay, jitter, loss)` (ou o alias legado `SetTargets`).

---

## Motor 2 — Link Capability Engine

### O que faz

Estima quanto throughput cada link consegue realmente entregar, e quanto disso ainda está disponível — **com consciência de prioridade EDCA**, não como uma fatia única partilhada por todos.

### `estimatedCapacityMbps`

```cpp
se houver dados PHY reais (EWMA de MCS/PER do PhyTxPsduBegin):
    estimatedCapacityMbps = dataRate_PHY × (1 − PER_EWMA)
senão (fallback, ainda sem dados):
    estimatedCapacityMbps = 400.0 Mbps  (link rápido)  ou  150.0 Mbps  (link lento)
```

A escolha entre "link rápido"/"link lento" no fallback vem de `ConfigureForPair`, que ordena os dois links por `GetFrequencyRank` (6 GHz > 5 GHz > 2.4 GHz).

### `availableCapacityPerAcMbps` — a correção mais importante deste motor

Em vez de um único valor de "capacidade disponível" partilhado por todas as ACs (que faria uma AC de alta prioridade parecer artificialmente prejudicada pelo consumo de uma AC de baixa prioridade), cada AC tem o seu próprio valor, calculado descontando **apenas o consumo de ACs com prioridade igual ou superior**:

```
availableCapacityPerAcMbps[VO] = estimatedCapacity − consumo(VO)
availableCapacityPerAcMbps[VI] = estimatedCapacity − consumo(VO) − consumo(VI)
availableCapacityPerAcMbps[BE] = estimatedCapacity − consumo(VO) − consumo(VI) − consumo(BE)
availableCapacityPerAcMbps[BK] = estimatedCapacity − consumo(VO) − consumo(VI) − consumo(BE) − consumo(BK)
```

Isto modela o comportamento real do 802.11: o VO, com AIFS/CW mais curtos, ganha sempre acesso ao meio primeiro — não "perde" capacidade só porque o BE está a transmitir muito. Já o BE, sendo de prioridade mais baixa, sofre o impacto de tudo o que está acima dele.

### `freeAirtime` e `capabilityScore`

```
freeAirtime = 1 − (tempo ocupado do link / duração da janela)
capabilityScore = min(1, availableCapacityMbps / estimatedCapacityMbps)
```

`capabilityScore` é uma fração 0–1 "genérica" (não por AC) usada como termo de ajuste na pontuação final (ver Motor 5).

---

## Motor 3 — Traffic Composition Engine

Mede, por janela de 0.5 s, o tráfego real que cada AC gerou em cada link:

- `voThroughputMbps`, `viThroughputMbps`, `beThroughputMbps`, `bkThroughputMbps` — throughput medido (bytes reais / duração da janela).
- `voAirtimeFrac`, etc. — fração do tempo de transmissão total do link que cada AC consumiu (usada como proxy de carga real no Motor 4).
- `voFlows`, etc. — número de fluxos distintos (pares STA+TID) ativos por AC.

Estes valores alimentam diretamente o Motor 2 (consumo absoluto) e o Motor 4 (carga normalizada).

---

## Motor 4 — EDCA Competition Engine

### `m_edcaWeights` — a matriz de pressão de contenção

```cpp
// linhas = AC candidata; colunas = outra AC presente no link
// ordem: BK, BE, VI, VO
{0.0,  0.5,  7.0,  9.0}   // candidata BK  — sente MUITO o VO (9.0) e o VI (7.0)
{0.5,  1.0,  6.0,  8.0}   // candidata BE  — sente MUITO o VO (8.0) e o VI (6.0)
{0.25, 0.5,  1.0,  2.0}   // candidata VI  — sente moderadamente o VO (2.0)
{0.0,  0.05, 0.25, 1.0}   // candidata VO  — quase não sente ninguém abaixo dele
```

Cada célula `m_edcaWeights[X][Y]` representa **"o quanto a AC X sente a presença da AC Y"** — não é simétrica, porque a contenção real do EDCA não é simétrica (VO sempre ganha acesso primeiro, então sente pouco as ACs abaixo dele; BE/BK sentem muito o VO/VI porque cedem espaço a eles).

**Como afinar**: se uma AC de baixa prioridade (ex: BE) está a ser excessivamente prejudicada por uma AC específica de alta prioridade (ex: VI), aumentar `m_edcaWeights[BE][VI]` torna o BE mais sensível/avesso a partilhar um link com o VI.

### `competitionPressure`

```
pressure[candAc] = Σ (m_edcaWeights[candAc][otherAc] × NormalisedLoad(otherAc, link))   para cada otherAc presente
```

`NormalisedLoad` é a fração de airtime que a outra AC já consome (do Motor 3). Quanto mais peso + mais carga real da outra AC, maior a pressão sentida.

### `expectedAccessOpportunity`

```
opportunity = freeAirtime / (1 + pressure)        (limitado a [0, 1])
```

Uma proxy de "que fração do tempo livre eu realisticamente consigo aceder", descontando a contenção.

### `effectiveAvailableCapacityMbps`

```
effectiveAvailableCapacityMbps[ac] = availableCapacityPerAcMbps[ac] × expectedAccessOpportunity[ac]
```

Esta é a estimativa final de "quantos Mbps eu, especificamente, conseguiria obter neste link agora" — combina capacidade física disponível (Motor 2, já priority-aware) com a fração de acesso realista dado a contenção (este motor).

---

## Motor 5 — Projection Engine (`ComputeExpectedQosSatisfaction`)

Esta é a função central que combina tudo num único número entre ~0 e ~1, usado por todo o resto do scheduler para comparar links.

```
expTp     = UtilityFunction(effectiveAvailableCapacityMbps, targetThroughputMbps, false)
expDelay  = UtilityFunction(delay_medido × (1 + pressure×0.3), maxDelayMs, true)
expJitter = UtilityFunction(jitter_medido × (1 + pressure×0.2), maxJitterMs, true)
expLoss   = UtilityFunction(PER_medido (EWMA), maxPacketLoss, true)

baseSat = (throughputWeight·expTp + delayWeight·expDelay
         + jitterWeight·expJitter + lossWeight·expLoss) / soma_dos_pesos

expectedScore = baseSat × 0.65 + capabilityScore × 0.25 + (1 − PER) × 0.10
```

Os fatores `0.30` e `0.20` no delay/jitter projetam o impacto adicional da pressão de contenção sobre a latência esperada — quanto mais pressão, mais o delay/jitter "parecem" piores do que o medido atualmente, antecipando degradação.

Os pesos `0.65 / 0.25 / 0.10` no blend final são fixos no código (não configuráveis via atributo) e determinam quanto a pontuação final depende da satisfação "pura" (QoS) vs. da capacidade geral do link vs. da fiabilidade (PER).

### Penalidade altruísta (dentro desta mesma função)

Depois de calcular `expectedScore`, o motor aplica uma penalidade adicional para proteger ACs já residentes no link candidato:

**(A) Reativa** — se uma AC residente já está esfomeada (`currentSat < 0.45`), soma-se `(0.6 − currentSat)` à penalidade. Quanto pior a AC residente estiver, maior a penalidade contra novos candidatos.

**(B) Preventiva** — mesmo que a AC residente ainda não esteja esfomeada, se o seu `headroomRatio` (`effectiveAvailableCapacityMbps / targetThroughputMbps`) já for inferior a 1.0 (não tem nem o seu próprio alvo de capacidade de sobra) **e** `currentSat < 0.75`, aplica-se uma penalidade proporcional à carga que o novo candidato traria e ao quão "apertado" já está o residente.

A penalidade final é o `max` entre (A) e (B) por cada AC residente (não a soma — evita que múltiplos residentes amplifiquem artificialmente a penalidade), e o resultado é subtraído ao `expectedScore`, nunca abaixo de 0.

---

## Motor 6 — Migration Decision Engine (`DecideLinkMigration`)

Esta é a lógica final que decide, por pacote, se um AC deve migrar de link.

### Passo 1 — Verificação rápida de satisfação (short-circuit)

```
se currentSat >= StayThreshold E já tenho uma âncora válida:
    fico no link atual, sem avaliar mais nada.
```

Isto evita recalcular tudo a cada pacote quando o AC já está claramente bem servido.

### Passo 2 — Cascata explícita de três categorias

Se não estiver suficientemente satisfeito, avalia **todos os links elegíveis** e classifica cada um:

| Categoria | Condição | Significado |
|-----------|----------|-------------|
| **1 — CAT1_CLEAN** | `MeetsOwnGoals(ac, link)` **e** `!WouldHarmResident(ac, link)` | Ideal: cumpro os meus objetivos sem prejudicar ninguém |
| **2 — CAT2_PRIORITY_OVERRIDE** | `MeetsOwnGoals(ac, link)` **mas** `WouldHarmResident(ac, link)` | Cumpro os meus objetivos, mas à custa de outro AC — aceitável porque a prioridade EDCA é legítima e não sou obrigado a sacrificar-me por um AC de prioridade igual/inferior |
| **3 — CAT3_BEST_EFFORT** | Não cumpro os meus objetivos em lado nenhum | Recurso de último caso: escolho o link com a melhor pontuação possível, mesmo que insuficiente |

O scheduler escolhe sempre a **melhor categoria disponível** (1 > 2 > 3), e dentro dela, o link com a melhor `ComputeExpectedQosSatisfaction`.

#### `MeetsOwnGoals(ac, linkId)`

```cpp
return ComputeExpectedQosSatisfaction(ac, linkId) >= m_ownGoalsThreshold;
```

Pergunta simples: "este link, na projeção atual, dá-me uma pontuação satisfatória?"

#### `WouldHarmResident(ac, linkId)`

Procura, entre todas as ACs atualmente ancoradas nesse link (exceto a própria `ac`), qual tem a pior `currentSat` (satisfação **atual medida**, não projetada). Se essa pior satisfação for `<= m_harmThreshold`, considera-se que colocar `ac` nesse link "prejudicaria" esse residente.

> **Limitação conhecida**: esta função é **reativa**, não preventiva — usa a satisfação atual medida dos residentes, não uma projeção de como ficariam *depois* de `ac` chegar. Deteta "alguém já está mal", não "vou piorar alguém que está bem". A penalidade preventiva (B) do Motor 5 complementa parcialmente esta lacuna, mas não a elimina.

### Passo 3 — Histerese

```
se o melhor link da melhor categoria != link atual:
    se (melhorScore − currentSat) > MigrationThreshold:
        MIGRA
    senão:
        fica (STAY_HYSTERESIS) — a melhoria não compensa o custo de migrar
senão:
    fica (STAY_HYSTERESIS ou STAY_BEST)
```

A histerese existe para evitar oscilação: pequenas flutuações de medição não devem disparar migrações constantes, que têm um custo real (reconfiguração de agregação A-MPDU, sincronização de Block-Ack).

---

## Tabela de todas as variáveis configuráveis

### Atributos ns-3 (configuráveis via `SetAttribute` em runtime)

| Atributo | Variável C++ | Valor atual | Intervalo válido | O que controla |
|----------|--------------|-------------|--------------------|------------------|
| `StayThreshold` | `m_stayThreshold` | **0.75** | [0, 1] | Satisfação mínima para nem avaliar alternativas. Mais alto → reavalia mais frequentemente; mais baixo → fica "preguiçoso" e raramente migra. |
| `MigrationThreshold` | `m_migrationThreshold` | **0.25** | [0, 1] | Ganho mínimo de pontuação exigido para justificar uma migração. Mais alto → mais estável, reage mais devagar; mais baixo → mais reativo, risco de oscilação. |
| `MetricsInterval` | `m_metricsIntervalSec` | **0.5 s** | [0.05, 10] | Frequência de recálculo dos Motores 1–4. Mais curto → reage mais depressa a mudanças reais, mas mais ruído estatístico (menos amostras por janela); mais longo → mais estável, mais lento a detetar degradação. |
| `OwnGoalsThreshold` | `m_ownGoalsThreshold` | **0.90** | [0, 1] | Pontuação mínima de `ComputeExpectedQosSatisfaction` para um link contar como "cumpro os meus objetivos" na cascata. Mais alto → mais exigente, mais ACs caem em Categoria 2/3; mais baixo → mais fácil entrar em Categoria 1. |
| `HarmThreshold` | `m_harmThreshold` | **0.70** | [0, 1] | Satisfação atual abaixo da qual um AC residente é considerado "prejudicado". Mais alto → o scheduler protege os residentes mais cedo (mais sensível); mais baixo → só reage a dano já severo. |

### Constantes globais (não atributos ns-3, hardcoded no `.h`)

| Constante | Valor | O que representa |
|-----------|-------|---------------------|
| `MAX_QUEUE_LENGTH` | 500.0 | Limite de pacotes na fila MAC (referência/diagnóstico — o limite real é imposto pelo `WifiMacQueue` do ns-3, não por este scheduler). |
| `MAX_DELAY_MS` | 200.0 | Usado como referência para normalizar `holDelay` (não confundir com `AcGoals.maxDelayMs`, que é por AC). |
| `MAX_THROUGHPUT` | 600.0 Mbps | Referência teórica para 6 GHz / 320 MHz / 2SS EHT (não usada diretamente no cálculo de score). |

### Pesos e objetivos por AC (estruturas, não atributos individuais)

Configuráveis via `SetWeights(ac, delay, jitter, loss, tp)` e `SetGoals(ac, tp, delay, jitter, loss)` chamados após a construção do scheduler (tipicamente no script de simulação).

| Estrutura | Campos | Onde são lidos |
|-----------|--------|------------------|
| `AcGoals` | `targetThroughputMbps`, `maxDelayMs`, `maxJitterMs`, `maxPacketLoss` | Motor 1 (`ComputeQosSatisfaction`), Motor 5 (`ComputeExpectedQosSatisfaction`), penalidade preventiva (B) |
| `AcWeights` | `delayWeight`, `jitterWeight`, `lossWeight`, `throughputWeight` | Motor 1, Motor 5 (`baseSat`) |

### `m_edcaWeights[4][4]` — matriz de pressão de contenção

Não é um atributo ns-3 (hardcoded no construtor da classe, com comentários `//ALTEREI` a marcar valores ajustados manualmente face ao default original). Ver tabela completa na secção do Motor 4. Para alterar, é necessário editar diretamente o array no `.h` e recompilar.

### Pesos fixos no blend final (Motor 5, não configuráveis via atributo)

```cpp
expectedScore = baseSat × 0.65 + capabilityScore × 0.25 + perPenalty × 0.10
```

Estes três coeficientes (0.65 / 0.25 / 0.10) estão hardcoded dentro de `ComputeExpectedQosSatisfaction`. Para os tornar configuráveis seria necessário adicioná-los como novos atributos `TypeId`.

### Constantes internas da penalidade altruísta (hardcoded, não atributos)

| Constante | Valor | Localização | Significado |
|-----------|-------|--------------|-------------|
| Limiar de "esfomeado" (reativa) | `0.45` | `ComputeExpectedQosSatisfaction`, componente (A) | Satisfação abaixo da qual um residente é tratado como esfomeado para efeitos de penalidade reativa |
| Penalidade máxima base (reativa) | `0.6` | idem | Usado como `(0.6 − otherSat)`, logo a penalidade reativa máxima possível é 0.6 (quando `otherSat=0`) |
| Limiar de headroom confortável (preventiva) | `0.75` | componente (B) | Satisfação abaixo da qual a penalidade preventiva pode disparar |
| Teto da penalidade preventiva | `0.5` | componente (B), `std::min(0.5, ...)` | Limite máximo que a componente (B) pode subtrair |
| Carga proxy em cold-start | `0.15` | componente (B) | Valor assumido para `NormalisedLoad` quando ainda não há medição real (evita penalidade nula só por falta de dados) |
| Fator de penalização por pressão (delay) | `0.3` | Motor 5, `delayMs × (1 + pressure×0.3)` | Quanto a pressão de contenção amplifica a projeção de delay |
| Fator de penalização por pressão (jitter) | `0.2` | Motor 5, `jitterMs × (1 + pressure×0.2)` | Idem para jitter |

### PER — suavização EWMA (correção aplicada nesta versão)

```cpp
constexpr double perAlpha = 0.3;
packetErrorRate = (1 − perAlpha) × packetErrorRate_anterior + perAlpha × packetErrorRate_da_janela
```

`perAlpha = 0.3` (hardcoded) controla a velocidade de adaptação do PER. Esta correção substituiu um reset duro por janela, que inflacionava artificialmente o PER quando a confirmação de ACK chegava numa janela de 0.5 s diferente daquela em que a tentativa de transmissão tinha sido contabilizada.

---

## APIs de alimentação externa (chamadas pelo script de simulação)

O scheduler não lê diretamente do PHY/MAC do ns-3 — precisa que o script de simulação ligue *traces* e chame estas funções:

| Função | Quando chamar | Alimenta |
|--------|----------------|----------|
| `FeedLinkPhyState(linkId, txVector, per)` | `PhyTxPsduBegin` | EWMA de data rate e PER do Motor 2 |
| `FeedLinkTxAttempt(linkId, ac, nFrames)` | `PhyTxPsduBegin` | `txAttempts` (numerador do cálculo de PER) |
| `FeedLinkMetrics(linkId, ac, bytes, frames, drops)` | `AckedMpdu` (sucesso confirmado) | `txSuccess`, `txBytes` |
| `FeedPacketTransmitted(linkId, ac, bytes, airtimeSec, peer, tid)` | `PhyTxPsduBegin` (frames SU) | Motor 3 (throughput/airtime/flows reais) |
| `FeedLinkTxStart` / `FeedLinkTxEnd` | `PhyTxBegin` / `PhyTxEnd` | Tempo ocupado do link (`channelUtilization`, `freeAirtime`) |

---

## Logging e diagnóstico

`EnableDecisionCsv(filename, nodeContext)` ativa um CSV com uma linha por decisão de migração, incluindo: timestamp, AC, link escolhido, categoria/motivo da decisão (`STAY_SATISFIED`, `MIGRATE_CAT1_CLEAN`, `MIGRATE_CAT2_PRIORITY_OVERRIDE`, `MIGRATE_CAT3_BEST_EFFORT`, `STAY_HYSTERESIS`, `STAY_BEST`), satisfação atual vs. esperada, pressão EDCA, capacidade efetiva, PER, delay, jitter e throughput.

`PrintFinalScores()` imprime, no fim da simulação, a pontuação projetada de cada AC em cada link e a sua satisfação atual — útil para um diagnóstico rápido pós-simulação sem precisar de abrir o CSV.

---

## Limitações conhecidas (a considerar em trabalho futuro)

1. **`WouldHarmResident` é reativo, não preventivo.** Só deteta dano depois de já ter acontecido na medição atual, não projeta o impacto futuro de uma migração antes dela ocorrer.
2. **Granularidade por AC, não por (STA, AC).** Duas STAs com a mesma AC partilham o mesmo estado de decisão (`m_lastSelectedLink[ac]`, `m_currentSatisfaction[ac]`) — isto foi identificado como uma fonte de instabilidade quando testado, mas a versão com granularidade por STA introduziu outras regressões e foi abandonada nesta versão.
3. **Pesos do blend final (0.65/0.25/0.10) e constantes da penalidade altruísta são hardcoded**, não expostos como atributos configuráveis.
4. **Cold-start**: nos primeiros ~1-2 segundos de simulação, antes de `UpdatePeriodicMetrics` ter corrido pelo menos uma vez, os valores de capacidade/satisfação usam fallbacks otimistas que podem levar a decisões iniciais sub-ótimas, geralmente corrigidas após a primeira ou segunda janela de métricas.
