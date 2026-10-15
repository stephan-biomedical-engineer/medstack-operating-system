# Plano de implementação — o link `usb` com as funções do link `amp`

> **Status** (2026-10-15): fases 0 a 4 escritas e testadas no host (§7.1–§7.5); **nenhuma foi à
> bancada**. A fase 5 **não será feita**: a D5 foi decidida pela fiação atual (§7.6). O que segue é o levantamento de 2026-10-14,
> feito lendo
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
| D5 | **Contar perdas de verdade** | **mudar a fiação**: DRDY no GP6, LED no GP5 (ou fora). Alternativa sem solda: nenhuma que funcione em produção | Só o contador de bordas vê uma conversão que ninguém leu. A troca é de hardware e de provisionamento da ponte (NVRAM), e é **decisão do dono da placa**. **Decidida em 2026-10-15: a fiação fica como está** (§7.6) |
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

Só começava depois da decisão D5, e a D5 foi pela fiação atual: **esta fase não será feita** (§7.6).

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
| 0 | `tests/ads1299` e o juiz da onda quadrada de pé, cada um com uma injeção vista | **cumprida** — §7.1 | 2026-10-14 |
| 1 | curto pelo `usb`: ruído ±20 % do `amp`, mesma placa e dia | **receita e host prontos, bancada pendente** — §7.3 | 2026-10-15 |
| 2 | lead-off por canal na tela e na auditoria; desligado diz "não monitorado"; 0 amostras faltando | **código e host prontos, bancada pendente** — §7.2 | 2026-10-14 |
| 3 | escrita recusada pelo chip vira recusa da prescrição | **kernel, framework e host prontos, bancada pendente** — §7.4 | 2026-10-15 |
| 4 | canal 8 a 43 °C ± 2 nos sete ganhos; vizinhos inalterados | **kernel, framework, receita e host prontos, bancada pendente** — §7.5 | 2026-10-15 |
| 5 | perda forçada contada igual pelo exportador e pela onda quadrada | **não será feita**: D5 pela fiação atual — §7.6 | 2026-10-15 |

### 7.1 Fase 0 — o que foi feito, e onde se afasta do plano (2026-10-14)

**0.c, o juiz da onda quadrada.** Não virou `scripts/square-check.py`: o `amp-bench.py converter`
já julgava o sinal de teste interno, com o período fixo de 250 e pela média. Ele passou a tirar a
taxa do cabeçalho de cada quadro, e a contar meio período a meio período quantas amostras faltam
e sobram. Vale para qualquer link: o `raw.bin` de uma sessão é uma captura.

| Verificação | Resultado |
|---|---|
| os dois registros da placa de 2026-10-14 (`amp`, gerador interno) | 4000: 117 meios períodos de 2048, 0 faltando; 16000: 117 de 8192, 0 faltando |
| `tests/amp-bench`: capturas sintéticas, escritas da especificação | 13 verificações, 0 falhas: um quadro sumido com a sequência contínua (14 faltando por canal, a sequência não vê), o mesmo com lacuna, uma conversão a menos (1), uma duplicada (1 sobrando) |
| **injeções** no juiz: cego à falta; período fixo de 250 | 5 e 6 falhas |

**0.b, o driver `iio` do framework sob teste.** O plano propunha tirar a raiz do sysfs do
`address`; o que se fez é menor: `MED_IIO_DEVICES_DIR`, constante de compilação com o valor de
sempre (`/sys/bus/iio/devices`), que só a suíte redefine. E `writeSysfs` passou a abrir com
`O_TRUNC`, que é o que `echo valor > atributo` faz e o kernel ignora num atributo.
`tests/framework/test_iio.cpp` monta a árvore de arquivos comuns e um buffer feito de varreduras.

