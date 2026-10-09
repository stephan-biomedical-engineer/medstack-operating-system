# Registro de engenharia — o front-end analógico

> **O que este documento é**: o registro do que foi *feito* com o front-end analógico — o que
> quebrou, qual era a causa real e o que provou cada conserto. É o companheiro do
> `BRINGUP_STM32MP2.md` para a metade do conversor, e é onde os planos
> (`implementation_plan_afe_bench.md`, `implementation_plan_iio_afe.md`,
> `implementation_plan_ads1299_upstream.md`) depositam número medido.
>
> **O que este documento não é**: plano. Nada aqui é intenção; o que está escrito aconteceu, com data.
> Onde uma seção diz "não executada", é porque a fase correspondente não aconteceu e a seção existe
> para que a ausência tenha lugar em vez de ser silêncio.
>
> **Como repetir**: o passo a passo para refazer as fases no host, do zero, com o valor esperado de
> cada uma, está no `ROTEIRO_BANCADA_AFE.md`.
>
> **Numeração**: as §1 a §10 correspondem, uma a uma, às fases do
> `implementation_plan_afe_bench.md` — que já aponta para elas por número. A §11 é o catálogo de
> defeitos, no formato do `BRINGUP_STM32MP2.md` §11. A §12 registra a migração dos drivers para a
> árvore do kernel, que não é fase de bancada. A §13 são as regras que este trabalho acrescenta.

---

## 0. A linha, dita primeiro

| | Estado em 2026-10-04 |
|---|---|
| Hardware do front-end | placa de AFE montada (ADS1299 + MCP2210), ligada a um PC e à STM32MP257 |
| Passada de datasheet (Fase 0) | **nenhuma marca aberta** nos dois drivers; a amplitude do gerador, que o datasheet não decide, foi **medida no silício** (§4) |
| Amostra real adquirida | **sim, pelo link `usb`**: no PC (30 min, §6.3) e na placa (30 min, §6.5) |
| Link `spi` (`DRDY` como interrupção) | **nunca rodou** |
| Link `amp` (Cortex-M33) | **nenhuma amostra**: não existe firmware |
| Vazão do link `usb` | **250 leituras/s**, em qualquer ODR e qualquer clock SPI; a 16 kSPS, 1,56% do que o conversor produz (§6.7) |

**Os drivers foram validados?** Funcionam em silício, no PC e no alvo, pelo link `usb` a 250 SPS.
"Validados", sem qualificação, não: boa parte do que implementam nunca foi exercitada, e não houve
verificação no sentido da IEC 62304.

| Camada | `hid-mcp2210` | `ti-ads1299` |
|---|---|---|
| compila limpo (`W=1`, arm64 6.6 e x86 6.8), `checkpatch --strict` 0/0/0 | sim | sim |
| constantes conferidas contra o datasheet | sim | sim, e a amplitude do gerador medida no silício |
| suíte de host sem hardware | **325 verificações**, 13 injeções de falha (`tests/mcp2210`) | **não existe** |
| silício, PC (Ubuntu 6.8) | Fase 1: 8 PASS; desplugar depois do `.remove` (§2) | Fases 3, 4 (só referências internas) e 5 (§4–§6.3) |
| silício, placa (6.6) | probe, chip select, dois desplugues sem `BUG` (§6.4, §6.6) | autoteste, 30 min de aquisição, recuperação após desplugar (§6.4–§6.6) |

**O que nunca foi exercitado:**

- **`hid-mcp2210`**:
  - o contador de bordas do GP6, porque nesta placa o `DRDY` está no GP5;
  - a Fase 2, loopback SPI;
  - a Fase 8-B, incluindo o teste negativo "a aquisição nunca escreve a NVRAM";
  - o clock SPI efetivo das leituras de dados (§6.7).
- **`ti-ads1299`**:
  - o caminho com `DRDY` como interrupção;
  - qualquer ODR acima de 250 sem perda;
  - sinal externo nos pinos de entrada;
  - detecção de eletrodo solto e SRB1, que o driver não controla;
  - a Fase 8-A.
- **Os dois**:
  - toda perda medida foi contada pelo próprio driver, sem referência independente (o analisador
    lógico, E0 do `implementation_plan_board_stats.md`);
  - nenhum foi submetido ao kernel.

O quadro anterior desta seção, de 2026-09-26, está no histórico do git; ele registrava o estado
em que nenhum hardware tinha chegado.

---

## 1. Fase 0 — a passada de datasheet · **parcial, 2026-09-26**

### 1.1 O insumo

`docs/Register_Map_ADS1299.md`: a Tabela 11 (atribuição de registradores) e as tabelas 12 a 28
(descrição de campo por registrador) do SBAS499, transcritas. Existe para que conferir uma constante
do driver seja uma consulta neste repositório e não num PDF que mais ninguém tem.

### 1.2 O que foi conferido

| Registrador | Tabela | Veredito |
|---|---|---|
| `ID` (0x00) | 12 / Fig. 50 | **confere** — `GENMASK(4,2) == 0x7` consolida o bit 4 "sempre lê 1" com `DEV_ID=11`; canais por `4 + 2·NU_CH` |
| `CONFIG1` (0x01) | 13 / Fig. 51 | **defeito D2** — faltava o segundo campo reservado (4:3 = 2h) |
| `CONFIG2` (0x02) | 14 / Fig. 52 | **defeito D1** — todos os campos uma posição acima |
| `CONFIG3` (0x03) | 15 / Fig. 53 | **confere**; e a falta de `BIAS_STAT` virou lacuna L1 |
| `LOFF` (0x04) | 16 / Fig. 54 | **confere**; nunca escrito, então a peça fica no reset — escolha de configuração feita por deixar o registrador como veio |
| `CHnSET` (0x05–0x0C) | 17 / Fig. 55 | **confere** — `PD` 7, `PGA` 6:4, `SRB2` 3, `MUX` 2:0 |
| Tabela de ganhos do PGA | 17 | **confere** — `{1,2,4,6,8,12,24}`, sete entradas porque 111 é "do not use" |
| Valores de `MUX` | 17 | **conferem**, os oito |
| `CONFIG4` (0x17) | 28 / Fig. 66 | **confere** — `SINGLE_SHOT` 3, `PD_LOFF_COMP` 1 |
| `LOFF_SENSP/N` (0x0F–0x10) | 20, 21 | **conferem**; nunca escritos → **defeito D3** |
| Amplitude do gerador de teste | 14 | **o datasheet não decide** — ambiguidade A1 |

Contagem de dívida: de 8 marcas `[SBAS499?]` originais, **9 viraram citação de tabela** e **1
permanece** — e essa uma deixou de ser leitura pendente para ser medição de bancada explícita.

### 1.3 O que a passada custou, e o que ela rendeu

Duas horas de leitura, dois registradores lidos com atenção antes do primeiro defeito aparecer. É o
primeiro argumento empírico de que a Fase 0 não é burocracia: **nenhuma das verificações posteriores
enxergaria o D1** (§11).

### 1.4 O que falta

- A seção de **temporização** do SBAS499 (§6/§7) não foi transcrita. Consequência imediata: o
  binding não pode declarar um máximo para `spi-max-frequency`, e os 4 MHz do exemplo vêm do
  parâmetro que a ponte usava, não do datasheet.
- O datasheet do **MCP2210** (DS20005176) não foi aberto. As 8 marcas `[DS20005176?]` continuam
  intactas.

---

## 2. Fase 1 — a ponte sozinha, sem conversor · **no host x86-64: 8 PASS, 0 FAIL, 2 SKIP (2026-10-03); a placa de AFE briga com a NVRAM de fábrica (§2.3); na STM32MP257: não executada**

A Fase 1 roda na STM32MP257 com o `hid-mcp2210` carregado, e isso continua sem acontecer. O que
aconteceu foi a ponte plugada num PC de desenvolvimento (Ubuntu, kernel `6.8.0-146-generic`, que
**não tem** driver de MCP2210 — nenhum alias `mcp2210` no `modules.alias`), dez ciclos de
plugar/desplugar, todos idênticos. É a primeira observação desta peça, e ela fecha uma constante:

```
usb 1-3: new full-speed USB device number 6 using xhci_hcd
usb 1-3: New USB device found, idVendor=04d8, idProduct=00de, bcdDevice= 0.02
usb 1-3: New USB device strings: Mfr=1, Product=2, SerialNumber=3
usb 1-3: Product: MCP2210 USB to SPI Master
usb 1-3: Manufacturer: Microchip Technology Inc.
usb 1-3: SerialNumber: 0002589022
hid-generic 0003:04D8:00DE.0007: hiddev1,hidraw3: USB HID v1.11 Device [...] on usb-0000:00:14.0-3/input0
```

| O que foi visto | O que isso fecha | O que isso **não** fecha |
|---|---|---|
| `04d8:00de` | o PID `0x00de` do `hid-ids.h:958` deixa de ser só corroborado: é o que **esta** peça enumera | que os ajustes de *power-up* da NVRAM sejam os de fábrica — só que, hoje, dão este par |
| *full-speed*, HID v1.11, uma interface (`input0`) | a metade de host do critério 1 da Fase 1 | os critérios 2 a 6, que precisam do nosso driver |
| `hid-generic` leva o dispositivo | o comportamento esperado **sem** o nosso driver — a linha de base da primeira injeção de falha da Fase 1 | — (o que o nosso driver faz diante dele está na §2.2) |
| `bcdDevice 0.02`, serial `0002589022` | a identidade da peça usada na bancada, para que registros futuros digam de qual unidade falam | — |

**O número do `hidraw` não é fato.** Na mesma sessão a ponte foi `hidraw3` e depois `hidraw6`,
porque um receptor de teclado replugado pegou o `hidraw3` no meio. É a regra do
`BRINGUP_STM32MP2.md` §11 ("um nome de dispositivo é sempre uma corrida") vista num terceiro
barramento, e é por isso que a camada adjunta dá nome à ponte por regra udev em vez de por ordem.

### 2.1 A NVRAM, antes de qualquer driver nosso

`scripts/mcp2210-nvram-snapshot.py`, por `hidraw`, com a ponte no `hid-generic`. Ele só envia `0x61`
(*Get NVRAM Settings*) com os cinco sub-comandos de leitura; qualquer outro par é recusado antes de
abrir o arquivo, e a recusa de `0x60`, `0x70` e de um sub-comando inválido foi testada. Nenhuma
ferramenta da Microchip foi aberta. Linha de base gravada em `~/mcp2210-nvram-antes.txt` (fora do
repositório; o conteúdo relevante está abaixo).

| Bloco | Campos documentados, decodificados | Fonte |
|---|---|---|
| `0x10` SPI de *power-up* | 1 000 000 bit/s; CS ocioso `0xffff`, ativo `0x0000`; atrasos todos 0; 4 bytes por transação; modo 0 | Tab. 3-18, bytes 4–20 |
| `0x20` chip de *power-up* | GP0 GPIO, **GP1 chip select**, GP2–GP6 função dedicada (GP6 = contador), GP7 GPIO, GP8 entrada; saídas `0xff`, direções `0xff` (todas entrada); outros `0x12` = *remote wake-up* ligado, **contar bordas de descida**, barramento liberado entre transferências; acesso `0x00` = **sem proteção** | Tab. 3-20, bytes 4–18 |
| `0x30` USB | VID `04d8`, PID `00de`, alimentado pelo barramento (`0x80`), 100 mA | Tab. 3-22, bytes 12–15, 29, 30 |
| `0x40` / `0x50` | `MCP2210 USB to SPI Master` / `Microchip Technology Inc.` | Tab. 3-26 / 3-24 |

**Os deslocamentos de `0x30`, `0x40` e `0x50` conferem com o silício por duas fontes
independentes da tabela.** A primeira é a enumeração: os quatro campos decodificados são idênticos
ao que o USB anunciou (`dmesg`, `lsusb`). A segunda é estrutural: os bytes que a tabela chama de
*don't care* em `0x30` são o **descritor de dispositivo USB do próprio chip** a partir do byte 4
(`12 01 00 02 00 00 00 08 d8 04 de 00 02 00 01 02 03 01` = bLength, bcdUSB 2.0, …, idVendor,
idProduct, bcdDevice `0x0002`, iManufacturer/iProduct/iSerial, 1 configuração), seguido do
descritor de configuração (`09 02 29 00 01 01 00 80 32`). Os bytes 12–15 e 29–30 da tabela caem
exatamente sobre idVendor/idProduct e bmAttributes/bMaxPower. Uma tabela errada não acertaria as
duas coisas por acaso.

**Dois defeitos da ferramenta, achados na primeira execução**, e nenhum mudou um byte lido:

- Os quatro cruzamentos com a enumeração deram FAIL com `enumeracao=None`. Ela subia **um** nível a
  partir do `hidraw` e chegava à interface USB (`1-3:1.0`), não ao dispositivo (`1-3`), onde moram
  `idVendor` e as strings. Agora sobe até achar `idVendor`. Os valores conferem à mão com o `dmesg`.
- O `--compare` era byte a byte sobre as 64 posições. Mas o fim das respostas `0x10` e `0x20` é o
  **mesmo resíduo de buffer** a partir do byte 21 (`20 04 e2 11 1d ac …`), e a tabela o declara
  *don't care*. Nada garante que esse resíduo sobreviva a outros comandos, e o teste negativo
  acusaria uma escrita que nunca aconteceu. O veredito agora é sobre os campos documentados, e
  diferença em *don't care* é informada sem contar. Injetado contra o snapshot gravado: inverter
  um byte *don't care* não muda o veredito; mudar o PID ou pôr o acesso em `0x80` muda.

