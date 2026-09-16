# Plano de Implementação — Front-end de Aquisição ADS1299 (PoC EEG)

> **Estado, revisto em 2026-09-07**: a metade deste plano que não dependia de hardware **está
> implementada**, e a que depende continua aberta. Implementado: a escala em nanovolts (§7.1), a
> mensagem de controle e o `driverOptions` (§7.4), o `eeg.conf` novo (§7.5), o envelope de segurança
> derivado (§7.6), o simulador que passa a simular *este* conversor (§7.7), a janela de reconexão
> (§7.9) e a contenção de *part-number* (§3), agora verificável por `grep`. **Não** implementado, e
> nada aqui deve ser lido como se estivesse: o firmware do Cortex-M33, a atribuição RIF (§11.4), e
> qualquer medição de bancada. Ver `implementation_plan_iio_afe.md` §13 para a lista exata.
>
> Duas coisas mudaram de forma em relação ao que este documento previa. O `selfTest()` real (§7.3)
> existe **do lado do kernel**, no driver IIO, e não como um passo do firmware — porque foi o
> caminho IIO que ficou pronto primeiro. E o modo hat ganhou uma segunda topologia: com um driver
> IIO do ADS1299 existindo, o conversor pode ficar no SPI6 do **A35**, com `DRDY` como interrupção
> de verdade, sem passar pelo bloqueio de firmware assinado da §11.1. Isso não substitui a Ligação
> A como caminho de produto — o argumento da §5.3 continua dependendo de o Linux não tocar no
> conversor — mas tira o cronograma de refém dele. Por isso `acquisition.link` tem quatro valores
> (`simulated`, `amp`, `spi`, `usb`) e não os três que a §7.5 previa.
>
> A **Ligação B (USB) foi redecidida em 2026-08-27** e passou a ser *kernel* com IIO, não uma ponte
> em userspace: ver **`implementation_plan_iio_afe.md`**, que é o plano vigente daquele modo. Tudo o
> que este documento diz sobre a Ligação B vale como **levantamento** — os fatos de kernel, os
> números do MCP2210 e a análise de jitter continuam corretos e foram a base da decisão nova — e
> **não vale como instrução de implementação**. Os pontos onde isso muda o que fazer estão marcados
> no texto.
>
> **Uma correção de número.** A §5 e a §7.3 falam de um sinal de teste interno de "±1 mV / ±2 mV".
> Esse é o valor do irmão ECG, cuja referência é 2,4 V: o gerador entrega V<sub>REF</sub>/2400, o
> que com a referência de 4,5 V do ADS1299 dá **1,875 mV**, não 1 mV. É por isso que
> `afe.test_signal` aceita `internal` e não um nome que cite amplitude — uma chave de configuração
> que carrega um número errado é pior que uma que não carrega nenhum, porque é citada. Conferir em
> SBAS499 junto com o resto.
>
> Revisado em 2026-09-06 para incorporar a decisão de hardware que define **duas ligações físicas**
> para o mesmo conversor: *hat* sobre SPI6 e USB através de uma ponte MCP2210. Complementa
> `PROJECT_CONTEXT.md`, que continua sendo a referência arquitetural autoritativa. Os demais
> `implementation_plan*.md` de numeração antiga são iterações de projeto anteriores e **não**
> descrevem o estado atual do repositório.
>
> **Escopo**: define como o conversor **Texas Instruments ADS1299** (AFE de biopotenciais, 8 canais,
> 24 bits) entra na arquitetura `MedStack` sem violar a fronteira entre camadas, agora por **dois**
> caminhos elétricos distintos, e enumera as mudanças concretas — arquivo a arquivo — que essa
> escolha impõe.
>
> **Números de datasheet** citados aqui precisam ser conferidos em **SBAS499** (ADS1299) e
> **DS20005176** (MCP2210) antes de irem para o texto final do TCC. Os fatos de kernel e de
> devicetree, ao contrário, foram **lidos na árvore que este repositório constrói** e trazem o
> arquivo e a linha; são verificáveis com um `grep`.

---

## 1. O que mudou nesta revisão

A revisão anterior deste documento foi escrita antes do primeiro `kas build` e antes de a placa
bootar. Cinco premissas dela envelheceram:

| Afirmação anterior | Situação hoje |
|---|---|
| "Nada aqui foi validado por bitbake ainda (nenhum `kas build` rodou)" | Os dois perfis constroem verdes e a placa bootou — `CLAUDE.md`, `BRINGUP_STM32MP2.md` §9 |
| "Hoje o repositório não tem layer de BSP própria" (§4.8, decisão (a) vs (b) pendente) | `meta-med-bsp` existe, prioridade 7, e a decisão (a) foi tomada e implementada |
| Máquina de referência `stm32mp257f-ev1` | O alvo é `stm32mp25-disco` (STM32MP257F-DK) — ver o comentário em `kas/project-eeg-stm32mp2.yml` |
| "Cortex-M4" | Neste SoC é um **Cortex-M33**, e ele **exige firmware assinado** (`BRINGUP_STM32MP2.md` §9.7) — restrição que este plano não contemplava e agora contempla, em §11.1 |
| Uma única topologia (conversor no coprocessador) | **Duas**, decididas pelo hardware em desenvolvimento — §2 |

O que **não** mudou é o critério de aceitação (§3): ele apenas ficou maior, porque agora há um
segundo *part-number* — o MCP2210 — que também não pode vazar para cima.

---

## 2. As duas ligações do front-end

A placa de EEG em desenvolvimento oferece dois modos de conexão ao mesmo ADS1299. Isso não é um
detalhe de bancada: **muda quem fala com o conversor**, e portanto muda a camada onde o
conhecimento do *part-number* pousa.

### 2.1 Ligação A — *hat* sobre SPI6, conversor no Cortex-M33

```text
 ADS1299 ──SPI6 + DRDY──► Cortex-M33 (firmware)  ──OpenAMP/rpmsg──►  Linux (Cortex-A35)
   AFE                     registradores, ISR de DRDY,               MedicalIPC → driver "rpmsg"
                           LSB → nanovolt, FrameHeader               → MedicalDevice → serviço EEG
```

O que a árvore construída já garante, com arquivo e linha (kernel `linux-stm32mp` 6.6.129,
`build/tmp-glibc/work-shared/stm32mp25-disco/kernel-source`):

- **SPI6 existe e está desligado**: `arch/arm64/boot/dts/st/stm32mp257f-dk.dts:826` traz
  `&spi6 { ... status = "disabled"; }`. Habilitá-lo é uma mudança de devicetree, não de kernel.
- **Os pinos já estão descritos**: `arch/arm64/boot/dts/st/stm32mp25-pinctrl.dtsi:782`,
  `spi6_pins_a` → `SPI6_SCK = PF7`, `SPI6_MOSI = PC7`, `SPI6_MISO = PC4`. Os pinos 19/21/23 do
  conector de expansão correspondem, na convenção do conector, a MOSI/MISO/SCK — ou seja, a
  PC7/PC4/PF7. **Não há NSS no grupo**: o *chip select* do pino 24 é um GPIO comum, declarado como
  `cs-gpios` no nó SPI, e qual GPIO ele é sai do esquemático da placa, não da árvore.
- **SPI6 é um recurso do RIF**: `arch/arm64/boot/dts/st/stm32mp251.dtsi:1597` declara
  `spi6: spi@40350000` com `access-controllers = <&rifsc 27>`. Atribuir o barramento ao M33 é
  escrever essa atribuição na configuração do *Resource Isolation Framework*, que neste SoC, com o
  A35 como TDCID, vive no devicetree do **firmware seguro** (OP-TEE), não no do Linux. Qual arquivo
  exatamente, na árvore da ST, é item de bancada — §11.4.
