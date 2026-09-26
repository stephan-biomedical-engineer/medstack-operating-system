# Plano de implementação — o firmware do Cortex-M33 (ligação `amp`)

> **Status**: **nada aqui foi implementado.** Não existe firmware, não existe receita, e
> `MED_AMP_FIRMWARE` continua comentado no arquivo KAS do alvo. O que existe, e é o que torna este
> plano escrevível hoje, é o diagnóstico da §2 — feito por inspeção dos artefatos que o build já
> produziu, sem energizar a placa.
>
> **Escopo**: da decisão de devicetree até quadros sintéticos medidos, atravessando a ligação `amp`
> fim a fim com a aplicação que já existe. **Não inclui o ADS1299**: o firmware desta fase gera
> sinal sintético no coprocessador, exatamente como o `PLANO_TCC.md` §4 (Fase 3) especifica, *antes*
> de qualquer conversor físico entrar na conta. O conversor entra na Fase 4 do TCC, com o
> `implementation_plan_afe_bench.md`.
>
> **Por que isso importa mais que uma ligação a mais**: `amp` é a topologia de produto. É a única em
> que "o Linux nunca toca no conversor" é literalmente verdade, e é o que sustenta o argumento de
> particionamento por processador da IEC 62304 §5.3. As outras três ligações produzem amostras; esta
> produz o argumento.
>
> **Leitura obrigatória antes**: `BRINGUP_STM32MP2.md` §9.7 (o primeiro sintoma), §11 (as regras),
> `implementation_plan_ads1299.md` §2.1 e §11.1 (a ligação A e seu bloqueio), e o §6 deste
> documento, que é o contrato de fio e não admite improviso.

---

## 1. Objetivo

Fazer a **mesma aplicação, sem recompilar**, consumir quadros produzidos no Cortex-M33, trocando um
valor de configuração. Concretamente, ao fim deste plano:

- o firmware é artefato de build deste repositório, não um binário depositado em `/lib/firmware`;
- ele sobe sem intervenção manual em um cartão recém-gravado;
- ele anuncia um canal rpmsg que o `MedicalIPC` abre sem adivinhar índice de enumeração;
- ele recebe a prescrição do front-end e **responde**, aceitando ou recusando opção por opção;
- ele emite `FrameHeader` + payload com carimbo de tempo tomado no próprio M33;
- e existem números — latência, jitter, perda de amostra, granularidade do carimbo — onde hoje a
  `RESULTS.md` §9 registra ausência.

### O que este plano **não** faz

- **Não** toca no conversor. Nenhum registrador do ADS1299, nenhuma constante `[SBAS499?]`. O sinal
  é gerado no M33.
- **Não** resolve a política de reinício do serviço de aquisição nem a prontidão declarada ao
  systemd. São o assunto do plano de *startup* e controle, que ainda não existe; a §7.5 aqui
  **depende** dele e diz onde.
- **Não** decide a atribuição RIF do SPI6 ao M33 (`implementation_plan_ads1299.md` §11.4). Isso é
  pré-requisito do conversor, não do firmware sintético.
- **Não** toca no QEMU, que não tem coprocessador.

---

## 2. A descoberta: `Support of signed firmware only` é uma escolha nossa

O primeiro boot (2026-08-18) registrou:

```
remoteproc remoteproc1: m33 is available
stm32-rproc 0.m33: Support of signed firmware only
```

e o `implementation_plan_ads1299.md` §11.1 tratou isso como restrição do silício — *"o firmware do
M33 terá de ser assinado"*. **Está errado, e a correção muda o tamanho da fase.**

O driver escolhe entre dois modos pelo `compatible` do nó, e só por ele
(`drivers/remoteproc/stm32_rproc.c:1252-1258`):

| `compatible` | ops | `fw_format` | firmware |
|---|---|---|---|
| `st,stm32mp2-m33` | `st_rproc_ops` | `RPROC_FW_ELF` | **ELF comum** |
| `st,stm32mp2-m33-tee` | `st_rproc_tee_ops` | `RPROC_FW_TEE` | **assinado, carregado pelo OP-TEE** |