| Verificação | Resultado |
|---|---|
| partida, escrita e leitura de volta, buffer, decodificação com sinal, carimbo, recusas, autoteste | 51 verificações novas; a suíte toda, 1646, 0 falhas |
| **injeções** no driver: sem extensão de sinal; escala pelo texto do kernel; sem conferir a leitura de volta | 3, 3 e 2 falhas |
| o que a árvore de arquivos **não** imita | um atributo que recusa ou programa outro valor (imitados por um arquivo somente leitura e um link para `/dev/null`), `poll()` num buffer vazio, tempo |

**0.a, o driver do kernel no host.** `tests/ads1299`: `ti-ads1299.c` incluído como embarca, contra
um shim do kernel (`kernel_shim.{h,c}`, 725 linhas) e um conversor falso no núcleo SPI
(`fake_ads1299.{h,c}`, 313 linhas) - abaixo do limite de risco da §5 (1.500 linhas de *stubs*). O falso conhece o conjunto de
comandos, o arquivo de registradores com os valores de reset lidos do silício, o RDATAC (em que o
chip ignora comandos de registrador), a palavra de status e o que cada posição do MUX lê. O
`regmap` do shim tem o cache do driver, e é isso que decide se uma leitura chega ao chip.
O `afe_regs.c` do firmware do M33 é compilado junto, como oráculo.

| Verificação | Resultado |
|---|---|
| a suíte | **179 verificações, 0 falhas**, com ASan e UBSan |
| oráculo do M33 | montagem × bias (nas duas ordens), as sete taxas, os sete ganhos, gerador interno e curto: **os mesmos bytes** que o firmware escreve |
| o gerador pelo buffer, a 250 | todo meio período com 128 conversões |
| **defeitos confirmados**, um por fase do plano | fase 2: comparadores ligados e nenhum canal habilitado; fase 2: a palavra de status lida e descartada; fase 3: uma montagem que o chip não guardou, aceita e relatada pelo cache; fase 5: conversões feitas e não lidas que nada conta |
| **injeções** no driver: bias negativo no referencial; DR errado acima de 4000; ID lido sem SDATAC; perda não contada | 2, 3, 5 e 1 falhas |

A quarta injeção achou um defeito **do teste**: a condição do defeito da fase 5 comparava com um
número esperado e "se resolvia" quando o driver deixava de contar perdas. Passou a medir
conversões não contabilizadas (feitas − lidas − contadas como perdidas).

**O que isto não significa**: o falso foi escrito da mesma leitura do datasheet que o driver,
exceto onde o silício já respondeu e onde o M33 é o oráculo - uma constante errada nos dois lados
passa. O shim não tem tempo, concorrência nem contexto de interrupção. E nada disto rodou em
placa: a fase 0 é o que as fases seguintes vão usar para se provar antes de chegar lá.

### 7.2 Fase 2 — o que foi escrito (2026-10-14)

**Kernel (`linux-med`, `ti-ads1299.c`).** O atributo `lead_off` (`off`, `dc`) programa `LOFF`
(no valor de reset, escrito), `LOFF_SENSP` (todo canal) e `LOFF_SENSN` (todo canal no bipolar,
só o bit do eletrodo de referência no referencial) e liga os comparadores por último; desligar
os apaga primeiro. A troca de montagem reescreve `LOFF_SENSN`. **No *probe* o lead-off fica
desligado**, comparadores inclusive: o estado antigo - comparadores ligados e nenhum canal - era
o registro falso. `lead_off_monitored` diz quais eletrodos a palavra cobre (`ff ff`, `ff 01`,
`00 00`), e o buffer ganhou o canal `in_voltage8_lead_off`: `(positivo << 8) | negativo`, tirado
da palavra de status de cada leitura e já sem os bits que não são sensoriados. Trocar
`lead_off` com o buffer ligado é recusado (`-EBUSY`).