- **O caminho AMP já está cabeado no devicetree**: `stm32mp257f-dk.dts:598` traz `&m33_rproc` com
  `status = "okay"`, `mboxes = <&ipcc1 ...>` e `memory-region` incluindo `vdev0vring0`,
  `vdev0vring1` e `vdev0buffer`. É o virtio/rpmsg que o driver `rpmsg` do framework consome. O boot
  de 2026-08-18 confirmou o outro lado: `remoteproc remoteproc1: m33 is available`.

**Esta é a ligação de produto.** É a que sustenta o argumento de segregação de criticidade por
processador (`CONTRIBUTION.md` §3), a que dá determinismo ao `DRDY`, e a única em que a afirmação
"o Linux nunca toca no conversor" é literalmente verdadeira.

**E é a que tem o bloqueio maior**: `stm32-rproc 0.m33: Support of signed firmware only`. Enquanto
esse bloqueio não cair, a ligação A não produz uma amostra sequer.

### 2.2 Ligação B — USB, ponte MCP2210, conversor no Linux (userspace)

> **SUPERSEDIDA em 2026-08-27.** A Ligação B passou a ser **IIO com driver de kernel próprio**, e
> não uma ponte em userspace sobre `/dev/hidraw*`. A decisão, o veredito sobre o driver de terceiro,
> a camada adjunta que hospeda os dois drivers e o custo disso no argumento da §5.3 estão em
> **`implementation_plan_iio_afe.md`**. O que segue abaixo permanece válido como levantamento — os
> fatos de kernel, os números do MCP2210 e a análise de jitter continuam corretos e são a base da
> decisão nova.



```text
 ADS1299 ──SPI──► MCP2210 ──USB HID──► Linux ──► med-afe-bridge ──AF_UNIX──► driver "socket"
   AFE            ponte USB-SPI        /dev/hidraw*   mapa de registradores,   → MedicalDevice
                  9 GPIO (CS, DRDY)                   LSB → nanovolt,          → serviço EEG
                                                      MESMO FrameHeader
```

Fatos verificados na mesma árvore, e três deles são desagradáveis:

1. **Não existe driver de MCP2210 no kernel.** `grep -ril mcp2210 drivers/ Documentation/` na árvore
   6.6.129 da ST retorna **nada**. O que existe é `drivers/hid/hid-mcp2200.c` e
   `drivers/hid/hid-mcp2221.c` — outros dois integrados da mesma família, e a ausência do 2210 entre
   eles é a confirmação, não uma omissão do meu `grep`. Consequência: o MCP2210 **não** aparece como
   um barramento SPI do Linux. Ele aparece como um dispositivo HID genérico, e quem fala com ele é
   *userspace*.
2. **`CONFIG_HIDRAW` está desligado nos dois kernels que este repositório constrói.** Medido:

   ```
   linux-stm32mp 6.6.129 : # CONFIG_HIDRAW is not set   (CONFIG_USB_HID=y, CONFIG_HID_GENERIC=y)
   linux-yocto   6.6.144 : # CONFIG_HIDRAW is not set   (idem)
   ```

   Sem `hidraw`, o MCP2210 enumera, o `hid-generic` faz *bind* nele, e **nenhum nó
   `/dev/hidraw*` é criado**. A ponte não encontraria nada. É exatamente a forma do defeito do
   `CONFIG_DM_VERITY` (§ do `med-kernel-features.cfg`): uma política que a plataforma assume e nunca
   escreveu. Com uma diferença que vale registrar: aqui **os dois** kernels estão desligados, então
   o QEMU não esconderia o defeito — ele falharia nos dois alvos, o que é a boa notícia.
3. **`spidev` não é uma saída.** `drivers/spi/spidev.c:727` (`spidev_of_check`) retorna `-EINVAL`
   quando o `compatible` do nó é literalmente `"spidev"` — "spidev listed directly in DT is not
   supported". Só probeiam os doze `compatible` da tabela `spidev_dt_ids`, nenhum deles de um AFE.
   E não há driver IIO do ADS1299: `ls drivers/iio/adc | grep ads` dá `ti-ads1015`, `ti-ads1100`,
   `ti-ads124s08`, `ti-ads131e08`, `ti-ads7924`, `ti-ads7950`, `ti-ads8344`, `ti-ads8688` — o
   parente próximo (`ti-ads131e08`) é outro conversor. Falar com o ADS1299 pelo lado Linux **sempre**
   custa código fora da árvore; a única pergunta é se ele fica em kernel (driver *out-of-tree*, o
   acoplamento que a arquitetura combate) ou em userspace (um processo, que a arquitetura já sabe
   isolar). Este plano escolhe userspace.

**Esta é a ligação de bancada e de portabilidade**, e ela tem duas virtudes que a de produto não tem:

- **Não depende de firmware assinado.** É o único caminho para amostras reais que não passa pelo
  bloqueio do §11.1 — ou seja, é seguro de cronograma (§9).
- **Não depende da placa.** Um MCP2210 é USB; a mesma ponte roda no PC de desenvolvimento e, com
  *passthrough*, dentro do QEMU. Isso transforma o perfil de simulação em um perfil de aquisição
  real sem recompilar nada — §11.6.

### 2.3 As duas ligações não são duas configurações clínicas equivalentes

Esta é a decisão que o resto do documento assume, e ela precisa estar escrita antes das mudanças de
código, porque é ela que decide o que é defeito e o que é limitação declarada:

> **A ligação A é o caminho clínico. A ligação B é caminho de desenvolvimento, demonstração e
> portabilidade, e um registro adquirido por ela tem de dizer isso de si mesmo.**

Três razões, todas objetivas:

| | Ligação A (SPI6 → M33) | Ligação B (USB → MCP2210) |
|---|---|---|
| Quem atende o `DRDY` | ISR no M33, jitter de microssegundos | *polling* do contador de interrupção do MCP2210 pelo host, jitter de milissegundos, **não medido** |
| Carimbo de tempo da amostra | instante do `DRDY`, gerado junto da conversão | instante em que o USB entregou o relatório ao host — depois da latência do barramento e do escalonador |
| Teto de taxa | limitado pelo SPI (o AFE vai a 16 kSPS) | limitado pelo USB *full-speed*: **≈250 SPS**, com a conta em §8 |
| Perda de amostra | detectável pelo firmware, no instante | detectável só por comparação com o contador de bordas do GP6 (§4) |

O carimbo de tempo é o ponto que pesa mais para IEC 62304 e é o mesmo problema que o `BRINGUP` §9.7
já registrou para o RTC: um registro de paciente com data errada é pior que um registro ausente. Na
ligação B o erro não é de data, é de *fase* — e ele tem de aparecer no registro, não em uma nota de
rodapé. Ver §7.5, campo `acquisition.link`.

---

## 3. Critério de aceitação — agora com dois *part-numbers*

O critério do plano original continua sendo o teste da tese, e ganha o segundo nome:

> **Nenhuma ocorrência das strings `ADS1299` ou `MCP2210` fora de `meta-med-bsp` e do firmware do
> M33.** Nenhum campo de `DeviceConfig`/`MedicalDevice` no MedFramework é específico de qualquer um
> dos dois. Nenhuma linha de `eeg-acquisition-service` ou de `eeg-hmi-gui` menciona qualquer um dos
> dois.

Verificável por `grep -ri`, que é a evidência que o `PLANO_TCC.md` §Fase 4 já cobra. O que muda por
camada:

| Camada | Ligação A | Ligação B |
|---|---|---|
| **`meta-med-bsp`** | *overlay* de devicetree (SPI6, `cs-gpios`, `DRDY`, atribuição RIF ao M33); receita do firmware, apontada por `MED_AMP_FIRMWARE` | receita `med-afe-bridge` (mapa de registradores do ADS1299 + protocolo do MCP2210), regra udev por VID/PID, unidade systemd, `DeviceAllow` do `/dev/hidraw*` |
| **`meta-med-distro`** | nada específico | `CONFIG_HIDRAW=y` no fragmento de kernel — política genérica ("userspace fala com dispositivos HID"), não *part-number* |

