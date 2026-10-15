# Plano de implementação — o link `usb` com as funções do link `amp`

> **Status**: **nada aqui foi implementado.** É o levantamento de 2026-10-14, feito lendo
> `linux-med/drivers/iio/adc/ti-ads1299.c` (commit `983c5cf`), o driver `iio` do framework, a
> tradução por link em `do_derive_device_options` e o firmware do M33, mais este plano.
>
> **Escopo**: dar ao link `usb` (o conversor atrás da ponte MCP2210, lido pelo driver IIO do
> kernel) as funções que o firmware do M33 dá ao link `amp`, **exceto a taxa de amostragem**, que
> no `usb` é 250 e continua 250: é o teto da ponte (`BRINGUP_AFE.md` §6).
>
> **Não é**: um plano para o link `spi`, embora quase tudo aqui valha para ele também (mesmo
> driver de kernel, mesmo driver do framework); nem para o carimbo de tempo na conversão, que a
> ponte não pode dar e que o registro já declara (`acquisition.link = usb`).
>
> **Leitura obrigatória antes**: `CLAUDE.md` (regras 6 e 9), `implementation_plan_iio_afe.md`
> §13, `BRINGUP_AFE.md` §6 (o link na placa) e §6.8 (a escala), `implementation_plan_mcp2210.md`
> §13 (o contador do GP6), `implementation_plan_hmi_desktop.md` §12.8–§12.14 (as funções do `amp`
> como foram medidas) e `implementation_plan_afe_bench.md` §3 (segurança: nenhuma pessoa ligada
> ao conversor).

---

## 0. A linha, dita primeiro

Hoje o `usb` faz bem três das sete funções do `amp`. Ele **grava uma coisa falsa** numa delas: o
lead-off é aceito como ligado, o registro diz que os eletrodos foram monitorados, e nenhum canal
foi monitorado. Corrigir isso vem antes de qualquer função nova.

| Função | `amp` (M33) | `usb` hoje | Fase |
|---|---|---|---|
| Ganho | 4–24 (1 e 2 não cabem no quadro) | os 7, `hardwaregain` | — |
| Sinal de teste interno | sim | sim, `input_mux`/`test_signal` | — |
| Bias derivado | sim | sim, `bias_drive` | — |
| Montagem bipolar/referencial | sim, lida de volta | sim, **não lida de volta** (cache do regmap) | 3 |
| **Lead-off** | liga/desliga; estado por canal em cada quadro; HMI, auditoria, metadado | comparadores ligados no *probe*, **nenhum canal habilitado**, nada chega ao quadro; a tabela recusa "desligado" e aceita "ligado" sem honrar | 2 |
| **Entradas em curto** | sim | o driver tem `input_mux = shorted`; a tabela recusa por não medido | 1 |
| **Sensor de temperatura no último canal** | sim, ganho próprio limitado a 12 | não: `input_mux` e `hardwaregain` valem para os oito canais juntos | 4 |
| Palavra de status conferida (`1100`) | sim; quadro fora de sincronia vira perda | não conferida | 3 |
| **Perda deixa marca na sequência** | sim (`med_producer_on_lost`) | **não**: o framework numera os quadros sozinho; o contador do GP6 não serve, porque o DRDY está no GP5 e o GP6 tem um LED | 5 |
| Carimbo de tempo na conversão | sim | impossível pela ponte | fora |

---

## 1. Onde estamos, lido no código

**Kernel (`ti-ads1299.c`).**
- A leitura de cada amostra (`rdata`) já traz a **palavra de status de 24 bits** no começo de
  `rx_buffer`: `1100`, `LOFF_STATP[7:0]`, `LOFF_STATN[7:0]`, `GPIO[7:4]`. `ads1299_rdata_complete`
  copia só os canais de dado para o buffer IIO e descarta o status.
- `ads1299_init` liga `PD_LOFF_COMP` e não escreve `LOFF_SENSP`/`LOFF_SENSN` nem `LOFF`.
  `lead_off_status` (sysfs) lê os registradores e diz `disabled` enquanto nenhum canal estiver
  habilitado.
- `ads1299_set_mux` escreve o mesmo MUX nos oito `CHnSET`; `hardwaregain` é
  `info_mask_shared_by_all`.
- O regmap é `REGCACHE_MAPLE` com só `LOFF_STATP..LOFF_STATN` voláteis: ler um registrador de
  configuração devolve o cache, não o chip.
- `lost_samples` conta amostras **pedidas** enquanto uma transferência estava em curso. No `usb`
  quem pede é um *hrtimer* do host, então isso conta atrasos do host, não conversões que o
  conversor fez e ninguém leu.

