# Plano — estatísticas na STM32MP257, nas ligações `usb` e `amp`

> **Status**: **nada aqui foi executado.** A parte `usb` pode começar no dia em que este plano for
> aceito: a placa adquire por ela desde 2026-10-04 (`BRINGUP_AFE.md` §6.4–6.6). A parte `amp` não
> produz uma amostra sequer, porque não existe firmware no Cortex-M33 (`implementation_plan_m33_firmware.md`,
> nada implementado), e por isso entra em duas etapas: primeiro com sinal sintético gerado no M33,
> depois com o ADS1299 ligado ao conector da DK.
>
> **Escopo**: a bancada que produz os números da comparação entre ligações, com o **mesmo
> conversor, o mesmo sinal e a mesma sessão**, mais a varredura das configurações do ADS1299 que
> o TCC precisa caracterizar. Este plano **preenche** a Fase 7 do `implementation_plan_afe_bench.md`
> e a Fase 5 do `implementation_plan_m33_firmware.md`; não as substitui. Onde os três se encontram,
> a definição de cada medida é a deste documento.
>
> **Fronteira de segurança, a mesma do `implementation_plan_afe_bench.md` §3**: nenhuma pessoa é
> ligada ao conversor. As entradas recebem gerador de função por atenuador, curto ou resistores que
> simulam a impedância do eletrodo.

---

## 0. A linha, dita primeiro

| | Hoje |
|---|---|
| `usb`, placa | adquire; 30 min medidos (perda 0,67% com `schedutil`, ≤ 0,003% em `performance`); jitter só pelo carimbo do host; nenhuma referência externa |
| `amp`, placa | **nenhuma amostra**: sem firmware, sem fiação, sem RIF |
| referência de tempo independente | **nenhuma medida ainda**: o analisador lógico nunca foi ligado ao `DRDY` |
| configurações do conversor caracterizadas | ganho e ruído em curto, só no PC e só pelas referências internas (`BRINGUP_AFE.md` §5) |

A pergunta que este plano responde não é "qual ligação ganha". É: **para cada ligação, que
garantias o registro clínico pode afirmar** — quantas amostras chegam, quão bem se sabe quando
cada uma foi convertida, quanto custa, e com que qualidade de sinal — e o que muda quando se mexe
em cada configuração do conversor.

---

## 1. O que se mede, e quem conta

Toda métrica tem uma **fonte de verdade** e, sempre que possível, uma **segunda fonte
independente**. Uma métrica com uma fonte só é marcada como tal na tabela final: é o que separa uma
medida de uma afirmação do próprio sistema sobre si mesmo.

### 1.1 Integridade do fluxo

| Métrica | Definição | Fonte primária | Fonte independente |
|---|---|---|---|
| taxa entregue | amostras gravadas no `raw.bin` / tempo | cabeçalhos dos quadros (`sequence`, `samplesPerChannel`) | bordas de `DRDY` no analisador |
| perda no front-end | conversões que ninguém leu | `lost_samples` (usb); contador do firmware (amp) | `DRDY` sem leitura entre duas bordas, no analisador |
| perda na plataforma | quadros lidos que não chegaram ao registro | saltos de `sequence` no `raw.bin` | `dropped` no `AcquisitionStopped` |
| perda para a tela | quadros que a HMI não recebeu | `viewer missed` no `AcquisitionStopped` | — (só uma fonte) |

**Por que a fonte independente é obrigatória aqui**: na placa, a perda **não aparece nos
carimbos de tempo** (`BRINGUP_AFE.md` §6.3 e §6.5). Até hoje, toda perda medida foi contada pelo
próprio driver. O analisador conta conversões e leituras pelos fios, sem passar por código nosso.

### 1.2 Tempo

| Métrica | Definição | Como |
|---|---|---|
| jitter da conversão | dispersão do intervalo entre bordas de `DRDY` | analisador. É o oscilador do conversor, e serve de controle: tem que ser ~ppm |
| atraso de leitura | `DRDY` → início da leitura (CS ativo) | analisador. No `usb` é o erro de fase do carimbo (o carimbo é tomado na leitura); no `amp` é a latência da ISR do M33 |
| jitter do carimbo | dispersão de (carimbo gravado − instante real da conversão) | carimbos do `raw.bin` contra as bordas de `DRDY`, alinhados por um evento comum (§3.3) |
| granularidade do carimbo | menor diferença não nula entre carimbos | `raw.bin` |
| latência até o registro | conversão → quadro escrito no `/data` | **declarada não medida** na primeira passada; exige marcador no caminho de escrita |