> **A coluna "Ligação B" desta tabela está superada.** Não há mais `med-afe-bridge` nem
> `CONFIG_HIDRAW`: os dois drivers passam a ser de kernel e moram numa **camada adjunta**
> (`meta-med-afe-ads1299`), fora das quatro camadas do `MedStack`. Ver
> `implementation_plan_iio_afe.md` §4 e §8.
| **`meta-med-framework`** | driver `rpmsg` ganha canal de controle; `DeviceConfig` ganha `driverOptions` opaco | driver **`socket`**, que é o mesmo decodificador de quadro sobre outro transporte (§6.2) |
| **`meta-med-app`** | `eeg.conf` ganha parâmetros clínicos | `eeg.conf` ganha `acquisition.link`; **código-fonte das aplicações inalterado nos dois casos** |

---

## 4. O que o hardware ainda precisa decidir — e é agora

Esta seção existe porque a placa está sendo projetada. Cada item aqui é barato antes do esquemático
fechar e caro depois.

**4.1 Quatro pinos não bastam para a ligação A.** SPI6 nos pinos 19/21/23/24 dá SCK, MOSI, MISO e
CS. O ADS1299 precisa de pelo menos mais um sinal obrigatório e três desejáveis:

| Sinal | Obrigatório? | Observação |
|---|---|---|
| `DRDY` | **sim** | É a interrupção. Sem ela não há aquisição contínua determinística — só *polling* de registrador, que é a ligação B com fio curto |
| `RESET` | recomendado | Pode ser amarrado a `DVDD` com um RC, ao custo de perder o reset por software |
| `PWDN` | recomendado | Idem; sem ele não há desligamento do analógico |
| `START` | opcional | Substituível pelo comando `START` por SPI, ao custo de determinismo no início da conversão |
| `CLKSEL` | decidir | Oscilador interno de 2,048 MHz vs. relógio externo. Interno simplifica; externo permite sincronizar múltiplos conversores |

Recomendação: **levar `DRDY`, `RESET` e `PWDN` ao conector**, e amarrar `START`. `DRDY` não é
negociável.

**4.2 O pino do `DRDY` tem de poder pertencer ao M33.** Na ligação A quem atende a interrupção é o
coprocessador, então o GPIO do `DRDY` precisa ser atribuível ao mesmo CID que o SPI6 no RIF. Os
bancos GPIO do STM32MP25 são cientes de RIF por pino, mas *quais* bancos e pinos o conector expõe é
fato de esquemático. **Escolher o pino do `DRDY` olhando o RIF, não só a numeração do conector** —
descobrir isso depois de fabricar a placa custa uma revisão de placa.

**4.3 Na ligação B, use os GPIO do próprio MCP2210.** A ponte tem nove pinos de uso geral (GP0–GP8),
e o *chip select* é gerado por ela — o host não precisa de GPIO nenhum. Três consequências de
projeto:

- `CS` sai de um GP do MCP2210, com atraso configurável antes/depois da transferência;
- `RESET`, `PWDN` e `START` também podem sair de GPs, o que dá à ponte o mesmo controle que o M33
  tem na ligação A;
- **`DRDY` deve ir ao GP6**, que é o pino com função alternativa de contador de interrupção. Não há
  entrega assíncrona de interrupção por USB HID — o host só consegue *ler o contador*. Mas ler o
  contador é o que permite dizer **quantas amostras se perderam**, comparando o número de bordas com
  o número de quadros lidos. Sem isso, a perda de amostras na ligação B é indetectável, e a
  `RESULTS.md` §9 já lista perda de amostras como não medida. Este é o pino que transforma um número
  em uma medição.

**4.4 Os dois modos compartilham o barramento SPI do AFE.** Garantir isolamento elétrico entre o
conector do *hat* e a saída do MCP2210 quando um dos dois não está em uso (jumper, *buffer* com
*output enable*, ou resistores de população alternativa). Dois mestres no mesmo SPI, mesmo que só um
esteja energizado, é uma fonte de defeito intermitente — e intermitente, num equipamento de medição,
é o pior tipo.

**4.5 Alimentação analógica.** AVDD 5 V (ou ±2,5 V) e DVDD 1,8–3,3 V, ≈5 mW/canal. Na ligação B o
barramento USB oferece 5 V, mas o ruído dele chega ao analógico; na ligação A o 5 V vem do conector
de expansão da placa. Em qualquer dos dois, o piso de ruído de ≈1 µV<sub>pp</sub> do datasheet só é
alcançável com regulagem própria e retorno de terra separado. É o parâmetro que o §10 (verificação)
mede com as entradas em curto — e a medida vale para a *placa*, não para o conversor.

---

## 5. Parâmetros do conversor que a plataforma precisa conhecer

Extraídos do datasheet (conferir em SBAS499):

| Característica | Valor | Onde impacta |
|---|---|---|
| Canais simultâneos | 8 (variantes ‑6 e ‑4) | `acquisition.channels = 8` já está correto |
| Resolução | 24 bits, ΔΣ, amostragem simultânea | Formato de amostra e quantização do simulador |
| Taxas de dados (ODR) | 250, 500, 1k, 2k, 4k, 8k, 16k SPS | **Conjunto discreto** — invalida o limite contínuo atual |
| Ganho do PGA | 1, 2, 4, 6, 8, 12, 24 | Novo parâmetro; define o fundo de escala |
| Referência interna | 4,5 V (ou externa) | Define a escala junto com o ganho |
| Fundo de escala | ±V<sub>REF</sub>/ganho → **±187,5 mV** com ganho 24 | Verificação DERS do envelope de segurança |
| Passo do LSB | V<sub>REF</sub>/(ganho·2²³) → **22,35 nV** com ganho 24 | **Colide com `scaleNanoUnitsPerLsb`** (§7.1) |
| Ruído referido à entrada | ≈1 µV<sub>pp</sub> (0,14 µV<sub>rms</sub>) a 250 SPS, ganho 24 | Piso de ruído do simulador e critério de autoteste |
| CMRR | ≈ −110 dB | Justificativa de projeto (capítulo de hardware) |
| Quadro por `DRDY` | 24 bits de status + 8 × 24 bits = **27 bytes** | Dimensionamento do quadro nas **duas** ligações (§7.2, §8) |
| **Ausência de FIFO** | o conversor não bufferiza: os 27 bytes têm de sair antes do próximo `DRDY` | É o que fixa o teto da ligação B (§8) |
| Recursos integrados | *bias drive*, detecção de eletrodo solto, sinal de teste (±1 mV / ±2 mV, ~1 Hz / ~2 Hz), sensor de temperatura, SRB1/SRB2, *daisy-chain* | `selfTest()` real (§7.3) |
| Interface | SPI (SCLK até 20 MHz; oscilador interno 2,048 MHz), `DRDY`, `START`, `RESET`, `PWDN` | Firmware M33 / devicetree / ponte |
| Alimentação | AVDD 5 V (ou ±2,5 V), DVDD 1,8–3,3 V, ≈5 mW/canal | §4.5 e capítulo de hardware |

E os do integrado da ponte (conferir em DS20005176):

| Característica | Valor | Onde impacta |
|---|---|---|
| Classe USB | HID, *full-speed* (12 Mbit/s), relatórios de **64 bytes** | Sem driver de kernel: `/dev/hidraw*` (§2.2) |
| Payload por comando de transferência SPI | **até 60 bytes** | 27 bytes de um quadro cabem em um comando — §8 |
| Taxa de bit SPI | até 12 MHz | Folgado para 27 bytes por período de amostra |
| GPIO | GP0–GP8; GP6 com contador de interrupção | §4.3 — `CS`, `RESET`, `PWDN`, `START`, `DRDY` |
| Entrega de interrupção ao host | **não existe**; só contagem lida por *polling* | É a origem do jitter da ligação B (§2.3) |