**Framework (`IioDevice`, `MedicalDevice.cpp`).** Lê o buffer de `/dev/iio:deviceN`, converte
cada código pelo passo declarado (`full_scale`, `resolution_bits`) e numera os quadros com
`++sequence_`. Não lê `lead_off_status`, `lost_samples` nem `timestamp_source`, e o bloco de
lead-off do quadro sai como "nada monitorado". O caminho de execução lê `/sys` direto: **nenhum
teste de host o alcança** (`CLAUDE.md`, parágrafo do `usb`).

**Receita (`do_derive_device_options`, ramo `spi`/`usb`).** Recusa `afe.input != normal` e
`afe.temperature = true` ("não medido"), recusa `afe.lead_off_detection = false` ("o driver liga
os comparadores e não oferece controle") e aceita `true` sem traduzir para nada.

**Hardware.** Na placa de AFE: CS no GP4, **DRDY no GP5**, **LED no GP6**, GP0–3, GP7 e GP8 no
terra (`BRINGUP_AFE.md` §2). O contador de bordas da ponte só existe no GP6.

---

## 2. Decisões que este plano pede

| # | Decisão | Proposta | Por quê |
|---|---|---|---|
| D1 | Como o estado de lead-off chega ao userspace | **um canal a mais no buffer IIO**, depois dos oito de dado e antes do carimbo, com os 24 bits da palavra de status | É o único caminho que não custa transação: ler `LOFF_STATP/N` por registrador a cada quadro são duas trocas HID a mais num link com folga zero a 250. O status já vem de graça em toda leitura. **É ABI própria**, e se soma às já registradas como pendência de *upstream* (`implementation_plan_ads1299_upstream.md`) |
| D2 | Como ligar e desligar o lead-off | atributo `lead_off` (`off`/`dc`), que programa `LOFF`, `LOFF_SENSP` e `LOFF_SENSN` conforme a montagem, como `afe_registers()` no M33 | O M33 já é o oráculo: as mesmas máscaras, os mesmos bits, a mesma regra do referencial (só `AFE_REFERENCE_BIT` no lado negativo) |
| D3 | Ganho próprio no canal do sensor | `hardwaregain` passa a `info_mask_separate`, e `input_mux` ganha uma forma por canal (`in_voltage7_input_mux`); o canal do sensor recebe min(ganho, 12) **no driver** | É onde o M33 decide (`afe_sensor_gain`). Uma regra no userspace seria uma constante do conversor fora do kernel, que a regra 9 proíbe |
| D4 | Passo por canal no framework | opção `full_scale.<n>` (microvolts), além de `full_scale` | Vocabulário de transporte: um canal com outro passo, sem dizer por quê. A receita calcula, como já calcula `full_scale` |
| D5 | **Contar perdas de verdade** | **mudar a fiação**: DRDY no GP6, LED no GP5 (ou fora). Alternativa sem solda: nenhuma que funcione em produção | Só o contador de bordas vê uma conversão que ninguém leu. A troca é de hardware e de provisionamento da ponte (NVRAM), e é **decisão do dono da placa** |
| D6 | Prova de escrita | leitura de volta sem o cache (`regcache_cache_bypass`) depois de cada escrita de configuração, recusando a escrita se não bater | O custo é uma troca HID por registrador, só na configuração, nunca no caminho de amostra |

---

## 3. As fases

Cada fase traz o que muda, os testes de host e os de bancada. Um critério sem número é uma
afirmação, e uma injeção de falha que nunca reprovou nada é uma afirmação também.

### Fase 0 — os testes que ainda não existem

Nenhuma das fases seguintes tem hoje como ser testada no host. Esta fase constrói o que falta.

**0.a `tests/ads1299`: o driver do kernel no host.** No molde do `tests/mcp2210`:
`ti-ads1299.c` compilado byte a byte como embarca, contra *stubs* em `linux/`, e um conversor
falso (`fake_ads1299.c`) **abaixo** do driver, em `spi_sync`/`spi_async`. O falso guarda um
mapa de registradores com os valores de reset da Table 11 (os mesmos que a ponte leu do silício
em 2026-10-04, `BRINGUP_AFE.md` §4), devolve a palavra de status com os bits de lead-off que o
teste mandar, e sabe recusar uma escrita (para a injeção da fase 3). Os *shims* de IIO, regmap e
SPI são o custo grande da fase: medir antes de prometer.

**0.b O driver `iio` do framework alcançável.** Hoje ele lê `/sys` e `/dev` por caminho
absoluto. Proposta: a raiz do sysfs do dispositivo e o nó do buffer vêm do `address` que a
configuração já dá (hoje `/dev/med-afe-eeg0` e o link `/run/med-afe/eeg0`), e um teste monta uma
árvore falsa num diretório temporário com `scan_elements/`, `buffer/` e um FIFO no lugar de
`/dev/iio:deviceN`. Nenhuma opção nova só para teste.

**0.c O oráculo da onda quadrada, no repositório.** O gerador interno deriva do relógio das
conversões: cada meio período tem **exatamente** `taxa × 2²⁰ / f_CLK` amostras (128 a 250). Uma
conversão que faltar encurta um meio período em uma. Foi assim que se provou, em 2026-10-14, que o
`amp` não perde nada a 16000 (`RESULTS.md` §3). Vira `scripts/square-check.py`, que lê um
`raw.bin` e diz quantas amostras faltam, e é o juiz das fases 2 e 5.

| Teste 0 | Critério |
|---|---|
| `tests/ads1299` compila o driver sem `#ifdef` de teste | o arquivo é o mesmo que o bitbake compila (`cmp` do objeto não; do fonte, sim) |
| o falso reproduz o *probe* | o driver passa o próprio autoteste contra o falso |
| injeção: o falso devolve ID errado | o *probe* falha, como no silício |
| `square-check.py` contra os registros de 2026-10-14 | 0 faltando a 4000 e a 16000 |
| injeção: apagar um quadro do meio de um `raw.bin` | 14 amostras faltando, um meio período curto |

### Fase 1 — entradas em curto

**Depende da fase 2**, e por isso o número 1 é só de custo: com as entradas em curto não há
eletrodo para monitorar, então a linha de curto exige lead-off **desligado** - como no `amp` - e
hoje o `usb` não tem linha com lead-off desligado. Antes da fase 2, oferecer o curto seria
oferecê-lo com o registro dizendo "eletrodos monitorados".

**Muda**: só a receita. O ramo `spi`/`usb` deixa de recusar `afe.input = shorted`, traduz para
`input_mux = shorted`, e passa a recusar curto com lead-off ligado, curto com `test_signal` e curto
com `temperature`, pelas mesmas razões do `amp`.

**Host**: a função de tradução executada fora do bitbake (o `derive.py` de 2026-10-13 vira
`tests/derive/`): as linhas de curto existem, nenhuma tem lead-off ligado, e a contagem da tabela
do `usb` antes e depois fica registrada.

**Bancada** (imagem `usb`; chave SRB1 **na GNDA**, montagem bipolar, sem eletrodos):

| Critério | Número |
|---|---|
| `metadata.json`: `"input": "shorted"`; o BDF+ não diz EEG | exato |
| ruído RMS por canal, 60 s, ganho 24 | dentro de ±20 % do mesmo número pelo `amp`, na mesma placa e no mesmo dia (o `amp` também nunca mediu: as duas medidas saem juntas) |
| média por canal | a do `amp` ± 3 LSB |
| injeção: `afe.input = shorted` com a chave SRB1 em lugar nenhum | nada muda: o curto é dentro do conversor (prova que a medida não depende da fiação) |

### Fase 2 — lead-off de verdade

É a fase que acaba com o registro falso.

**Kernel** (D1, D2): atributo `lead_off` (`off`, `dc`); com `dc`, `LOFF` no valor de reset
(95 %/5 %, 6 nA, DC, escrito para ser lido de volta), `LOFF_SENSP` = canais com eletrodo,
`LOFF_SENSN` = os mesmos no bipolar ou só o bit 0 no referencial; com `off`, as duas máscaras em
0. A troca de montagem reescreve `LOFF_SENSN`. Um canal de buffer, `status`, com os 24 bits da
palavra de status de cada amostra.

**Framework**: o driver `iio` habilita o canal `status` quando ele existe, e por quadro faz o OU
dos bits como o M33 (`off_positive |= ...`), preenchendo `monitoredPositive/Negative` pelas
máscaras que **ele mesmo** lê dos atributos no `start()` - nunca por constante. Um status com o
nibble alto diferente de `1100` vira quadro abandonado (fase 3).

**Receita**: `lead_off_detection` passa a traduzir para `lead_off` (`dc`/`off`); as linhas com
"desligado" voltam a existir no `usb`.

**Host**:

| Teste | Critério |
|---|---|
| `tests/ads1299`: `lead_off = dc`, bipolar | `LOFF_SENSP = ff`, `LOFF_SENSN = ff`, `LOFF = 00`, iguais às de `afe_registers()` do M33 para a mesma prescrição (o produtor do M33 vira oráculo, compilado no mesmo teste) |
| idem, referencial | `LOFF_SENSN = 01` |
| idem, com o sensor no canal 8 (depois da fase 4) | o bit 7 sai das quatro máscaras |
| o falso devolve status com o eletrodo 3 positivo solto | o canal `status` do buffer traz o bit; o framework põe o canal 3 em `offPositive` |
| injeção: o driver não habilita o canal `status` | o framework diz "nada monitorado", e a receita **recusa** `lead_off = dc` para essa versão do driver (como a montagem fez) |
| injeção: inverter P e N na decodificação | o teste do eletrodo 3 reprova |

**Bancada** (bipolar, chave SRB1 na GNDA; um canal com dois resistores de 10 kΩ simulando
eletrodo, os outros abertos):

| Critério | Número |
|---|---|
| canais abertos | "solto" no indicador da HMI em até **5 s** (estimativa: 4,7 nF da placa × ~2,3 V / 6 nA ≈ 1,8 s para cruzar o limiar; não medido) |
| canal com resistores | "conectado" |
| tirar um resistor | o canal passa a "solto" em até 5 s; `ElectrodeContactChanged` na auditoria |
| `lead_off = off` | o metadado diz "não monitorado", e a HMI não mostra indicador |
| comparação | a mesma sequência pelo `amp`, no mesmo dia, dá os mesmos canais e tempos dentro de 1 s |
| integridade | `square-check.py` numa sessão com o gerador interno e lead-off **desligado**: 0 faltando (o lead-off não pode custar amostra) |

### Fase 3 — o que foi escrito é o que está no chip

**Kernel** (D6): depois de cada escrita de configuração (`CHnSET`, `CONFIG1..4`, `MISC1`,
`BIAS_SENS*`, `LOFF*`), uma leitura que passa por cima do cache; diferente do escrito, a escrita
falha com `-EIO` e o atributo devolve o erro. O bit 0 de `CONFIG3` (`BIAS_STAT`, só leitura) é
mascarado, como no M33.

**Framework**: a palavra de status de cada amostra é conferida (`1100`); fora disso, o quadro é
abandonado e conta como perda (a mesma regra do `amp`).

| Teste | Critério |
|---|---|
| `tests/ads1299`: o falso guarda `ff` como `7f` em `LOFF_SENSP` | `lead_off = dc` falha com `-EIO`; o serviço recusa a prescrição e volta à anterior (o caminho que o `session.configure` já tem) |
| o falso devolve status `0x40...` numa amostra | um quadro a menos, contado |
| bancada: trocar a montagem 20 vezes | 20 leituras de volta corretas; nenhuma sessão abre com prescrição recusada |

### Fase 4 — o sensor de temperatura no último canal

**Kernel** (D3): MUX por canal (`in_voltageN_input_mux`; o atributo global continua, valendo
para todos), `hardwaregain` por canal, e a regra do sensor: canal em `temperature` recebe
min(ganho, 12), dito em `in_voltage7_hardwaregain`. A escala publicada por canal segue o ganho do
canal.

**Framework** (D4): `full_scale.<n>` sobrepõe `full_scale` no canal `n`; a escala do kernel
continua sendo a conferência da declaração (`exactStep`), agora por canal.

**Receita**: o ramo `usb` deixa de recusar `afe.temperature = true`; traduz para
`in_voltage7_input_mux = temperature` e `full_scale.7` = referência / min(ganho, 12); o canal 8
sai das máscaras de lead-off e de bias (como `afe_registers()`).

| Teste | Critério |
|---|---|
| `tests/ads1299`: sensor, ganho 24 | `CH8SET` com PGA 12 e MUX 100; `CH1..7SET` com PGA 24 e MUX 000 |
| idem, ganho 6 | `CH8SET` com PGA 6 |
| framework: passo do canal 8 | `4,5 V / (12 × 2²³)`, os outros `4,5 V / (24 × 2²³)` |
| injeção: o driver ignora o teto de 12 | a bancada reproduz o −79 °C do `amp` em 2026-10-12 |
| bancada: canal 8 nos sete ganhos | **43 °C ± 2** (o `amp` leu 42–43 °C na mesma placa), e o mesmo número pelo `amp` no mesmo dia ± 1 °C |
| bancada: os outros sete canais | média e ruído iguais aos da sessão sem o sensor (o sensor não pode mexer nos vizinhos) |

### Fase 5 — uma perda deixa marca

Só começa depois da decisão D5.

**Hardware**: DRDY para o GP6, LED para fora do GP6; NVRAM da ponte reprovisionada para o GP6
contar bordas de descida (`implementation_plan_mcp2210.md` §13, o `counter`). O
`hid-mcp2210` da branch `board/*` já registra o `counter` só quando o GP6 está provisionado
assim (`tests/mcp2210`, seção A).

**Kernel**: o driver do conversor lê o contador do GP6 junto com cada amostra e publica, no canal
`status` ou num segundo, **quantas conversões houve desde a amostra anterior**. Atenção à volta:
o contador tem 16 bits e dá a volta a cada 65.536 bordas (262 s a 250 SPS), então a conta é
sempre uma diferença curta, nunca uma subtração de longo prazo (`CLAUDE.md`).

**Framework**: uma diferença maior que 1 é uma perda: o quadro em curso é abandonado e a
sequência pula, exatamente como `med_producer_on_lost`. O exportador já conta quadros faltantes.

| Teste | Critério |
|---|---|
| `tests/ads1299`: o falso informa 3 bordas entre duas amostras | o framework abandona o quadro e pula a sequência |
| idem, com a volta do contador entre duas leituras | nenhuma perda falsa |
| **bancada, injeção**: carregar o A35 até a ponte atrasar (`stress`/`dd` como em `RESULTS.md` §3) com o gerador interno ligado | o exportador conta quadros faltantes **e** `square-check.py` conta as mesmas amostras faltando, ±14 (um quadro) |
| bancada, sem carga, 30 min | 0 faltando pelos dois juízes |

---

## 4. Ordem, e o que bloqueia o quê

```
Fase 0 ── Fase 2 ──┬── Fase 1 (só receita e bancada)
                   ├── Fase 3
                   └── Fase 4 (as máscaras são da 2)
D5 (fiação) ── Fase 5
```

A fase 2 é a que importa e destrava as outras: até ela, o `usb` diz no registro uma coisa que não
fez, e o curto não tem como ser oferecido sem repetir o erro. Depois dela, a fase 1 é a mais
barata: só receita e uma bancada. Cada fase com kernel (2, 3, 4, 5) custa um ciclo
`linux-med` → imagem → placa; o `rauc install` não entrega kernel (`med-boot` é compartilhado),
então é cartão novo.

---

## 5. Riscos

| Risco | Efeito | O que fazer |
|---|---|---|
| Os *shims* da fase 0.a crescem mais que o driver | a fase 0 consome o plano | medir o tamanho dos *stubs* nos primeiros dois dias; se passar de ~1.500 linhas, testar só as funções puras (máscaras, decodificação do status) extraídas, e dizer isso |
| O canal `status` muda o tamanho de cada amostra no buffer | a ponte, que já está sem folga a 250, fica pior? | **não** muda o tráfego SPI (o status já vem na leitura); muda só o buffer do kernel. Medir `lost_samples` e o `square-check.py` antes e depois |
| ABI própria (D1, D3) | atrapalha o *upstream* | registrar no `implementation_plan_ads1299_upstream.md`; a forma *upstream* pode ser outra, e a do `board/*` fica |
| A troca de fiação da D5 | placa de AFE modificada, LED perdido | é decisão do dono; sem ela, a fase 5 não acontece e o plano diz isso, sem substituto fingido |
| Os tempos de lead-off da fase 2 | o critério de 5 s é uma conta, não uma medida | a primeira medida vira o critério, registrada como tal |
| Toda constante do datasheet continua `[SBAS499?]` | as fases interpretam silício com números não conferidos | a Fase 0 do `implementation_plan_afe_bench.md` continua bloqueando a afirmação final, não o código |

---

## 6. O que este plano não muda

- A regra 9: nenhum nome de peça entra no framework nem na aplicação. O framework continua
  falando de canais, passos, máscaras e bits; o que cada bit quer dizer no conversor vive no
  driver do kernel e na receita.
- A regra 6: o framework continua com duas dependências.
- O `amp`: nenhuma linha do firmware muda; ele é o oráculo.
- A taxa do `usb`: 250.

---

## 7. Validação

*(Vazio de propósito. Uma fase sem número medido e data é código escrito, não resultado.)*

| Fase | Critério | Resultado | Data |
|---|---|---|---|
| 0 | `tests/ads1299` e `square-check.py` de pé, cada um com uma injeção vista | — | — |
| 1 | curto pelo `usb`: ruído ±20 % do `amp`, mesma placa e dia | — | — |
| 2 | lead-off por canal na tela e na auditoria; desligado diz "não monitorado"; 0 amostras faltando | — | — |
| 3 | escrita recusada pelo chip vira recusa da prescrição | — | — |
| 4 | canal 8 a 43 °C ± 2 nos sete ganhos; vizinhos inalterados | — | — |
| 5 | perda forçada contada igual pelo exportador e pela onda quadrada | — | — |