### 2.2 A Fase 1 com o nosso driver, no host

`hid-mcp2210` do `linux-med` `09163164d422`, sem modificação, compilado fora da árvore contra os
headers do `6.8.0-146-generic` com `W=1` — **zero warnings, zero deriva de API de 6.6 para 6.8** —,
assinado com uma chave MOK de bancada (o host tem Secure Boot e `lockdown=integrity`) e instalado
em `updates/`. `sudo sh scripts/afe-phase1.sh`:

| Critério | Observado | Veredito |
| :--- | :--- | :--- |
| 1. VID/PID, *full-speed* | `04d8:00de`, 12 Mbit/s | PASS |
| 2. linha do probe | `USB-SPI bridge ready, 9 GPIOs, 8 chip selects, nothing attached` | PASS |
| 3. bind nosso, não do `hid-generic` | `mcp2210` = `0003:04D8:00DE.0005`, `hid-generic` = nada | PASS |
| 4. `gpiochip` com 9 linhas | debugfs: `gpiochip1: GPIOs 872-880, parent: hid/0003:04D8:00DE.0005, mcp2210` = 9 | SKIP no script, **9 à mão** |
| 5. contador lê e zera | `counter0`, sinal GP6, `increase`, `falling edge`; 0 e 0 em repouso; escrever 0 zera; não-zero recusado | PASS (ver o limite abaixo) |
| 6. 1000 pulsos → 1000 ± 0 | — | não executado: pede gerador |

**O critério 3 responde metade da primeira injeção de falha.** O módulo foi carregado com a ponte
**já** presa ao `hid-generic`, e o nosso driver tomou o *bind* sem `unbind` explícito. Isso fecha,
no kernel 6.8, a hipótese que a §13.3 do plano marcava como "lida no fonte e não observada": o
`id_table` basta, sem `HID_QUIRK`. A outra metade (tirar o nosso módulo e ver o `hid-generic`
retomar) e a injeção de ordem de carga não foram feitas.

**O limite do critério 5, que o script exagerava.** Ele dizia que ler o contador "confirma os
deslocamentos" de `GET_CHIP_SETTINGS`/`SET_CHIP_SETTINGS`/`GET_INT_COUNT`. Não confirma. Confirma
que o silício aceitou os três com estado `0x00` e eco certo (o driver descarta eco errado). Mas um
contador em repouso lê 0, e um deslocamento errado sobre um byte zerado também lê 0. O valor só é
evidência no critério 6. A mensagem foi corrigida.

**O SKIP do critério 4 era um defeito do script que também teria aparecido na placa.** Ele lia
`label`/`ngpio` de `/sys/bus/gpio/devices/gpiochipN`, que só têm esses atributos com
`CONFIG_GPIO_SYSFS`. A imagem não tem esse símbolo, e o Ubuntu também não os expõe ali. Agora o
número de linhas sai do intervalo do debugfs.

**O que esta seção não diz**: nada sobre a placa. O build arm64 contra o `linux-stm32mp` 6.6, o
controlador USB da STM32MP257, o empacotamento do Yocto e a regra udev dentro da imagem continuam
sem medição. O *bind* observado vale para o 6.8, e o 6.6 da placa tem o mesmo `hid_generic_match`
apenas por leitura. E o *taint* `loading out-of-tree module` é do build no host, não do driver.

### 2.3 A placa contra a configuração de fábrica · **achado e provisionado em 2026-10-04; driver da placa em `board/medplatform-afe`**

A pinagem da placa de AFE, informada depois da §2.2 e **fixa**, porque a placa já está montada:
VDD = VUSB = 3,3 V (a configuração que o DS20005176 §1.5 pede para I/O em 3,3 V); MOSI, MISO, SCK;
**CS no GP4**, **DRDY no GP5**, um LED no GP6, e **GP0–GP3, GP7 e GP8 ligados direto ao terra**.

Cruzada com o bloco `0x20` lido na §2.1, a configuração de fábrica briga com a placa desde o reset,
antes de qualquer driver:

| GP | Placa | Fábrica | Efeito |
|---|---|---|---|
| GP1 | GND | chip select, ocioso alto | **saída alta contra o terra** |
| GP2 | GND | SSPND (§1.6.1.3) | **saída alta contra o terra** com o USB ativo |
| GP3 | GND | LED de tráfego SPI (§1.6.1.9) | **saída alta contra o terra** em repouso |
| GP4 | CS do conversor | LOWPWR (§1.6.1.5) | saída que não acompanha o SPI; o conversor nunca é selecionado |
| GP5 | DRDY (saída do conversor) | USBCFG (§1.6.1.4) | **duas saídas uma contra a outra** |
| GP6 | LED | entrada do contador | o LED nunca acende; o contador "conta" o nó do LED |

Isso explica o critério 5 da §2.2 sem invalidá-lo: os dois zeros vieram de um contador ligado num
LED apagado, e o critério nunca afirmou mais do que o protocolo. As sessões das §2.1–2.2 rodaram
nesse estado, e a peça respondeu normalmente em todas. Isso não prova que ela não foi estressada.

**Duas consequências que a placa fixa torna permanentes**:

1. **O power-up só fica seguro gravando a NVRAM uma vez.** Ela é carregada no reset, então só ela
   chega antes de qualquer software. `scripts/mcp2210-provision.py` grava **apenas** o bloco de chip
   settings: GP0–GP3, GP5, GP7 e GP8 como entrada, GP4 como chip select, GP6 como saída que nasce em
   1 (o LED, ativo em nível alto, aceso como indicador de ponte alimentada e configurada; em
   2026-10-04 a primeira gravação o deixava em 0) e saída padrão zero em todos os outros pinos.
   Assim, um pino aterrado que alguém troque para saída dirige 0, não 1. Acesso continua `0x00`, por construção. A ferramenta mostra antes, exige
   `--write` e uma palavra digitada num terminal, e relê os cinco blocos depois, provando que só um
   mudou. É o "estado de fábrica é um estado, e alguém tem que criá-lo" do `BRINGUP_STM32MP2.md`
   §11, num terceiro componente. O snapshot da §2.1 passa a ser o registro do estado de **fábrica**,
   e a linha de base do teste negativo da Fase 8-B vira o snapshot tirado depois do provisionamento.
2. **Nesta placa, o link `usb` não conta amostras perdidas.** O MCP2210 só conta bordas no GP6, e o
   DRDY está no GP5. O DRDY continua legível como nível de GPIO, mas sem contagem não há como o host
   saber que uma amostra se perdeu. Esse é o limite que o plano de bancada (Fase 5) e o
   `acquisition.link` existem para registrar, e precisa constar de toda sessão adquirida por esta
   placa. Não é um defeito a corrigir em software.

**O driver da placa**, numa branch própria do `linux-med` (`board/medplatform-afe`, a partir de
`09163164d422`), para que `feature/ads1299-mcp2210-driver` siga intacta para mainline. O princípio
vem do autor do projeto: **a configuração é da placa e se pensa em userspace; nenhuma escolha da
placa pode impedir o driver; o que não pode acontecer é configurar errado para a placa.** Auditado
por esse critério, o driver upstream violava isso em quatro lugares, e os quatro mudaram:

| # | Upstream | Branch da placa | O que a placa ganha |
|---|---|---|---|
| 1 | `claim_chip_select()` transformava um GPIO em chip select na RAM | `check_chip_select()` só aceita pino já designado CS, recusa nomeando o pino e manda provisionar | `spi_chip_select` tem default **0**, e o GP0 está aterrado: o upstream o deixaria em nível alto contra o terra |
| 2 | o probe **gravava** o modo de contagem (`0x21`) e falhava se não conseguisse | o modo é **lido**, e o `counter` reporta borda de descida ou de subida conforme o chip | o driver não envia `0x21` em lugar nenhum; `mcp2210_write_chip_settings()` foi removida |
| 3 | o `counter` era registrado mesmo sem GP6 dedicado | sem GP6 dedicado e em modo de borda, não há `counter`, e o log diz que o link não reporta perdas | nesta placa (DRDY no GP5), nenhum instrumento que leia 0 para sempre |
| 4 | filho SPI recusado = probe inteiro falha | o filho falha sozinho; ponte, `gpiochip` e `counter` ficam | uma configuração incompleta não derruba a ponte |

Medido: compila sem warnings em `W=1` contra o 6.8, `checkpatch --strict` dá 0/0/0, e a suíte de
host ganhou uma variante por branch (`tests/mcp2210`, `VARIANT` segue a branch). Na variante da
placa são **248 verificações e 0 falhas**, e toda sub-verificação da seção A termina conferindo que
nenhum `0x21` saiu. **Injetado**: as expectativas da placa contra o driver upstream dão 16 falhas,
todas na seção A, cobrindo as quatro mudanças, e nenhuma fora dela; a variante upstream continua
com 209/0 no driver dela. Não medido: nada disso foi carregado contra a peça ainda.

**Risco residual, depois do provisionamento**: o `gpiochip` continua oferecendo GP0–GP3, GP7 e GP8
como GPIO. Um `gpioset` neles com valor 1 é um curto ao terra, e o driver não tem como saber disso
sem uma descrição da placa. A imagem de produto não toca nessas linhas. Na bancada, a regra é
escrita: **nunca dirigir 1 em GP0–GP3, GP7 ou GP8**.

**Discrepância na transcrição**: a §1.6.1.6 de `Register_Map_MCP2210.md` diz que a entrada de
interrupção é o **GP4**. As linhas 265 e 301 da mesma transcrição e o driver dizem **GP6**. Quase
certamente é erro de transcrição; falta conferir no PDF.

#### 2.3.1 O provisionamento, executado · **2026-10-04 00:07**

`sudo python3 scripts/mcp2210-provision.py --write`, serial `0002589022`. A releitura deu PASS
nos cinco blocos: o de chip settings igual ao alvo, e SPI, USB e as duas strings intocados. Os
campos documentados de `0x20` agora são `00 00 00 00 01 00 00 00 00 | 00 ff bf ff 12 00`. A nova
linha de base do teste negativo é `~/mcp2210-nvram-provisionada.txt` (`sha256-documented
e42a2450…06402`; **substituída pela da §2.3.2**), e o snapshot da §2.1 fica como registro do estado de fábrica. Os quatro
cruzamentos com a enumeração deram PASS, agora que a ferramenta acha o dispositivo USB.

**A decisão de comparar só os campos documentados foi confirmada pela peça.** Entre o snapshot
de fábrica e o provisionado, o resíduo *don't care* do fim de `0x10` e `0x20` passou de
`20 04 e2 11 1d ac …` para um trecho da string do fabricante (`70 00 20 00 54 00 …`), e o byte 3
de todas as respostas foi de `0x46` para `0x62`. Uma comparação byte a byte teria acusado uma
escrita na NVRAM que nunca aconteceu.

**A NVRAM só vale no reset**, e isso foi visto. Um `modprobe` logo depois da gravação, sem
replugar, subiu sem aviso nenhum sobre o GP6: a RAM ainda era a de fábrica. Depois de desplugar
e plugar:

```
00:10:03 mcp2210 …: GP6 is not provisioned to count edges; no counter, and this link cannot report lost samples
00:10:03 mcp2210 …: USB-SPI bridge ready, 9 GPIOs, 8 chip selects, nothing attached
```

`/sys/bus/counter/devices/` vazio, `gpiochip1` presente, ponte presa ao `mcp2210`. É o item 3
da tabela acima observado na peça: sem instrumento, nenhum `counter` lendo 0.

**Observado de passagem**, com o driver da placa:

- **default `spi_chip_select=0`, peça de fábrica** (00:07:39): `GP0 is a GPIO in the bridge's
  settings, not a chip select; provision the board, this driver does not repurpose pins`,
  `cannot attach 'ads1299' … -16`, e mesmo assim `ready`. O item 1 e o item 4 da tabela, na
  peça: o driver upstream, nesse mesmo ponto, teria posto o GP0 aterrado em nível alto.
- **`rmmod` com a ponte presa** (00:07:42 e 00:09:51): o `hid-generic` retoma a ponte sozinho.
  Isso fecha a metade da injeção A da Fase 1 que a §2.2 deixou aberta.
- Um módulo já carregado guarda os parâmetros do último `modprobe`. Replugar com ele carregado
  não volta ao default. Isso explica por que o replug das 00:09:40 não recusou o GP0.

#### 2.3.2 O LED do GP6, e o power-up com ele aceso · **2026-10-04**

**O LED, pelo `gpiochip`.** Com o driver da placa preso à ponte, `/dev/gpiochip1`
(`label=mcp2210`, 9 linhas) mostrou o GP6 como saída e livre. A linha foi pedida pela ABI do
char device (`GPIOHANDLE_REQUEST_OUTPUT`, consumidor `led-test`, sem `libgpiod`, que o host não
tem) e alternada 5 vezes, 1 s em cada nível. A releitura deu `1`/`0` em todos os ciclos, e o LED
piscou. A releitura sozinha não provava nada além do latch de saída da ponte; quem fechou foi o
olho. A **polaridade** foi medida à parte, porque uma piscada simétrica não a distingue: GP6
deixado em 1, LED aceso. Ativo em nível alto.

