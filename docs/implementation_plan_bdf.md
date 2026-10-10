# Plano de Implementação — O registro clínico do EEG em BDF+, com montagem referencial por padrão

> **Status**: **Fases 0 a 3 executadas** (2026-10-09, §6). Toda sessão encerrada no QEMU ganha um
> BDF+ selado, um `+C` por trecho contínuo, conferido no alvo (`make check` 26/26) e por três
> leitores que não são o escritor. No caminho, o registro deixou de passar as amostras por `float`.
> Faltam a placa (Fase 4), a montagem referencial (Fase 5) e paciente e operador (Fase 6). O plano foi escrito em 2026-10-09, no dia em que o
> serviço passou 11 h adquirindo pelo link `amp` na placa e a única forma de olhar uma amostra era
> `od` sobre o `raw.bin`. O que já existe e este plano usa: o `raw.bin` de cada sessão em
> `/data/eeg/session-<id>/`, com CRC por quadro, sequência e carimbo do M33 (`med_amp_abi.h`); o
> `metadata.json` ao lado dele; os eventos de lead-off que já vão para a auditoria; e o
> `scripts/amp-bench.py`, que é uma segunda leitura do formato de fio escrita sem olhar o código do
> framework. O que **não** existe: nenhum arquivo que um software clínico abra, nenhuma montagem
> referencial em nenhum dos links (o firmware deixa o SRB1 em zero, `afe_regs.c:37`) e nenhum rótulo
> de eletrodo em lugar nenhum.
>
> **Escopo**: gerar, para cada sessão, um arquivo **BDF+** ao lado do `raw.bin`. O BDF+ é
> derivado do `raw.bin`, sem perda e de forma verificável. Junto, o plano torna a **montagem
> referencial** a prescrição padrão, e declara os eletrodos de cada canal na configuração, porque
> sem isso o arquivo é válido mas não é clínico.
>
> **Relação com os outros planos.** O `implementation_plan_eeg_control.md` é dono da **sessão**: o que
> a abre e a fecha, quem é o operador, o paciente e as anotações do operador. Este plano é dono do
> **arquivo**: o que entra nele e como. Ele funciona com as sessões de hoje, uma por vida de processo,
> e aproveita as do plano de controle quando elas existirem (Fase 6). O
> `implementation_plan_afe_characterization.md` D5 já previa `afe.montage`. Este plano o implementa,
> e inverte o padrão que lá era `bipolar`.
>
> **Leitura obrigatória antes**: `med_amp_abi.h` inteiro (o formato de que o BDF é derivado), o
> `main.cpp` do serviço entre a abertura e o fechamento da sessão, e o
> `implementation_plan_eeg_control.md` §2 e §6.

---

## 0. A linha, dita primeiro

| | Hoje |
|---|---|
| Registro da sessão | `raw.bin` no formato de fio, autoritativo, com CRC por quadro. **Nenhum software clínico o abre** |
| Metadado | `metadata.json` com link, canais, taxa, unidade, `config_digest`. **Sem ganho, sem referência, sem escala, sem montagem**: só o digest diz qual prescrição foi usada, e só para quem tiver o `eeg.conf` que bate com ele |
| Fim de sessão | só o `AcquisitionStopped` no journal. Nada no diretório da sessão diz que ela terminou, nem se terminou bem |
| Montagem | **diferencial em todos os links**, por omissão e não por decisão: o SRB1 nunca é ligado |
| Rótulo de canal | nenhum. Canal é índice |
| Data e hora | relógio de parede de uma placa **sem RTC inicializado** (`BRINGUP_STM32MP2.md` §9.7) |
| Paciente, operador | não existem (`implementation_plan_eeg_control.md` §2) |

---

## 1. Objetivo

Que toda sessão fechada tenha, ao lado do `raw.bin`, um `record.bdf` que o EDFbrowser, o MNE e o
EEGLAB abram sem configuração, com:

- **os mesmos códigos que o conversor entregou.** A conversão é sem perda, e isso é verificado em
  cada amostra, não presumido;
- **rótulos clínicos** quando a montagem estiver declarada, e rótulos genéricos honestos quando não
  estiver;
- **as descontinuidades à vista.** Uma lacuna no `raw.bin` vira uma lacuna no BDF, nunca amostras
  coladas;
- os eventos do registro (lead-off, sinal de teste, sessão interrompida, relógio não confiável) como
  anotações;
- um `.sha256` e um registro de auditoria.

### O que este plano **não** faz

- **Não substitui o `raw.bin`.** Ele continua sendo o registro, por D1.
- **Não escolhe onde os eletrodos vão na cabeça.** A configuração declara e o arquivo registra o
  que foi declarado. Escolher a montagem clínica é decisão de quem usa o aparelho.
- **Não corrige o RTC.** O plano torna o problema visível no arquivo (D8), e a correção é outro
  trabalho.
- **Não gera DICOM.** Fica como exportação futura, se um destino PACS aparecer.
- **Não mexe na HMI.**

---

## 2. Decisões que este plano toma

**D1 — O `raw.bin` é o registro e o BDF é uma derivação determinística dele, nunca uma escrita
paralela.** O `raw.bin` carrega o que o BDF não tem onde guardar: o CRC de cada quadro, a sequência
e o carimbo do núcleo de tempo real. Escrever os dois em paralelo durante a aquisição abriria a
possibilidade de os dois divergirem, e com dois registros divergentes ninguém sabe qual é o
verdadeiro. Derivar significa que o mesmo `raw.bin` gera sempre o mesmo `record.bdf`, byte a byte.
Qualquer um pode refazer a conversão e comparar o hash.

**D2 — BDF+ (24 bits), não EDF+ (16 bits).** O conversor entrega 24 bits. O EDF+ obriga a escolher
entre jogar fora 8 bits de resolução e estreitar a faixa, e estreitar a faixa satura com o offset
DC de dezenas de mV de um eletrodo num conversor acoplado em DC. O BDF+ tem a mesma estrutura do
EDF+, as mesmas anotações e as mesmas convenções de rótulo, com amostras de 3 bytes. Os leitores-alvo
abrem os dois. Exportar para EDF+ seria uma conversão **com perda**, e por isso fica fora deste
plano.

**D3 — Um registro de dados do BDF é um quadro de transporte.** A duração do registro é
`samplesPerChannel / taxa`: 14/250 = 0,056 s no `amp` e 25/250 = 0,1 s no `simulated`. A
alternativa óbvia, registros de 1 s, foi rejeitada. Os 14 não dividem os 250, e uma lacuna no meio
de um registro de 1 s deixa um registro incompleto. Não há o que fazer com ele que não seja mentir:
completar com valores inventados ou descartar amostras boas. Com registro igual a quadro, uma
lacuna de quadros é uma lacuna de registros, e o BDF+D a representa exatamente. O custo está nos
riscos (§9): um acoplamento entre o arquivo e a geometria do transporte, e uma duração não inteira
que precisa ser testada nos leitores.

A Fase 0 conferiu a legitimidade disso. O EDF+ só **recomenda** registros de um número inteiro de
segundos ("recommended to be a whole number of seconds") e admite registros mais curtos que 1 s
"unconditionally". A descrição do BDF+ vai além: "any datarecord duration greater than 0 is
allowed", com até 15 MB por registro. D3 não viola nenhuma das duas especificações. Ela só se afasta
de uma recomendação do EDF+ que o BDF+ não repete, e o risco que sobra é de leitor, não de formato.