---

## 6. Arquitetura: um formato de quadro, duas origens

### 6.1 O `FrameHeader` como única superfície de conformidade

`med::amp::FrameHeader` (`MedicalDevice.h`) já é um contrato de 40 bytes com `static_assert`,
projetado para atravessar dois compiladores e duas arquiteturas. O serviço de aquisição **já** o
usa em dois transportes diferentes: recebe do `rpmsg` e publica para a HMI em `AF_UNIX`
(`main.cpp:232`, `encodeFrame`). Ou seja, o formato já provou que viaja em socket.

A decisão deste plano é aproveitar isso: **as duas ligações entregam o mesmo `FrameHeader`.** O que
muda entre elas é apenas o transporte e quem produz o quadro. Nada acima do produtor sabe qual das
duas está em uso — exceto pelo campo declarativo `acquisition.link`, que existe justamente para que
o registro clínico saiba (§2.3).

### 6.2 O driver `socket` — o mesmo decodificador, outro transporte

`RpmsgDevice` (`MedicalDevice.cpp:174`) faz três coisas: abre um `MedicalIpcChannel`, valida o
quadro (magic, versão, tamanho, CRC, continuidade de sequência) e converte LSB→µV. Só a primeira é
específica de rpmsg, e ela já é uma linha (`endpoint.transport = IpcTransport::RpmsgChar`).

Refatorar em `FrameStreamDevice`, com o transporte vindo da configuração, e registrar **dois nomes**
na fábrica:

- `rpmsg` → `IpcTransport::RpmsgChar`, endereço `/dev/rpmsg0` (comportamento atual, bit a bit);
- `socket` → `IpcTransport::UnixSeqpacket`, endereço `/run/medplatform/afe.sock`.

`MedicalIpcChannel::connect` já aceita os dois (`MedicalIPC.h:57`). O nome do driver continua sendo
o do **transporte**, nunca o do *part-number* — é a mesma regra que já vale para `rpmsg`, e é o que
impede um driver chamado `mcp2210` de nascer dentro do framework.

Ganho colateral que vale registrar: `zero` dependências novas. O framework continua com
`libsystemd` + `libcrypto` e nada mais (`med-framework-api_1.0.0.bb:32`), o que é o que o mantém
compilável sem alteração para os dois alvos.

### 6.3 `med-afe-bridge` — o firmware do M33, compilado para o Linux

> **Superada como instrução.** A ideia central desta seção — *o mesmo mapa de registradores, dois
> ambientes de execução* — sobrevive e é boa; o que muda é que o segundo ambiente passa a ser um
> driver IIO de kernel, e não um processo em userspace. Ver `implementation_plan_iio_afe.md` §6.

Um processo pequeno que faz **exatamente o que o firmware do M33 faz**, e é assim que ele deve ser
lido:

| | firmware M33 (ligação A) | `med-afe-bridge` (ligação B) |
|---|---|---|
| Fala com o AFE por | SPI6, registradores diretos | HID → MCP2210 → SPI |
| Atende o `DRDY` por | ISR | leitura do contador do GP6 + *polling* |
| Converte | LSB → nanovolt | LSB → nanovolt |
| Publica | `FrameHeader` em rpmsg | `FrameHeader` em `AF_UNIX` seqpacket |
| Declara a ABI | redeclarando a struct de 40 bytes | redeclarando a struct de 40 bytes |
| Depende do MedFramework | não (outro compilador, outra arquitetura) | **não, e é deliberado** |

A última linha é a que preserva a direção das dependências. A ponte **não linka `medframework`**:
ela redeclara os 40 bytes com o mesmo `static_assert`, exatamente como o firmware é obrigado a
fazer. O formato de quadro é uma **especificação**, não um cabeçalho compartilhado — e é bom que
seja, porque é isso que permite que a ponte viva abaixo do framework na pilha de camadas.

Dependências externas da ponte: **nenhuma**. `/dev/hidraw*` é `open`/`read`/`write`; não é preciso
`libusb` nem `hidapi`. Isso importa mais do que parece — uma dependência de `libusb` na ponte não
violaria a regra 6 do `CLAUDE.md` (que fala de *aplicações*), mas violaria o espírito dela, e
`hidraw` torna a questão discutível.

### 6.4 Onde a ponte mora

**`meta-med-bsp`.** Três argumentos, em ordem de força:

1. **Simetria de papel.** A ponte e o firmware do M33 são o mesmo papel em dois hardwares. O
   firmware já é artefato de BSP (`MED_AMP_FIRMWARE`, `packagegroup-med-amp.bb`). Pôr a ponte em
   outra camada seria dizer que o mesmo papel muda de dono conforme o barramento.
2. **É onde os *part-numbers* podem morar.** O critério de §3 exige que `ADS1299` e `MCP2210` não
   apareçam acima do BSP. A ponte contém os dois. Não há outra camada legal para ela.
3. **É onde nós de dispositivo podem morar** (regra 1 do `CLAUDE.md`). A ponte abre `/dev/hidraw*` e
   a unidade dela precisa de `DeviceAllow=`.

E a regra da camada continua respeitada: `meta-med-bsp` "fornece e nunca consome" — a ponte não faz
`DEPENDS` de nada acima, não traz `bbappend` sobre receita de camada superior, e é instalada por
`MED_BSP_INSTALL`, o gancho que `med-image-base.bb` já declara e esta camada já preenche.

**Objeção honesta, registrada porque ela é razoável**: o MCP2210 está na *placa de EEG*, não na
placa STM32 — então ele é fato de produto (domínio de `meta-med-app`), não de máquina. A resposta é
que a regra 1 é literal sobre nós de dispositivo e sobre nomes de *part-number*, e que o que
`meta-med-bsp` protege é a portabilidade entre *hosts*: um VID/PID de USB é justamente o que **não**
muda de host para host, então mantê-lo aqui não custa portabilidade nenhuma. Se essa leitura for
recusada, a alternativa é uma quinta camada para o *front-end*, e isso é uma decisão de arquitetura
maior do que este plano — **está em aberto em §11.5**.

### 6.5 Por que a ponte não é um driver de kernel

A alternativa óbvia — escrever um driver de kernel que registre o MCP2210 como `spi_controller`, de
modo que o ADS1299 vire um dispositivo SPI de verdade — foi avaliada e **recusada**. Ela parece mais
robusta e não é, por cinco razões em ordem de força. Registrada aqui porque a pergunta volta.

**1. Um driver do MCP2210 sozinho não produz uma amostra.** Ele entrega um barramento SPI; falta
quem fale ADS1299 nesse barramento. As duas continuações possíveis são ambas ruins:

- **Driver IIO do ADS1299**: não existe (verificado, §2.2), e o modelo IIO **não se aplica a este
  enlace**. Um driver IIO de AFE é construído em torno de um *trigger* ligado ao `DRDY` como IRQ; no
  USB HID **não há entrega de interrupção**, só o contador do GP6 lido por *polling* (§4.3). O que
  se escreveria é um driver IIO com `iio-trig-hrtimer` sondando o USB — a forma do IIO sem a
  propriedade que faz o IIO valer a pena.