Duas escolhas que se afastam do plano. A palavra entregue não é a palavra de status crua (D1
dizia os 24 bits): o *layout* do status é fato do conversor, e entregá-lo cru poria esse fato no
framework, contra a regra 9. O kernel decodifica, mascara, e publica um formato próprio dele. E o
framework não deduz o monitorado da montagem: lê `lead_off_monitored`.

**Framework (`IioDevice`).** Com o canal presente, habilita-o, confere o tipo (`u16/32>>0`), lê
`lead_off_monitored` e preenche o bloco de lead-off de cada quadro com o OU das amostras -
exatamente o que o M33 faz no `amp`. Sem o canal, "nada monitorado". Com o canal e sem a lista,
ou com outro tipo, recusa.

**Receita.** `afe.lead_off_detection` traduz para `lead_off` (`dc`/`off`); "desligado" deixou de
ser recusado; gerador interno com lead-off ligado passou a ser recusado, como no `amp`. A tabela
do `usb` foi de **28 para 42 linhas** (lead-off desligado em 28, ligado em 14). Um kernel sem o
atributo faz o driver recusar a opção e o serviço não sobe - como com a montagem.

| Verificação | Resultado |
|---|---|
| `tests/ads1299` | **245 verificações, 0 falhas**; os dois defeitos da fase 2 viraram verificação; restam os das fases 3 e 5 |
| oráculo do M33, lead-off | montagem × lead-off, nas duas ordens: `LOFF`, `LOFF_SENSP`, `LOFF_SENSN` e `PD_LOFF_COMP` iguais aos de `afe_registers()` |
| a palavra no buffer | bipolar: 3+ e 5- soltos chegam como `0x0410`; referencial: um 5- solto não chega, o eletrodo de referência chega; desligado: nada |
| **injeções** no kernel: oito fontes no referencial; bits não sensoriados passando; P e N trocados; comparadores ligados no *probe* | 4, **0**, 2 e 1 falhas |
| a injeção que passou | o conversor falso só produzia bit onde havia sensoriamento, então a máscara do driver nunca era exercitada. O falso passou a produzir os bits não sensoriados também (o datasheet não diz o que eles leem; o pior caso é parecerem medida), e a injeção dá **1 falha** |
| `tests/framework` | 22 verificações novas, 1668 no total, 0 falhas |
| **injeções** no framework: palavra no lugar errado; máscara ignorada; canal ignorado | 4, 2 e 12 falhas |
| contra um kernel de verdade | `make -C scripts/oot-modules W=1` (Ubuntu 6.8.0-147): compila sem aviso; `checkpatch --strict` no diff: 0/0/0 |

**O que falta, e é a bancada.** Imagem `make stm32 KEY=development KERNEL=med LINK=usb`, e o
roteiro da fase 2 na §3: resistores num canal, os outros abertos, tirar um resistor, desligar a
detecção, comparar com o `amp` no mesmo dia, e o juiz da onda quadrada numa sessão com lead-off
desligado. O código do kernel precisa estar **commitado** no `linux-med` para entrar na imagem
(`KERNEL=med` constrói o `HEAD`).

### 7.3 Fase 1 — o que foi escrito (2026-10-15)

**Receita.** O ramo `spi`/`usb` de `do_derive_device_options` deixou de recusar `afe.input =
shorted`: traduz para `input_mux = shorted` e recusa curto com lead-off ligado, como o `amp`.
Curto com sinal de teste e curto com sensor já eram recusados antes do ramo, para todo link.
Nada mudou no kernel (o `input_mux = shorted` existe desde o *probe* e o `tests/ads1299` já o
confere contra os bytes do M33), no framework nem na tela, que já oferecia o curto pelo `amp`
com o aviso "BENCH: INPUTS SHORTED". A tabela do `usb` (e a do `spi`, mesmo ramo) foi de **42
para 56 linhas**: 7 ganhos × 2 montagens, todas com lead-off desligado, sem gerador, sem sensor.