Ambos estão na tabela de *match* (`:1118-1124`). E os DTBs que este build produz se dividem
exatamente nisso — lido dos binários em `build/tmp-glibc/deploy/images/stm32mp25-disco/kernel/`:

```
stm32mp257f-dk.dtb                             st,stm32mp2-m33        ← ELF
stm32mp257f-dk-ca35tdcid-ostl.dtb              st,stm32mp2-m33-tee    ← assinado
stm32mp257f-dk-ca35tdcid-ostl-m33-examples.dtb st,stm32mp2-m33-tee    ← assinado
```

O nó nasce `st,stm32mp2-m33` no `stm32mp251.dtsi:3407`; quem o promove a `-tee` é a configuração
OSTL da ST. E o nosso `.wks` grava o FIP `fip-stm32mp257f-dk-ca35tdcid-ostl-optee-sdcard.bin`
(`med-partitions-stm32mp2.wks.in:91`), de cuja família **todo** DTB traz o `-tee`.

Três fatos independentes concordam: a variante que gravamos, o `compatible` que ela carrega e a
mensagem que a placa imprimiu. **A mensagem é consequência da variante escolhida, não uma trava de
fábrica.**

### O que isso **não** prova

Que a Estrada B funciona. Os resets do M33 chegam por SCMI
(`resets = <&scmi_reset RST_SCMI_C2_R>, <&scmi_reset RST_SCMI_C2_HOLDBOOT_R>`), e se o mundo seguro
considerar o coprocessador seu, ele pode recusar o *hold boot* independentemente do que o
devicetree do Linux diga. **É hipótese, e a Fase 0 existe só para testá-la.** Nível de evidência
atual: leitura de fonte e inspeção de artefato; nenhuma observação em placa.

---

## 3. As duas estradas

### 3.1 Estrada A — TEE, firmware assinado

Mantém o DT OSTL. O firmware é construído no formato TF-M (`tfm_s` + `_ns`), assinado com
`SIGN_ENABLE = "1"` e as chaves de `SIGN_M33FW_KEY_PATH_LIST`, e carregado pelo OP-TEE através da
TA de remoteproc.

**A favor**: é a topologia que a ST suporta nesta placa; firmware de coprocessador autenticado é
argumento *a favor* da tese, não contra; e não mexe em nada que já boota.
**Contra**: acrescenta uma cadeia de chaves distinta da do RAUC — mesmo precedente (`pki/`), outra
ferramenta e outro formato; e arrasta TF-M inteiro para produzir o que, nesta fase, é um gerador de
senóide.

### 3.2 Estrada B — ELF não assinado

Faz o nó do M33 voltar a `st,stm32mp2-m33`. O fluxo passa a ser o clássico: ELF em `/lib/firmware`,
`remoteproc` carrega, sem assinatura e sem TF-M.

**A favor**: destrava a fase imediatamente e sem ferramenta nova.
**Contra, e é sério**: **não se troca o DT inteiro.** O `ca35tdcid-ostl` difere do `stm32mp257f-dk`
em muito mais que o nó do M33, e trocá-lo por atacado é mudar a configuração que hoje boota — pelo
mesmo raciocínio que fez `med-partitions.wks` ser um defeito: um nome genérico sobre conteúdo
específico. A forma correta é uma **sobreposição mínima**, alterando apenas o `compatible` daquele
nó, em `meta-med-bsp`, keyed por máquina. E isso herda a lacuna que o *overlay* do hat já tem: não
existe `fdt apply` no `bootcmd` (`med-bootcmd.env:145` carrega um `${fdtfile}` e nada mais), então
ou a sobreposição é aplicada no build sobre o `.dtb`, ou o `bootcmd` ganha um passo.

### 3.3 Qual escolher