### 1.3 Custo

| Métrica | Como |
|---|---|
| CPU do A35 por hora de aquisição, por processo (serviço, `spi0`/`irq`, `sugov`) | `/proc/<pid>/stat` e `/proc/stat`, amostrados (§3.1) |
| CPU do M33 (amp) | tempo ocioso do firmware, exportado por ele; ou pino de "ocupado" no analisador |
| memória do serviço | `VmRSS`/`VmHWM` |
| energia por política de frequência | corrente da placa (medidor USB-C em linha, ou fonte de bancada no lugar da USB), `performance` contra `schedutil` contra 1,2 GHz fixo |

### 1.4 Qualidade do sinal

| Métrica | Como |
|---|---|
| piso de ruído, entradas em curto | µV RMS e pico a pico, em 10 s, por ganho e por ODR. Comparar com a tabela de ruído do SBAS499, que **precisa ser transcrita antes** |
| variação entre partidas | o ruído do autoteste em N probes (hoje 49–205 códigos na placa, contra 23 no PC, sem explicação) |
| ganho e linearidade | gerador por atenuador, em vários níveis, contra o multímetro |
| saturação | código no fundo de escala, sem troca de sinal nem *wrap* |
| efeito da alimentação | o mesmo piso com AVDD vindo da USB e da fonte de bancada |

### 1.5 Configurações do conversor (o eixo que atravessa os quatro grupos)

| Fator | Valores | Pelo `usb` | Pelo `amp` |
|---|---|---|---|
| ganho | 1, 2, 4, 6, 8, 12, 24 | sysfs `hardwaregain` | opção do `ControlMessage` |
| ODR | 250 … 16 000 SPS | **só 250** (teto da ponte, §6.1 do `BRINGUP_AFE.md`) | todas |
| clock SPI das leituras | 1, 2, 4, 8, 12 MHz (teto da ponte) | parâmetro `spi_max_speed_hz` | firmware |
| entrada | diferencial (`normal`), curto, sinal de teste, temperatura, alimentação | sysfs `input_mux` | opção |
| montagem referencial ("unipolar", referência comum por SRB1) | sim / não | **bloqueada**: o driver não escreve `MISC1.SRB1`, e a chave da placa sozinha não basta | firmware |
| bias | `off`, `reference`, `derived` | sysfs `bias_drive` | opção |
| detecção de eletrodo solto | ligada / desligada | `lead_off_status` só lê; o controle não existe | firmware |
| temperatura | sensor interno do ADS1299 (MUX) e as duas zonas térmicas do SoC como covariáveis; aquecimento controlado como fator, opcional | — | — |

Um fatorial completo disso passa de dez mil combinações. A §4 diz quais fatores cada métrica
varre, e por quê.

---

## 2. Os dois caminhos até a primeira medida

### 2.1 `usb` — pronto, com quatro correções antes

1. **O carimbo usa `realtime`.** `current_timestamp_clock` é `realtime` numa placa sem RTC, e um
   ajuste pelo NTP no meio da sessão vira jitter falso. Para estatística, `monotonic`, e o relógio
   em uso vai para o manifesto da sessão (§3.4).
2. **O `lost_samples` não chega ao registro.** O serviço não o lê; o `AcquisitionStopped` registra
   `dropped`, que é outra coisa. É o item já aberto da Fase 6, e ele vem antes de qualquer sessão
   que se pretenda citar.
3. **O governador fica fixo** (`performance`, `med-cpufreq-policy`). Medido sem isso, o resultado
   seria sobre o DVFS, não sobre a ligação.
4. **O SRB1**, se a montagem referencial entrar na varredura: decidir e implementar o controle no
   driver (Fase 8-A §12.3 do plano de bancada) antes de medir.

### 2.2 `amp` sintético — o `implementation_plan_m33_firmware.md`, Fases 0 a 4

O firmware gera quadros sintéticos no M33 com carimbo tomado no próprio M33. Isso mede **o
caminho** (rpmsg, serviço, registro) e o determinismo do coprocessador, sem o conversor. É a
coluna `amp` da tabela com "fonte sintética" escrito no título, e é comparável com uma coluna `usb`
de fonte sintética também, que é o gerador de teste interno do ADS1299.

### 2.3 `amp` com silício — quatro coisas que ainda não existem