**O power-up.** O LED passou a nascer aceso, como indicador de ponte alimentada e configurada.
Isso é a saída padrão da NVRAM (o byte 13 do bloco `0x20`, Tabela 3-1), **não a EEPROM do
usuário**, que o chip não lê para nada. `scripts/mcp2210-provision.py` agora declara o nível de
power-up por pino, ao lado da direção, e recusa um nível alto declarado numa entrada. Só o GP6
vai a 1; todo pino aterrado continua com saída padrão 0, que é o motivo da regra original.

`--write`, serial `0002589022`: a prévia marcou **uma única linha**, `saida padrao 0x00 -> 0x40`,
e a releitura deu PASS nos cinco blocos. Depois de desplugar e plugar, **o LED acendeu sozinho**.
Os campos documentados de `0x20` agora são `00 00 00 00 01 00 00 00 00 | 40 ff bf ff 12 00`,
diferentes dos da §2.3.1 só no byte da saída padrão. A nova linha de base do teste negativo da
Fase 8-B é `~/mcp2210-nvram-provisionada.txt` (`sha256-documented 4cad3871…9aafe45`). Os quatro
cruzamentos com a enumeração deram PASS.

**O que isso não diz:** que o LED nasce aceso antes de o driver carregar. No replug o
`hid_mcp2210` foi carregado sozinho pelo modalias, então o "acendeu sozinho" foi observado com o
driver presente. O argumento de que foi a NVRAM e não o driver é de leitura: o driver só escreve
valor de GPIO quando alguém pede a linha (`mcp2210_gpio_set`), e ninguém pediu. Para observar sem
o driver, basta plugar com o módulo fora do caminho (`modprobe -r` e a ponte em `hid-generic`).

**De passagem:** o snapshot recusou com `nenhum hidraw` logo depois do replug, porque o
`hid_mcp2210` autocarregado não expõe `/dev/hidrawN`. É o comportamento esperado, mas a mensagem
aponta para "a ponte está plugada?", e a causa era o driver que a segurava. Corrigido: o
snapshot agora olha o barramento HID e, se a ponte estiver presa a um driver, diz qual
(verificado com a ponte presa ao `mcp2210`).

### 2.4 Desplugar a ponte derruba o kernel · **2026-10-03 23:15; corrigido nas duas branches (upstream: `d658fd3ac242`, falta o `Signed-off-by`)**

A ponte foi desplugada às 23:15:14, doze minutos depois da Fase 1 da §2.2, com o
`hid-mcp2210` upstream preso a ela. Ninguém viu nada na hora. O efeito apareceu no dia
seguinte, quando um `rmmod hid_mcp2210` ficou em estado `D` para sempre (módulo `going`,
`refcnt -1`). O journal tinha guardado:

```
usb 1-3: USB disconnect, device number 5
kernel BUG at mm/slub.c:553!
RIP: 0010:kfree+0x2cd/0x370
 hid_free_buffers.isra.0+0x44/0x70 [usbhid]
 usbhid_stop+0x164/0x1b0 [usbhid]
 hid_hw_stop+0x22/0x40 [hid]
 mcp2210_hid_stop+0x1a/0x30 [hid_mcp2210]
 devm_action_release+0x12/0x30
 devres_release_group+0x110/0x140
 hid_device_remove+0x57/0xd0 [hid]
```

**Causa, lida em `hid-core.c:2694-2710` desta árvore**: o `hid_device_remove()` chama
`hdrv->remove()` se ele existir e, **se não existir, chama `hid_hw_stop()` sozinho**. Só
depois ele libera o grupo de devres do driver. O driver não tinha `.remove`, e a ação devm
`mcp2210_hid_stop()` também chama `hid_hw_stop()`. A segunda parada fez o `usbhid` liberar
os mesmos buffers outra vez, e o alocador detectou a liberação dupla (`slub.c:553`). O
worker do hub USB morreu no meio, segurando o dispositivo, e por isso o `rmmod` nunca
voltou. Com o slab corrompido, o único estado seguro é reiniciar.

**Por que nada viu antes**: o `hid-mcp2221`, adotado como referência no
`implementation_plan_mcp2210.md` §2, tem `.remove` (`hid-mcp2221.c:1242`), e esse padrão
não estava entre os oito que o plano extraiu dele. A suíte de host não podia ver: o shim
liberava o devres sem antes fazer o que o núcleo HID faz. Nenhum build vê isso, e a Fase 1
também não, porque nenhum critério dela desplugava a ponte.

**Correção, na `board/medplatform-afe`**: um `.remove` vazio com o comentário de por que
ele precisa existir. **Teste que move a verificação para a suíte**: o `do_release()` de
`tests/mcp2210` agora reproduz o `hid_device_remove()` na ordem dele, e cada remoção de um
probe bem-sucedido confere que o hardware foi parado exatamente uma vez. Injetado: sem o
`.remove`, **37 falhas** (uma por remoção); com ele, **285/0**.

**A branch de upstream tinha o mesmo defeito**, porque o teardown é o mesmo código. A
mesma correção entrou nela como `d658fd3ac242`, no fim da série: antes, a variante
`upstream` da suíte dava **240 verificações e 31 falhas**; depois, **240/0**. O `checkpatch
--strict` deu 0/0/0, e o `W=1` contra o 6.8 não deu warnings. O commit ainda não tem
`Signed-off-by`, que é do autor. O build arm64 contra o `linux-stm32mp` não foi refeito
para esta mudança.

**Observado na peça depois da correção**: `rmmod` com a ponte presa às 00:07:42 e às 00:09:51,
e **desplugar com o driver preso às 00:09:31** (o caminho exato do BUG). Nenhum `kernel BUG`,
`Oops` ou `WARNING` no boot inteiro. Vale para o 6.8 do host, não para o 6.6 da placa.

**Regra que esta ocorrência acrescenta à bancada**: depois de qualquer sessão, `journalctl
-k -b 0 | grep -iE 'BUG|Oops|WARNING'` antes de concluir que nada aconteceu. O BUG ficou
gravado doze minutos antes de qualquer sintoma, e foi o sintoma, não a verificação, que
o achou.

## 3. Fase 2 — o barramento SPI transfere bytes · **não executada**
## 4. Fase 3 — o conversor responde, identidade do silício · **cumprida no host (2026-10-04), exceto o symlink da imagem; na STM32MP257: não executada**

Placa de AFE: AVDD 5 V, DVDD 3,3 V, clock e referência internos, START em pull-down, PWDN em
pull-up (header para o modo AMP), RESET com botão em pull-up, CLKSEL desativado, chave de SRB1.
**Nenhum eletrodo conectado: todas as entradas analógicas flutuam.** Tudo nesta seção e na Fase 4 usa
entradas internas do MUX (`shorted`, `test_signal`, `supply`, `temperature`), que desconectam os
pinos do canal. O `LOFF_STATP = 0x03` e o bit de status `c00800` vistos pelo oráculo são os
comparadores de lead-off indicando eletrodo solto, como devem. Qualquer leitura com o MUX em
`normal` nessa condição lê pinos abertos e não significa nada.
Ponte provisionada (§2.3.1), CS no GP4, SPI modo 1. A Fase 2 (jumper MOSI↔MISO) **não foi feita**:
com o conversor soldado no barramento, ela não é possível sem retrabalho.

### 4.1 O driver: passa do ID, reprova no autoteste

`sudo modprobe hid_mcp2210 spi_device=ads1299 spi_chip_select=4`, com `ti-ads1299` de
`6d8d5f7eeff9` compilado e assinado para o 6.8 (sem warnings em `W=1`):

```
mcp2210 …: USB-SPI bridge ready, 9 GPIOs, 8 chip selects, ads1299 attached
ads1299 spi1.4: supply avdd not found, using dummy regulator
mcp2210 …: asked for 2048000 Hz, bridge programmed 2000000 Hz
ads1299 spi1.4: no DRDY interrupt: sampling on a host timer, timestamps are host side
ads1299 spi1.4: self test: test signal swings 25..38 codes peak to peak across channels, expected 167772 +-33554 at gain 24
ads1299 spi1.4: error -EIO: self test failed, front-end not registered
```

Não houve `Unknown ID`, então o probe passou do registrador de identidade. O autoteste errou por
**um fator de ~5000**, não o fator de dois da A1. 25 a 38 códigos no ganho 24 são ~0,6–0,8 µV de
pico a pico, o nível de ruído de entrada em curto do conversor.

### 4.2 O oráculo: o silício responde certo a tudo que lhe foi pedido

`scripts/afe-spi-oracle.py` fala com o conversor pela ponte, via `hidraw`, **sem nenhum dos dois
drivers no caminho**, e foi escrito a partir das transcrições, não do código dos drivers. Na ponte
ele só usa a RAM (`0x40`) e transferências (`0x42`); NVRAM, chip settings e senha são recusados por
construção. SPI a 1 MHz, modo 1, e conversão **contínua**.

- **ID `0x3E`**: REV_ID 1, bit 4 = 1, DEV_ID 3 (ADS1299-x), NU_CH 2, ou seja, 8 canais. Primeira
  leitura do registrador de identidade crua, em silício. É o critério 2 da Fase 3, na parte do ID.
- **As escritas do driver chegaram ao silício.** O mapa "como encontrado" era o rastro do probe
  anterior, e não o de reset: `CONFIG3 = 0xE1` (referência ligada pelo driver; o bit 0 é status),
  `CHnSET = 0x60` (o MUX normal que o autoteste devolve), `CONFIG4 = 0x0A` (o single-shot do
  driver). Isso derruba a primeira hipótese, a de que o regmap com cache escondia escritas
  perdidas.
- **Escrita e releitura**: `CONFIG2` 0xC0→0xD0 e `CH1SET` 0x60→0x65 relidos iguais.
- **Sinal de teste, medido pelo oráculo**: os 8 canais oscilam **167 259 a 167 335 códigos de
  pico a pico** (de ≈ −84 500 a ≈ +82 750), contra 167 772 esperados para VREF/1200 pp. São 0,3%
  abaixo, dentro da tolerância da referência interna. A palavra de status começa por `1100`, como
  deve.

**Dois defeitos da primeira versão do oráculo**, nenhum da peça: ele comparava o `BIAS_STAT`
(bit 0 de `CONFIG3`, só leitura) e acusou `FAIL CONFIG3`; e comparava `LOFF_STATP/N`, que são
estado dos comparadores, e acusou "segunda leitura diferente". Os bits de status agora ficam fora
de toda comparação (`STATUS_MASK`). Com a correção, a saída original reproduz sem falhas.

### 4.3 O que isso localiza

O silício faz tudo o que o autoteste do driver precisa. Os registradores do driver estão certos e
chegaram ao silício. **O defeito está no procedimento de amostragem do driver**, e as duas
diferenças entre ele e o oráculo são as suspeitas:

1. **modo**: o driver usa single-shot com um START por leitura (`ads1299_read_one()`), e o oráculo
   usa conversão contínua;
2. **velocidade do `RDATA`**: o driver lê à velocidade máxima do barramento (`spi_max_speed_hz`,
   4 MHz), acima do f_CLK de 2,048 MHz, e só limita os acessos a registrador. O oráculo lê a 1 MHz.

`afe-spi-oracle.py diff`, 2026-10-04, 30 leituras de cada:

| Modo | `RDATA` | Pico a pico (8 canais) |
|---|---|---|
| contínuo | 1 MHz | 167 260 .. 167 326 |
| **single-shot** | 1 MHz | **24 .. 38** |
| contínuo | 4 MHz | 167 272 .. 167 329 |
| **single-shot** | 4 MHz | **21 .. 33** |

**A velocidade não tem efeito; o single-shot sozinho reproduz a falha do driver**, nas duas
velocidades, com o mesmo número que o driver relatou (25..38). O `RDATA` a 4 MHz, acima do f_CLK,
funciona. A suspeita 2 está descartada, e a suspeita 1 está confirmada como **condição**. Falta o
**mecanismo**: (a) o START reiniciar o divisor do gerador de teste, de modo que cada leitura pega
a onda sempre na mesma fase e o autoteste está mal desenhado; ou (b) a conversão single-shot desse
procedimento estar errada, o que afetaria o `read_raw` também. `afe-spi-oracle.py single` separa as
duas, mostrando níveis em vez de só pico a pico e medindo o MVDD, que não depende do gerador, nos
dois modos.

`afe-spi-oracle.py single`, CH1, 20 leituras de cada:

| Caso | Contínuo (mín / média / máx) | Single-shot (mín / média / máx) |
|---|---|---|
| sinal de teste ~1 Hz | −84 384 / 7 623 / 82 908 | **82 884 / 82 895 / 82 909** |
| sinal de teste DC (`CAL_FREQ = 11`) | −84 389 / −84 378 / −84 358 | −84 391 / −84 379 / −84 365 |
| entradas em curto | −795 / −780 / −771 | −787 / −780 / −771 |
| MVDD, ganho 1 | 4 655 662 / 4 656 134 / 4 656 432 | 4 655 044 / 4 655 221 / 4 655 364 |