**Não decidir no papel.** A Fase 0 decide com uma sessão de bancada, e a decisão tem de ser
registrada aqui com a evidência que a produziu. A preferência, se as duas forem viáveis, é a
**Estrada A** — porque um dispositivo médico que autentica o firmware do seu coprocessador é o que
se quer defender, e porque a Estrada B deixa uma dívida de devicetree que alguém pagará depois.

---

## 4. O que a `meta-st-stm32mp` já entrega

Verificado nas camadas que este projeto já clona. Nada disto precisa ser escrito:

| Peça | Onde | Serve para |
|---|---|---|
| `gcc-arm-none-eabi-native` | `recipes-devtools/gcc-arm-none-eabi/` | toolchain *bare-metal*, **sem camada nova** |
| `m33projects-stm32mp2.bb` | `recipes-extended/stm32mp2-projects/` | padrão de receita que compila firmware do M33 |
| `m33fw-stm32mp2.bb` + `m33fw-utils-stm32mp.bbclass` | idem / `classes/` | empacotamento e o envoltório de assinatura (`create_st_m33fw_binary.sh`) |
| `st-m33firmware-load.service` | `recipes-extended/stm32mp2-projects/files/` | unidade `oneshot` que carrega e para o firmware |
| `fw_cortex_m33.sh` | idem | **resolve o remoteproc pelo `name`, não pelo índice** — varre `/sys/class/remoteproc/*/name` procurando `m33` |
| `st,auto-boot` | `stm32_rproc.c:1164` | o kernel sobe o coprocessador no *probe*, o que pode dispensar unidade nenhuma |
| `MACHINE_FEATURES += "m33copro"` | `conf/machine/stm32mp25-disco.conf:56` | já presente: as receitas acima são construíveis para o nosso alvo |

O script da ST merece nota: ele faz, por conta própria, o que a regra 9 do `BRINGUP_STM32MP2.md`
§11 exige — endereçar por identidade e não por ordem de enumeração. É o padrão a imitar, não a
reescrever. (Ressalva menor: ele usa `==` dentro de `#!/bin/sh`; funciona no `ash` do busybox e
quebraria em `dash`. Se for reaproveitado, corrigir.)

---

## 5. Onde o firmware mora — e a inversão que ele esconde

`packagegroup-med-amp.bb:10` diz que *"um BSP preenche `MED_AMP_FIRMWARE`"*. Isso está certo sobre o
**gancho** e errado sobre a **camada**, por duas razões independentes:

1. **Ele vai carregar um mapa de registradores.** Na Fase 4 do TCC este mesmo firmware ganha o
   ADS1299. Isso é fato de *peça*, e a regra 9 do `CLAUDE.md` admite exatamente dois lugares para um
   *part-number*: a camada adjunta e o firmware do M33. Nascer em `meta-med-bsp` e mudar depois é o
   padrão que este repositório já pagou duas vezes.
2. **Ele precisa do ABI do framework.** Uma receita em `meta-med-bsp` (prioridade 7) com `DEPENDS`
   no `med-framework-api` (prioridade 9) **inverte a cadeia de dependências**, que é a primeira
   regra do projeto.

**Decisão proposta**: o firmware nasce na **camada adjunta**, ao lado dos dois drivers de kernel —
`meta-med-afe-ads1299/recipes-firmware/med-m33-firmware/`. Ela já é priorizada como adjunta, já
declara `LAYERDEPENDS = "core"`, já está excluída da métrica de reuso, e já é o lugar onde um
*part-number* pode aparecer. `MED_AMP_FIRMWARE` continua sendo o gancho e é valorado no arquivo KAS
do alvo, de modo que nenhuma camada acima nomeia máquina.

### 5.1 O ABI tem de sair do `MedicalDevice.h`

`amp::FrameHeader` e `amp::ControlMessage` vivem hoje num cabeçalho C++17 que inclui `<map>`,
`<string>` e `<memory>`. **Um M33 *bare-metal* não consegue incluí-lo.** As saídas são duas, e só
uma é aceitável:

- ❌ **Duplicar as structs no firmware.** Duas fontes de verdade para um formato de fio, com a
  garantia de que um dia divergem em silêncio. É literalmente *"dois programas com o mesmo nome não
  são o mesmo programa"* (`BRINGUP_STM32MP2.md` §11).
- ✅ **Extrair um cabeçalho *freestanding*, compatível com C**, com as structs empacotadas, os
  magics, as versões, os tamanhos e os `static_assert`, sem uma única inclusão da libstdc++.
  `MedicalDevice.h` passa a incluí-lo; o firmware também. Uma fonte, dois compiladores.

O pacote resultante (`med-amp-abi`, só cabeçalhos, zero dependências) fica abaixo dos dois
consumidores e não inverte nada. A Fase 2 faz isso, **sem hardware**, e pode começar hoje.

---

## 6. O contrato do firmware

Cada item abaixo é uma condição que o driver `RpmsgDevice` já impõe, com o ponto exato onde ele
aborta. Não há margem de interpretação: é ABI.

### 6.1 O canal, e a decisão que precede o código

`MedicalIpcChannel::connect` para `RpmsgChar` é um `open()` puro (`MedicalIPC.cpp:90`) — **não há
`ioctl`**. E no kernel real da placa, `rpmsg_chrdev` só casa com canal cujo nome seja
**`rpmsg-raw`** (`drivers/rpmsg/rpmsg_char.c:523-526`); qualquer outro nome exige
`RPMSG_CREATE_EPT_IOCTL` sobre `/dev/rpmsg_ctrl0` (`rpmsg_ctrl.c:94`). O nó criado chama-se
`rpmsg%d` por índice de `ida` (`rpmsg_char.c:453`), o que é ordem de *probe* e portanto uma corrida.

**Duas saídas, e uma tem de ser escolhida antes de escrever o firmware:**

| Saída | Custo | Consequência |
|---|---|---|
| Firmware anuncia `rpmsg-raw` | zero no Linux | `/dev/rpmsgN` aparece sozinho; resta o índice, que uma regra de udev resolve |
| `MedicalIPC` ganha o caminho de `ioctl` | código de transporte no framework (legítimo ali) | independe do nome escolhido pelo firmware, e é o que qualquer outro produtor futuro vai querer |

Recomendação: **anunciar `rpmsg-raw` agora** (destrava a Fase 3 sem tocar no framework) e abrir o
`ioctl` como trabalho seguinte, porque a primeira saída deixa o nome do canal como acoplamento
implícito entre dois repositórios.

Em qualquer caso, `MED_EEG_ADDRESS = "/dev/rpmsg0"` no arquivo KAS é um índice de enumeração e
**não deve sobreviver a esta fase**.

### 6.2 A prescrição, e a obrigação de responder

Ao `start()`, o driver envia uma `ControlMessage` e **exige `ControlAck` em 1 s**
(`kControlAckTimeout`), senão derruba o canal e falha. O firmware tem de:

| Campo | Valor |
|---|---|
| `magic` | `0x4D435452` (`"MCTR"`) na mensagem, `0x4D435441` (`"MCTA"`) na resposta |
| `version` | `1` |
| `optionCount` | ≤ 8 |
| `crc32` | sobre as `optionCount` primeiras opções apenas |
| `ControlOption` | `key[32]` + `value[24]`, ambos terminados em NUL, **56 bytes** |
| `ControlMessage` | **464 bytes** |
| `ControlAck.rejectedIndex` | `0` = tudo aceito; senão `1 + índice` da primeira recusada |
| `ControlAck.detail[64]` | motivo legível, terminado em NUL |

As chaves que chegam na ligação `amp` são a prescrição `afe.*` repassada literalmente
(`do_derive_device_options`, ramo `link == 'amp'`). **Uma opção não entendida é uma recusa**, nunca
um descarte silencioso — a alternativa é um aparelho adquirindo num ganho que ninguém prescreveu.