~~O arquivo é **BDF+C** quando a sessão não tem lacuna, e **BDF+D** quando tem.~~ **Revisto em
2026-10-09, depois da Fase 2:** todo arquivo é **BDF+C**, e uma sessão com lacuna vira **um arquivo
por trecho contínuo** (`record-001.bdf`, `record-002.bdf`, …). Sem lacuna, continua sendo um
`record.bdf` só. A Fase 2 mediu que o pyedflib recusa o `+D` e o MNE o concatena em silêncio,
desalinhando toda anotação depois da lacuna. Com um `+C` por trecho, nenhum leitor desalinha nada. A
lacuna fica dita duas vezes, no fim de um arquivo (`gap follows: N frames`) e no começo do seguinte
(`after gap: …`), e o início de cada arquivo é o que a sequência dos quadros diz. Um produtor que
reinicia a sequência também abre um trecho novo, com `gap unknown`, porque a duração da ausência não
está no registro.

**D4 — A conversão roda fora do processo de aquisição, e a aquisição nunca espera por ela.** Uma
sessão de 12 h tem ~350 MB de `raw.bin`. Converter isso no caminho de parada do serviço atrasaria o
`stop`. Converter no caminho de partida atrasaria o `READY=1` e, com ele, a confirmação do slot A/B,
porque o `rauc-mark-good` espera pela aquisição. Então:

- Um segundo binário, `eeg-record-export`, no **mesmo pacote** do serviço. Ele linka só
  `medframework` (regra 6). Ficar no mesmo pacote mantém em 3 o delta de pacotes do EEG, que é uma
  medida da tese (`RESULTS.md`).
- Uma unidade `eeg-record-export.service`, `Type=oneshot`, idempotente: converte toda sessão
  fechada que ainda não tem `record.bdf` e não faz mais nada.
- Ela é acionada por `ExecStopPost=+/usr/bin/systemctl start --no-block eeg-record-export.service`
  na unidade de aquisição, que retorna na hora, e uma vez no boot, **depois** da aquisição e nunca
  antes dela. Quando o plano de controle der um `session.stop` que não termina o processo, esse verbo
  passa a ser o terceiro gatilho (Fase 6).

**D5 — Uma sessão viva nunca é convertida, e uma sessão interrompida é convertida e diz isso.** O
serviço escreve o id da sessão corrente em `/run/medplatform/session`. É o `RuntimeDirectory` da
unidade, e o systemd o apaga quando o serviço para. No fechamento normal, o serviço escreve
`closed.json` no diretório da sessão, com o instante e os mesmos contadores do `AcquisitionStopped`.
O conversor trata três casos:

| Estado da sessão | O conversor |
|---|---|
| tem `closed.json` | converte |
| não tem, e é a sessão de `/run/medplatform/session` | **não toca**: está viva |
| não tem, e não é a corrente | converte, com a anotação "registro interrompido" no fim. Foi uma queda de energia ou um processo morto, e o arquivo tem de dizer isso |

**D6 — O escritor fica no framework, sem um sétimo cabeçalho e com vocabulário de biossinal.** O
BDF+ serve para ECG, EMG e polissonografia, não só para EEG. O escritor entra em `MedicalStorage.h`
e fala de sinal, rótulo, dimensão física e anotação. Ele não sabe o que é um eletrodo nem uma
montagem. Os rótulos clínicos vêm do app. Isso mantém os seis cabeçalhos que a tese conta, mantém o
framework sem dependência nova (o formato é texto ASCII e inteiros little-endian, sem `edflib`) e
passa no `grep` da regra 9. O `grep` é necessário mas não suficiente, então o escritor também não
pode ter um padrão que só faça sentido para EEG.

**D7 — Sem perda, e verificado em cada amostra.** O `raw.bin` guarda nanovolts. O BDF guarda o
código do conversor. A conversão é `código = round(nV / LSB)`, com `LSB = full_scale / 2^(bits−1)`.
No `amp`, isso dá `4,5 V / 24 / 2^23 ≈ 22,35 nV`. Como o LSB é maior que 1 nV, o arredondamento
recupera o código exato, **desde que o LSB declarado seja o verdadeiro**. Por isso o conversor
exige `|nV − código·LSB| ≤ 0,5 nV` em **toda** amostra e recusa a sessão inteira na primeira que
falhar. Uma sessão gravada com um ganho e convertida com outro deixa um resíduo que não é múltiplo
do LSB, e isso aparece já na primeira amostra. É o mesmo erro silencioso que o comentário sobre
`scaleNanoUnitsPerLsb` descreve, pego na saída em vez de na entrada.

Isso exige que o `metadata.json` passe a carregar a escala efetiva: `full_scale` e
`resolution_bits` como o link os declarou. O `simulated` já tem os dois como opções (`full_scale`,
`resolution_bits`, `MedicalDevice.cpp:274`), e no `amp` eles saem de `afe.gain` e
`afe.reference_uv`. Uma sessão anterior a este plano não tem esses campos e é **recusada**, com o
motivo. O conversor não pode deduzir a escala de um `eeg.conf` que pode não ser o da sessão.

**D8 — A data entra no arquivo como o relógio a deu, e o arquivo diz se o relógio era
confiável.** O cabeçalho do EDF exige uma data, e não existe "data desconhecida". O serviço registra
no `metadata.json` se o relógio estava sincronizado quando a sessão abriu. Se não estava, o BDF ganha
uma anotação no instante 0, "relógio não sincronizado". Um aparelho sem RTC produz arquivos com data
errada, e este plano não esconde isso dentro de um cabeçalho de aparência normal. A fonte do "estava
sincronizado" é decidida na Fase 1, e ela só pode vir do sistema, nunca de um palpite do serviço.

**E a hora é local, não UTC.** A Fase 0 achou isto no item 8 das especificações adicionais do EDF+:
a hora de início é a hora local do lugar onde o paciente está. O `metadata.json` e a auditoria usam
UTC (`MED_AUDIT_TIME=…Z`), e a imagem não configura fuso. Então o cabeçalho do BDF usa a hora local
do fuso configurado no sistema. O `metadata.json` passa a registrar o deslocamento de UTC em vigor na
abertura da sessão, e o BDF ganha no instante 0 a anotação `UTC offset +HH:MM`. Com o fuso que a
imagem tem hoje, isso dá `+00:00`, e o arquivo diz isso em vez de deixar o leitor presumir. Escolher
o fuso de um aparelho é decisão de produto e fica fora deste plano.

**D9 — Paciente anônimo até existir paciente.** O campo de paciente do EDF+ tem quatro
subcampos: código, sexo, nascimento e nome. Enquanto o `session.start` do plano de controle não
carregar um `subject`, todos são `X`. O campo de gravação recebe a data por extenso, o id da sessão
como código administrativo, `X` para o técnico e o equipamento como `device_id` + versão. Quando
houver identidade, ela entra por esse caminho, e a decisão sobre o que dela vai para o arquivo
(LGPD) é tomada lá, não aqui.

**D10 — Montagem referencial por padrão; os eletrodos são declarados, não deduzidos.**
- **`afe.montage = referential | bipolar`** é configuração do conversor. Vai ao produtor pelo
  caminho de sempre, e o firmware liga o SRB1 no referencial.
- **`montage.electrodes`** (um nome por canal) e **`montage.reference`** usam de propósito um
  prefixo diferente de `afe.`. O `do_derive_device_options` repassa todo `afe.*` ao produtor, e onde
  o operador pôs um eletrodo não é um fato que o conversor precise saber. Além disso, uma lista de 8
  nomes não cabe nos 24 bytes de valor de uma `ControlMessage`.