**É o mecanismo (a).** O DC, o curto e o MVDD batem entre os dois modos (o MVDD em 0,02%), e o
sinal de ~1 Hz em single-shot fica **preso no nível alto**: cada START reinicia o divisor do
gerador, e toda leitura cai na mesma fase. A conversão single-shot está certa. **O autoteste é que
mede a oscilação de um sinal AC com um modo que zera a fase dele**, e por isso reprovaria silício
bom também na ligação `spi`, onde o driver dá o mesmo START por leitura e espera o DRDY.

De passagem, três medidas: o nível DC do gerador é **negativo** (−84 378, o sinal de "−(VREFP −
VREFN)/2400" da Tabela 14 confirmado); o offset com entradas em curto é −780 códigos no ganho 24;
e o MVDD do CH1, com ganho 1, dá 4 656 134 × 4,5 / 2²³ = **2,498 V**. Se o CH1 mede
(AVDD + AVSS)/2, como se entende do datasheet (o detalhe por canal não está transcrito), isso dá
AVDD = 4,996 V, contra os 5 V informados.

### 4.4 A correção do autoteste · **na `board/medplatform-afe` (`e9f382da54ba`); validada na peça, 2026-10-04 00:50**

O passo de oscilação foi trocado por um passo de **nível DC**: primeiro as entradas em curto (o
ruído, como antes, e agora também o offset de cada canal); depois o gerador em DC, e a amplitude
é |nível − offset| por canal, contra ganho × 2²³ / 2400 ± 20%. O DC não tem fase para perder, e a
mesma medida serve às duas ligações. O comentário que marcava a A1 como `[SBAS499?]` virou citação
da medição, e o cabeçalho do arquivo diz o que foi conferido em silício e em qual peça. Com menos
leituras (10 + 10, e não 40 + 10), o autoteste fica mais rápido.

Medido: compila sem warnings em `W=1` contra o 6.8; `checkpatch --strict` com os mesmos 2
warnings e 2 checks que o arquivo original já tinha (falsos positivos num membro `__aligned`),
nenhum novo. Pelos números do oráculo, o critério novo dá 83 598 contra 83 886 ± 16 777, e o
ruído fica muito abaixo do teto de ~447 códigos. O defeito também está na série de upstream, onde o `ti-ads1299` é o mesmo arquivo.

**Validado na peça**, 00:50:

```
ads1299 spi1.4: no DRDY interrupt: sampling on a host timer, timestamps are host side
ads1299 spi1.4: self test passed: test signal DC level 83542..83829 codes from offset (expected 83886), shorted-input noise 23 codes
```

Os 8 canais ficaram entre 0,07% e 0,4% abaixo do previsto, com ruído de 23 códigos contra um teto de
~447, e o autoteste levou ~2,3 s. Critérios da Fase 3 que se aplicam no host:

| Critério | Observado | Veredito |
|---|---|---|
| 1. probe sem erro | nenhum `dev_err_probe`; `iio:device0` registrado | PASS |
| 2. nome e ID | `name` = `ads1299-8`, derivado do ID; o byte cru `0x3E` e o mapa de reset conferidos pelo oráculo (§4.2–4.3) | PASS |
| 3. `/dev/med-afe-eeg0` | a regra udev é da imagem e não está no host | não aplicável aqui |
| 4. `timestamp_source` | `host_timer`: o `info.irq = 0` da ponte chega ao driver do AFE | PASS |

`sampling_frequency` = 250. Nenhum `kernel BUG`, `Oops` ou `WARNING` no boot.

**Um tropeço de bancada que vale como regra**: a primeira tentativa, às 00:49, ainda rodou o código
**antigo**. O `ti_ads1299` estava carregado desde o probe que falhou às 00:19 (um probe que falha não
descarrega o módulo), e instalar o `.ko` novo não substitui o que está em memória. O `modprobe` da
ponte só recriou o filho SPI, e o código antigo o atendeu. Quem denunciou foi o texto da mensagem
de erro, que era o antigo. Depois de qualquer reinstalação, a sequência é tirar **os dois** módulos
e conferir `/sys/module/ti_ads1299/srcversion` contra `modinfo -F srcversion` do arquivo **antes**
de ler o log.


O "como encontrado" desta execução já estava no reset (era o RESET final da execução anterior; não
houve probe no meio), e o mapa depois de RESET deu **PASS contra a Tabela 11** e leitura estável.
Com isso fecha o critério 2 da Fase 3: ID e valores de reset conferidos em silício.


## 5. Fase 4 — o caminho analógico, estático · **no host, com as referências internas do conversor, 2026-10-04**

`sudo python3 scripts/afe-phase4.py --avdd 5.00`, pelo sysfs e pelo `/dev/iio:device0`, com o
driver da §4.4. Sem fonte DC de precisão, atenuador nem termômetro, e **sem eletrodos** (§4): cada
seção diz o que trocou.

### 5.1 Varredura de ganho (8.1), com o gerador interno como fonte DC

O gerador lido por `in_voltage0_raw` fica preso na fase zero (§4.3), ou seja, é uma DC conhecida
de +VREF/2400. Descontado o offset em curto, no mesmo ganho:

| Ganho | 1x (mV) | 2x (mV) | Razão 2x/1x |
|---|---|---|---|
| 1 | +1,8837 | +3,7520 | 1,9919 |
| 2 | +1,8786 | +3,7477 | 1,9949 |
| 4 | +1,8735 | +3,7429 | 1,9978 |
| 6 | +1,8721 | +3,7413 | 1,9985 |
| 8 | +1,8713 | +3,7406 | 1,9990 |
| 12 | +1,8706 | +3,7396 | 1,9991 |
| 24 | +1,8702 | +3,7394 | 1,9994 |

**PASS**: a dispersão entre ganhos é de 0,72% (1x) e 0,34% (2x), contra o critério de 1%; a média
fica a −0,04% e −0,18% de 1,875 e 3,750 mV, contra 5%. A tabela de ganhos do driver e a escala
(VREF = 4,5 V) estão certas: o erro de 4× e o de 12,5% que o plano procura teriam aparecido como
dois padrões diferentes, e nenhum apareceu. O que isso **não** valida: o pino do eletrodo, porque o
gerador entra pelo MUX, e não pela entrada.

De passagem: o offset em curto é quase constante em **códigos** (−1002 no ganho 1, −780 no ganho 24),
e não em µV referidos à entrada. Ele nasce depois do PGA, e por isso a referida à entrada cai com o
ganho.

### 5.2 Piso de ruído (8.4), entradas em curto, 40 conversões avulsas por ganho

| Ganho | pp (µV) | RMS (µV) | pp (códigos) |
|---|---|---|---|
| 1 | 5,364 | 1,109 | 10 |
| 2 | 2,146 | 0,461 | 8 |
| 4 | 1,341 | 0,275 | 10 |
| 6 | 0,894 | 0,218 | 10 |
| 8 | 0,872 | 0,217 | 13 |
| 12 | 0,581 | 0,131 | 13 |
| 24 | 0,626 | 0,130 | 28 |

O critério do plano compara com a tabela de ruído do datasheet, que **não está transcrita**, então
não há veredito. Os 0,13 µV RMS no ganho 24 são da ordem do que se cita para o ADS1299 a 250 SPS,
uma afirmação a conferir na tabela. Todos ficam bem abaixo do limite de "não está quebrado" de
10 µV pp do framework. Valem para "conversor + placa com entradas abertas, alimentada pela USB", e
foram 40 conversões a ~10/s, não 10 s a 250 SPS.

### 5.3 MVDD e temperatura (8.5)

MVDD do CH1, ganho 1: **2,4972 V**. Contra AVDD/2 = 2,500 V do multímetro, dá **−0,11%: PASS**
(critério 2%), **supondo** que o CH1 mede (AVDD+AVSS)/2. Esse detalhe por canal não está transcrito.

Temperatura: **149 813 µV**. Pela fórmula lida de memória do SBAS499, (µV − 145 300)/490 + 25, isso
dá **34,2 °C**. A fórmula não está transcrita, então isso cumpre só a metade do critério que diz "o
MUX comuta e o número não está fora por uma ordem de grandeza". Sem termômetro de contato, a
comparação de ±5 °C não foi feita.

### 5.4 Injeção de falha: MUX em curto com o gerador ligado

Em curto com o gerador desligado: −784; em curto com o gerador 2x **ligado**: −785; em `test_signal`
com o gerador 2x: 166 516. **PASS**: a leitura em curto não seguiu o gerador (moveu 2 códigos,
contra os 167 300 que o gerador vale). O MUX comuta, e o ruído da §5.2 é do conversor, não de um
gerador vazando. É a injeção que o plano chama de a mais importante da fase.

### 5.5 O gerador pelo buffer (8.3), em modo contínuo, 5 s por ajuste, ganho 24

| Ajuste | Pico a pico (µV) | Esperado | Frequência (Hz) | Esperada |
|---|---|---|---|---|
| `1x_slow` | 3739,4 | 3750 (−0,28%) | 0,9766 | 0,9766 |
| `1x_fast` | 3739,3 | 3750 (−0,29%) | 1,9522 | 1,9531 |
| `2x_slow` | 7477,5 | 7500 (−0,30%) | 0,9766 | 0,9766 |
| `2x_fast` | 7477,5 | 7500 (−0,30%) | 1,9522 | 1,9531 |

**PASS** nos quatro, contra a tolerância de 5%. A frequência fica dentro de 0,05% de
f<sub>CLK</sub>/2²¹ e /2²⁰, o que mede o oscilador interno desta peça. O procedimento do plano, que
lia isto pelo `raw`, mediria uma linha reta; o plano foi anotado.

**Não medido nesta fase**: 8.2 (fundo de escala e saturação, que pede DC externa), a varredura 8.1
contra tensão externa, a temperatura contra termômetro, e o bias (`reference` × `derived`), que
pede eletrodos.

## 6. Fase 5 — aquisição contínua, e as quatro medidas · **parcial no host: taxa medida, explicada e corrigida; 30 min ociosos medidos (§6.1–6.3)**

**O link `usb` não sustenta 250 SPS, e o driver conta exatamente o que perde.** Em cada janela de
5 s da §5.5 chegaram **833 amostras, 167 por segundo**, contra 1250 devidas. Na primeira janela o
`lost_samples` contou **416**: 833 + 416 = 1249 ≈ 1250. A contabilidade do driver fecha. As janelas
2 a 4 mostraram "+0" por um defeito do roteiro, e não do driver: o contador é zerado a cada vez que
o buffer liga (`ads1299_buffer_postenable()`), e o roteiro subtraía o valor anterior. Corrigido: ele
agora lê o contador como o valor da janela e mostra entregues + perdidas contra devidas.

167/s é 1/(6,0 ms): cada amostra custa ~6 ms de ida e volta pela ponte, contra os 4 ms que 250 SPS
dão. E 250 SPS é a **menor** taxa do conversor (`sampling_frequency_available` vai de 250 a 16000).
Portanto, do jeito que o driver lê hoje, um RDATA por amostra com o timer do host, **o link `usb`
perde um terço das amostras em qualquer taxa que o conversor oferece**. É o número que esta fase
existe para medir. Ele contradiz uma suposição da plataforma (o `usb` como ligação de bancada
utilizável a 250 SPS) e precisa entrar no `acquisition.link` e no `RESULTS.md`.

### 6.1 Onde estão os 6 ms · **medido, 2026-10-04**

O endpoint da ponte é interrupt, de 64 bytes, com `bInterval = 1`: o host o consulta a cada 1 ms
(`lsusb -v`). `afe-spi-oracle.py timing`, 300 repetições, pela ponte, sem driver, com o `RDATA` de
28 bytes do driver a 4 MHz:

| Medida | Mediana | p90 | Máximo | Trocas HID | Teto |
|---|---|---|---|---|---|
| (a) uma troca (`0x41`, só leitura) | **1,986 ms** | 2,110 | 2,437 | 1 | 503/s |
| (b) `RDATA` como o driver: `0x40` + `0x42` + `0x42` | **5,995 ms** | 6,099 | 6,972 | 3 | **167/s** |
| (c) `RDATA` sem reenviar o `0x40` | **3,994 ms** | 4,084 | 4,283 | 2 | 250/s |

Nenhuma repetição por `0xF8` nas 900 transações.

**Cada troca HID custa dois quadros USB, de forma determinística**: o OUT vai num quadro e a resposta
IN vem no seguinte. Os 167/s da §6 são as **3 trocas por amostra** do driver × 2 ms, sem resto.
**Uma delas é desnecessária**: `mcp2210_do_transaction()` reenvia as transfer settings (`0x40`)
antes de toda transação, mesmo quando velocidade, CS, modo, tamanho e atrasos são os mesmos da
anterior, como é o caso de cada `RDATA` de uma aquisição. Isso custa 2,001 ms por amostra.

**Sem ela, chega-se a 250/s com margem zero**: a mediana é 3,994 ms e o p90 é 4,084 ms, contra os
4,000 ms que 250 SPS dão. E duas trocas são o mínimo do protocolo: o `0x42` que inicia a transação
devolve "iniciada, sem dados" (Tabela 3-60), e os dados só vêm no seguinte (Figura 3-23). No driver
ainda há o timer do kernel e a fila de mensagens SPI por cima das trocas.