### 6.3 O quadro

`FrameHeader` são **40 bytes**, empacotados, nesta ordem:

| Campo | Tipo | Observação |
|---|---|---|
| `magic` | `u32` | `0x4D454547` (`"MEEG"`) |
| `version` | `u16` | `1` |
| `channelCount` | `u16` | |
| `samplesPerChannel` | `u32` | |
| `sampleRateMilliHz` | `u32` | |
| `sequence` | `u64` | monotônico; lacuna é relatada pelo driver |
| `timestampMicros` | `u64` | §6.4 |
| `scaleNanoUnitsPerLsb` | `i32` | **entregar nanovolts e declarar `1`** |
| `crc32` | `u32` | **sobre o payload apenas** |

O CRC é o CRC-32 IEEE 802.3 refletido, polinômio `0xEDB88320`, início `0xFFFFFFFF`, XOR final —
`MedicalDevice.cpp:30-54`. Um quadro com CRC divergente é descartado, não transformado em onda.

Sobre a escala, e o comentário no cabeçalho merece ser lido inteiro: entregar contagens brutas com
passo arredondado introduz erro de ganho invisível (22,35 nV virando 22 são 1,6% em **toda** amostra
de **todo** traçado, com a onda continuando plausível). Uma multiplicação no produtor elimina isso e
um `int32` de nanovolts cobre ±2,1 V.

**Geometria**: 8 canais × 14 amostras × 4 B = 448 B de payload, mais 40 de cabeçalho = **488 B**,
dentro dos ~496 úteis de um buffer rpmsg de 512. A 250 SPS isso são **17,857 quadros/s**.

### 6.4 O carimbo de tempo é do M33, e é a razão da ligação existir

`timestampMicros` tem de ser tomado **no coprocessador, no instante da conversão** — na ISR de
`DRDY` quando houver conversor, e no disparo do timer enquanto o sinal for sintético. Um carimbo
tomado no lado Linux destruiria a única propriedade que distingue `amp` de `usb`, e não quebraria
nada: o quadro continuaria válido, o CRC fecharia, a onda apareceria na tela. É exatamente a classe
de defeito que este repositório chama de *"medir estado não é medir função"*, e por isso vira
critério explícito na Fase 5.

Nota herdada do primeiro boot: **a placa não tem RTC inicializado**. O carimbo do M33 é relativo ao
seu próprio tempo de subida; a conversão para tempo absoluto é problema do lado Linux e continua
sem tratamento (`RESULTS.md` §9). Não confundir os dois: este plano entrega *fase*, não *data*.

### 6.5 O mapa de memória que o firmware herda

Do `stm32mp257f-dk-resmem.dtsi`, e o firmware tem de respeitá-lo no seu *link script*:

| Região | Endereço | Tamanho |
|---|---|---|
| `tfm_code` | `0x80000000` | 1 MiB |
| `cm33_cube_fw` | `0x80100000` | 8 MiB |
| `tfm_data` | `0x80900000` | 1 MiB |
| `cm33_cube_data` | `0x80a00000` | 8 MiB |
| `ipc_shmem_1` | `0x81200000` | 992 KiB |
| `vdev0vring0` | `0x812f8000` | 4 KiB |
| `vdev0vring1` | `0x812f9000` | 4 KiB |
| `vdev0buffer` | `0x812fa000` | 24 KiB |

`vdev0buffer` com 24 KiB comporta 48 buffers de 512 B — folga confortável sobre 17,9 quadros/s, e o
número a reconferir no dia em que a taxa subir para 16 kSPS.

---

## 7. Fases

Mesma estrutura do `implementation_plan_afe_bench.md` §1, e pelas mesmas razões: objetivo único,
critério numérico, injeção de falha, e o que uma reprovação derruba.

### Fase 0 — a decisão de devicetree