- **No build**, o número de nomes tem de ser igual ao número de canais e cada rótulo tem de caber
  nos 16 caracteres do campo do BDF. Uma configuração que falhe nisso é recusada, como o
  `do_seal_configuration` já recusa um comentário inline.
- **Sem `montage.electrodes`**, os rótulos saem genéricos e honestos: `EEG 1-REF` … `EEG 8-REF` no
  referencial, `EEG 1+-1-` … no bipolar.

**O padrão só vira `referential` quando os links que existem souberem cumprir.** Pela regra de D5 da
caracterização, um link que não sabe cumprir uma prescrição a recusa no build. Virar o padrão antes
do kernel saber ligar o SRB1 quebraria os builds `usb` e `spi`. Por isso a inversão do padrão é uma
fase própria, depois das implementações (Fase 5).

**Um fato que o software não verifica.** Se a placa do conversor escolhe o modo unipolar por
**jumper**, nada no sistema sabe em que posição ele está. A configuração declara, o arquivo registra a
declaração, e uma declaração que não bate com o jumper produz rótulos falsos. Isso fica escrito aqui
e no `LAYER_MED_APP.md`, e entra na rotina de bancada da Fase 5.

---

## 3. O arquivo, campo a campo

Conferido na Fase 0 contra três fontes: a especificação do EDF (edfplus.info/specs/edf.html), a do
EDF+ (edfplus.info/specs/edfplus.html) e a descrição do BDF+ do EDFbrowser
(teuniz.net/edfbrowser/bdfplus format description). Esta última é a única fonte do BDF+ que foi
encontrada. O edfplus.info não tem página de BDF+. Regras gerais que valem para todo campo: ASCII
imprimível 32..126 (o `0xFF` do campo de versão é a exceção que o BDF+ define), alinhado à esquerda,
completado com espaços, com ponto como separador decimal e sem separador de milhar.

### 3.1 Cabeçalho geral

| Campo | Bytes | Valor | Nota |
|---|---|---|---|
| versão | 8 | `FF 42 49 4F 53 45 4D 49` (`0xFF` + `BIOSEMI`) | conferido: BDF+ |
| paciente | 80 | `X X X X` (D9) | quatro subcampos no EDF+ e um quinto, "additional info", no BDF+ |
| gravação | 80 | `Startdate DD-MMM-AAAA <sessão> X MedPlatform_<device_id>_<versão>` | mês em três letras maiúsculas em inglês (`OCT`); espaços internos viram `_`; o `:` do id da sessão é ASCII imprimível e fica |
| data de início | 8 | `dd.mm.yy` | conferido: 1985–1999 → `85`–`99`, 2000–2084 → `00`–`84` |
| hora de início | 8 | `hh.mm.ss`, **hora local** (D8) | o subsegundo vai na primeira TAL, `+0.X` |
| bytes do cabeçalho | 8 | `256 × (ns + 1)` | |
| reservado | 44 | `BDF+C` ou `BDF+D` (D3), o resto em espaços | conferido: BDF+ |
| nº de registros | 8 | quadros válidos | `-1` só durante a gravação; o conversor sempre conhece o número |
| duração do registro | 8 | `samplesPerChannel / taxa`, ex. `0.056` | D3 |
| nº de sinais | 4 | canais + 1 (anotações) | |

### 3.2 Por sinal de EEG

| Campo | Bytes | Valor | Nota |
|---|---|---|---|
| rótulo | 16 | `EEG Fp1-A1A2` / `EEG Fp1-F3` / genérico (D10) | |
| transdutor | 80 | vazio | nada no sistema declara o tipo de eletrodo, e o arquivo não inventa um (Fase 3) |
| dimensão física | 8 | `uV` | |
| mínimo / máximo físico | 8 + 8 | `−full_scale`, `+full_scale − 1 LSB` | ver a nota abaixo |
| mínimo / máximo digital | 8 + 8 | `−8388608`, `8388607` | |
| pré-filtragem | 80 | `HP:DC LP:65Hz` a 250 SPS | conferido: banda de −3 dB do filtro sinc³ é 65 Hz a 250 SPS (SBAS499C Tabelas 1–4, igual em todo ganho). É **só o filtro digital**: o RC antialiasing da placa do conversor não é conhecido e não entra no campo enquanto não for medido |
| amostras por registro | 8 | `samplesPerChannel` (14 no `amp`) | |

**O que o campo de 8 caracteres custa** (corrigido pela Fase 2). No ganho 24, o máximo físico exato
é `8388607 × 22,3517 nV = 187.499,977 µV`, e o valor mais próximo que cabe em 8 caracteres é
`187500`. O passo que um leitor reconstrói sai maior em 1/(2²⁴−1), cerca de 6×10⁻⁸. Pouco por
código, mas multiplicado pelos 2²⁴ códigos da faixa dá **1 LSB (22 nV) no fundo de escala**, e
nada além disso. Esta seção dizia "~4×10⁻⁷" e "`187499.9`": uma estimativa da inclinação que não
olhou para a soma, e a soma é o que um leitor vê. Foi o leitor independente que mediu, e não o
teste em C++, que aceitava 1×10⁻⁶ e, com isso, até 16 LSB. Os códigos dentro do arquivo são exatos
(D7). O erro de 1 LSB fica só no valor físico que um leitor calcula a partir dos campos do
cabeçalho, e fica declarado aqui para ninguém medir e achar que é ganho do canal.

O LSB de D7 é o mesmo do firmware: `afe_code_to_nv()` usa `VREF / (ganho × 2²³)`, que é
`full_scale / 2^(bits−1)` com `full_scale = VREF/ganho` e `bits = 24`. O firmware recusa os ganhos 1
e 2 (`afe_gain_fits_frame`), porque o fundo de escala deles não cabe em `int32` de nanovolts. O maior
fundo de escala que chega ao BDF é o do ganho 4, `1125000` µV, que cabe no campo.

### 3.3 O sinal de anotações

Conferido: rótulo `BDF Annotations ` (16 octetos, com o espaço final), mínimo/máximo digital
`-8388608`/`8388607`, mínimo/máximo físico diferentes entre si (`-1`/`1`) e os outros campos em
espaços. O tamanho é fixo por registro, em unidades de 3 bytes. TAL: `+<início>` [`0x15`
`<duração>`] `0x14` `<texto>` `0x14` … `0x00`. A primeira TAL de cada registro é vazia e dá o início
dele, e no `+D` esse início vem da sequência. Os eventos vão nas TALs seguintes.

Duas regras conferidas decidem o que fazer com um evento que não cabe no registro dele:

- **EDF+**: uma TAL, incluindo o `0x00`, nunca atravessa para outro registro.
- **BDF+**: as anotações "do not need to be in that same data record". Isso relaxa a regra do EDF+
  que exige que fiquem no registro a que se referem.

Um evento que não couber vai **inteiro** para o registro seguinte, com o próprio instante de início.
O tamanho do espaço de anotação por registro é decidido na Fase 2 pelo pior caso medido de eventos
por quadro. Ele não é chutado.

| Anotação | Origem |
|---|---|
| `montage referential, reference A1A2` / `montage bipolar` | prescrição, no instante 0 |
| `test signal internal` | prescrição, no instante 0. Um traço de gerador não pode ser lido como EEG |
| `clock not synchronized` | D8, no instante 0 |
| `lead-off: 3+ 5-` / `lead-off: clear` | bloco de lead-off do quadro, só nas **mudanças**, como o serviço já faz na auditoria |
| `gap: N frames` | lacuna de sequência ou quadro com CRC ruim. O BDF+D mostra o buraco, e a anotação diz quanto foi perdido |
| `record interrupted` | D5, no último registro |
| as do operador | Fase 6, pelo `session.annotate` do plano de controle |