**O que isso estabelece**: o teto do link `usb` não é do software. É do protocolo de comando e
resposta da ponte sobre o quadro de 1 ms do USB full-speed: ~250 amostras por segundo, que é
exatamente a menor taxa do conversor. Cachear as transfer settings deve levar a perda de 33% para
perto de zero, mas não para zero com garantia. Nenhuma taxa acima de 250 SPS cabe neste link.
Isso precisa constar do `RESULTS.md` e do que o `acquisition.link = usb` declara.

Não medido: a perda depois do cache, pelo buffer do driver; e se uma sessão pelo
`eeg-acquisition-service` vê os mesmos números.

### 6.2 O cache das transfer settings · **na `board/medplatform-afe` (`b8dbcb083402`); validado na peça, 2026-10-04**

`mcp2210_set_transfer_settings()` guarda o que escreveu (velocidade, CS ocioso e ativo, tamanho,
atraso de CS, modo) e não reenvia o `0x40` quando a transação seguinte pede exatamente o mesmo. O
cache é zerado antes de qualquer escrita, e também em todo caminho de falha da transação, ao lado do
cancelamento: depois de uma falha, o estado da ponte é justamente o que está em dúvida.

`tests/mcp2210`, variante `board`: **325 verificações, 0 falhas**. A seção K mostra que dez leituras
idênticas mandam um único `0x40` e devolvem os dados certos, que mudar o tamanho ou a velocidade
reescreve, que voltar ao anterior também reescreve, que uma falha esquece o cache, e que a leitura
repetida custa duas trocas, e não três. **Injetado**: com o cache desligado, falham exatamente K1 e
K6; sem a invalidação na falha, falha exatamente K5. `checkpatch --strict` 0/0/0, `W=1` sem warnings.

**Na peça**, `afe-phase4.py --only ac --seconds 10`, com o módulo conferido pelo `srcversion`
(`0D82FC61363C3A20351533F`), a 250 SPS, ganho 24, pelo buffer:

| Ajuste | Entregues | `lost_samples` | Entregues + perdidas | Devidas |
|---|---|---|---|---|
| `1x_slow` | 2498 | 1 | 2499 | ~2498 |
| `1x_fast` | 2499 | 0 | 2499 | ~2498 |
| `2x_slow` | 2499 | 0 | 2499 | ~2498 |
| `2x_fast` | 2498 | 1 | 2499 | ~2498 |

**De 167/s para 250/s; a perda caiu de 33% para 0 a 1 amostra em 10 s (≤ 0,04%)**, e a
contabilidade do driver fecha em todas as janelas. Amplitudes e frequências do gerador continuam
PASS; a frequência ficou mais precisa (1,9530 Hz no `1x_fast`, antes 1,9522) porque a onda agora
chega inteira.

O que isto **não** diz: são quatro janelas de 10 s, com o host ocioso. A margem medida na §6.1 é
nula (p90 de 4,084 ms contra 4 ms), então carga de CPU, outro dispositivo no mesmo barramento USB ou
uma janela longa podem fazer a perda voltar a crescer. O número que o `RESULTS.md` deve levar é
"250 SPS com ≤ 0,04% de perda em 10 s, host ocioso, contada pelo driver", e não "sem perda". Uma
sessão longa e uma sessão sob carga são as próximas medidas desta fase.

### 6.3 Sessão longa, 30 min, host ocioso · **2026-10-04**

`sudo python3 scripts/afe-phase5-long.py --minutes 30 --csv ~/fase5-longa-30min.csv`: 250 SPS,
ganho 24, os 8 canais pelo buffer, o gerador `1x_slow` como sinal conhecido.

| Medida | Resultado |
|---|---|
| devidas / entregues / perdidas (driver) | 449 998 / 449 873 / **125** |
| perda | **0,0278%**, ~4 por minuto, em rajadas (27 na janela 20, 10 na janela 14) |
| diferença não contada | **+0**: o `lost_samples` foi exato em 30 min |
| intervalo entre timestamps | p99 de 4,13 a 4,22 ms por janela; o maior foi de 9,04 ms |
| lacunas (> 1,5 período) | 49 |
| amostras corrompidas | **0** em 449 873 |

**O detector de corrupção foi testado antes de se acreditar nele.** Contra uma onda quadrada
sintética com transições de filtro reais, deu 0 falsos positivos; detectou um quadro deslocado de um
byte e um byte de registrador no lugar de uma amostra. Ele **não** vê erro menor que 5% da excursão,
por construção.

**Os timestamps escondem perdas.** As 49 lacunas, com no máximo ~2 amostras cada (9,04 ms), explicam
no máximo ~98 das 125 perdas. O resto não deixa marca no tempo: com uma leitura atrasada, o
`rdata_xfer_busy` dispara a seguinte imediatamente (`spi_async` no caminho de "ocupado"), e a amostra
de recuperação chega num intervalo de aparência normal. **Neste link, perda não se infere de buraco
de timestamp**, só do `lost_samples`. Isso é requisito para o `eeg-acquisition-service`: ele tem de
ler o contador e levá-lo ao registro da sessão, e não pode reconstruir o índice de amostra a partir
do tempo.

Não medido: sessão sob carga (CPU, outro dispositivo no barramento USB) e sessões mais longas.

### 6.4 A ligação `usb` na STM32MP257 · **2026-10-04: a ponte, o conversor e o serviço na placa**

Até aqui tudo foi medido num PC. Esta é a primeira vez que a ponte e o conversor rodam no alvo,
com a ponte numa porta USB da STM32MP257F-DK.

**Por que não deu para atualizar só por OTA.** A placa rodava o kernel de fábrica da ST
(`6.6.129`, de 5 de março), sem nenhum dos dois drivers. Os drivers vivem no `linux-med`, e o
kernel mora na `med-boot`, que os dois slots compartilham e que nenhum bundle troca. Então o
primeiro passo foi regravar o cartão com `make stm32 KERNEL=med KEY=development`, a partir da
`board/medplatform-afe` em `b8dbcb083402`. Antes, as correções da placa, que ainda não tinham
commit (§4.4, §6.2, o `.remove` da §2.4), viraram dois commits; sem isso o build teria buscado o
`HEAD` antigo, sem elas. A pinagem também entrou na imagem: `options hid_mcp2210 spi_chip_select=4`
em `/etc/modprobe.d`, na receita nova `med-afe-modprobe` da camada adjunta, instalada só no link
`usb`. Conferido no `.wic` antes de gravar: kernel `6.6.129-gb8dbcb083402` na `med-boot`, os dois
módulos e o `modprobe.d` no slot A.

**O lado do kernel funcionou na primeira subida:**

```
mcp2210 0003:04D8:00DE.0001: USB-SPI bridge ready, 9 GPIOs, 8 chip selects, ads1299 attached
ads1299 spi0.4: self test passed: test signal DC level 83453..83783 codes from offset (expected 83886), shorted-input noise 117 codes
```

O conversor virou `iio:device2`, e não `device0` como no PC: os dois primeiros são filtros da ST.
O `/dev/med-afe-eeg0` apontou para o certo, e é exatamente para isso que a regra udev existe. Sem
ela, o serviço teria aberto um filtro.

**O serviço entrou em laço: 54 reinícios, com o log dizendo só `front-end refused to start`.**
Refeita à mão, como root, a sequência do driver `iio` do framework funcionava inteira. A diferença
era o sandbox. Com o conjunto completo de propriedades da unit, `ProtectKernelTunables=yes` monta o
`/sys` só leitura, e o driver configura o conversor escrevendo atributos de sysfs. Confirmado nos
dois sentidos com `systemd-run` e as mesmas propriedades: `yes` dá `Read-only file system`, `no`
passa. **Sozinha, a propriedade não faz isso**: um primeiro teste avulso, só com ela, deu `exit 0`
nos dois casos e teria descartado a hipótese certa. O defeito estava latente desde que o driver
`iio` existe. No QEMU o driver é `simulated`, no perfil `amp` é `rpmsg`, e os testes no PC
rodaram como root, sem sandbox.

**Três defeitos, todos corrigidos e entregues por OTA:**

| # | Defeito | Correção | O que provou |
|---|---|---|---|
| 1 | o sandbox monta `/sys` só leitura e o link `iio` precisa escrever | a regra udev da camada adjunta liga `/run/med-afe/eeg0` ao diretório sysfs do conversor; a unit ganha `ReadWritePaths=-/run/med-afe/eeg0`, e o systemd segue o link ao montar o namespace | só aquele diretório fica gravável, e o `/proc/sys` segue só leitura (`systemd-run`). Injeção: sem o link, o serviço volta ao laço; com o link recriado pela udev, ele se recupera sozinho |
| 2 | o motivo da falha se perdia: os drivers guardavam `lastError_` e a interface não o expunha | `MedicalDevice::lastError()`, virtual e acrescentado no fim da classe, o que preserva a vtable do ABI 1; o serviço o grava no audit da falha de autoteste e da falha de partida | injeção na placa: `MED_STATUS=OUT_OF_RANGE`, `MED_DETAIL=the front-end refused bias_drive = 'derived'` |
| 3 | uma sessão por tentativa: o `metadata.json` era gravado antes do `device.start()`, e o laço deixou ~60 sessões que nunca adquiriram nada | `device.start()` antes de criar a sessão | injeção na placa: 75 sessões antes e 75 durante a falha; 76 depois de recuperar |

E um quarto defeito, na ferramenta: o `make bundle` não repassava o `KERNEL_ARGS`. Com
`KERNEL=med`, ele teria empacotado os módulos do kernel de fábrica da ST para uma placa que roda o
`linux-med`, e a instalação teria sido assinada, verificada e bem-sucedida, com todos os módulos
recusados no boot. Corrigido no `Makefile`, e conferido no bundle: `/lib/modules/6.6.129-gb8dbcb083402`.

**A OTA.** `make bundle BOARD=stm32 KERNEL=med KEY=development` e depois `rauc install`: slot B
gravado, `BOOT_ORDER=B A`, reboot, `Booted from: rootfs.1 (B)`. No primeiro boot do slot B:
`NRestarts=0`, `ProtectKernelTunables=yes`, sem nenhum drop-in. A chave de host SSH mudou com o
slot, que é o defeito 17 do `BRINGUP_STM32MP2.md`, visto de novo.

**A aquisição, 20 s, no slot A com o drop-in temporário:** 355 quadros de 488 bytes (cabeçalho de
40 bytes mais 14 × 8 × 4), ou seja **4970 amostras gravadas, 248 por segundo**, e 34 perdidas
segundo o `lost_samples`. 4970 + 34 = 5004, contra ~5002 devidas: a contabilidade fecha. **Perda de
0,68%, contra 0,03% no PC ocioso (§6.3)**, numa janela de 20 s. O ruído com entradas em curto no
autoteste foi de 117 e depois de 49 códigos, entre dois boots, contra 23 no PC. Os dois números são
de uma janela curta só e pedem a sessão longa da §6.3 na placa antes de qualquer conclusão.

**O que esta seção não diz:**

- Os critérios da Fase 1 não foram rodados formalmente na placa. O que se viu foi o probe, o
  autoteste e a aquisição.
- Nada sobre jitter, CPU ou sessão longa na placa.
- O slot A/B era confirmado com a aquisição quebrada: a cada uma das 54 tentativas e, antes
  delas, pelo `rauc-mark-good.service` aos 12,9 s. **Corrigido no mesmo dia** com *boot
  assessment* (unit `Type=notify` exigida pela `boot-complete.target`), e o fallback foi medido
  com essa mesma falha injetada: `implementation_plan_uboot_ab.md` §8, "passo 6".
- As ~60 sessões órfãs continuam no `/data` da placa, que não é A/B, como evidência do defeito 3.

### 6.5 Sessão longa na placa, e de onde vêm os 0,67% · **2026-10-04**

Sem `python3` nem `bash` na placa, as ferramentas de bancada não rodam lá. A sessão longa foi
medida **pelo próprio serviço de aquisição**, que estava gravando havia 30 min no slot A: o
`lost_samples` do driver, o tamanho do `raw.bin` e os cabeçalhos dos quadros copiados para o PC.

| Medida, 1813,8 s de aquisição | Resultado |
|---|---|
| gravadas / perdidas | 450 240 / 3017; somadas, 453 257, contra ~453 440 devidas (a diferença de 0,7 s é a partida) |
| perda | **0,67%**, a mesma taxa da janela de 20 s da §6.4 |
| saltos de sequência entre quadros gravados | **0**: nada se perde entre o driver e o disco |
| intervalo entre quadros (esperado 56 ms) | mediana 56,01; p99 58,03; p99,9 62,93; máx. 162 ms |
| CPU do serviço | ~1,4% de um núcleo |

Como no PC (§6.3), **a perda não aparece nos carimbos de tempo**: só o `lost_samples` a conta.

**A causa, isolada por um experimento A/B/A.** O `cpufreq` da placa tem só duas frequências
(1,2 e 1,5 GHz), e o `schedutil` alternava entre elas **87 vezes por segundo** (157 979 trocas em
30 min); a thread do governador, que roda em prioridade de tempo real, já tinha consumido 92 s de
CPU. Com o mesmo serviço e o mesmo conversor, mudando só o governador em tempo de execução:

| Governador | Duração | Perdidas / devidas | Perda | Trocas/s | CPU do serviço |
|---|---|---|---|---|---|
| `schedutil` | 180 s | 325 / 45 012 | 0,72% | 86,4 | 1,40% |
| `performance` | 180 s | **0 / 45 012** | **0,000%** | 0 | 1,02% |
| `schedutil` | 180 s | 284 / 45 010 | 0,63% | 86,3 | 1,38% |

A perda volta quando o governador volta, e por isso não é deriva no tempo. **Os 0,67% da placa são
inteiramente o custo da troca de frequência**: com a CPU fixa, o link `usb` não perdeu nenhuma
amostra em 3 min, no mesmo hardware que perde 0,7% com o governador padrão.

**E por mais tempo, com um terceiro governador** (`/data/gov2.log`, 40 min, executado na placa por
`nohup` para não depender da sessão SSH):

| Governador | Tempo | Perdidas / devidas | Perda | Trocas/s | Frequência |
|---|---|---|---|---|---|
| `performance` | 6 × 300 s | 1, 2, 1, 1, 2, 0, 2 / ~75 010 por janela | **0,000–0,003%** | 0 | 1,5 GHz fixo |
| `ondemand` | 300 s | 4 / 75 010 | 0,005% | 0,0 | quase todo em **1,2 GHz** |
| `ondemand` | 300 s | 16 / 75 013 | 0,021% | 0,1 | idem (`time_in_state`: ~309 s a 1,2 GHz, ~29 s a 1,5 GHz) |

**O `ondemand` separa as duas hipóteses.** Parado na frequência mais *baixa*, ele quase não perde.
Então a perda não vem de a CPU estar lenta: vem de a frequência **mudar**. A perda acompanha a taxa
de troca, e não a frequência: 86 trocas/s dão ~0,7%, 0,1 troca/s dá 0,02%, e nenhuma troca dá
≤ 0,003%, que é a ordem do PC ocioso (0,03%, §6.3). O custo de CPU do serviço vai de 1,02% a
1,5 GHz para 1,20% a 1,2 GHz, coerente com a razão das frequências. `NRestarts=0` ao fim.

**O que isto não diz**: o mecanismo dentro da troca. A hipótese é que cada mudança de frequência
(no STM32MP2 ela passa pelo firmware seguro, via SCMI) atrasa o timer do host que dispara a
leitura; não foi medida. E nada sobre o custo de energia de cada política. A correção, onde mora e
se vale só para o link `usb` ou para o produto todo, está em aberto.

### 6.6 Desplugar a ponte na placa · **2026-10-04 23:21**

O teste que derrubava o kernel do PC antes do `.remove` (§2.4), agora no 6.6 da placa e no
controlador EHCI dela, com o serviço adquirindo: cabo USB fora por ~10 s, e de volta.

**O kernel passou.** Nenhum `BUG`/`Oops`, `tainted=4096` igual ao de antes (só o `galcore`, fora da
árvore). O driver soltou a ponte e a reassumiu como `.0002`, o conversor passou de novo no
autoteste (83465..83779 códigos, ruído 111), e a udev refez o `/dev/med-afe-eeg0` e o
`/run/med-afe/eeg0`, este apontando para o caminho novo. É a correção do `.remove` vista em outro
kernel e em outro controlador USB, e é também a prova de que a regra udev não precisa de regra de
remoção: o `add` seguinte substitui o link.

**O serviço não voltou, e em silêncio.** Na leitura que falhou, ele fechou a sessão direito
(`acquisition read failed` → `session record closed`), mas saiu com status 0, e o systemd
registrou `Deactivated successfully`. Com `Restart=on-failure`, uma saída limpa não é reiniciada:
a ponte voltou em 10 s, passou no autoteste, e nada mais a leu. A unit ficou `inactive`, não
`failed`, e por isso nenhuma lista de units com falha a mostraria. É o padrão "não falha,
desaparece" que o `CLAUDE.md` já registra para unidades puladas por `Condition*`.

**Correção**: o laço de aquisição marca a falha do front-end, o serviço sai com 1, e o registro
`AcquisitionStopped` ganha um campo `reason` (`front-end fault` ou `stop requested`). Enquanto a
ponte está ausente, cada tentativa falha no autoteste e não deixa sessão no registro, porque o
`device.start()` vem antes da criação da sessão (§6.4).

**Validada com a mesma falha**, depois de entregue por OTA (slot B, 23:47), junto com o governador
fixo da §6.5:

| t (s) | Evento |
|---|---|
| 608,4 | ponte desplugada; leitura falha, serviço sai com **status 1** |
| 608–643 | 14 tentativas, todas `front-end self test failed`; **nenhuma sessão vazia** |
| 641,8 | ponte replugada |
| 644,6 | `bridge ready … ads1299 attached`, autoteste do conversor passou |
| **646,3** | **`acquisition session started`**, 1,7 s depois da ponte, sem intervenção |

A sessão interrompida fechou com `MED_REASON=front-end fault`, `MED_FRAMES=10623`. O kernel passou
de novo: `tainted=4096`, nenhum `BUG`.

**Em aberto: o ruído do autoteste varia entre partidas.** Com entradas em curto, a placa deu 117,
49, 111, 50 e 205 códigos pico a pico em cinco probes, contra 23 no PC (§4.4). O teto é ~447 códigos
(10 µV a ganho 24), então todos passam. Mas o curto é interno ao conversor, o que exclui os pinos
flutuantes, e a variação de 4× não tem explicação medida. A suspeita é a alimentação de 5 V vinda da
USB da placa. Comparar com uma fonte de bancada é exatamente a medida que o §3 do plano de bancada
pede.

Mais uma janela de perda, de 180 s, logo após esse boot e já em `performance`: 9 perdidas em 45 012
(**0,020%**). Fica uma ordem de grandeza abaixo do `schedutil`, mas acima dos ≤ 0,003% da sessão de
30 min. A janela começou ~45 s após o boot, e uma janela isolada não separa essas duas coisas.

### 6.7 O que chega pelo USB, por ODR e por clock SPI · **2026-10-04, na placa**

`/tmp/rate.sh` e `/tmp/rate16k.sh` (`scripts` do scratchpad da sessão, em `sh` do BusyBox), com o
serviço parado: `dd` do `/dev/iio:deviceN` por 10 s (20 s a 16 kSPS), 40 bytes por amostra
(8 × `le:s24/32` + carimbo `s64`), e o `lost_samples` da janela. Governador `performance`. O clock
SPI foi trocado recarregando o módulo com `spi_max_speed_hz`.

| ODR | Devido (216 bits × ODR) | Entregue | Perdido | Clock SPI 1 / 4 / 12 MHz |
|---|---|---|---|---|
| 250 | 54 kbit/s | 249,8/s = **54 kbit/s** | 0% | idêntico |
| 500 | 108 kbit/s | 249,9/s | 50% | idêntico |
| 1 000 | 216 kbit/s | 249,9/s | 75% | idêntico |
| 2 000 | 432 kbit/s | 249,9/s | 87,5% | idêntico |
| 16 000 | **3 456 kbit/s** | 249,9/s | **98,4%** | 4 e 12 MHz: idêntico |

**O link entrega 250 leituras por segundo, e nada mais, em qualquer ODR e qualquer clock SPI.** A
demanda do conversor cresce linearmente com a ODR, mas o que chega não. O gargalo é a latência por
transação (duas trocas HID de 64 bytes por amostra, ~2 ms cada, §6.1), e não a largura de banda:
os 54 kbit/s úteis são uma fração pequena do que um endpoint de interrupção *full-speed* carrega.
A 16 kSPS, cada amostra teria 62,5 µs para ser lida, e a ponte leva ~4 ms. Só `amp` e `spi`
alcançam as ODR acima de 250.

**Contabilidade**: entregues + perdidas somaram 16 001/s a 4 MHz (exato) e 15 955/s a 12 MHz,
0,3% abaixo do devido. Isso não tem explicação, e é o que a E0 do `implementation_plan_board_stats.md`
existe para resolver, com o analisador lógico contando o `DRDY`.

**O que isto não diz**: que a ponte de fato usou 1 e 12 MHz. O driver só registra o clock das
leituras de registrador (2 MHz); o das leituras de dados não foi observado. A conclusão "o clock
SPI não importa" vale pela conta e por esta medida, mas a confirmação é o SCLK no analisador.

**Por que 1,56%, e de quem é o limite.** 1,56% = 250/16 000. A 16 kSPS sai uma amostra a cada
62,5 µs. Durante os ~4 ms de uma leitura, o conversor produz 64 amostras, e o link entrega uma: as
outras 63 são sobrescritas. Três fatos se somam:

1. **O ADS1299 não tem fila.** O registrador de saída guarda só a última conversão, e cada `DRDY`
   sobrescreve a anterior.
2. **O MCP2210 não lê sozinho.** Ele não dispara uma leitura no `DRDY` e não acumula amostras.
   Cada transferência é um comando que o host envia e cuja resposta espera.
3. **Cada pergunta e resposta custa ~2 ms.** O chip é HID *full-speed*: o host fala com ele uma vez
   por quadro de 1 ms, em relatórios de 64 bytes. O protocolo exige duas trocas por leitura, uma
   que inicia a transferência e outra que busca os dados (§6.1–6.2), o que dá **~4 ms por amostra**.

**O limite é a arquitetura do MCP2210, não a velocidade SPI dele.** O SPI do chip vai a 12 MHz, e
uma amostra de 27 bytes passaria pelo fio em ~18 µs. Também não é o conversor, nem a CPU da placa
(o serviço usa ~1%), nem, depois do cache de §6.2, o nosso driver. O driver **já foi** parte do
problema: reescrever as configurações de transferência a cada leitura custava uma terceira troca
(167/s). Hoje ele está no piso do protocolo do chip. A medida sustenta isso: as mesmas 250
leituras/s com 1, 4 e 12 MHz. Se o limite fosse o SPI, a vazão mudaria com o clock.

**Nem uma ponte HID ideal resolveria.** Sem latência nenhuma, um endpoint de interrupção
*full-speed* carrega 64 bytes por quadro, ou 512 kbit/s. Com 27 bytes por amostra, o teto absoluto
seria ~2 400 amostras/s, e 16 kSPS precisam de 432 kB/s, quase sete vezes isso. Hardware diferente
passaria disso: uma ponte USB *high-speed* com transferência em bloco e buffer, ou um
microcontrolador que lê no `DRDY` e envia as amostras em rajada. **Para este projeto, a conclusão é
a outra: o MCP2210 serve para bancada e desenvolvimento, e o teto dele é um argumento com número a
favor da ligação `amp`**, em que a leitura acontece no próprio chip, na interrupção do `DRDY`.

**De passagem, uma pista sobre o ruído do autoteste** (§6.6): nas três recargas desta medida, com
o serviço parado, deu **28–29 códigos**, perto dos 23 do PC. Os 49–205 anteriores vieram de probes
no boot e no replug, enquanto o serviço tentava partir. A hipótese nova, ao lado da alimentação: a
atividade concorrente durante o autoteste. Não foi testada.

## 7. Fase 6 — framework, aplicação e suíte · **não executada**
## 8. Fase 7 — as duas ligações lado a lado · **não executada**
## 9. Fase 8-A — varredura do datasheet do conversor · **não executada**
## 10. Fase 8-B — varredura do datasheet da ponte · **não executada**

Todas dependem do módulo, exceto a §10 (que roda com placa de avaliação e jumper) e a §13.1 do plano
de bancada (testes de host da máquina de transferência, que não precisam de hardware nenhum).

---

## 11. Defeitos, ambiguidades e lacunas

Formato do `BRINGUP_STM32MP2.md` §11: o que era, o que teria causado, e o que o pegou.

### D1 — `CONFIG2` com todos os campos uma posição acima

**O que era.** Reservado declarado em 7:6 em vez de 7:5; `INT_CAL` no bit 5 em vez do 4; `CAL_AMP`
no bit 3 em vez do 2 — sendo o bit 3 o *outro* campo reservado, que o datasheet manda escrever 0.

**O que teria causado.** Calculado antes e depois:

| escrita | antes | depois | o que o antes fazia |
|---|---|---|---|
| `off` | `0xc0` | `0xc0` | correto por acidente |
| `1x_slow` | `0xe0` | `0xd0` | reservado 7h (proibido) e **`INT_CAL` = 0** |
| `2x_slow` | `0xe8` | `0xd4` | idem, mais o bit 3 reservado escrito como 1 |