**Objetivo**: saber qual das duas estradas existe nesta placa.
**Pré-requisito**: nenhum além da placa e do console serial.
**Procedimento**:

```sh
# Na placa, com a imagem atual
cat /sys/class/remoteproc/*/name              # qual índice é o m33
cat /proc/device-tree/soc*/m33@0/compatible   # -tee ou não
ls /dev/rpmsg*                                # deve não existir nada
dmesg | grep -i 'rproc\|m33'
```

Depois, com um `.dtb` cujo nó do M33 seja `st,stm32mp2-m33` (sobreposição mínima, §3.2), repetir e
tentar o fluxo clássico com **qualquer** ELF — inclusive um deliberadamente inválido:

```sh
echo test.elf > /sys/class/remoteproc/remoteprocN/firmware
echo start    > /sys/class/remoteproc/remoteprocN/state
```

**Critérios**:
1. o `compatible` lido da placa confirma (ou refuta) a §2;
2. com o nó não-tee, `state` aceita `start` **ou** falha com erro nomeado no `dmesg`;
3. o erro, se houver, distingue "SCMI recusou o reset" de "ELF inválido".

**Injeção de falha**: o ELF deliberadamente inválido é a injeção. Se `start` "funcionar" com um
arquivo que não é firmware, o caminho não está vivo — está sendo ignorado.
**Uma reprovação significa**: o mundo seguro é dono do coprocessador, a Estrada B está fechada, e a
Estrada A deixa de ser preferência para ser obrigação.
**Registro**: `BRINGUP_STM32MP2.md` §9.12, e a decisão volta para a §3.3 deste documento.

### Fase 1 — o firmware de exemplo da ST, com zero linhas nossas

**Objetivo**: exercitar o mecanismo inteiro — memórias reservadas, mailboxes, carga, anúncio de
canal — antes de existir código nosso para culpar.
**Pré-requisito**: Fase 0.
**Procedimento**: construir `m33projects-stm32mp2` (ou `m33fw-stm32mp2`, conforme a estrada),
apontar `MED_AMP_FIRMWARE` para a receita no arquivo KAS, e deixar `packagegroup-med-amp` puxá-la.
Carregar pela unidade da ST ou por `st,auto-boot`.

**Critérios**:
1. o firmware carrega e `state` lê `running`;
2. `dmesg` traz a criação de um canal rpmsg, com o **nome** anunciado visível;
3. se o nome for `rpmsg-raw`, `/dev/rpmsgN` existe — e isso resolve empiricamente a §6.1.

**Injeção de falha**: parar o firmware (`echo stop > state`) e confirmar que o nó desaparece.
**Uma reprovação significa**: o problema é de plataforma (memória, mailbox, DT) e não do nosso
firmware — que ainda não existe. É precisamente por isso que esta fase vem antes.
**Registro**: `BRINGUP_STM32MP2.md` §9.12.

### Fase 2 — o ABI fora do C++ (sem hardware nenhum)

**Objetivo**: uma fonte de verdade para o formato de fio, utilizável pelos dois compiladores.
**Pré-requisito**: nenhum. **Pode começar hoje.**
**Procedimento**: extrair as structs, magics, versões e tamanhos de `MedicalDevice.h` para um
cabeçalho *freestanding* compatível com C; `MedicalDevice.h` passa a incluí-lo; acrescentar ao
`tests/framework` um teste que verifica tamanhos e **deslocamento de cada campo**.

**Critérios**:
1. `sizeof` de `FrameHeader`, `ControlOption`, `ControlMessage` e `ControlAck` = 40, 56, 464, 72;
2. `offsetof` de cada campo conferido individualmente — tamanho igual não é layout igual;
3. o cabeçalho compila com `-std=c99 -ffreestanding` sem nenhuma inclusão da libstdc++;
4. `make test` continua passando, agora com os novos casos;
5. um vetor conhecido de CRC-32 confere entre as duas implementações.