Um quadro com CRC ruim **não vira dado**. Ele é tratado como lacuna e anotado. O conversor nunca
escreve no BDF uma amostra que o próprio `raw.bin` diz não ser confiável.

---

## 4. A montagem referencial, no hardware

| Link | O que muda | Estado |
|---|---|---|
| `amp` | `afe.montage` no `med_producer.c`. No referencial, `MISC1` (0x15) bit 5 = SRB1 fecha as chaves que ligam o pino SRB1 às entradas inversoras dos 8 canais. O eletrodo de referência vai no SRB1 e os outros nas entradas positivas (SBAS499C §10.2.2). `CHnSET` não muda: o MUX continua `000`. Mudam `LOFF_SENSN` e `BIAS_SENSN`, ver abaixo | **feito e visto no sinal (2026-10-10)**: `MISC1 = 20h` relido, e a chave fecha e abre num canal com as duas entradas definidas (`BRINGUP_STM32MP2.md` §9.17) |
| `spi`, `usb` | atributo no `ti-ads1299` do `linux-med` e tradução no `do_derive_device_options`. A recusa do build saiu junto | **atributo `montage` escrito e a tradução feita (2026-10-10)**; compila contra o 6.8 do host; nunca carregado, e ainda não compilado contra o 6.6 da placa |
| `simulated` | nada: não há eletrodo, e qualquer montagem é trivialmente o que ele faz | — |

**O lead-off muda de significado no referencial.** Com as entradas negativas presas ao SRB1, a
detecção no lado negativo de cada canal deixa de ter sentido, e passa a importar o contato do
**eletrodo de referência**, de que todos os canais dependem. Um eletrodo de referência solto
invalida os 8 canais de uma vez.

A Fase 0 achou a resposta no datasheet: *"when configured in a referential montage, only use one
lead-off current source with the reference electrode"* (SBAS499C §10.2.2). Com o SRB1 fechado, as 8
entradas inversoras são um só nó. Ligar as 8 fontes de corrente do lado negativo, que é o que o
firmware faz hoje (`LOFF_SENSN = 0xFF`, `afe_regs.c`), injetaria oito vezes a corrente de excitação
no eletrodo de referência. Então, no referencial:

| Registrador | Bipolar (hoje) | Referencial |
|---|---|---|
| `LOFF_SENSP` | `0xFF` | `0xFF`: cada eletrodo de medida |
| `LOFF_SENSN` | `0xFF` | **um bit só**, o do canal 1: esse bit passa a significar "eletrodo de referência" |
| `BIAS_SENSP` | `0xFF` | `0xFF` |
| `BIAS_SENSN` | `0xFF` | `0x00`. **Decisão, não fato de datasheet**: o datasheet não diz nada do bias no referencial, e somar o mesmo nó oito vezes na média faria o bias seguir a referência em vez do modo comum dos eletrodos. Vai ser medido como a rejeição de 60 Hz do plano de caracterização §3.5, com as duas escolhas |

A consequência para o registro: no referencial, o `1-` que o serviço reporta hoje passa a ser `REF`
na auditoria e na anotação do BDF, e os `2-`…`8-` deixam de ser monitorados e não aparecem. É a
tradução que D10 já pede, agora com o bit certo.

---

## 5. Mudanças, arquivo a arquivo

| Arquivo | Camada | Mudança |
|---|---|---|
| `meta-med-framework/.../MedicalStorage.h`, `.cpp` | framework | escritor BDF+ genérico (D6): cabeçalho, sinais de 24 bits, sinal de anotações, `+C`/`+D` |
| `meta-med-app/.../src/main.cpp` | app | `metadata.json` ganha escala, montagem, eletrodos e relógio; escreve `/run/medplatform/session` e `closed.json` (D5) |
| `meta-med-app/.../src/export.cpp` (novo) | app | `eeg-record-export`: varre `/data/eeg`, aplica D5 e D7, escreve `record.bdf` + `.sha256`, audita |
| `eeg-acquisition-service_1.0.0.bb` | app | instala o segundo binário e a unidade no **mesmo pacote**; valida `montage.*` no build; repassa `afe.montage`; recusa `referential` em `spi`/`usb` até a Fase 5 |
| `eeg-acquisition.service` | app | `ExecStopPost=+… --no-block` (D4) |
| `eeg-record-export.service` (novo) | app | oneshot com o mesmo sandbox do serviço: lê e escreve só em `/data`, sem rede, sem dispositivo |
| `eeg.conf` | app | `afe.montage`, `montage.electrodes`, `montage.reference`. O padrão vira `referential` só na Fase 5 |
| `med-m33-firmware/.../med_producer.c`, `afe_regs.c` | adjunta | `afe.montage`, com recusa explícita para valores desconhecidos |
| `linux-med`: `ti-ads1299.c` | kernel | atributo de montagem (Fase 5) |
| `scripts/amp-bench.py` | host | subcomando `bdf RAW BDF`: segunda leitura que confere o BDF contra o `raw.bin` sem passar pelo código do framework |
| `scripts/med-check.py` | host | a assertiva `record-bdf` (Fase 3) |

O `grep` da regra 9 continua vazio no framework e no app. `montage.electrodes` são nomes de posição
do sistema 10-20, não de peça.

---

## 6. Fases

Cada fase tem critério numérico, injeção de falha e lugar onde o número fica registrado. Uma
assertiva que nunca viu a falha que procura é afirmação, não verificação.

### Fase 0 — conferir as constantes (bloqueia todas as outras) — **executada, 2026-10-09**

Conferir cada `[EDF+?]` contra a especificação do EDF+ e a nota do BDF+. Conferir cada `[SBAS499?]`
deste plano (SRB1/MISC1, `LOFF_SENSN`/`BIAS_SENSN` no referencial, a −3 dB do filtro de decimação a
250 SPS) contra o datasheet.
**Critério**: nenhuma marca sobrando neste documento. Onde a especificação contradisser o texto, o
texto é corrigido e a correção registrada aqui.

**Resultado.** Nenhuma marca sobrou. Fontes: edfplus.info/specs/edf.html, edfplus.info/specs/edfplus.html,
a descrição do BDF+ do EDFbrowser (teuniz.net) e o SBAS499C (jan/2017), texto extraído do PDF da TI
com `pdftotext`. O Table 11 também bate com `docs/Register_Map_ADS1299.md`.

| Constante | O texto dizia | A fonte diz | Efeito |
|---|---|---|---|
| versão, reservado, rótulo e faixa das anotações | como escrito | igual | nenhum |
| regra do século | `[EDF+?]` | 1985–2084, `yy` depois | nenhum |
| duração não inteira | risco de formato | o EDF+ recomenda segundos inteiros, o BDF+ aceita qualquer > 0 | D3 fica, o risco é de leitor (§9) |
| TAL em outro registro | "o formato permite" | o EDF+ proíbe que uma TAL atravesse um registro; o BDF+ dispensa a anotação de ficar no registro a que se refere | o evento vai **inteiro** para o registro seguinte (§3.3) |
| hora de início | não tratada | **hora local** do paciente (item 8 do EDF+) | **D8 cresceu**: deslocamento de UTC no metadado e anotado |
| −3 dB a 250 SPS | `[SBAS499?]` | 65 Hz, em todo ganho | campo de pré-filtragem definido; o RC da placa fica de fora |
| SRB1 | `[SBAS499?]` | `MISC1` bit 5, liga o pino às 8 entradas inversoras | confirmado |
| lead-off no referencial | pergunta aberta | "only use one lead-off current source with the reference electrode" | **o firmware de hoje está errado para o referencial** (`LOFF_SENSN = 0xFF`): um bit só (§4) |
| bias no referencial | `[SBAS499?]` | o datasheet não diz | **decisão** `BIAS_SENSN = 0`, medida na caracterização (§4) |