O gerador de teste interno **nunca era ligado**. Em silício: o autoteste do *probe* pede o sinal, lê
entrada em curto, o `swing` reprova, `dev_err_probe` recusa registrar — sintoma ("não aparece
`/dev/med-afe-eeg0`") a três passos da causa (um bit), e uma sessão de bancada gasta olhando
alimentação e fiação.

**O que o pegou.** Leitura do datasheet. **Nada mais poderia**: o valor de reset que a transcrição
errada produz é `0xc0`, que é o correto — uma varredura de registradores contra valores de reset é
**cega** a erro de posição de campo que preserve o reset, e uma releitura de escrita também, porque o
registrador é R/W e devolve o que se escreveu. Só a leitura do datasheet ou um osciloscópio
discordam. O critério 5 da Fase 3 do plano de bancada ganhou essa ressalva por causa deste defeito.

### D2 — `CONFIG1` sem o segundo campo reservado

**O que era.** O datasheet manda escrever 1 no bit 7 **e 2h nos bits 4:3**. O driver declarava só
`BIT(7)`.

**O que teria causado.** Nada, hoje — e a razão importa mais que o fato: `CONFIG1` só é tocado por
`regmap_update_bits()` no campo `DR`, que preserva os bits do reset da peça. O defeito era **latente
e armado**: com `CONFIG2` e `CONFIG3` reescritos no idioma `_RESERVED_VALUE`, quem alcançasse
`MASK_CONFIG1_RESERVED` seguindo o mesmo idioma escreveria `0x80 | DR` e zeraria o bit 4 **em toda
troca de taxa**. Conferido: o valor correto é `0x90`, e `0x90 | 6 = 0x96`, que é o reset do datasheet.

**O que o pegou.** A mesma leitura. E vale a nota: a armadilha foi *criada* ao harmonizar os dois
registradores vizinhos — consertar um registrador armou o seguinte.

### D3 — `lead_off_status` reportava bytes que o datasheet manda ignorar

**O que era.** O driver liga os comparadores de *lead-off* (`CONFIG4.PD_LOFF_COMP`) com o comentário
*"so lead_off_status means something"*, e **nunca** escreve `LOFF_SENSP`/`LOFF_SENSN`. O datasheet é
explícito: os bits de `LOFF_STATP` só são válidos onde o bit correspondente de `LOFF_SENSP` está em 1.

**O que teria causado.** O atributo lia dois bytes inválidos, e eles leem `00 00` — indistinguível de
**"todos os eletrodos conectados"**. Um status que não consegue relatar a falha que existe para
relatar, reportando justamente o valor tranquilizador.

**O que o pegou.** A leitura da Tabela 20/21 junto com o `grep` de quem escreve o quê. **Conserto
aplicado**: o atributo lê `LOFF_SENS*` primeiro, responde `disabled` quando nenhum canal tem detecção
habilitada, e mascara o status aos bits válidos quando tem. **Conserto não aplicado**: habilitar a
detecção por canal, porque isso injeta a corrente de *lead-off* nos eletrodos do paciente e a
magnitude vem do `LOFF`, que ninguém configura — é decisão clínica, não transcrição.

Efeito colateral: a recusa que o `do_derive_device_options` faz a `afe.lead_off_detection = false`
alega que *"o driver habilita os comparadores no probe e não oferece controle para desligar"* —
**meia verdade**. Comparadores ligados, detecção desligada.

### D4 — o driver não fixa o modo SPI

**O que era.** Nenhum `spi->mode` em lugar nenhum do driver. O ADS129x exige CPOL=0, CPHA=1.

**O que teria causado.** No caminho da ponte USB o modo vinha do parâmetro de módulo do MCP2210, e
funcionava por acidente de quem instanciava o dispositivo. Num **controlador SPI real** — que é a
ligação `spi`, o modo hat — o barramento subiria em modo 0 e o driver leria lixo com convicção.

**O que o pegou.** Escrever o binding do devicetree. A pergunta "quem declara o modo?" não tinha
resposta no código.

**Como foi fechado.** No devicetree, não no driver: `spi-cpha: true` declarada **e exigida** no
binding. Um DT sem ela é recusado pelo esquema, o que torna impossível a placa que configura o
barramento errado. Forçar `spi->mode` no driver continua sendo alternativa em aberto, e a decisão foi
descrever primeiro.

### A1 — a amplitude do gerador de teste: o datasheet não decide, e as camadas discordam

A Tabela 14 dá a amplitude como `1 × −(VREFP − VREFN)/2400` e **não diz** se isso é pico, pico a pico
ou passo diferencial. Fator de dois. E o repositório carrega **as duas leituras, em camadas que nunca
se encontram**:

| Onde | Afirma |
|---|---|
| `eeg.conf:57` e a tabela `simulated` da receita | gerador oscila VREF/2400 = 1,875 mV |
| `ti-ads1299.c`, `ADS1299_TEST_SIGNAL_PP_DIVISOR` | pico a pico é VREF/1200 = 3,75 mV |

Se a leitura certa for VREF/2400, o autoteste espera o dobro da oscilação real e **reprova silício
bom** — mesma forma do D1, alcançada pelo lado oposto. Um número alimenta um simulador e o outro uma
tolerância, então nenhum dos dois jamais pegaria o outro.

**Não resolvido de propósito.** Virou `[SBAS499? — o datasheet não decide. Medir na Fase 4]`, com as
duas leituras e a consequência escritas no fonte. Um palpite que faz um autoteste passar é pior que
um que o faz falhar.

**Resolvido por medição, 2026-10-04** (§4.2): o gerador interno oscila **VREF/1200 de pico a
pico**, ±VREF/2400 em torno de zero. Medidos 167 259 a 167 335 códigos nos 8 canais, no ganho 24,
contra 167 772 previstos. A Tabela 14 dá a **amplitude de pico**. A constante do driver estava
certa. E a "discordância entre camadas" era só de texto: o gerador `simulated` produz ±A com
A = VREF/2400 (`MedicalDevice.cpp:425`), ou seja, o mesmo VREF/1200 de pico a pico. O marcador
`[SBAS499? …]` no fonte do driver pode virar citação da medição, e ainda não virou.

### D5 — `CONFIG_TI_ADS1299` no fragmento do distro

**O que era.** Ao migrar o driver para a árvore do kernel, o símbolo foi declarado em
`meta-med-distro/.../med-kernel-features.cfg`, que é aplicado a **todo** kernel que o distro constrói.

**O que teria causado.** Nada no STM32 e um silêncio no QEMU: o `linux-yocto` do poky não tem a
entrada `TI_ADS1299` no seu `Kconfig`, então o `merge_config.sh` descarta o pedido sem dizer nada. A
imagem sairia sem o driver e sem erro em lugar nenhum.

**O que o pegou.** A guarda `do_med_check_kernel_config`, na **primeira execução real**, falhando o
build com `asked for and not present: CONFIG_TI_ADS1299`. Ela nasceu paga.

**A fronteira que isso revelou**, e é o valor real do defeito: aquele fragmento declara
**capacidades** que qualquer kernel tem — cgroups, verity, IIO, SPI. O símbolo de um driver nosso é
**conteúdo** de uma árvore específica, e mora onde a árvore é conhecida.

### D6 — o driver do conversor na imagem de produto, puxado por um metapacote

**O que era.** Com o driver in-tree e `CONFIG_TI_ADS1299=m`, ele passou a ser um módulo do kernel
como qualquer outro. A imagem instala o metapacote `kernel-modules`, e o `kernel.bbclass` faz esse
metapacote **depender de todos os ~1040 módulos** que o kernel constrói.

**O que teria causado — e causou, num `.wic` que existiu.** O driver do conversor foi instalado no
perfil `amp`, cujo argumento inteiro é que o conversor vive no coprocessador e o lado Linux **não
carrega código de conversor**. É a propriedade que sustenta o argumento de particionamento da
IEC 62304 §5.3, e ela morreu sem que nada reclamasse: build verde, 5596 tarefas, zero erros.

**O que o pegou.** Ler o manifesto da imagem em vez de acreditar no código de saída. Literalmente a
regra da casa — *"compila e boota não é evidência"* — aplicada a quem a escreveu.

**Conserto**, e ele é a montante do empacotamento: a camada adjunta passou a declarar
`MED_AFE_KCONFIG` por ligação, ao lado do `MED_AFE_INSTALL` que já tinha. Uma ligação que não usa o
driver não habilita o símbolo, então **não existe módulo para o metapacote puxar**. Remover depois
seria tratar o sintoma; o `BAD_RECOMMENDATIONS` não alcança um `RDEPENDS`.

**A generalização, que é o que importa**: a migração para in-tree custou, em silêncio, duas
propriedades que o módulo *out-of-tree* dava de graça — compilar contra qualquer kernel (D5) e não ser
arrastado por um metapacote (D6). Nenhuma das duas estava escrita em lugar nenhum como propriedade, e
por isso nenhuma delas falhou ao desaparecer.

### D7 — um filtro de `SRC_URI` escrito como deny-list

**O que era.** O bbappend que aponta o kernel para o fork descartava as entradas `http://`, `https://`
e `ftp://` do `SRC_URI` da ST e mantinha o resto. A entrada da ST para a variante `devupstream` é
`git://github.com/STMicroelectronics/linux.git;protocol=https` — que **começa com `git://`** e
passava.

**O que causou.** Dois SCMs no `SRC_URI` e falha de parse, na variante `class-devupstream`, que o
`BBCLASSEXTEND` faz o bitbake parsear independentemente de a gente selecioná-la.

**O que o pegou.** Um `bitbake -e` com as variáveis do fork **setadas**. E aqui está a parte
transferível: o `make parse` tinha passado verde minutos antes, porque sem as variáveis a função
retorna na primeira linha — **o parse verde testou o ramo vazio**. Virou allow-list: só `file://`, e
desses não os `.patch`.

### D8 — prosa que começa com `# CONFIG_` num fragmento `.cfg`

**O que era.** Duas linhas de comentário no `med-kernel-features.cfg` começavam com `# CONFIG_DM_VERITY`
e `# CONFIG_TI_ADS1299`.

**O que causou.** O `do_kernel_configcheck` do próprio `linux-yocto` as reportou como
*"badly formatted configuration options"*. Num fragmento `.cfg`, `# CONFIG_X is not set` é **sintaxe**,
não comentário — então prosa que comece assim é lida como opção malformada.

**O que o pegou.** O aviso do fornecedor, num build que passou. Note que a nossa guarda **não** tinha
esse falso positivo, porque a regex dela exige o sufixo `is not set` exato; o avaliador da ST é mais
frouxo e, neste caso, estava certo em reclamar. A regra ficou escrita no próprio fragmento: nomeie um
símbolo no meio da frase, nunca no começo de uma linha de comentário.

### L1 a L3 — lacunas, não defeitos

- **L1, `BIAS_STAT` (CONFIG3 bit 0) não é reportado.** É o *lead-off* do eletrodo de BIAS: 0
  conectado, 1 não conectado. Expor exige pôr `CONFIG3` na tabela de voláteis do `regmap` na mesma
  mudança — o registrador é cacheado, e um atributo que o lesse reportaria "conectado" para sempre.
  (A redação anterior desta linha afirmava que um eletrodo de referência solto corrompe todos os
  canais ao mesmo tempo. É mais forte do que a folha de dados do registrador sustenta, e foi
  suavizada aqui e no comentário do driver — o efeito elétrico é hipótese, o estado é o que o bit
  reporta.)
- **L2, `SRB1` (MISC1 bit 5) não tem controle.** É a montagem de referência comum, que é como um
  gorro de EEG normalmente se liga. Decisão de hardware pendente: a chave da PCB alterna GND e o
  eletrodo do canal, e **o software não consegue ler a posição dela** — então o registro não pode
  afirmar a montagem. Sugestão registrada: ligar a chave a um dos 4 GPIOs do próprio conversor, que
  a diretriz de completude vai implementar de qualquer forma, e a montagem passa a ser medida em vez
  de afirmada.
- **L3, `BIAS_SENSP/N` nunca escritos**, coerente com o amplificador de bias estar inerte.

---

## 12. A árvore do kernel como fonte de verdade · **2026-09-26**

Decisão de arquitetura e sua execução, no mesmo dia. O detalhamento está no
`implementation_plan_ads1299_upstream.md` §2.2 e §2.3; aqui fica o que foi medido.

### 12.1 A base validada

| Item | Valor |
|---|---|
| Versão | 6.6.129, release ST `r3.1` → `PV = 6.6.129-stm32mp-r3.1` |
| Aquisição padrão | tarball `linux-6.6.129.tar.xz` + **um** patch, `0001-v6.6-stm32mp-r3.1.patch` |
| sha256 do tarball | confere byte a byte com `SRC_URI[kernel.sha256sum]` |
| Delta da ST | 4,9 MB, **162.253 linhas, 749 arquivos** |
| Ponto Git equivalente | `STMicroelectronics/linux`, `v6.6-stm32mp`, `548f960c059bc4b9165cb69895fd67551f6061ca` |
| Tag conferida na placa | `git tag --points-at 548f960c` → **`v6.6-stm32mp-r3.1`** |

### 12.2 A equivalência, provada antes de migrar

`scripts/med-kernel-fingerprint.sh` compara **conteúdo** e nunca metadados — modo, mtime e
diretórios vazios divergem legitimamente entre tarball desempacotado e checkout git, e nenhum deles
muda o kernel compilado.

| Verificação | Resultado |
|---|---|
| Arquivos, árvore do Yocto | 81889 |
| Arquivos, checkout Git | 81889 |
| Comuns **efetivamente comparados** | 81889 |
| Com hash divergente | **0** |
| Digest das duas | `35e19311ab288cf56345ea6396d47d0cc071dab2dfe7ed028c575f2202b3bf50` |
| `cmp` dos manifestos | idênticos, arquivo por arquivo |

**A ferramenta precisou de duas exclusões, e as duas apareceram ao executá-la, não ao raciocinar
sobre ela**: `.pc/` (768 backups pré-patch do quilt, um por arquivo que o patch da ST toca) e
`patches/` (o `series` e o symlink do quilt na raiz). Sem elas a comparação acusava 770 divergências
onde não havia nenhuma — uma verificação **incapaz de passar**, que é pior que nenhuma porque produz
alarme falso e consome a atenção que o alarme verdadeiro precisaria.

Segunda disciplina, que vale registrar: a contagem de arquivos comuns foi conferida (81889, e não
zero) **antes** de aceitar "nenhum hash divergente". Uma comparação que não compara nada também
devolve "nenhuma diferença".

### 12.3 O que foi para a árvore do kernel

Clone `linux-med`, branch `feature/ads1299-mcp2210-driver` a partir do commit validado — digest
inalterado, o que prova que a branch parte da árvore provada.

| # | Commit | O quê |
|---|---|---|
| 1 | `8ad9d41cda64` | `dt-bindings: iio: adc: add TI ADS1299 biopotential AFE` |
| 2 | `2b605504d251` | `iio: adc: ti-ads1299: add TI ADS1299 biopotential AFE driver` + `Kconfig` + `Makefile` + `MAINTAINERS` |
| 3 | `0c09dda8f323` | **A** — `fix the CONFIG2 field positions` (defeito D1) |
| 4 | `1aece8814b26` | **B** — `write both reserved fields of CONFIG1` (defeito D2) |
| 5 | `bf2119c02d99` | **C** — `spell out the CONFIG3 reserved value` |
| 6 | `55c319262d75` | **D** — `do not report lead-off status that is not valid` (defeito D3) |
| 7 | `da723f985714` | **E** — `cite the datasheet tables that were checked` |

A ordem não é arbitrária. **A antes de B** porque é A que introduz o idioma `_RESERVED_VALUE`, e é
esse idioma que torna o `CONFIG1` perigoso — a justificativa de B não existiria antes de A. E **E por
último** porque anotar o que foi conferido só faz sentido depois de os consertos terem entrado.

Dois commits anteriores, `f38399ec6832` e `efc4220f6c3a`, foram reescritos para os hashes 1 e 2 acima:
autoria e `Signed-off-by` passaram a trazer nome legal, que é o que a DCO do kernel certifica, e o
`MODULE_AUTHOR` e a entrada de `MAINTAINERS` foram para dentro do commit que adiciona o driver, onde
pertencem. Nada foi fundido.

**O congelamento vai dobrar A a E no commit 2**, e isso é esperado: uma série enviada ao kernel
apresenta o driver já correto, e não a sequência "introduz / acha defeito / corrige". O valor
histórico dessas cinco mensagens fica aqui, na §11, que é fora do git.

Medido na série:

- **compila para arm64 com `W=1`, zero avisos** (`CC [M] drivers/iio/adc/ti-ads1299.o`, ELF aarch64);
- `allmodconfig` resolve `CONFIG_TI_ADS1299=m` sozinho, o que prova que o `Kconfig` expressa
  dependência de verdade e não decoração;
- `make dt_binding_check` limpo, com o esquema **observado recusando** uma propriedade inesperada
  (`spi-cpha`, antes de ser declarada) — que é o que faz "o exemplo valida" significar algo;
- `checkpatch --strict --git` sobre os sete commits: **0 erros em todos**. Os cinco de correção
  saem `0/0/0` — *"no obvious style problems and is ready for submission"*. O ruído está só nos dois
  de introdução: o advisório de `MAINTAINERS`, que dispara em todo arquivo novo, um `CHECK` sobre um
  argumento de macro, e duas vezes o mesmo falso positivo sobre um membro declarado `__aligned(8)`;
- e o `spdxcheck` só passou a rodar depois de `ply` e `GitPython` existirem no ambiente. Antes disso
  ele abortava com `ModuleNotFoundError` **e o `checkpatch` seguia adiante** — ou seja, uma
  verificação que não acontecia parecia uma verificação que passava.

### 12.3.1 O arquivo in-tree **não** é idêntico ao do repositório do projeto

A verificação que eu tinha proposto era comparar os dois arquivos e exigir igualdade. Ela está errada,
e a correta é outra: comparar o **código**, com os comentários removidos. Assim ele é **byte a byte
idêntico** — zero diferença.

O que difere são comentários, e as diferenças são deliberadas, para o arquivo servir a uma submissão:

- saíram os selos `- CHECKED 2026-09-26`: a data de uma leitura mora no git, não no fonte;
- saíram as referências a `BRINGUP_STM32MP2.md` e ao plano de bancada — documento de repositório
  privado não pertence a um arquivo do kernel;
- e saíram as duas afirmações suavizadas, que estavam **também no fonte** e não só nas mensagens
  (ver L1 na §11).

Mais uma alteração mecânica: o `*/` de um comentário de banner passou para a própria linha, que era o
único achado real do `checkpatch` no arquivo original.

### 12.4 O que ficou pendente

- As **quatro correções de datasheet** (D1, D2, D3 e a documentação de `CONFIG3`) ainda não foram
  committadas na árvore do kernel — vão uma por commit, cada uma citando sua tabela.
- O `mcp2210-spi.c` ainda não migrou. Ao migrar, sai com ele o
  `static char *spi_device = "ads1299"`: uma ponte que instancia um dispositivo **por nome do
  conversor** é dependência real, em código, e enquanto ela existir os dois drivers não são
  independentes, apenas parecem.
- O `S:class-devupstream` da receita da ST **aparentemente** não é ajustado para `${WORKDIR}/git`.
  É leitura da receita, **não observação de build**, e fica pendência até a primeira integração dizer.
- Cinco comentários no `ti-ads1299.c` ainda justificam decisões citando MCP2210 e USB. O critério
  `grep -ci 'mcp2210\|usb\|bridge'` só vai a zero quando eles forem reescritos em termos genéricos.

### 12.5 A diferença de base 6.6 → mainline

A submissão é contra a mainline; a placa roda 6.6.129. Um arquivo, duas bases. A expectativa — e é
expectativa, não medida — é que o custo seja pequeno: o *backport* 6.9→6.6 do arquivo de origem já
custou **zero linhas de API**, medido. O que se sabe que precisará de retrabalho no congelamento:

- o binding, que aqui é arquivo novo porque a base 6.6 não tem o irmão, e contra a mainline vira
  **extensão do `ti,ads1298.yaml`**. Pré-criar esse nome nesta árvore seria pior: produziria conflito
  *add/add* no rebase em vez de uma conversão mecânica;
- os *hunks* de `drivers/iio/adc/Kconfig` e `Makefile`, porque o patch da ST já toca esses dois
  arquivos (20 arquivos sob `drivers/iio` e `drivers/spi` no total). Os `.c`, sendo arquivos novos,
  atravessam sem atrito.

---

### 12.6 O Yocto consumindo a árvore do kernel · **2026-09-27**

O passo 5 do arranjo: as receitas de módulo saem, o kernel passa a vir do fork, e a configuração
volta a ser conferida.

| Peça | Onde |
|---|---|
| Receita do módulo removida | `meta-med-afe-ads1299/recipes-kernel/ads1299/` — receita, `Makefile` e as 1549 linhas do driver |
| Ponteiro do fork | `MED_KERNEL_GIT` / `MED_KERNEL_SRCREV`, parâmetros de build com default nulo, no `kas-base.yml` |
| Quem consome | `meta-med-bsp/dynamic-layers/stm-st-stm32mp/recipes-kernel/linux/linux-stm32mp_%.bbappend` |
| Símbolo do driver | `med-stm32mp-drivers.cfg`, no BSP, aplicado sob **duas** condições (§11 D5, D6) |
| Qual ligação precisa dele | `MED_AFE_KCONFIG_<link>` na camada adjunta, ao lado do `MED_AFE_INSTALL_<link>` |
| A guarda | `do_med_check_kernel_config` no `linux-%.bbappend` do distro, sobre `MED_KERNEL_REQUIRED_CFG` |

#### O que foi medido

| Build | Resultado |
|---|---|
| `make qemu` | 5653 tarefas, todas bem-sucedidas; guarda executou e **passou** no `linux-yocto`; nenhum `ti-ads1299.ko` produzido, que é o correto — aquele kernel não tem o driver |
| `make stm32` com o fork | 5596 tarefas, todas bem-sucedidas; `PV = 6.6.129-stm32mp-r3.1+medda723f985714`, `S` terminando em `/git`, `SRCREV` fixo; `CONFIG_TI_ADS1299=m` no `.config` final e `kernel-module-ti-ads1299` empacotado |
| `amp`, depois do conserto do D6 | **0** linhas com `ads1299` no manifesto da imagem, e **0** ocorrências de `CONFIG_TI_ADS1299` no `.config` |
| `spi`, por `bitbake -e` | `MED_AFE_KCONFIG=CONFIG_TI_ADS1299` e os **dois** fragmentos em `MED_KERNEL_REQUIRED_CFG` |

Uma confirmação que não estava planejada: o kernel se identifica pelo commit sozinho. O arquivo de
configuração entregue chama-se `config-6.6.129-gda723f985714`, porque o `setlocalversion` do kernel lê
o git da árvore. Nenhum artefato desse build pode ser confundido com o release da ST.

#### E o `S:class-devupstream` deixou de ser pendência

A leitura era que a variante `devupstream` da ST ajusta `SRC_URI`, `SRCREV` e `PV` e nunca o `S`.
Sobrescrever a receita **normal** em vez de selecionar aquela variante torna a questão **irrelevante
em vez de respondida**: `S` é definido explicitamente pelo nosso bbappend, e o `do_configure` real da
ST o usou. Uma pendência que deixa de importar é melhor que uma resolvida.

#### O que continua não medido

- **Nenhuma imagem `spi` ou `usb` foi construída.** A ligação que de fato usa o driver está resolvida
  corretamente e não compilada. E na `spi` o `do_derive_device_options` recusa duas prescrições
  (`afe.bias_drive = true`, `lead_off_detection = false`), o que nunca foi exercitado.
- ~~A guarda nunca falhou no perfil do STM32.~~ **Medido em 2026-09-27.** Um commit temporário no
  fork removeu a entrada `config TI_ADS1299` do `drivers/iio/adc/Kconfig` e o kernel foi construído
  com `MED_EEG_LINK = "spi"`: a guarda reprovou com
  `CONFIG_TI_ADS1299: asked for by med-stm32mp-drivers.cfg, not in the final .config`, e o commit foi
  descartado depois. Prova as duas metades de uma vez — o fragmento do BSP **é** aplicado na ligação
  que usa o driver, e a ausência do símbolo **é** detectada no alvo físico. Ver `RESULTS.md` §8.
- **O `mcp2210-spi.c` não migrou.** A receita dele continua na camada adjunta, e com ela o
  `static char *spi_device = "ads1299"`, que é a dependência real entre os dois drivers.

---

## 13. As regras que este trabalho acrescenta

1. **Um defeito de posição de campo que preserva o valor de reset é invisível a toda verificação de
   registrador.** Nem varredura de reset, nem releitura de escrita. Só o datasheet ou o
   osciloscópio. (D1)
2. **Consertar um registrador pode armar o vizinho.** Harmonizar `CONFIG2` e `CONFIG3` criou a
   armadilha do `CONFIG1`, que não existia antes. (D2)
3. **Um status que não pode relatar a falha reporta o valor tranquilizador.** `00 00` lido de
   registradores inválidos é idêntico a "todos os eletrodos conectados". (D3)
4. **Escrever o binding é uma revisão do driver.** A pergunta "quem declara isto?" encontra o que o
   código esqueceu de declarar. (D4)
5. **Uma ambiguidade de datasheet é medição, não escolha.** Quando a folha não decide, registrar as
   duas leituras e a consequência vale mais que escolher uma — e um palpite que faz um autoteste
   passar é pior que um que o faz falhar. (A1)
6. **Uma ferramenta de comparação nova é suspeita até provar que pode reprovar.** As duas exclusões
   que a impressão digital precisou só apareceram ao executá-la, e sem elas ela era incapaz de
   passar. (§12.2)
7. **Antes de aceitar "nenhuma diferença", conferir que algo foi comparado.** Uma comparação vazia
   devolve o mesmo resultado que uma comparação bem-sucedida. (§12.2)
8. **Um recurso parametrizado precisa ser exercitado com o parâmetro ligado.** Um build verde com a
   variável vazia testou o ramo que retorna na primeira linha, e não diz nada sobre o código. (D7)
9. **Trocar um mecanismo por outro custa propriedades que ninguém escreveu.** Sair de módulo
   *out-of-tree* para driver in-tree perdeu duas — compilar contra qualquer kernel, e não ser
   arrastado por um metapacote — e nenhuma falhou ao desaparecer, porque nenhuma estava declarada.
   Ao substituir um mecanismo, listar o que o antigo garantia **de graça**. (D5, D6)
10. **Um metapacote pode desfazer uma política de instalação por perfil.** `kernel-modules` depende de
    todos os módulos que o kernel constrói, então a decisão de "o que entra na imagem" sobe para "o
    que o kernel compila". (D6)
11. **Num fragmento `.cfg`, `# CONFIG_X is not set` é sintaxe.** Nomear um símbolo no começo de uma
    linha de comentário faz a ferramenta lê-la como opção malformada. (D8)
12. **Um erro de configuração de kernel é de duas espécies, e confundi-las custa uma investigação.**
    Um símbolo sem prompt no `Kconfig` não pode ser ligado por fragmento nenhum; um símbolo que **não
    existe** naquela árvore é descartado em silêncio. A primeira é um mecanismo errado, a segunda é
    conteúdo no lugar errado. (D5)