Uma escolha que se afasta da regra da receita: **a linha entra antes de ser medida** neste link.
A bancada que a mede precisa dela na tela, como as taxas do `amp` em 2026-10-13. O que a
sustenta até lá: o comutador é programado com os mesmos bytes que o produtor do `amp` escreve, e
o registro diz `shorted`, então o arquivo clínico nunca chama aquilo de EEG.

**`tests/derive`.** Não é o `derive.py` de 2026-10-13 copiado: tira `do_derive_device_options`
e `do_seal_configuration` do texto da receita como estão, roda-os com substitutos de `d` e `bb`
sobre o `eeg.conf` de fábrica, e confere os arquivos que uma imagem embarcaria - nunca o interior
da função. Entrou no `make test`.

| Verificação | Resultado |
|---|---|
| a suíte | **1348 verificações, 0 falhas** |
| o tamanho de cada tabela, com o motivo de cada mudança escrito no teste | `simulated` 700, `amp` 490, `spi` 56, `usb` 56 |
| as 14 linhas de curto do `usb` e do `spi` | `input_mux = shorted`, `lead_off = off`, `test_signal = off`, `record.input = shorted`, as duas montagens e os sete ganhos |
| em nenhum link com conversor | curto com lead-off, com gerador ou com sensor; e uma entrada normal nunca chega ao comutador como curto |
| um `eeg.conf` de fábrica com curto | com lead-off ligado, o build recusa nomeando a chave (`amp`, `spi`, `usb`); desligado, deriva e o registro diz `shorted` |
| **injeções** na receita: sem a recusa curto+lead-off; curto traduzido como `normal`; o estado de antes (curto recusado) | 36, 4 e 10 falhas |

O primeiro rascunho da suíte exigia "nenhum curto com lead-off" também do `simulated`, e
reprovou: lá é decisão escrita (sem eletrodos, qualquer valor é o que ele faz). O erro era do
teste, e a regra passou a valer só para links com conversor.

**O que falta, e é a bancada** (§3, fase 1): imagem `usb`, chave SRB1 **na GNDA**, montagem
bipolar, sem eletrodos; escolher o curto na tela; `metadata.json` com `"input": "shorted"` e o
BDF+ sem rótulo EEG; ruído RMS e média por canal em 60 s a ganho 24, contra o mesmo pelo `amp` na
mesma placa e no mesmo dia (nenhum dos dois foi medido ainda); e a injeção da chave SRB1 em lugar
nenhum. Vai na mesma imagem da bancada da fase 2, que ainda não foi construída.

### 7.4 Fase 3 — o que foi escrito (2026-10-15)

**Kernel (`linux-med`, `ti-ads1299.c`).** O `reg_write` do regmap lê de volta, pelo barramento,
cada registrador que escreve, e uma diferença é `-EIO` com o registrador, o escrito e o lido no
`dmesg`. Ficou no `reg_write` e não em cada atributo porque tudo passa por ali: *probe*,
autoteste, os sete atributos de configuração e o `debugfs`. A máscara é a do M33, só o bit 0 do
`CONFIG3` (`BIAS_STAT`); o kernel não escreve `GPIO`, `MISC2` nem `LOFF_FLIP`, que teriam bits de
leitura. Custa uma troca a mais por registrador, só na configuração.

Um achado que o plano não previa, e é o que dá sentido ao resto: **o regmap do 6.6 escreve o cache
antes do barramento** (`_regmap_write()`), então uma escrita que falha deixa no cache o valor
recusado, e o atributo, o próximo `update_bits` e o registro passariam a relatá-lo. Por isso as 27
escritas de configuração do driver passam por `ads1299_write()`/`ads1299_update_bits()`, que
chamam `regcache_drop_region()` na falha: a próxima leitura vai ao chip. O shim do
`tests/ads1299` tinha a ordem trocada (cache depois do barramento, só em sucesso), a ordem em que
esse defeito não aparece; agora tem a do kernel.