O que a Fase 0 **não** cobre: a página do BDF+ é a descrição do autor do EDFbrowser, não um
documento de norma. Ela é a referência de fato, e é também o leitor-alvo da Fase 4, e essas duas
coisas são uma só fonte e não duas. Os leitores independentes da Fase 2 (MNE, pyedflib) são o que
separa as duas.

### Fase 1 — o metadado sabe o que a sessão foi — **executada, 2026-10-09**

`metadata.json` com escala, montagem, eletrodos e relógio; `closed.json`; `/run/medplatform/session`.
Decidir de onde vem o "relógio sincronizado" (D8).
**Critério**: no QEMU, uma sessão parada pelo `make check` tem `closed.json` com `frames` igual ao
`MED_FRAMES` do `AcquisitionStopped`. **Injeção**: `kill -9` no serviço → a sessão fica sem
`closed.json` e `/run/medplatform/session` some com o `RuntimeDirectory`.

**O que foi feito, e três decisões que o texto acima não tinha:**

- **A escala e a montagem são declaradas pela receita, não pelo framework.** O
  `do_derive_device_options`, que já é o único lugar que traduz a prescrição para cada link, agora
  acrescenta ao `eeg.conf` selado `record.full_scale_uv` (`afe.reference_uv / afe.gain`, o mesmo
  cálculo do `relationalLimits` do serviço), `record.resolution_bits` (24, agora uma variável só,
  que também alimenta a tabela do `simulated`) e `record.montage`. O serviço copia essas linhas para
  o metadado. O framework continua sem vocabulário de conversor, e o driver `rpmsg`, que não sabe
  o LSB, não precisa saber. `record.*` escrito à mão é recusado, como `device.option.*` já era.
- **`record.montage = bipolar` em todos os links.** É o que os links fazem hoje, não um padrão
  escolhido: nenhum fecha o SRB1. A Fase 5 muda isso na mesma tabela.
- **O relógio sincronizado vem do `systemd-timesyncd`** (`/run/systemd/timesync/synchronized`), por
  `med::clockSynchronized()`. Perguntar ao kernel por `adjtimex()` seria mais geral, mas o
  `ProtectClock=yes` da unidade filtra essa syscall inteira, leitura incluída. O deslocamento de UTC
  vem de `med::utcOffset()`, pelo fuso do sistema.
- **O marcador de sessão viva passa pelo `MedicalStorage`**, numa segunda instância com raiz em
  `storage.runtime = /run/medplatform` e sem exigir cifra. O app continua sem nenhuma chamada de
  sistema. O serviço **recusa partir** se não conseguir abri-lo ou escrevê-lo, porque um serviço
  sem marcador teria a própria sessão convertida como interrompida. O marcador é escrito **antes**
  de o diretório da sessão existir, e o `closed.json` **antes** de o marcador sair. Não há instante
  em que uma sessão esteja no disco sem fechamento e sem dono.

**Medido.**

| Verificação | Resultado |
|---|---|
| framework no host | 1343 → **1347** verificações, 0 falhas (`utcOffset` em UTC, −03:00 e +05:30) |
| injeção: minutos do deslocamento zerados | 1 falha, a do fuso +05:30, e só ela |
| `make service`, `eeg.conf` produzido | `record.full_scale_uv = 187500`, `record.resolution_bits = 24`, `record.montage = bipolar`; sidecar confere |
| injeções no build | contagem de eletrodos errada, rótulo de 19 caracteres, `record.*` à mão, `montage.reference` no bipolar: **as quatro recusam**, cada uma com a própria mensagem. Oito pares válidos passam |
| `make check` | **25/25**, com duas assertivas novas: `session-live-marker` e `session-closed` |
| injeção: sem `closed.json` | **24/25**, só `session-closed`: `closed=` vazio, auditoria 1382 quadros, escala presente |
| injeção: sem marcador vivo | **24/25**, só `session-live-marker`: `viva=` vazio |

A injeção prevista no critério, `kill -9`, foi trocada por estas duas. Elas atacam o código que
escreve cada marcador, e cada uma prova que a sua assertiva vê a falha e que a outra não vê. Um
`kill -9` provaria o comportamento do systemd com o `RuntimeDirectory`, que é documentado, e ele
volta como injeção da Fase 4, onde a sessão interrompida é o caso sob teste.

**O que isto não significa.** Que o relógio sincronizado foi observado como `true` em algum alvo: no
QEMU e na placa ele não foi lido, e a assertiva só confere que a escala está no metadado. Que o
metadado é JSON válido: nenhuma ferramenta o parseou. O `grep` da assertiva lê um campo. A Fase 2
lê o metadado com um parser, porque é o que o conversor vai fazer.

**Encontrado de passagem, não corrigido:** a `acq-sample-rate` calcula o quadro como `40 + CH·N·4`,
sem os 8 bytes do bloco de lead-off da v2. Ela superestima a taxa em ~1,6%, dentro da tolerância de
10%. É a assertiva que existe para não confiar em geometria embutida, errando a geometria.
**Corrigido em 2026-10-10**: o quadro passou a `40 + CH·N·4 + 8`, e o `make check` deu 26/26 com
a correção. A injeção do `SOCK_NONBLOCK` não foi refeita depois dela.

### Fase 2 — o escritor, no host — **executada, 2026-10-09**

Escritor no framework e checagens de host junto às 55 existentes.
**Critério**: o BDF de um `raw.bin` sintético lido de volta dá os códigos exatos. O mesmo `raw.bin`
gera o mesmo hash duas vezes. Um leitor independente (MNE ou pyedflib, só no host e nunca na imagem)
abre o arquivo e devolve os valores em µV dentro de 1 LSB. Um teste escrito com a mesma leitura da
especificação que escreveu o código prova consistência, não correção. Por isso o leitor de fora é o
que conta, e um vetor do próprio documento de especificação, se houver, vale mais que os dois.
**Injeções** (cada uma deve falhar só o que deve):
- escala declarada com outro ganho → recusa na primeira amostra (D7);
- um byte trocado num quadro → lacuna anotada e arquivo `+D`;
- `raw.bin` truncado no meio de um quadro → o quadro parcial é descartado e anotado;
- uma sessão sem `full_scale` no metadado → recusada com o motivo.

**O escopo mudou na execução.** As quatro injeções acima são do **conversor**, que lê o `raw.bin` e
o metadado e decide o que vira dado, lacuna ou recusa. Esse código só existe na Fase 3, então elas
foram para lá. A Fase 2 entregou o que D6 põe no framework: o **escritor do formato**, que não sabe
o que é um `raw.bin`.

**O que foi feito.** `BiosignalWriter` em `MedicalStorage.h`, com a implementação em
`src/MedicalStorageBiosignal.cpp` e o SHA-256 incremental em `MedDigest.h`. O escritor valida o
cabeçalho inteiro antes do primeiro byte e recebe as amostras **intercaladas**, como a aquisição as
entrega, gravando-as por sinal, como o formato pede. Ele confere o início de cada registro e recusa
código fora da faixa em vez de cortar. Anotação que não cabe vai **inteira** para o próximo registro,
e o `finish()` recusa se sobrou alguma. O arquivo só fica completo quando o `.sha256` existe, no
formato do `sha256sum -c`, e o `create()` apaga arquivo e selo antigos.