**Injeção de falha**: trocar a ordem de dois campos no espelho e confirmar que o teste **falha
nomeando o campo**. Sem isso, o teste é alegação.
**Uma reprovação significa**: o empacotamento difere entre os toolchains, e o formato de fio precisa
de alinhamento explícito antes de qualquer firmware.
**Registro**: `RESULTS.md`, junto das verificações de host.

### Fase 3 — o nosso firmware mínimo

**Objetivo**: quadros sintéticos válidos, produzidos por código nosso, consumidos pela aplicação que
já existe.
**Pré-requisito**: Fases 0, 1 e 2.
**Procedimento**: receita em `meta-med-afe-ads1299/recipes-firmware/`, `DEPENDS` em
`gcc-arm-none-eabi-native` e `med-amp-abi`. O firmware anuncia o canal, responde `ControlAck`,
e emite a 250 SPS.

**Critérios**:
1. `eeg-acquisition.service` chega a `AcquisitionStarted` com `NRestarts=0`;
2. o registro em `/data` tem tamanho múltiplo exato de 488 B;
3. `metadata.json` traz `"link": "amp"` e `"driver": "rpmsg"`;
4. em 120 s, **nenhuma** lacuna de sequência e nenhum descarte por CRC;
5. a taxa entregue é 250 ± 1 amostra/s/canal — o mesmo critério que `acq-sample-rate` aplica no
   QEMU, agora contra silício.

**Injeções de falha** (três, e todas obrigatórias):
- firmware responde `rejectedIndex != 0` → o serviço **recusa adquirir** e escreve o motivo na
  trilha de auditoria;
- firmware emite um quadro com CRC errado → descartado, e contado;
- firmware pula um número de sequência → lacuna relatada.

**Uma reprovação significa**: o contrato da §6 está mal implementado de um dos dois lados, e o
critério 4 diz qual.
**Registro**: `RESULTS.md` §3 ganha a coluna `amp`; `BRINGUP_STM32MP2.md` §9.13.

### Fase 4 — integração, e o fim dos nomes adivinhados

**Objetivo**: um cartão recém-gravado sobe adquirindo, sem comando manual.
**Pré-requisito**: Fase 3.
**Procedimento**: `MED_AMP_FIRMWARE` valorado no arquivo KAS; carga por `st,auto-boot` ou por
unidade; regra de udev (ou o caminho de `ioctl` da §6.1) eliminando `/dev/rpmsg0` do arquivo KAS;
ordenação revista.

**Critérios**:
1. cartão gravado do zero → aquisição ativa sem intervenção;
2. `MED_EEG_ADDRESS` não contém índice de enumeração;
3. três boots seguidos produzem o mesmo resultado, com enumerações diferentes se ocorrerem.

**Injeção de falha**: remover o arquivo de firmware da imagem. O serviço tem de falhar **alto** —
dentro da janela de 5 s de `kConnectWindow` — e a unidade tem de chegar a um estado visível. Hoje
**não chega**: sem `StartLimitBurst`/`StartLimitIntervalSec` a unidade nunca atinge `failed`
(163 reinícios medidos em 27/08, e note que a janela de 5 s mudou a aritmética daquela medição e
ainda não foi remedida). **Esta injeção depende do plano de *startup*, e é a dependência a
declarar.**
**Registro**: `RESULTS.md` §9 perde o item "caminho AMP não exercitado".

### Fase 5 — as quatro medidas que a Fase 3 do TCC deve

**Objetivo**: números, não adjetivos, e comparáveis entre ligações.
**Pré-requisito**: Fase 4.
**Medidas**: jitter entre amostras, perda de amostra sob carga, granularidade do carimbo de tempo,
custo de CPU — cada uma com o alvo ocioso e sob carga no A35.

**Critérios**: cada medida tem de vir com o comando que a produziu e com uma declaração do que ela
**não** significa, no padrão da `RESULTS.md` §1.
**Injeção de falha**: carregar o A35 (`stress-ng` ou a própria HMI) e confirmar que o jitter **não**
se move — que é a afirmação inteira da ligação `amp`. Se mover, a segregação por processador não
está entregando o que promete, e isso é um resultado tão publicável quanto o contrário.
**Registro**: `RESULTS.md` §3 e §9.