A palavra de status é conferida em toda leitura de conversão: o nibble `1100`, decidido no kernel
como o M33 decide (`afe_status_valid()`). Fora disso, no buffer, a amostra não é entregue e conta
em `lost_samples`; numa leitura avulsa, `-EIO`. Isso se afasta do plano, que punha a conferência
no framework: depois da fase 2 o *layout* do status é do kernel (regra 9), e o framework nunca vê
a palavra crua.

**Framework (`IioDevice`).** Lê `lost_samples` ao ligar o buffer e depois de cada quadro. Se o
contador subiu enquanto o quadro se enchia, o quadro é abandonado inteiro e o número de sequência
é pulado - a regra do `amp` (`med_producer_on_lost`). Vale para **toda** perda que o kernel
conta, não só a nova: as amostras pedidas pelo timer do host enquanto uma transferência estava em
curso já eram contadas desde a fase 0 e não deixavam marca nenhuma no registro. Um front-end sem o
atributo adquire como antes; um contador ilegível impede a partida ou o quadro (`IoError`), sem
palpite.

Dois limites, ditos. O lugar da marca é tão bom quanto a pontualidade do contador: um leitor
atrasado em relação ao buffer pode ver o contador subir ainda lendo varreduras de antes da perda,
e a lacuna cai até dois quadros antes (o tamanho do buffer); a marca nunca se perde. E, como no
`amp`, a lacuna de um quadro diz "faltou algo aqui", não quantas conversões: o quadro abandonado
tinha 14 varreduras e a perda pode ter sido de uma. As conversões que o conversor fez e ninguém
pediu continuam invisíveis: isso é a fase 5.

**O serviço.** Nada mudou. O recuo que o plano pede já existe (`main.cpp`, `session.configure`):
uma partida que falha reabre o dispositivo com as opções anteriores e recusa o pedido. Com o
kernel novo, uma escrita que o chip não guardou chega lá como `-EIO` no atributo. Esse caminho não
tem teste de host; o `make check` o exercita no QEMU, pelo link `simulated`.

| Verificação | Resultado |
|---|---|
| `tests/ads1299` | **272 verificações, 0 falhas**; o defeito da fase 3 virou verificação; resta o da fase 5 |
| toda escrita de configuração | seguida de uma leitura do mesmo registrador |
| `MISC1` com o SRB1 preso aberto | `montage = referential` falha com `-EIO`, o atributo continua `bipolar`, a derivação do bias volta à bipolar, e a próxima leitura de `MISC1` vai ao chip e lê `0` |
| o caso do plano: `ff` guardado como `7f` em `LOFF_SENSP` | `lead_off = dc` falha com `-EIO`, continua `off` com `00 00` monitorado, os comparadores nunca ligam; sem a falha, o mesmo pedido passa |
| barramento que falha no meio de uma escrita | o registro é esquecido e a próxima leitura vai ao chip |
| `BIAS_STAT` posto pelo chip | não reprova a escrita do `CONFIG3` |
| o nibble, kernel contra M33 | os 256 primeiros bytes possíveis, mesma decisão |
| resposta fora de passo no buffer | 10 conversões, 9 varreduras, `lost_samples = 1`; numa leitura avulsa, `-EIO`, e a seguinte volta a dar valor |
| **injeções** no kernel: sem comparar; sem esquecer o cache; sem a máscara; buffer sem conferir; leitura avulsa sem conferir; nibble errado | 9, 5, 1, 2, 1 e 3 falhas (a última reprova no *probe*: o autoteste) |
| o shim com a ordem antiga e sem esquecer o cache | a verificação do **sintoma** (o valor lido) passa; só as do mecanismo reprovam - a razão de o shim ter mudado |
| `tests/framework` | 13 verificações novas, **1681** no total, 0 falhas |
| **injeções** no framework: abandona sem pular a sequência; pula e entrega o quadro com a perda; ignora o contador | 2, 2 e 4 falhas |
| contra um kernel de verdade | `make -C scripts/oot-modules W=1` (Ubuntu 6.8.0-147): sem aviso do código; `checkpatch --strict` no diff: 0/0/0 |