Uma regra mudou ao escrever o teste. A duração do registro é recusada quando a **deriva acumulada no
arquivo** passa de 1 µs, e não quando o erro de um registro passa disso: 1/3 s cabe como `0.333333`,
e a diferença é nada num registro e um terço de milissegundo em mil.

**Medido.**

| Verificação | Resultado |
|---|---|
| framework no host | 1347 → **1424** verificações, 0 falhas; igual com AddressSanitizer e UBSan |
| selo | o digest do escritor é o que o `sha256sum` do coreutils calcula |
| leitor independente (`bdf_oracle.py`: pyedflib 0.1.42, MNE 1.13.2) | **22/22** no `+C`: os 8 canais, código a código, exatos; 250 Hz; rótulos, dimensão, pré-filtragem e anotações como escritos; valor físico no MNE a ≤ 1,000 LSB |
| injeção: *byte order* invertido no escritor **e** no leitor do teste | `make check` **continua 1424/0**; o oráculo falha os 8 canais |

**Três achados, e o terceiro pede decisão.**

1. **Anotação "no instante 0".** Os dois leitores medem anotações a partir da primeira amostra, não
   do segundo inteiro do cabeçalho. Uma anotação em 0 com `startFraction = 0,356` virou −0,356 no
   pyedflib e **sumiu** no MNE, que descarta o que fica antes dos dados. O conversor põe os eventos
   de início em `startFraction`. A §3.3 dizia "no instante 0", e isso significa a primeira amostra.
2. **O pyedflib lê errado a fração de segundo do início** (`02:33:37.0356` para 0,356 s). É defeito
   do leitor, porque o arquivo diz `+0.356` como o EDF+ define. Fica registrado para ninguém ler
   esse número como dado.
3. **Nenhum dos dois leitores mostra uma lacuna de um BDF+D.** O pyedflib (EDFlib) **recusa** o
   arquivo: *"The file is discontinuous and cannot be read"*. O MNE abre e **concatena os registros
   em silêncio**: 42 amostras contíguas onde havia uma lacuna de dois registros. A anotação `gap`
   chega e marca o ponto da emenda, mas qualquer anotação depois de uma lacuna fica deslocada em
   relação aos dados pelo tamanho da lacuna. O EDFbrowser, que é a referência do BDF+, não foi
   testado. É a Fase 4.

O risco que a §9 previa para o `+D` virou fato medido: o arquivo é correto e dois leitores comuns o
tratam mal. A §2 D3 continua correta quanto ao formato, mas a escolha do que gravar quando há
lacuna precisa ser revista antes da Fase 3. As alternativas estão na §9.

### Fase 3 — o conversor, no QEMU — **executada, 2026-10-09**

`eeg-record-export`, a unidade e o `ExecStopPost`. Nova assertiva `record-bdf` no `make check`
(25 → 26): depois do `stop`, existe `record.bdf`, o `.sha256` confere, o número de registros é o
`MED_FRAMES` e o subcomando `amp-bench.py bdf` concorda.
**Injeção**: desabilitar o `ExecStopPost` → só a `record-bdf` falha, 25/26.
**Medir**: o tempo de conversão por hora de sessão. Isso é número para a `RESULTS.md`, não critério.

**Um defeito anterior apareceu primeiro, e foi corrigido antes do conversor.** `SampleFrame::samples`
era `std::vector<float>`, e o serviço grava um quadro decodificando-o e codificando-o de novo. Medido
nos 2²⁴ códigos do ganho 24: **12.736.188 amostras (76%) voltavam diferentes do que o produtor
mandou, até 8 nV (0,358 LSB)**. Todas ainda recuperavam o código por arredondamento, mas o `raw.bin`
não era cópia da medida, e D7 (≤ 0,5 nV) recusaria sessões reais. O teste de codec que existia nunca
viu isso, porque usava meios microvolts, que um `float` representa exato. Agora as amostras são
`double`, e uma verificação nova passa os 2²⁴ códigos pela ida e volta byte a byte. Injetar o
`static_cast<float>` de volta no `decodeFrame` falha só essa verificação, com 136.708 quadros
alterados. Vale para toda sessão gravada antes desta correção: o `raw.bin` delas difere do produtor
em até 8 nV, e a D7 vai recusá-las.

**O que foi feito.**

- **`MedicalStorage`** ganhou `size()` e uma leitura por faixa. O `read()` carrega o arquivo inteiro,
  e um `raw.bin` de 12 h tem ~350 MB.
- **`BiosignalWriter::annotationBytes`/`timekeepingBytes`.** Quem dimensiona o espaço de anotação usa
  os tamanhos do próprio escritor, em vez de repetir o formato.
- **`record_export.cpp`**, o núcleo, separado do `main` para ser testado como é entregue. Duas
  passadas sobre o `raw.bin`, em janelas de 1 MiB:
  - **planejamento:** quadros válidos, CRC ruim, bytes ilegíveis e cauda parcial; D7 em **toda**
    amostra; os trechos contínuos pela sequência; as anotações;
  - **escrita:** um `+C` por trecho.
  O espaço de anotação é o menor que esvazia a fila em cada trecho, achado simulando o escritor.
  Anotações de início: `link`, montagem, sinal de teste, relógio não sincronizado, deslocamento de
  UTC e, depois de uma lacuna, `after gap: N frames (X s)`. De fim: `gap follows`, cauda parcial,
  CRC ruim depois do último quadro válido e `record interrupted`. Contato de eletrodo vai nas
  mudanças, com os nomes declarados. O `metadata.json` é lido por um leitor de JSON estrito, e o que
  falta é recusado, não presumido.
- **`export_main.cpp`.** Lista as sessões e **só depois** lê o marcador de sessão viva, porque na
  ordem inversa uma sessão aberta entre as duas leituras pareceria interrompida. O `export.json` é
  escrito por último e marca a sessão como tratada, convertida ou recusada. Recusa (escala, metadado)
  é final e auditada como `SecurityEvent`. Erro de I/O não marca nada e tenta de novo.
- **`eeg-record-export.service`.** Oneshot, `Nice=10`, `IOSchedulingClass=idle`, o sandbox da
  aquisição sem tempo real e sem dispositivo (`PrivateDevices=yes`), habilitada no boot **depois** da
  aquisição. A aquisição dispara a unidade com
  `ExecStopPost=+/usr/bin/systemctl start --no-block`.
- **`record.prefiltering`.** O `do_derive_device_options` declara `HP:DC LP:<banda>Hz` pela taxa,
  com a tabela da Fase 0, nos links com conversor, e nada no `simulated`, que não filtra.
- **Transdutor vazio**, ao contrário do que a §3.2 dizia. Nada no sistema declara o tipo de
  eletrodo, e o arquivo não inventa um.

**Medido.**