1. **Fiação**: SPI6 (pinos 19/21/23/24 do conector), `DRDY` num GPIO **atribuível ao M33 pelo
   RIF** (`implementation_plan_ads1299.md` §4.2: escolher pelo RIF, não pela numeração), `RESET` e
   `PWDN` se possível. A ponte MCP2210 tem de soltar o barramento: as duas não podem dirigir o
   mesmo SCLK. Desconectar fisicamente, e não confiar em alta impedância.
2. **RIF e devicetree**: SPI6 e o pino do `DRDY` atribuídos ao CID do M33, num *overlay* que o
   `meta-med-bsp` aplica.
3. **O driver do ADS1299 dentro do firmware**: inicialização, `RDATAC`, a ISR do `DRDY` e as
   opções da prescrição. É código novo, e o mapa de registradores conferido (Fase 0) é o insumo.
4. **A mesma prescrição pelos dois caminhos**: o `do_derive_device_options` já traduz o `afe.*`
   por link. A tabela só é válida se as duas colunas forem configuradas pela mesma prescrição.

---

## 3. Instrumentação

### 3.1 Na placa: o produto é o coletor

A placa não tem `python3` nem `bash`, e as ferramentas de bancada do PC não rodam lá. **Nada novo
entra na imagem para medir.** O serviço de aquisição já grava o que importa (`raw.bin` com
cabeçalhos, auditoria no journal). Ao lado dele roda um amostrador em `sh`/`awk` do BusyBox,
copiado por `scp` para `/tmp` e iniciado com `nohup`, como o `gov2.sh` de 2026-10-04. Ele grava em
`/data/stats/<execução>.csv`, a cada 10 s: `lost_samples`, `/proc/stat`, `/proc/<pid>/stat` do
serviço e das threads de kernel relevantes, `VmRSS`, frequência e trocas da CPU, as duas zonas
térmicas e `NRestarts`.

**Por que não uma ferramenta compilada na imagem**: ela seria código que a imagem de produto não
carrega, medindo uma imagem que não é a de produto. O amostrador lê o que o kernel já publica e
termina com a sessão.

### 3.2 No PC: a análise

`scripts/board-stats/`, em Python, sem dependência além da biblioteca padrão (a mesma disciplina do
`tests/framework`):

- `frames.py`: lê o `raw.bin` (cabeçalho de 40 bytes, `static_assert` do `MedicalDevice.h`) e
  produz saltos de sequência, intervalos, granularidade e taxa;
- `counters.py`: lê o CSV do amostrador e produz perda por janela, CPU e memória;
- `la.py`: lê a exportação CSV do analisador e produz bordas de `DRDY`, leituras, perda pelos fios
  e atraso de leitura;
- `report.py`: junta os três num relatório com o manifesto e a seção "o que não significa".

### 3.3 O analisador lógico: a referência que o sistema não controla

| Canal | Sinal | Ligação |
|---|---|---|
| 0 | `DRDY` | ambas |
| 1 | CS do conversor | ambas (no `usb`, o GP4 da ponte) |
| 2 | SCLK | ambas, para checar o clock SPI efetivo |
| 3 | marcador do M33 (GPIO alternado na ISR e no envio do quadro) | `amp` |
| 4 | marcador de alinhamento | ambas (abaixo) |

**Alinhar os relógios**, para comparar carimbo com borda: o analisador e a placa não compartilham
relógio. O alinhamento é um evento visto pelos dois. No `usb`, a primeira leitura depois de
`buffer/enable` aparece no canal 1 e é o primeiro carimbo do `raw.bin`. No `amp`, o marcador do
canal 3 leva o número de sequência. Com um ponto comum, mais a deriva estimada por regressão ao
longo da sessão, o erro de cada carimbo vira mensurável.

Taxa de amostragem do analisador: ≥ 1 MHz basta para `DRDY` a 250 SPS e para medir atraso de
leitura com resolução de 1 µs. A 16 kSPS, ≥ 10 MHz.

### 3.4 Manifesto de cada execução

Toda execução grava, junto dos dados: a versão do bundle e o hash do kernel, o slot, o
`config_digest` do `eeg.conf`, o governador e a frequência, o relógio do carimbo, a fonte de
alimentação analógica (USB ou bancada), o estado da HMI, a carga aplicada, a temperatura ambiente,
e o hash dos arquivos de dados. **Uma execução sem manifesto não entra em tabela nenhuma.**