- **`spidev` por cima do controlador**: não funciona sem uma mentira, e desta vez uma mentira
  *silenciosa*. `spidev_spi_ids` (`drivers/spi/spidev.c:706`) lista os mesmos doze *part-numbers* da
  tabela de DT e **não contém `"spidev"`**; um `spi_new_device()` com `modalias = "spidev"` não casa
  com nada. Casaria com `modalias = "dh2228fv"` — e note o que acontece então: `spidev_probe`
  chama `device_get_match_data`, que para um dispositivo sem DT e sem ACPI devolve `NULL`, o
  `match` é pulado e **`spidev_of_check` nunca roda** (`spidev.c:783-788`). Ou seja, é a mesma
  falsificação de identidade do §11.3, com a agravante de que o kernel não a reporta. E o resultado
  final é aquisição em *userspace* de qualquer jeito — tendo pago um módulo de kernel por ela.

**2. O gargalo não é o espaço de endereçamento, é o USB.** Todo o teto do §8 — 64 bytes por quadro
de 1 ms, 60 bytes de payload por comando, uma transação por amostra porque o ADS1299 não tem FIFO —
é do barramento e do integrado da ponte. Nenhum driver de kernel muda qualquer um dos três. O que o
kernel ganharia é o carimbo de tempo tirado no *completion handler* da URB em vez de depois de um
acordar de processo: dezenas de microssegundos contra um quantum de 1 ms do USB, isto é, algo entre
1 % e 3 % do orçamento de jitter. Não é o fator que decide.

**3. É código que não se aproveita.** Na ligação de produto o conversor pertence ao M33 e o Linux
nunca o enxerga — um driver de kernel do ADS1299 é escrito para o caminho que o §2.3 declara ser de
desenvolvimento, e é jogado fora quando a ligação A funcionar. A ponte em userspace, ao contrário,
**é o firmware do M33 escrito uma vez antes** (§6.3): mesmo mapa de registradores, mesma conversão
para nanovolt, mesmo `FrameHeader`. Escrever a ponte é adiantar a Fase 3; escrever o driver não é.

**4. Contenção de falha, que aqui é argumento regulatório e não de gosto.** A tese inteira defende
segregação por criticidade (`CONTRIBUTION.md` §3), e o `eeg-acquisition.service` já leva o
*sandboxing* mais apertado que este repositório sabe escrever. Um módulo de kernel está no domínio
de confiança do kernel: um *oops* nele derruba o aparelho, com o watchdog reiniciando um dispositivo
médico por causa do enlace de bancada. A ponte em userspace falha, é reiniciada, deixa registro de
auditoria e a HMI mostra a perda. Pôr o código do caminho **menos** crítico no domínio **mais**
crítico inverte exatamente o princípio que o trabalho argumenta.

**5. Custo de manutenção contra dois kernels de terceiros.** Um módulo fora da árvore é uma receita
`inherit module` que precisa compilar contra `linux-stm32mp` 6.6.129 **e** `linux-yocto` 6.6.144, e
que quebra na próxima subida de versão da ST. A regra 1 do §11 do `BRINGUP_STM32MP2.md` — "uma
suposição documentada sobre comportamento de terceiro é dívida" — cobre precisamente isso. Se
existir implementação fora da árvore para reaproveitar, ela tem de ser avaliada quanto a versão de
kernel alvo e licença antes de contar como reuso; **não avaliado aqui**.

#### O que reabriria a decisão

Duas condições, e nenhuma delas vale hoje:

- **A ligação B virar caminho de produto** — se o bloqueio de firmware assinado (§11.1) se mostrar
  insolúvel e o USB tiver de ser o enlace do aparelho, então IIO, carimbo de tempo no kernel e um
  driver mantido passam a valer o preço, e esta seção deve ser revertida.
- **A medição do §8 mostrar que o gargalo é userspace, e não o USB** — se o jitter medido for
  dominado pelo escalonamento do processo e não pelo quantum do barramento, o argumento 2 cai. A
  ordem correta é medir primeiro: pagar por espaço de kernel antes de ter visto o userspace falhar é
  o mesmo erro que a suíte de aceitação já cometeu uma vez, quando `acq-active` afirmava verificar
  algo que nunca tinha visto falhar.

---

## 7. Mudanças concretas, arquivo a arquivo

### 7.1 Precisão da escala no protocolo AMP — corrigir antes de qualquer bancada

Inalterado em relação à revisão anterior, e continua sendo a primeira coisa a fazer.

`FrameHeader::scaleNanoUnitsPerLsb` é `int32_t` em **nanovolts por LSB**. Com ganho 24 o passo real
é 22,3517 nV; arredondar para 22 nV dá **erro de ganho sistemático de 1,6 %** em todo o traçado —
inaceitável e, pior, invisível (o sinal continua "bonito").

| Opção | Efeito | Custo |
|---|---|---|
| **(A) O produtor converte para nanovolts e envia nV; `scaleNanoUnitsPerLsb = 1`** | Erro < 0,5 nV, muito abaixo do piso de 140 nV<sub>rms</sub> | Uma multiplicação por amostra. **Zero mudança de ABI**; é exatamente o que `encodeFrame` já faz ao publicar para a HMI (`main.cpp:72-76`) |
| (B) Contagens brutas + escala racional no cabeçalho | Exato | Quebra a ABI e o `static_assert` |
| (C) Mudar a unidade do campo para picovolts | Exato | Muda a semântica de um campo sem mudar o nome — o pior tipo de mudança silenciosa |

**Recomendação: (A)**, e vale para os **dois** produtores. ±187,5 mV = ±187.500.000 nV cabe folgado
em `int32_t`. Documentar no comentário do `FrameHeader` que o produtor entrega nanovolts.

### 7.2 Dimensionamento do quadro — dois tetos diferentes

**Ligação A (rpmsg).** Buffer padrão de 512 bytes, ~496 úteis, menos 40 de cabeçalho → 456 bytes.
Com `int32`: 114 amostras → **14 amostras/canal** para 8 canais. A 250 SPS é um quadro de 56 ms.

**Ligação B (USB).** O teto não é o quadro, é a **transação**: até 60 bytes de dados por comando de
transferência SPI do MCP2210, e o ADS1299 não tem FIFO — os 27 bytes de uma amostra têm de sair
antes do próximo `DRDY`. Logo **uma amostra por transação**, e o agrupamento em quadro acontece na
ponte, em memória, não no fio. A ponte pode montar quadros de 14 amostras/canal como a ligação A, e
**deve** — para que o mesmo `samples_per_frame` sirva às duas e o `eeg.conf` não bifurque.

O comentário atual em `eeg.conf` cita 12 amostras/quadro e diverge da conta; **recalcular e alinhar**
ao ajustar o arquivo (a conta acima dá 14). Se um perfil de alta taxa (≥4 kSPS) entrar no escopo,
avaliar empacotar as amostras nos 24 bits nativos (456/3 = 152 → 19 amostras/canal, +35 % de
payload) — mudança de formato de fio, fora deste plano, e irrelevante para a ligação B, que não
chega lá (§8).

### 7.3 `selfTest()` deixa de ser um *stub*

O ADS1299 permite uma verificação de desempenho essencial (IEC 60601‑1 §14) genuína, executada pelo
produtor (firmware ou ponte) e reportada ao Linux como resultado agregado:

1. **Identidade**: ler o registrador de ID e conferir o valor da variante de 8 canais.
2. **Sinal de teste interno**: MUX de todos os canais no gerador interno; verificar amplitude de
   1 mV ±tolerância e período de ~1 s em cada canal — valida cadeia analógica, PGA, ADC e o enlace
   de ponta a ponta.
3. **Ruído com entradas em curto**: MUX em *shorted input*; ruído RMS dentro do especificado
   (≈0,14 µV<sub>rms</sub> a 250 SPS/ganho 24).
4. **Eletrodo solto**: acionar a detecção integrada e reportar status por eletrodo.

Na ligação B acrescenta-se um passo 0: **identidade da ponte** — conferir VID/PID e a versão de
firmware do MCP2210 antes de qualquer transação SPI. Um `/dev/hidraw*` que não é a ponte é um
dispositivo HID qualquer do usuário, e escrever nele é pior que falhar.