| Verificação | Resultado |
|---|---|
| `tests/eeg-export` (novo, no `make test`, com ASan e UBSan) | **67** verificações, 0 falhas: sessão íntegra (60 quadros, códigos exatos, hora local −03:00 cruzando a meia-noite, anotações), contato de eletrodo, determinismo (mesmo hash duas vezes) e as quatro injeções que vieram da Fase 2 |
| injeção de entrada: escala do ganho 12 numa sessão de ganho 24 | recusa na primeira amostra, `IntegrityError`, nenhum selo escrito |
| injeção de entrada: um byte trocado no quadro 7 de 20 | 1 CRC ruim, 1 quadro faltando, `record-001.bdf` (7) + `record-002.bdf` (12); o segundo começa 0,448 s depois |
| injeção de entrada: `raw.bin` sem os últimos 300 bytes | 5 quadros, 196 bytes de cauda descartados e anotados |
| injeção de entrada: metadado sem escala | recusa, `InvalidArgument`, nada escrito |
| injeção de código: D7 desligada | falha só a seção de escala (3 verificações) |
| injeção de código: lacunas ignoradas | falham só as duas seções de lacuna (13). Antes disso, o teste **abortava** no ASan por indexar um arquivo que não existia; os auxiliares agora falham em vez de travar a suíte |
| `amp-bench.py bdf` (leitor próprio do `raw.bin` e do BDF, sem o framework) | sessões íntegra e corrompida: 6720 + 2128 códigos, 0 diferentes, resíduo ≤ 0,500 nV, trechos e inícios conforme a sequência |
| pyedflib e MNE sobre a saída do conversor | abrem os três arquivos; todas as anotações chegam, nos instantes certos |
| `make check` | **26/26** |
| injeção: sem `ExecStopPost` | **25/26**, só a `record-bdf` (`export.json` ausente) |

**O que isto não significa.** Não houve conversão de um `raw.bin` real da placa: o QEMU usa o link
`simulated`, sem filtro declarado e sem lead-off. O tempo de conversão não foi medido. No QEMU ele
não significaria nada, e é medida da Fase 4. Uma sessão com data anterior a 1985, que é o que uma
placa sem RTC e sem rede produz, é **recusada**: o formato não consegue escrever essa data. Isso é
coerente com D8, mas quer dizer que o arquivo clínico depende de o relógio estar certo, e não só de
ser declarado. O EDFbrowser continua sem teste.

### Fase 4 — na placa, pelo `amp`

Uma sessão com `afe.test_signal = internal` (o lead-off desligado, porque o firmware recusa os dois
juntos, `BRINGUP_STM32MP2.md` §9.16). O `amp-bench.py converter` sobre o `raw.bin` e a mesma
análise sobre o BDF lido pelo leitor independente têm de dar o mesmo veredito: amplitude, período
de 256 amostras, zero lacunas. O arquivo abre no EDFbrowser com a duração de 0,056 s.
**Medir**: o tempo de conversão de uma sessão de 12 h no A35 e o espaço extra no `/data`.
**Injeção**: tirar a energia no meio de uma sessão → no boot seguinte o BDF existe e termina em
`record interrupted`.

### Fase 5 — a montagem referencial — **o passo do `amp` executado e visto no sinal, 2026-10-10; faltam `spi`/`usb` e a troca do padrão**

Primeiro o firmware, depois o `ti-ads1299` com a tradução no `do_derive_device_options`, e só então
o padrão do `eeg.conf` vira `referential`.
**Critério**: no `amp`, o readback do MISC1 confirma o SRB1 em cada configuração, como o
`LOFF_STAT` já é conferido. No `usb`, idem pelo driver. O sinal com eletrodo ou simulador de
eletrodo é medido pelo plano de caracterização §5.2, que ganha a montagem referencial como condição.
**Injeção**: `referential` num link sem suporte → o build recusa com a mensagem. Um eletrodo de
referência solto no simulador → o que o lead-off reporta, conforme a resposta da Fase 0.

**O que foi feito (o primeiro dos três passos, e a parte do segundo que não precisa do kernel).**

- **Firmware.** `afe.montage` é a sexta chave que o produtor entende: `bipolar`, `referential`, e
  qualquer outro valor é recusa com o nome da opção. Ausente, vale `bipolar`, que é o que o produtor
  fazia antes de a chave existir. O `afe_registers()` ganhou a montagem e o `afe_apply()` escreve e
  relê o `MISC1` **nas duas montagens**: uma sessão bipolar depois de uma referencial tem de abrir a
  chave, e é o readback que diz que a montagem do registro é a que foi programada. No referencial,
  `MISC1 = 0x20`, `LOFF_SENSN = 0x01` e `BIAS_SENSN = 0x00`, como a §4.
- **O bloco de lead-off do quadro.** No referencial, `monitoredNegative = 0x0001` e o produtor
  mascara os outros sete bits negativos antes de eles chegarem ao registro. O formato do fio não
  mudou: é o `med-amp-abi` v2, com outro valor num campo que já existia.
- **O produtor sintético aceita as duas montagens.** Ele não tem entrada para ligar, e as duas são
  trivialmente o que ele faz, como o link `simulated`. É diferente do lead-off, que ele recusa,
  porque aquele seria um monitor que nunca rodou.
- **Receita.** `afe.montage` é obrigatório no `eeg.conf`, e `record.montage` passou a ser **derivado
  dele** em vez de ser a constante `bipolar`. No `amp` a chave segue ao produtor com as outras
  `afe.*` (seis opções, de oito que a mensagem carrega). Em `spi` e `usb`, `referential` é recusado
  no build. O padrão do `eeg.conf` **continua `bipolar`**, como este plano manda, até o silício
  confirmar o `MISC1`.
- **Serviço.** A auditoria de contato diz `REF` quando a montagem é referencial, em vez de `1-`.
- **Conversor.** Não mudou: o caminho referencial dele já existia desde a Fase 3, e **não tinha
  nenhum teste**. Agora tem.

**Medido.**

| Verificação | Resultado |
|---|---|
| `tests/m33-producer` | 198 → **211** verificações, 0 falhas, com ASan e UBSan |
| injeção: as oito fontes no lado negativo (o firmware de antes) | 1 falha, a dos registradores do referencial |
| injeção: SRB1 no bit 4 | 2 falhas, as duas que leem o `MISC1` do referencial |
| injeção: `BIAS_SENSN` ligado no referencial | 1 falha |
| injeção: os 8 bits negativos chegam ao quadro | 1 falha, a do bloco de lead-off |
| injeção: montagem desconhecida vira `bipolar` | 4 falhas, as quatro recusas de valor |
| `tests/eeg-export` | 67 → **82** verificações, 0 falhas: rótulos `EEG Fp1-A1A2` e `EEG 1-REF`, anotação de montagem, referência solta relatada como `A1A2`/`REF` e nunca como `1-` |
| injeção: o conversor ignora a montagem ao nomear o contato | 4 falhas, só as de contato referencial |
| build, `amp`, `referential` com 8 eletrodos e referência | `device.option.afe.montage = referential` na ordem do teste do produtor, `record.montage = referential` |
| build, injeções | `afe.montage = average`; `referential` no `usb`; pares `Fp1-F3` sob `referential`: **as três recusam**, cada uma com a própria mensagem |
| `med-m33-firmware`, `do_compile` para o M33 | sem avisos, pacote escrito |
| `make test` inteiro | framework 1442, MCP2210 452, produtor 211, conversor 82: 0 falhas |
| `make qemu` e `make check` | verde, **26/26**. É regressão, não medida da montagem: o QEMU usa o `simulated` |

**Um nome trocado de passagem.** A seção nova do `tests/eeg-export` usou primeiro `session-g`, que a
seção de lacunas também usa, e três verificações **de lá** falharam com um arquivo a mais. O
`MedicalStorage::append` acrescenta, e duas seções com o mesmo diretório somam os quadros. Não era
defeito do conversor, mas vale saber que as seções dessa suíte não são isoladas entre si.