### 3.5 Os outros três instrumentos

- **Osciloscópio**: o ripple do AVDD com USB e com a fonte de bancada, que é a hipótese em aberto
  para a variação do ruído; e a forma do `DRDY`, antes de confiar no limiar do analisador.
- **Gerador de função, com atenuador**: sinais conhecidos na entrada para ganho, linearidade e
  saturação; um seno de 10 Hz como sinal de controle nas sessões de integridade.
- **Fonte de bancada**: o AVDD de 5 V fora da USB, com limite de corrente ajustado **antes** de
  energizar.

---

## 4. Desenho experimental

**Condições fixas em toda execução, salvo quando forem o fator variado**: governador
`performance`, HMI desligada, relógio `monotonic`, ganho 24, 250 SPS, os 8 canais, entrada em curto
ou sinal de teste interno, e AVDD pela fonte de bancada.

**Repetição e ordem**: cada condição roda pelo menos três vezes, intercalada (A/B/A/B/A/B, nunca
AAA/BBB), para que uma deriva no tempo não se confunda com efeito. O experimento do governador em
2026-10-04 só foi conclusivo porque foi A/B/A.

**Duração**: janelas de 10 min para integridade e tempo. A perda é de rajadas, e 3 min já se
mostraram curtos (0,020% contra ≤ 0,003% no mesmo governador). Uma sessão de 2 h por ligação, para
dizer algo sobre sessões clínicas. Para ruído, 10 s por condição bastam.

**Quais fatores cada métrica varre**, para não cair no fatorial:

| Métrica | Fatores varridos | Fixos |
|---|---|---|
| integridade do fluxo | ligação × carga × clock SPI × governador (só como controle) | ganho, entrada |
| tempo | ligação × carga × ODR (só no `amp`) | ganho, entrada |
| custo | ligação × ODR × governador | entrada |
| ruído | ganho × ODR (a grade da tabela do SBAS499) × alimentação × ligação | — |
| ganho e saturação | ganho × nível do gerador | ODR 250 |
| configurações | entrada × bias × montagem (SRB1) × detecção de eletrodo solto | ganho 24 |
| temperatura | sensor interno e zonas do SoC, registrados em toda execução; aquecimento controlado como fator, opcional | — |

**Carga**, em quatro níveis nomeados e reproduzíveis: nenhuma; CPU (`yes > /dev/null` nos dois
núcleos); E/S (escrita contínua no `/data` criptografado); USB (um segundo dispositivo no mesmo
hub). A HMI ligada é um quinto nível, porque é a carga que o produto realmente tem.

---

## 5. Fases

### E0 — a instrumentação, testada antes de medir

**Objetivo**: amostrador, análise e captura funcionando, e **vistos falhando**.
**Injeção de falha** (cada ferramenta tem de ver o que procura antes de ser usada):
- `frames.py` contra um `raw.bin` com um quadro removido e um corrompido: tem de achar os dois;
- `la.py` contra uma captura em que uma leitura foi suprimida: tem de achar a perda;
- uma execução com o governador em `schedutil`: amostrador e analisador têm de concordar na perda
  (~0,7%), e o analisador tem de vê-la **sem consultar o `lost_samples`**.
**Critério**: a perda pelos fios e a do `lost_samples` coincidem, ±1 amostra por janela. Se não
coincidirem, um dos dois conta errado, e descobrir qual é o primeiro resultado do plano.

### E1 — `usb`: integridade e tempo, sem carga

Três janelas de 10 min, mais uma sessão de 2 h. Saída: taxa, perda pelas duas fontes, atraso de
leitura (`DRDY` → CS), jitter do carimbo contra o `DRDY`, granularidade.
**O número que importa** é o atraso de leitura. Ele é o erro de fase que o `acquisition.link`
avisa ("o carimbo é de quando o host soube"), e hoje não tem valor.

### E2 — `usb`: sob carga

E1 para cada nível de carga da §4, intercalado.
**Injeção**: a carga de CPU tem de mover o atraso de leitura. Se não mover, a carga não está
chegando ao caminho, e o experimento é inválido.

### E3 — `usb`: custo e energia

CPU e memória por hora, em E1 e E2. Energia em `performance`, `schedutil` e 1,2 GHz fixo (o último
ainda não foi medido como política, só observado com o `ondemand`). Esta fase fecha a pergunta que
a escolha do `performance` deixou aberta.