---

## 8. Ordem, e o que bloqueia o quê

```
Fase 2 (ABI, host)  ──────────────┐
                                  ├──► Fase 3 ──► Fase 4 ──► Fase 5
Fase 0 (DT) ──► Fase 1 (ST fw) ───┘                  ▲
                                                     │
                              plano de startup/controle (não escrito)
```

- **A Fase 2 não depende de nada** e paraleliza com tudo. É por onde começar hoje.
- **A Fase 0 cabe na mesma sessão de bancada** que a injeção de falha do fallback A/B
  (`implementation_plan_uboot_ab.md` §8, passo 6): as duas precisam só da placa, e sessões de
  bancada não paralelizam.
- **A Fase 4 fica incompleta** enquanto a política de reinício do serviço não existir. Ela pode ser
  executada; sua injeção de falha, não.

---

## 9. Riscos

| Risco | Impacto | Mitigação |
|---|---|---|
| SCMI recusa o reset com o nó não-tee | fecha a Estrada B | Fase 0 descobre em uma sessão, não em três semanas |
| Cadeia de assinatura da ST exige provisionamento de chave no SoC | encarece a Estrada A | investigar junto da Fase 0; se for provisionamento irreversível, **não** fazer — a placa é a única |
| Nome do canal rpmsg vira acoplamento implícito | defeito silencioso entre dois repositórios | §6.1: o `ioctl` no `MedicalIPC` é a saída definitiva |
| Empacotamento difere entre arm-none-eabi e aarch64 | quadro ilegível, CRC fechando sobre lixo | Fase 2, critério 2 (`offsetof`, não `sizeof`) |
| Firmware carimba o tempo no lado errado | destrói a tese da ligação, sem quebrar nada | Fase 5, injeção de carga |
| A fase consome as 3 semanas e o conversor chega no meio | duas frentes abertas | a Fase 4 do TCC tem portão próprio em 20/10 |

---

## 10. Consequências regulatórias

- **SBOM**: construir o firmware no Yocto o põe no SPDX que `med-os.conf` já gera. Um binário
  depositado em `/lib/firmware` ficaria fora, e a Seção 524B pede o contrário.
- **IEC 62304 §5.3**: o argumento é *item de software de classe distinta, no seu próprio
  processador*. Ele só se sustenta se o firmware for artefato de build deste projeto, com
  rastreabilidade a requisito — não um blob recebido pronto.
- **SOUP §8.1.2**: se a Estrada A vencer, TF-M e as ferramentas de assinatura da ST entram como SOUP
  a declarar. A Estrada B não acrescenta SOUP, e esse é o seu único argumento regulatório a favor.
- **Caminho de atualização**: o firmware mora no rootfs, logo viaja no *bundle* RAUC e troca junto
  com o slot. Isso é desejável e é preciso dizê-lo: firmware e aplicação que o consome são
  atualizados atomicamente, ao contrário do kernel, que mora na `med-boot` compartilhada.

---

## 11. Validação

*(Vazio de propósito. Cada fase preenche a sua linha com o número medido e a data, no padrão da
§8 do `implementation_plan_uboot_ab.md`. Enquanto uma linha estiver vazia, aquela fase é código
escrito e não resultado.)*

| Fase | Critério | Resultado | Data |
|---|---|---|---|
| 0 | estrada decidida com evidência | — | — |
| 1 | firmware da ST carrega e anuncia canal | — | — |
| 2 | `offsetof` de cada campo confere; injeção vista | — | — |
| 3 | 250 ± 1 SPS/canal, zero lacunas em 120 s | — | — |
| 4 | cartão novo adquire sem intervenção | — | — |
| 5 | jitter imóvel sob carga no A35 | — | — |