**O que isto não significa.** Nada disto viu o conversor. O bit do SRB1 saiu de uma leitura da
Figura 64 do datasheet e o teste foi escrito com a mesma leitura: prova consistência, não correção,
e o kernel não serve de oráculo porque o `ti-ads1299.c` nunca escreve o `MISC1`. O readback na
placa prova que o registrador aceitou o byte, não que a chave fechou. O que prova a chave é sinal:
um gerador no pino SRB1 aparecendo invertido nos oito canais. `BIAS_SENSN = 0` continua sendo
decisão não medida. E o `make check` não cobre nada disto, porque o QEMU usa o link `simulated`.

**Na placa, no mesmo dia (`BRINGUP_STM32MP2.md` §9.17).** A prescrição referencial foi aceita:
`SENS ff 00 ff 01 MISC1 20` relido, auditoria `1+ … 8+ REF`, e a sessão convertida num BDF+ de 3842
registros com rótulo `EEG 1-REF` e selo `OK`. A volta ao bipolar releu `SENS ff ff ff ff MISC1 00`.
O parágrafo acima foi escrito antes dessa sessão; dele, só "nada disto viu o conversor" deixou de
valer, e o resto continua: o readback não prova a chave.

**E a chave, no sinal.** Com o BIAS ligado por fio ao `IN_SRB` e ao `IN1P`, o canal 1 foi de
181,8 mV a 39 µV na primeira amostra do referencial, e ao topo quando a chave manual levou o SRB1 a
GNDA; os sete canais sem o fio não tiveram degrau. Antes disso, três rodadas com as entradas
abertas não viram nada, e o esquemático mostrou por quê: há 4,7 nF entre P e N de cada canal e
nenhum caminho DC, então um canal aberto mede a carga desse capacitor em qualquer montagem. O
"readback não prova a chave" acima está pago para o canal 1.

**O que falta, na ordem.** (1) ~~Na placa, pelo `amp`: o sinal no SRB1.~~ Feito. Sobra medir o
bias no referencial (`BIAS_SENSN = 0` é decisão, e os canais abertos foram ao fundo negativo aos
14,6 s daquela sessão, sem causa conferida). (2) ~~O atributo de montagem no `ti-ads1299` e a tradução na receita.~~ Escrito, ver abaixo; falta
carregá-lo. (3) Só então o padrão vira `referential`.

**O segundo passo: o atributo no driver do kernel (2026-10-10, só compilado).** `montage` e
`montage_available` no `ti-ads1299` do `linux-med`, com os mesmos dois valores da prescrição.
`referential` escreve `MISC1 = 20h` e tira as entradas negativas da derivação do bias
(`BIAS_SENSN = 0`), a mesma decisão do firmware; `bipolar` desfaz as duas coisas. A derivação é
corrigida com as entradas negativas ainda separadas (antes de fechar, depois de abrir), o atributo
recusa mudar com o buffer rodando, como o `bias_drive`, e o probe escreve `bipolar` em vez de
presumir o reset. O driver não liga o lead-off por canal, então a regra de "uma fonte só" não tem
onde ser aplicada ali; fica escrita no comentário para quem ligar. Na receita, `spi` e `usb`
passam `device.option.montage`, e a recusa de `referential` saiu.

| Verificação | Resultado |
|---|---|
| `checkpatch --strict` | 0 erros, 2 avisos e 2 checks, os mesmos de antes da mudança |
| `make -C scripts/oot-modules`, kernel 6.8 do host, e de novo com `W=1` | compila e liga sem avisos |
| build, `usb`, as duas montagens | `device.option.montage` e `record.montage` iguais à prescrição |

**O que isto não significa.** O módulo nunca foi carregado: nenhum `echo referential > montage`
foi escrito num conversor, e o readback do `MISC1` pelo driver não existe. O `checkpatch` limpo
não diz nada sobre o registrador estar certo (foi 0/0/0 com três constantes invertidas no
MCP2210). Não foi compilado contra o 6.6 da placa: o build do Yocto usa o `HEAD` do `linux-med`, e
a mudança não está commitada lá. Uma imagem `usb` com a receita nova e o módulo antigo **não
adquire**: o driver do framework recusa a opção que o kernel não tem, e o slot não se confirma.

### Fase 6 — com o plano de controle

Quando o `session.start`/`session.stop`/`session.annotate` existirem: sujeito e operador nos campos
do EDF+ (D9), anotações do operador no BDF e o `session.stop` como gatilho do conversor. Esta fase
**depende** das Fases 4 a 6 do `implementation_plan_eeg_control.md`, e nada antes dela depende
daquele plano.

---

## 7. Ordem, e o que bloqueia o quê

```
Fase 0 ──► Fase 1 ──► Fase 2 ──► Fase 3 ──► Fase 4
   │                                           
   └──────────────────────────────► Fase 5      (independe de 1–4)
                                          
eeg_control Fases 4–6 ─────────────────────► Fase 6
```

As Fases 1–4 entregam o arquivo com a montagem de hoje, que é diferencial. A Fase 5 pode correr em
paralelo e só precisa da Fase 0. Um BDF gravado antes da Fase 5 sai com `montage bipolar` e rótulos
`EEG n+-n-`, o que é verdade sobre aquele registro.

---

## 8. Consequências regulatórias

- **Rastreabilidade (IEC 62304).** O arquivo clínico é reprodutível a partir do registro, e o hash
  dos dois fica na trilha de auditoria (`PatientDataWritten`). Uma divergência entre o BDF e o
  `raw.bin` é detectável por qualquer um, sempre.
- **Integridade.** O BDF não tem CRC. Um BDF corrompido em repouso se detecta pelo `.sha256` e se
  corrige refazendo a conversão a partir do `raw.bin`. É mais uma razão para D1.
- **Dado do paciente.** O BDF vive no `/data`, cifrado como o `raw.bin`. Sair do aparelho é
  `PatientDataExported`, e o caminho de exportação **não** é deste plano.

---

## 9. Riscos

| Risco | Tratamento |
|---|---|
| Leitores que tropeçam em duração não inteira (D3) | Fase 4 abre o mesmo arquivo em EDFbrowser, MNE e EEGLAB. Se um falhar, a alternativa é agrupar N quadros por registro com N fixo, ainda sem completar registros |
| Leitores com suporte ruim a `+D` | **medido na Fase 2** (o pyedflib recusa, o MNE concatena em silêncio) e **resolvido** por decisão de 2026-10-09: um `+C` por trecho contínuo (D3) |
| Sobrecarga das anotações em registros curtos | dimensionar na Fase 2 pelo pior caso medido. Na estimativa de hoje é ~20% sobre 336 B de dados por registro |
| Conversão lenta no A35 para sessões longas | D4 tira a conversão do caminho crítico. O número entra na `RESULTS.md` |
| Espaço no `/data` | BDF ≈ 7 kB/s contra 8,9 kB/s do `raw.bin`. Os dois juntos dobram o consumo, e isso é medido na Fase 4 |
| Jumper de montagem não detectável | D10: declarado, registrado, conferido na bancada |
| Data errada sem RTC | D8: anotada no arquivo. A correção é outro trabalho |
| O escritor atrair vocabulário de EEG para o framework | D6 e regra 9, conferidos em revisão e não só pelo `grep` |

---

## 10. Onde cada número vai

- Tempo de conversão, espaço, abertura nos leitores: `RESULTS.md`, nova seção, com o comando e o
  que **não** significa.
- `make check` 26/26 e a injeção da `record-bdf`: `RESULTS.md` §3.
- Readback do SRB1 e montagem referencial na placa: `BRINGUP_STM32MP2.md` §9, sessão da data.
- Correções da Fase 0: neste documento, no lugar da constante.