### E4 — `usb`: qualidade do sinal e configurações

1. ruído em curto, por ganho, com AVDD da USB e da bancada, e N = 20 probes por alimentação: isso
   explica, ou não, os 49–205 códigos;
2. ganho e linearidade com o gerador, em 5 níveis por ganho;
3. saturação e *wrap* em ganho 1 e 24;
4. entradas: `normal`, curto, sinal de teste, temperatura e alimentação; bias nas três
   configurações (`reference` é o controle de `derived`);
5. clock SPI das leituras de 1 a 12 MHz: o efeito no atraso de leitura e na perda;
6. montagem referencial (SRB1), quando o driver a suportar.

**Pré-requisito**: transcrever a tabela de ruído do SBAS499. Sem ela, os itens 1 e 2 não têm
veredito.

### E5 — `amp` sintético

Depois das Fases 0–4 do plano do M33: E1, E2 e E3 com fonte sintética. A ODR varre de 250 a
16 000, o que o `usb` não consegue.
**Injeção (a do plano do M33)**: carregar o A35 e verificar que o jitter do carimbo **não** se
move. Essa é a afirmação inteira da ligação `amp`. Se mover, isso é resultado, não falha do plano.

### E6 — `amp` com o ADS1299

Depois da §2.3: E1 a E4 pela ligação `amp`, com o mesmo conversor, o mesmo gerador, a mesma
prescrição e a mesma sessão de bancada em que a ponte foi medida. A troca de ligação é física
(fiação), então cada troca entra no manifesto e a ordem entre as duas é intercalada por dia.

### E7 — a tabela

A tabela da Fase 7 do `implementation_plan_afe_bench.md`, com uma coluna `amp` sintética, uma
`amp` com silício e uma `usb`. Cada célula tem número, método, a fonte usada (uma ou duas) e o
link para a execução. **Uma célula vazia fica vazia e nomeada.**

---

## 6. Ordem, e o que bloqueia o quê

```
E0 instrumentação ──► E1 usb ──► E2 carga ──► E3 custo
                        │
                        └──► E4 sinal e configurações ◄── tabela de ruído do SBAS499 transcrita
                                                       ◄── SRB1 no driver (só o item 6)

plano do M33, Fases 0–4 ──► E5 amp sintético
fiação + RIF + overlay + driver no firmware ──► E6 amp com silício

E1–E6 ──► E7 a tabela
```

E0 a E4 podem começar já, com a placa, o analisador e a fonte de bancada. E5 depende de um plano
inteiro que ainda não começou, e E6 de E5 mais a §2.3. **O plano não pode esperar o `amp` para
produzir resultado**: a metade `usb` é um resultado por si mesma, e é a que mede a ligação que hoje
está no perfil da placa.

---

## 7. Riscos

- **O analisador e o driver discordarem na perda.** É o risco mais provável e o mais útil: a E0
  existe para descobrir isso antes de qualquer tabela.
- **Ligar o SPI6 com a ponte ainda no barramento.** Duas fontes dirigindo SCLK/MOSI. Desconectar a
  ponte fisicamente na E6.
- **O pino do `DRDY` não ser atribuível ao M33.** Vale escolher pelo RIF antes de soldar
  (`implementation_plan_ads1299.md` §4.2).
- **Ruído de bancada pior que o datasheet.** É esperado (cabos, terra, fonte). O risco real é o
  oposto: um piso bom demais porque a entrada não estava onde se pensava. A injeção da Fase 4 do
  plano de bancada (MUX em curto com o gerador ligado) se repete aqui.
- **A sessão de 2 h encher o `/data`.** A 8,7 kB/s, 2 h são ~63 MB, e o volume tem 390 MB livres.
  As sessões órfãs do item 19 podem ser apagadas antes.
- **Estatística de rajadas.** A perda não é Poisson, vem em rajadas. A média por janela esconde
  isso, então o relatório traz a distribuição, além da taxa.

---

## 8. Onde cada número vai

- cada execução: `BRINGUP_AFE.md`, numa seção por fase (E0–E7), com manifesto e comando;
- os números que o TCC cita: `RESULTS.md` §3, e o que sair de "não medido" sai da §9;
- a tabela final: a Fase 7 do `implementation_plan_afe_bench.md`, preenchida;
- os dados brutos: fora do repositório (são grandes), com o hash no manifesto. O caminho de
  arquivamento é uma decisão pendente deste plano.