O resultado atravessa `MedicalDevice::selfTest()` como `Status` + evento de auditoria via
`MedicalLogger` — a aplicação continua sem saber que existe um registrador de ID.

### 7.4 Como os parâmetros clínicos chegam ao produtor sem contaminar o framework

Ganho e ODR são prescrições **da aplicação médica** e registradores **do chip**. Para não criar
campos `gain`/`leadOff` em `DeviceConfig`:

- `DeviceConfig` ganha **um** campo genérico: `std::map<std::string, std::string> driverOptions`.
- O driver serializa as opções em uma mensagem de **controle** no mesmo canal, antes de iniciar a
  aquisição; o produtor traduz para escritas de registrador. Vale para os **dois** transportes, o
  que é mais um motivo para o refactor do §6.2 ser um só decodificador.
- O driver `simulated` **aceita as mesmas chaves** e as honra (§7.7). É isso que mantém o `eeg.conf`
  idêntico entre os alvos exceto pelas duas linhas substituídas.
- Chave desconhecida → falha explícita, nunca ignorar. Uma opção de segurança silenciosamente
  descartada é um defeito, não uma tolerância.

Isso exige uma extensão pequena do protocolo: um tipo de mensagem de controle além do quadro de
amostras, com layout fixo e `static_assert`, descrita junto ao `FrameHeader` e com o mesmo rigor.

### 7.5 `eeg.conf` — novos parâmetros

```ini
# --- front-end -------------------------------------------------------------
# "simulated" - sinal sintético no framework (perfil QEMU)
# "rpmsg"     - quadros do firmware do Cortex-M33 (ligação A, hat/SPI6)
# "socket"    - quadros do med-afe-bridge (ligação B, USB/ponte)
device.driver = @MED_EEG_DRIVER@
device.id = eeg0
device.address = @MED_EEG_ADDRESS@

# --- opções do front-end (repassadas opacamente ao produtor) ---------------
afe.gain = 24                    # PGA: 1,2,4,6,8,12,24
afe.reference_uv = 4500000       # referência interna de 4,5 V
afe.lead_off_detection = true
afe.bias_drive = true            # amplificador de bias (eletrodo de referência)
afe.test_signal = off            # off | internal_1mv_1hz  (usado por selfTest)

# --- acquisition -----------------------------------------------------------
acquisition.channels = 8
acquisition.sample_rate_hz = 250 # ODR válidos: 250,500,1000,2000,4000,8000,16000
acquisition.samples_per_frame = 14
# Qual ligação física produziu este registro. NÃO é um parâmetro de driver: é
# uma declaração que vai para o registro clínico, porque o carimbo de tempo de
# "usb" é o instante da entrega ao host e não o da conversão (§2.3).
acquisition.link = @MED_EEG_LINK@   # simulated | amp | usb
```

Notar o que **não** aparece: nem `ads1299`, nem `mcp2210`. A revisão anterior propunha
`device.model = ads1299` "consumido apenas pelo firmware"; isso é retirado — um *part-number* em
`meta-med-app` reprova o `grep` do §3, e o `DeviceInfo::model` que a HMI mostra pode vir do produtor,
que é quem sabe.

`@MED_EEG_ADDRESS@` e `@MED_EEG_LINK@` seguem exatamente o mecanismo de `@MED_EEG_DRIVER@`
(`eeg-acquisition-service_1.0.0.bb`, `do_install:append`), e o `do_seal_configuration` continua
valendo sem mudança: o `.sha256` é gerado sobre o arquivo já substituído. **Nenhum alvo de `make`
novo, nenhum overlay de kas** — as três variáveis são definidas no `local_conf_header` do
`kas/project-*.yml` que já existe, e trocar de ligação é trocar o valor delas.

### 7.6 Envelope de segurança — de limites fixos para limites derivados

Os limites em `safetyLimits()` (`eeg-acquisition-service/files/src/main.cpp:51`) ficam
inconsistentes com o conversor real:

| Limite atual | Problema com o ADS1299 | Correção |
|---|---|---|
| `acquisition.sample_rate_hz` ∈ [125, 2000] | 125 Hz é inatingível; o conversor só oferece taxas discretas | Verificação de **pertinência ao conjunto** {250, 500, 1k, 2k, 4k, 8k, 16k} |
| `acquisition.channels` ∈ [1, 64] | O chip tem 8; >8 exige *daisy-chain* | Múltiplo de 8, máximo 32 (4 em cascata) |
| `safety.max_input_uv = 500`, limite [10, 5000] | Sem relação declarada com o fundo de escala | **Verificação DERS relacional**: `safety.max_input_uv ≤ 1e6·V_REF/ganho`. Com ganho 24 → 187.500 µV, então 500 µV é um limiar de artefato fisiológico, e isso passa a estar explícito |
| — | Ganho não verificado | `afe.gain` ∈ {1,2,4,6,8,12,24} |

O comportamento permanece: violação **para o serviço**, não satura o valor.

**O que deliberadamente não entra aqui**: o teto de taxa da ligação B. `safetyLimits()` é uma tabela
estática e independente de hardware, e enfiar nela um limite que só vale para USB seria vazamento de
BSP para a aplicação. Quem recusa uma ODR que não consegue sustentar é a **ponte**, na inicialização,
com mensagem explícita — mesma regra do §7.4 sobre chave desconhecida.

### 7.7 Driver `simulated` — passa a simular *este* conversor

Para que o QEMU exercite os mesmos caminhos de código:

- quantizar em passos de V<sub>REF</sub>/(ganho·2²³) e saturar em ±V<sub>REF</sub>/ganho;
- somar piso de ruído coerente com o datasheet (≈0,14 µV<sub>rms</sub> a 250 SPS/ganho 24);
- aceitar apenas os ODR válidos e as mesmas chaves de `driverOptions`;
- simular deriva de offset DC de eletrodo e eventos de eletrodo solto (para exercitar a HMI);
- reproduzir a onda quadrada de 1 mV/1 Hz quando `afe.test_signal` estiver ativo, para que o
  `selfTest()` tenha o mesmo roteiro nos três casos.

`DeviceInfo::model` passa a refletir o que está sendo emulado.

### 7.8 Kernel — o que entra, e o que a revisão anterior pedia sem precisar

**Entra**, em `meta-med-distro/recipes-kernel/linux/files/med-kernel-features.cfg`:

```
# --- Acesso de userspace a dispositivos HID. Política de plataforma, não
# --- part-number: é o que permite a um processo falar com uma ponte USB de
# --- barramento sem driver de kernel. Desligado nos DOIS kernels que este
# --- repositório constrói (linux-yocto e linux-stm32mp), com USB_HID=y - ou
# --- seja, o dispositivo enumera, hid-generic faz bind, e nenhum /dev/hidraw
# --- aparece. Mesma forma do defeito do CONFIG_DM_VERITY: política que a
# --- plataforma assume e nunca escreveu.
CONFIG_HIDRAW=y
```

> **Superado.** Sem ponte em userspace, ninguém abre `/dev/hidraw*` — `CONFIG_HIDRAW` deixa de ser
> necessário. O que o fragmento passa a precisar é a ABI do IIO: `CONFIG_IIO`, `CONFIG_IIO_BUFFER`,
> `CONFIG_IIO_TRIGGERED_BUFFER` e `CONFIG_IIO_HRTIMER_TRIGGER`. Ver
> `implementation_plan_iio_afe.md` §8. O raciocínio acima — "política que a plataforma assume e
> nunca escreveu" — continua valendo para os símbolos novos.

**Não entra** o que a revisão anterior pedia:

- `CONFIG_SPI=y` / `CONFIG_SPI_MASTER=y` — **nenhuma das duas ligações usa SPI do lado do Linux**.
  Na A o barramento pertence ao M33; na B o mestre SPI é o MCP2210, do outro lado do USB. Pedir SPI
  na política de distro seria pedir uma capacidade que nada consome, e no `qemux86-64` hoje
  `# CONFIG_SPI is not set` — o custo seria só ruído. Se um dia a variante Linux/IIO da ligação A
  for feita (§11.3), este item volta, com o driver junto.
- Driver de ADS1299 no kernel: não existe, e não é para existir aqui.

### 7.9 `meta-med-bsp` — o que a camada ganha

**Ligação A**:
- *overlay* de devicetree: `&spi6` para `okay`, `cs-gpios` do pino 24, nó filho do AFE, `DRDY` como
  interrupção;
- atribuição RIF de SPI6 (`rifsc 27`) e do GPIO do `DRDY` ao CID do M33 — no devicetree do firmware
  seguro, ver §11.4;
- receita do firmware do M33, empacotando em `/lib/firmware`, e
  `MED_AMP_FIRMWARE = "<receita>"` em `kas/project-eeg-stm32mp2.yml`, onde hoje há um comentário.

**Ligação B**:
- receita `med-afe-bridge` (§6.3), instalada por `MED_BSP_INSTALL`;
- **regra udev por VID/PID**, criando um symlink estável. Não é conveniência: `BRINGUP_STM32MP2.md`
  §11 já registra que "um nome de dispositivo é sempre uma corrida, nunca um fato", e `/dev/hidrawN`
  é numerado por ordem de enumeração. A ponte abre o symlink, nunca `hidrawN`;
- unidade systemd da ponte, com `DeviceAllow=` do symlink e o mesmo *sandboxing* do serviço de
  aquisição (`PrivateNetwork=yes`, `RestrictAddressFamilies=AF_UNIX`, sem escrita fora do runtime);
- as variáveis que respondem por máquina, na `conf/layer.conf`, no mesmo formato dos pares
  `MED_KEY_STORE_*`.

**Ordenação entre a ponte e o serviço**: nenhuma referência cruzada de unidade. A unidade da ponte
**não** deve nomear `eeg-acquisition.service` (seria consumir para cima), e o `eeg.conf` não deve
ganhar um `Requires=`. Em vez disso, `start()` do driver passa a **tentar reconectar por uma janela
limitada** antes de falhar. Isso resolve os dois casos com um mecanismo só — na ligação A o M33
também sobe depois do Linux — e não cria acoplamento entre camadas. **Consequência para o
`make check`**: o serviço tem `Restart=on-failure` (`eeg-acquisition.service`), e a suíte já
aprendeu, por injeção de falha, que `is-active` passa em serviço em *crash loop*; a asserção que
consulta `NRestarts` tem de continuar consultando, e a janela de reconexão existe justamente para
que o número certo seja zero.

### 7.10 O que não muda

`eeg-acquisition-service/files/src/main.cpp`, todo o `eeg-hmi-gui`, e as duas receitas de aplicação.
Se qualquer um dos três precisar mudar por causa desta revisão, a mudança está no lugar errado — é
o teste, e é o mesmo teste do §3.

---

## 8. Vazão da ligação B — a conta, e o que ela ainda não é

USB *full-speed*, endpoint de interrupção: **64 bytes por quadro de 1 ms por sentido**, ou seja
≈64 kB/s. Uma transferência SPI pelo MCP2210 é um relatório OUT de 64 bytes (até 60 de dados) e um
relatório IN de 64 (até 60 lidos). O ADS1299 entrega 27 bytes por `DRDY` e **não tem FIFO**, então é
uma transação por amostra.

| ODR | Transações/s | Quadros USB/s necessários (2–4 relatórios por transação) | dos 1000 disponíveis |
|---|---|---|---|
| 250 SPS | 250 | 500–1000 | **no limite, viável** |
| 500 SPS | 500 | 1000–2000 | **acima do barramento** |
| ≥1 kSPS | ≥1000 | ≥2000 | impossível |

Some-se o *polling* do contador do GP6, que consome relatórios adicionais.

**Conclusão de projeto**: a ligação B opera a **250 SPS**, e isso é uma limitação declarada do
caminho, não um defeito. Para EEG clínico 250 SPS é uma taxa legítima (banda de interesse até
~100 Hz); o que se perde é a margem, não a utilidade.

**E é uma conta, não uma medição.** Os multiplicadores de 2–4 relatórios por transação vêm do
protocolo do MCP2210 e precisam ser conferidos em DS20005176 e depois **medidos**: latência,
jitter e perda de amostras com o contador do GP6 como referência. Enquanto não forem medidos, esta
tabela é uma estimativa, e a `RESULTS.md` deve dizer isso — a §9 dela existe exatamente para que uma
ausência não seja lida como resultado.

---

## 9. Ordem de execução

A mudança de sequência em relação à revisão anterior é a consequência prática de haver duas
ligações: **a ligação B não depende da ligação A**, e portanto não depende do firmware assinado.
Encaixando no `PLANO_TCC.md`:

1. **Framework, testável no host, sem hardware nenhum** — escala em nanovolts (§7.1), mensagem de
   controle (§7.4), `driverOptions`, refactor `FrameStreamDevice` + driver `socket` (§6.2),
   `simulated` enriquecido (§7.7), `safetyLimits()` (§7.6), `eeg.conf` (§7.5). Tudo isso compila e
   é testado com o teste funcional do host, e nada disso espera placa.
2. ~~**`CONFIG_HIDRAW=y`** (§7.8)~~ → **superado**: os símbolos do IIO no lugar, e a camada
   adjunta vazia construindo dois módulos triviais, que é o que verifica o *tooling* antes de
   existir driver. Ver `implementation_plan_iio_afe.md` §12.
3. ~~**`med-afe-bridge`**~~ → **superado**: no lugar dele, o driver `mcp2210` registrando um
   `spi_controller`, exercitado sem o ADS1299 (enumerar, conferir VID/PID, acender um GP, ler o
   contador do GP6). A verificação é a mesma; o que muda é onde o código roda.
4. **Ligação B completa** — ADS1299 no MCP2210, sinal de teste interno, os oito canais, e a medição
   do §8. **Aqui já existe aquisição real**, e ela chega ao serviço e à HMI sem uma linha de
   aplicação mudada. Este é o resultado que a Fase 4 do `PLANO_TCC.md` cobra, e ele fica disponível
   **sem** depender da Fase 3.
5. **Fase 3 em paralelo**: firmware mínimo no M33 com sinal sintético, e o enfrentamento do
   `Support of signed firmware only`. O `FrameHeader` que ele emite já terá sido exercitado pela
   ponte, o que reduz o firmware ao que é genuinamente novo.
6. **Ligação A completa** — SPI6 e `DRDY` no M33, RIF, *overlay*, mapa de registradores no firmware.
   O critério de pronto é o `diff`: as mesmas aplicações, o mesmo `eeg.conf` exceto por três valores
   substituídos.

Ordem invertida em relação ao plano antigo, e o motivo é de risco: o passo mais caro (firmware
assinado) deixa de estar no caminho crítico do resultado que a tese precisa mostrar.

---

## 10. Plano de verificação