**O que falta, e é a bancada.** Na imagem `usb` de sempre (o kernel muda, então é cartão novo):
trocar a montagem 20 vezes e ver 20 leituras de volta corretas, sem `register .. read back` no
`dmesg`; nenhuma sessão abrindo com uma prescrição recusada; e `lost_samples` contra as lacunas
da sequência numa sessão de 10 minutos, que pela primeira vez devem bater.

### 7.5 Fase 4 — o que foi escrito (2026-10-15)

**A forma se afasta da D3, e por um motivo que só apareceu ao ler o framework.** A D3 pedia
`input_mux` e `hardwaregain` por canal. Mas o framework escreve as opções em **ordem alfabética
de chave** (`applyOptions`, que diz no próprio comentário que um driver dependente de ordem se
corrige no driver): `in_voltage7_input_mux` seria escrito antes de `input_mux`, e o atributo
global desfaria o canal 7. O que se fez é um atributo só, **`temperature_channel`** (`none` ou o
número do canal), e um driver que guarda o estado - ganho prescrito, MUX dos eletrodos, canal do
sensor - e reprograma todos os `CHnSET` a partir dele sempre que um muda. A ordem das escritas
deixou de importar. `hardwaregain` continua um atributo só e passou a devolver a prescrição; o
ganho real de cada canal está na escala dele.

**Kernel (`linux-med`, `ti-ads1299.c`).** O canal do sensor recebe MUX `temperature` e
min(ganho, 12) - a mesma regra de `afe_sensor_gain()` no M33 - e sai da derivação do bias e do
lead-off (`ads1299_electrodes()`), como em `afe_registers()`. As máscaras saem antes de o MUX ir
ao sensor e voltam depois de ele voltar aos eletrodos. Trocar o canal com o buffer ligado é
recusado (`-EBUSY`), e uma troca que o chip recusa (leitura de volta da fase 3) desfaz tudo.

**Framework (`IioDevice`).** Dois achados, e o primeiro existia antes da fase. **O framework lia
só `in_voltage0_scale` e aplicava essa escala aos oito canais**: com o sensor a ganho 12 e o
resto a 24, o canal 8 sairia com o dobro do valor, plausível e calado. A escala agora é lida e
conferida canal a canal. E `full_scale.<n>` declara o passo exato de um canal; sem a declaração,
a escala desse canal no kernel reprova a partida, nomeando o canal. `full_scale.<n>` sem
`full_scale` é recusado: seria um canal exato e sete pelo texto do kernel.

**Receita.** O ramo `spi`/`usb` traduz `afe.temperature` para `temperature_channel = 7` e
`full_scale.7` = referência / min(ganho, 12). O teto, assim, está dito em dois lugares: no
kernel, que o aplica, e na receita, que o declara. Isso é de propósito: o framework confere um
contra o outro, e um driver e uma tabela que discordem param a partida em vez de gravar o sensor
pela metade. Toda linha diz `temperature_channel`, `none` inclusive, porque o kernel guarda o
valor de uma escolha para a outra. A tabela do `usb` foi de **56 para 98 linhas** (7 ganhos × 2
montagens × lead-off ligado, desligado, gerador). A linha entra antes da medida pelo `usb`, como o
curto, e pelo mesmo motivo.