| Nível | Verificação |
|---|---|
| Host | Compilação limpa das unidades do MedFramework sob `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` |
| Host | Teste funcional estendido: quantização de 22,35 nV, saturação em ±187,5 mV, rejeição de ODR inválida, rejeição de ganho inválido, `max_input_uv > FS` recusado |
| Host | `static_assert` do quadro **na ponte** (redeclaração independente) e teste de ida e volta ponte→driver `socket` com quadro sintético, incluindo CRC deliberadamente errado |
| Build | `make qemu` e `make stm32`; conferir o `.sha256` do `eeg.conf` no rootfs e `CONFIG_HIDRAW` no `.config` produzido — no artefato, não no log |
| QEMU | `make check` continua verde (23 asserções); a asserção de reinícios continua consultando `NRestarts` (§7.9) |
| QEMU | Serviço publica quadros com CRC válido; HMI reconstrói a amplitude a partir de `scaleNanoUnitsPerLsb = 1` |
| Bancada B | VID/PID conferidos; nó estável por udev; onda de teste de 1 mV nos 8 canais; ruído com entradas em curto; eletrodo solto até a HMI; **perda de amostras medida** contra o contador do GP6; latência e jitter medidos |
| Bancada A | Frequência de `DRDY` medida = ODR programado; mesmos quatro testes do `selfTest`; latência e jitter medidos e **comparados com a ligação B** — é a comparação que justifica a arquitetura AMP com número, não com adjetivo |
| Injeção de falha | Desconectar o USB durante a aquisição: o serviço tem de registrar e falhar, nunca continuar publicando quadros antigos. Uma asserção que nunca viu a falha que procura é uma alegação, não uma verificação |
| Tese | `grep -ri 'ads1299\|mcp2210'` não retorna nada fora de `meta-med-bsp` e do firmware; `diff` do código de aplicação entre as ligações permanece **vazio** |

---

## 11. Riscos e questões em aberto

### 11.1 `Support of signed firmware only` — o bloqueio da ligação A

Medido no primeiro boot (`BRINGUP_STM32MP2.md` §9.7):

```
remoteproc remoteproc1: m33 is available
stm32-rproc 0.m33: Support of signed firmware only
stm32-rproc 0.m33: mbox_request_channel_byname() could not locate ...
```

O firmware do M33 terá de ser assinado, o que acrescenta uma cadeia de chaves ao plano — e este
repositório já tem uma (`pki/`, para os *bundles* RAUC), o que é precedente mas não é solução: são
chaves de propósitos diferentes e a da ST tem ferramenta e formato próprios. **Não avaliado**: se o
bloqueio é configurável na TF-A/OP-TEE desta variante, ou se exige provisionamento de chave no SoC.
Os avisos de *mailbox* sugerem, além disso, que o `detach` não está descrito no devicetree desta
variante; consequência não avaliada.

A existência da ligação B é o que impede esse risco de ser fatal para o cronograma (§9).

### 11.2 A vazão da ligação B é conta, não medida

§8. Se os multiplicadores reais forem piores que 4 relatórios por transação, 250 SPS também sai do
alcance e a ligação B vira ferramenta de demonstração, não de aquisição. É a primeira coisa a medir
no passo 3 do §9, antes de o ADS1299 entrar.

### 11.3 A variante "Linux fala SPI direto" continua fechada

Se algum dia se quiser o AFE no lado Linux pelo *hat*, o custo é código fora da árvore: `spidev`
recusa `compatible = "spidev"` (`drivers/spi/spidev.c:727`) e não existe driver IIO do ADS1299. Há o
truque conhecido de declarar um `compatible` alheio que está na tabela do `spidev` — funciona, e
**não deve entrar em imagem nenhuma**: é uma mentira no devicetree, e a regra do
`BRINGUP_STM32MP2.md` §11 sobre "um nome genérico sobre conteúdo específico é o disfarce padrão"
cobre exatamente esse tipo de coisa. Como experimento de bancada, com o nó fora da imagem, é
legítimo e às vezes é o caminho mais rápido para saber se a placa está boa.

O mesmo raciocínio aplicado à ligação B — um driver de kernel para o MCP2210, com o AFE virando
dispositivo SPI de verdade — está avaliado e recusado em **§6.5**, com as duas condições que
reabririam a decisão.

### 11.4 Onde se escreve a atribuição RIF

`spi6` declara `access-controllers = <&rifsc 27>` no `stm32mp251.dtsi`, mas o `st,protreg` que
atribui periféricos a CIDs, com o A35 como TDCID, não está nos `fdts` da TF-A que este build usa
(procurado; só há `RISAFPROT` de memória no `stm32mp257f-dk-ca35tdcid-fw-config.dtsi`). Ele vive na
árvore do OP-TEE, cujo fonte a *build* já removeu. **Item de bancada, e é pré-requisito de código**:
descobrir o arquivo e a sintaxe antes de escrever o *overlay*, exatamente como o
`implementation_plan_uboot_ab.md` §7.1 exigiu saber quais variáveis dão o dispositivo MMC antes de
escrever o `bootcmd`.

### 11.5 A camada da ponte é uma decisão de arquitetura, não só de arquivo

§6.4. A recomendação é `meta-med-bsp`; a objeção (o MCP2210 é fato de produto, não de placa) é
razoável e a alternativa seria uma camada de *front-end*. **Decidir antes de escrever a receita**, e
registrar a decisão aqui — porque se essa fronteira for atravessada sem se perceber, será a terceira
vez neste repositório, e as duas anteriores só apareceram quando uma segunda máquina precisou de
outra resposta.

### 11.6 Oportunidade, não requisito: o MCP2210 no QEMU

Um front-end USB é alcançável de qualquer host Linux. Com *passthrough* de USB, o perfil `qemux86-64`
poderia adquirir sinal real trocando `MED_EEG_DRIVER` de `simulated` para `socket` — a mesma imagem,
as mesmas aplicações, um valor de configuração. Isso fortaleceria a métrica de portabilidade de
"duas máquinas" para "duas máquinas e três topologias físicas". **Não é requisito deste plano** e
não deve consumir tempo antes do §9 passo 4; está aqui para não se perder.

### 11.7 Carimbo de tempo, de novo

O `BRINGUP_STM32MP2.md` §9.7 registra que a placa não tem RTC inicializado e que todo registro sai
com data errada. A ligação B acrescenta a esse problema um erro de *fase* (§2.3). Os dois são
independentes e os dois são de rastreabilidade IEC 62304. Nada no repositório trata de nenhum dos
dois hoje; o campo `acquisition.link` do §7.5 é o mínimo — declarar a incerteza no registro — e não
substitui resolver.

### 11.8 Riscos herdados que continuam válidos

Valores de datasheet (SBAS499, DS20005176) a conferir antes do texto final, com atenção especial ao
ID do dispositivo, à tolerância do sinal de teste, às figuras de ruído e ao tamanho máximo de
payload por comando do MCP2210. E a mensagem de controle (§7.4) é extensão de ABI: layout fixo e
`static_assert`, como já foi feito para o `FrameHeader`.

---

## 12. O que esta revisão tornou obsoleto

Registrado para quem leu a versão anterior:

1. **A decisão (a)/(b) de camada de BSP** (antigo §4.8) está tomada e implementada: `meta-med-bsp`
   existe. O antigo item 4 dos riscos sai.
2. **`CONFIG_SPI=y` na política de distro** era desnecessário e agora está justificado como tal
   (§7.8). No lugar dele entra `CONFIG_HIDRAW=y`, que é o item que realmente faltava.
3. **`device.model = ads1299` no `eeg.conf`** era uma violação do próprio critério de aceitação do
   plano — um *part-number* em `meta-med-app`. Retirado (§7.5).
4. **"Cortex-M4"** em todo o documento: neste SoC é um Cortex-M33, e ele exige firmware assinado.
   O `CLAUDE.md`, o `PROJECT_CONTEXT.md` e o `CONTRIBUTION.md` ainda dizem M4 em vários pontos —
   correção pendente, fora do escopo desta edição, mas anotada.
5. **A ordem de execução** foi invertida: a ligação B (USB) vem antes da A (AMP), porque ela produz
   amostras reais sem depender do bloqueio de firmware assinado (§9).
6. **"O ADS1299 fica no Cortex-M4"** deixou de ser a única topologia e passou a ser a topologia de
   *produto*, com a segunda declarada explicitamente como não equivalente (§2.3) em vez de
   silenciosamente tolerada.