| Verificação | Resultado |
|---|---|
| `tests/ads1299` | **1874 verificações, 0 falhas** (o laço do oráculo é a maior parte) |
| oráculo do M33, com o sensor | 7 ganhos × 2 montagens × bias × lead-off × gerador × 2 ordens de escrita = **224 combinações**: os oito `CHnSET`, os quatro registradores de sensoriamento, `MISC1`, `CONFIG3` e `CONFIG4` iguais aos de `afe_registers()` |
| os casos do plano | ganho 24: `CH8SET` com PGA 12 e MUX 100, os outros PGA 24 e MUX 000; ganho 6: `CH8SET` com PGA 6 |
| escala por canal | canal 8 a 4,5 V / (12 × 2²³), os outros a 4,5 V / (24 × 2²³); `hardwaregain` lê 24 |
| lead-off com o sensor | `lead_off_monitored` = `7f 7f` |
| recusas | canal 8 ou `seven`: `-EINVAL`; com o buffer ligado: `-EBUSY`; `CH8SET` que não aceita o MUX: `-EIO`, e o bias volta a somar os oito |
| **injeções** no kernel: sem o teto; sensor nas máscaras; estado zerado no init; sem desfazer numa recusa; canal do sensor sem o MUX do sensor | 3, 2, 3 (o *probe* reprova: o autoteste vê o canal 0 no sensor), 2 e 6 falhas |
| `tests/framework` | 17 verificações novas, **1698** no total, 0 falhas |
| **injeções** no framework: um passo para todos; só a escala do canal 0; `full_scale.N` ignorado | 2, 5 e 3 falhas |
| `tests/derive` | **1452 verificações, 0 falhas**; as 42 linhas do sensor com `temperature_channel = 7` e `full_scale.7` = 4,5 V / min(ganho, 12); toda outra linha com `none` |
| **injeções** na receita: passo sem o teto; `temperature_channel` omitido; o estado de antes | 3, 4 e 7 falhas |
| contra um kernel de verdade | `make -C scripts/oot-modules W=1`: sem aviso do código; `checkpatch --strict`: 0/0/0; `make risks`: 0 erros |

**O que o host não vê.** O conversor falso devolve um código plausível para o sensor a qualquer
ganho (145,3 mV × 24 ainda cabe na escala dele); o −79 °C do `amp` a ganho 24 é do silício, e a
injeção "sem o teto" só é reprovada aqui pelos bytes, não pela leitura. A ordem das máscaras em
relação ao MUX também não: o falso não guarda estados intermediários.

**O que falta, e é a bancada** (§3, fase 4): o canal 8 a 43 °C ± 2 nos sete ganhos, contra o
`amp` no mesmo dia ± 1 °C; os outros sete canais com média e ruído iguais aos de uma sessão sem o
sensor. Uma imagem com esta receita sobre um kernel sem `temperature_channel` **não adquire** (o
serviço recusa a opção), como com a montagem e o lead-off: o kernel tem de ser o desta fase.

### 7.6 D5 decidida: a fiação fica (2026-10-15)

O dono da placa decidiu manter a fiação: DRDY no GP5, LED de status no GP6. A fase 5 não será
feita, e o plano diz isso sem substituto fingido, como a §5 pedia.

**O que o registro do `usb` marca, e o que não marca.** Desde a fase 3 toda perda que o **kernel**
conta deixa lacuna na sequência: uma amostra pedida pelo timer do host enquanto a transferência
anterior estava em curso, e uma resposta fora de passo. Uma conversão que o conversor fez e que o
host **nunca pediu** - o timer do host atrasado em relação ao relógio do conversor - não é vista
por ninguém, porque só o contador de bordas do GP6 a veria e o GP6 tem o LED. Num registro do
`usb` uma sequência sem lacuna quer dizer "nada que o kernel tenha visto se perdeu", não "nada se
perdeu". Quem precisa da segunda frase tem o juiz da onda quadrada (`amp-bench.py converter`, fase
0.c), que conta as faltas pelo sinal de teste interno, numa sessão com o gerador ligado.

Isso não é novo: é o que `acquisition.link = usb` já declara em cada sessão, e é uma das razões
de o `amp` ser o link do produto. O `tests/ads1299` mantém o caso como **DEFEITO** contado à parte
(as conversões que ninguém pediu), para que continue visível; ele não reprova a suíte.

