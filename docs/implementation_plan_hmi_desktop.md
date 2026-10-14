# Plano de implementação — a estação de operador: HMI ao vivo, configuração do front-end e revisão

> **Status**: **nada aqui foi implementado.** A HMI de hoje são 173 linhas de QML com um `Canvas` e
> nenhum botão; um cartão recém-gravado não mostra imagem no HDMI sem três correções manuais; não
> existe socket de controle; e nenhum visualizador de registros está na imagem.
>
> **Escopo**: ligar um monitor HDMI, um mouse e um teclado na placa e ter um ambiente de trabalho —
> a aquisição ao vivo numa janela, as funções do front-end escolhidas pela tela, e o EDFbrowser ao
> lado para revisar as sessões fechadas.
>
> **Não é** um plano de tela de toque (trabalho futuro), nem de troca de link em runtime (`amp`/`usb`
> continuam sendo fato de build), nem substitui o `implementation_plan_eeg_control.md`: as Fases B e
> C daqui **são** as fases 2 a 6 de lá, mais o que a escolha de configuração acrescenta.
>
> **Leitura obrigatória antes**: `BRINGUP_HMI_STM32MP2.md` §3–§5 e §9 (os três consertos que nunca
> saíram do `chmod`/`vi`), `implementation_plan_eeg_control.md` §3, §5 e §6 (quem manda no
> front-end, e os não-verbos), e `do_derive_device_options` na receita do serviço, que é onde a
> tradução por link mora hoje.

---

## 1. Objetivo

Ao fim deste plano:

- um cartão recém-gravado, com HDMI e mouse, mostra o ambiente em um boot, sem console serial;
- a janela da HMI mostra os 8 canais em tempo real, com escala por canal, filtros de exibição,
  estado de lead-off por canal e a taxa **entregue** ao lado da configurada;
- o operador inicia, anota e encerra uma sessão pela tela;
- o operador escolhe ganho, taxa, gerador interno, montagem, lead-off e entrada (normal, curto,
  temperatura, alimentação) pela tela, **dentro do que a imagem daquele link sabe honrar**;
- cada escolha é uma sessão nova, com metadado próprio e registro de auditoria;
- o EDFbrowser, na própria placa, abre o BDF+ de qualquer sessão fechada — e nada além disso.

### O que este plano **não** faz

- **Não** deixa a HMI programar o conversor. Ela pede; o serviço decide. É a mesma linha do plano de
  controle e pela mesma razão: se ela cair, a HMI vira item de software crítico.
- **Não** muda nada no meio de um registro. Trocar o ganho é fechar uma sessão e abrir outra.
- **Não** põe o *bias drive* na tela. Continua prescrição de build — é o único parâmetro do bloco
  que injeta corrente no paciente (`implementation_plan_eeg_control.md` §6).
- **Não** faz do EDFbrowser um visualizador em tempo real. Ele tem um modo que acompanha um arquivo
  em escrita, mas o BDF+ daqui é derivado **depois** que a sessão fecha, de propósito; um exportador
  incremental é outra decisão e não está aqui.
- **Não** entrega tela de toque nem teclado virtual.

---

## 2. Decisões já tomadas

| Decisão | Motivo |
|---|---|
| **Qt Quick, não Node/web** | Uma interface web no dispositivo é um Chromium na imagem: horas de build, uma integração nova com a `galcore`, e uma terceira dependência numa aplicação que a tese afirma ter duas. QML já é declarativo, acelerado por GPU e com lógica em JavaScript; o ganho visual da web não paga isso |
| **Ambiente com janelas** | O weston da imagem já roda o `desktop-shell` (o plano DRM de 1030x633 medido em 2026-08-27 era a janela QML mais a decoração). Não é um componente novo; é configurá-lo |
| **EDFbrowser na imagem** | Revisão na própria estação. Qt6 ≥ 6.4.2 e só `widgets`/`network`/`printsupport`, todos no `qtbase` que a imagem já tem |
| **Mouse e teclado USB** | O EDFbrowser é Qt Widgets, todo em menus; sem ponteiro ele não é utilizável |
| **O link não muda em runtime** | Decisão anterior, mantida |

---

## 3. O estado de hoje, verificado

| Fato | Onde se vê | Consequência |
|---|---|---|
| Regra udev da `galcore`, `idle-time=0` e modo CEA **não estão no repositório** | não há `weston.ini` nem regra em `meta-custom`; só o `seatd` foi persistido | um cartão novo: weston morre em 6 s |
| `CONFIG_INPUT_EVDEV` vem dos defconfigs | presente nos dois `.config`, ausente de `med-kernel-features.cfg` (que só declara `USB_HID` e `HID_GENERIC`) | é a armadilha da regra 3: uma capacidade assumida e não escrita |
| A HMI roda como root | `eeg-hmi.service` não tem `User=` | a partição de identidade não existe ainda |
| A HMI usa `Core Gui Qml Quick` | `CMakeLists.txt` | `QtQuick.Controls` e `QtQuick.Shapes` vêm no `qtdeclarative`: nenhuma receita nova para botões |
| `EegClient` expõe `waveform(int)` como `QVariantList` | `EegClient.h` | caminho lento demais para 8 canais acima de 250 SPS |
| `/data` é `0700` de root em todos os níveis | `MedicalStorage.cpp` | nenhum outro usuário lê um `record.bdf` |
| O `weston.ini` do poky não tem lançador | `weston-init/weston.ini` | o painel existe e está vazio |
| No link `usb` a ponte ocupa uma USB-A | `BRINGUP_AFE.md` | mouse + teclado precisam de um hub, ou de um combo num receptor só |

---

## 4. Arquitetura: três identidades, não uma

```
                    weston (desktop-shell, usuário do compositor)
                      │                         │
          ┌───────────┴──────────┐   ┌──────────┴───────────┐
          │ eeg-hmi-gui          │   │ EDFbrowser           │
          │ usuário med-hmi      │   │ usuário med-review   │
          │ vê amostras, pede    │   │ lê BDF+ fechados     │
          └───┬──────────────┬───┘   └──────────┬───────────┘
   eeg.sock   │              │ eeg-control.sock │ só leitura, só BDF+
   (assistir) ▼              ▼ (pedir)          ▼
          ┌──────────────────────────┐   ┌──────────────────────┐
          │ eeg-acquisition-service  │──►│ /data  (LUKS)        │
          │ único dono do front-end  │   │ bruto, metadado, BDF+│
          └──────────────────────────┘   └──────────────────────┘
```

Quem adquire, quem mostra ao vivo e quem revisa são processos e identidades distintos. A HMI não
abre `/data`; o revisor não abre nenhum socket; nenhum dos dois abre o nó do front-end.

### 4.1 As duas janelas são unidades do systemd, não lançadores

Proposta: `eeg-hmi.service` e `med-review.service` sobem no boot, cada uma com o seu usuário e o
seu sandbox, com `Restart=always` — fechar uma janela a reabre.

Um lançador no painel do weston executa o programa **como o usuário do compositor**. Para o
EDFbrowser ler registros por esse caminho, o compositor precisaria de leitura em dados de paciente,
e a identidade "quem revisa" passaria a ser "quem desenha a tela". O painel fica, portanto, sem
lançador de aplicação nesta versão (§10, decisão em aberto).

### 4.2 O caminho de leitura do revisor

`MedicalStorage` cria tudo com `0700`. O revisor precisa ler `record.bdf` de sessões fechadas e
**nada mais** — nem o bruto, nem o `metadata.json`, nem a sessão viva.

Proposta: ao terminar de derivar um BDF+, o `eeg-record-export` o **publica** por uma API nova do
`MedicalStorage` — um *hard link* numa árvore de revisão `root:medreview 0750`, com o arquivo em
`0640`. Sem cópia (mesmo sistema de arquivos, mesmo inode), sem tocar na sessão viva (o exportador
já a evita pelo marcador em `/run/medplatform`), e com a decisão dentro do framework, que é onde a
política de persistência mora.

### 4.3 O EDFbrowser é SOUP, e é tratado como tal

Software de terceiros sob IEC 62304 §3.29. Entra na `med-image-eeg` como ferramenta de revisão e
**fica fora da `med-image-prod`**. O seu sandbox tem `PrivateNetwork=yes` (ele linka `QtNetwork`) e
escrita apenas no próprio diretório de estado.

### 4.4 Os usuários, e por que os ids são fixos

São **usuários de sistema**, sem senha e sem shell: ninguém entra neles, quem os assume é a unidade
do systemd, pelo `User=`. Este repositório não cria nenhum usuário hoje; estes são os primeiros.

| Usuário | Quem roda | Grupos | Fase |
|---|---|---|---|
| `med-review` | EDFbrowser | `medreview`; `wayland` dado pela unidade (`SupplementaryGroups=`), não pelo `/etc/group` | A3 |
| `med-hmi` | `eeg-hmi-gui` (hoje root) | `wayland`, `medop` | B (`implementation_plan_eeg_control.md`, Fase 6) |

| Grupo | O que concede | Origem |
|---|---|---|
| `wayland` | desenhar: o socket do compositor é `0775`, grupo `wayland` | já existe, criado pelo `weston-init` do oe-core |
| `medreview` | ler a árvore dos BDF+ fechados | novo |
| `medop` | comandar pelo socket de controle; assistir não exige grupo | novo, do plano de controle |

Criados **no build** (`inherit useradd` na receita de cada aplicação): o rootfs é somente-leitura e
não há primeiro boot em que criá-los.

**uid e gid são estáticos**, declarados uma vez em `med-os.conf` (`MED_REVIEW_UID`/`MED_REVIEW_GID`) e passados explícitos ao `useradd` pela receita, que falha o build sem eles. (A primeira versão deste plano dizia `useradd-staticids`; duas variáveis fazem o mesmo sem obrigar toda receita da imagem a constar numa tabela.) A árvore de
revisão vive em `/data`, que sobrevive à troca de slot. Um gid atribuído pela ordem de instalação
pode mudar de um build para o outro, e depois de uma atualização os BDF+ antigos pertenceriam a um
número que não é mais `medreview` — o revisor perderia os registros anteriores, sem erro em lugar
nenhum. É o mesmo formato da chave do LUKS e do journal: um estado fora do slot que depende de um
valor dentro dele.

O operador humano **continua sem usuário**. Não há tela de login, e o nome numa sessão é afirmado,
não verificado.

---

## 5. A tabela de escolhas selada

O problema de projeto central. Hoje `afe.gain = 24` é traduzido para o vocabulário do link **no
build**, em Python, por `do_derive_device_options`, e o resultado é selado. Para a tela escolher
ganho 12, alguém precisa traduzir em runtime — e não pode ser o framework, que não tem vocabulário
de front-end (regra 9), nem a HMI, que não fala com o front-end.

**Proposta**: o build gera, para o link da imagem, um segundo store selado
(`eeg-choices.conf` + `.sha256`) com, para cada chave `afe.*` e `acquisition.sample_rate_hz`:

- os valores permitidos **neste link**;
- para cada valor, as linhas `device.option.*` e `record.*` já traduzidas.

O serviço carrega a tabela por `MedicalConfiguration` (que recusa um store sem selo), aceita apenas
o que está nela, e aplica as linhas já traduzidas — sem conhecer o que significam. A HMI recebe a
lista de escolhas do serviço e mostra só o que existe.

Três regras:

1. **Um valor entra na tabela de um link só depois de medido naquele link.** `usb` começa com
   `sample_rate_hz = 250` e nada mais (é o teto da ponte, `BRINGUP_AFE.md` §6), e com lead-off
   indisponível até a Fase E. Uma tela que oferece o que o link não honra é o defeito que o link
   `usb` já tem hoje com `lead_off_detection = true`.
2. **O envelope de segurança roda a cada escolha**, não só na partida: ganho × `safety.max_input_uv`
   é verificado contra a combinação pedida, e uma recusa deixa o serviço em `ready`.
3. **O `eeg.conf` selado é a configuração de fábrica** e o estado para o qual um boot volta. Uma
   escolha da tela vale para as sessões seguintes até o próximo boot; não é gravada no rootfs.

### 5.1 O verbo novo

`session.start` já gasta três das oito opções de `kControlMaxOptions` (`operator`, `exam`,
`subject`); seis configurações não cabem ao lado. Em vez de crescer a mensagem:

| Verbo | Estado em que vale | Parâmetros | Auditoria |
|---|---|---|---|
| `session.configure` | só `ready` | pares `afe.*` / taxa, cada um da tabela | `ConfigurationChanged` com o uid do par, antes e depois |
| `device.choices` | qualquer | — | nada |

Em `acquiring`, `session.configure` é recusado com motivo — é o não-verbo do plano de controle
expresso como recusa, e a recusa é `SecurityEvent`.

### 5.2 A chave SRB1

A posição da chave de referência da placa do front-end é manual e ilegível por software. Uma troca
de montagem pela tela exige que o operador **afirme** a posição (GNDA para bipolar, eletrodo de
referência para referencial), e a afirmação vai para o metadado rotulada como afirmada — a mesma
distinção entre identidade verificada e afirmada do plano de controle (§7.3 de lá).

---

## 6. Cada função, por link

| Função | `amp` | `usb` / `spi` | O que falta |
|---|---|---|---|
| Ganho | firmware (`afe.gain`) | `hardwaregain` | só a tabela |
| Taxa | firmware | `sampling_frequency`; em `usb` só 250 | medir as outras taxas em `amp` antes de listá-las |
| Gerador interno | firmware | `test_signal` | só a tabela |
| Montagem | firmware (`MISC1`) | atributo `montage` | tabela + afirmação da SRB1 |
| Lead-off | firmware + bloco de 8 bytes do frame v2 | **não honrado**: o driver liga os comparadores e não habilita canal | Fase E |
| Entrada (curto, temperatura, alimentação) | **não existe chave no firmware** | `input_mux` existe no driver | Fase D: a chave `afe.input` não existe na prescrição |
| Bias drive | — | — | fora da tela, por decisão |

---

## 7. Mudanças, arquivo a arquivo

| Onde | O quê | Fase |
|---|---|---|
| `meta-med-distro`, `weston-init.bbappend` | `require-outputs=none` (o compositor sobe sem monitor — defeito achado na placa, §12.2); `idle-time=0` pelo `PACKAGECONFIG` `no-idle-timeout` que a receita do oe-core já tem, conferido no arquivo produzido; seção `[output]` a partir do gancho `MED_WESTON_OUTPUT_NAME`/`_MODE`. Sem `weston.ini` próprio: o do oe-core já traz `require-input=false` e o painel | A1 |
| `meta-med-bsp`, `conf/layer.conf` | `MED_WESTON_OUTPUT_NAME`/`_MODE` para `stm32mp25-disco` (`HDMI-A-1`, `1920x1080@50`) | A1 |
| `meta-med-bsp`, receita `med-gpu-access` (nova), via `MED_GPU_PACKAGES` | `KERNEL=="galcore", GROUP="video", MODE="0660"` — acompanha o driver, então só existe com a EULA aceita | A1 |
| `med-kernel-features.cfg` | `CONFIG_INPUT_EVDEV` escrito; verificado por `do_med_check_kernel_config` | A1 |
| `eeg-hmi-gui`, `WaveformItem` (novo, C++) | `QQuickItem` no scene graph, alimentado direto pelo cliente | A2 |
| `eeg-hmi-gui`, `EegClient` | lead-off por canal, taxa entregue, amostras sem passar por `QVariantList` | A2 |
| `eeg-hmi-gui`, QML | janela maximizável, `QtQuick.Controls`, painéis | A2, B, C |
| `meta-med-app`, receita `edfbrowser` (nova) | `inherit qt6-qmake`, `SRCREV` fixo, licença conferida | A3 |
| `meta-med-app`, `med-review.service` (novo) | `User=med-review`, sandbox, `Restart=always` | A3 |
| `meta-med-distro`, `med-os.conf` | `MED_REVIEW_UID`/`MED_REVIEW_GID` (A3); os de `med-hmi`/`medop` na Fase B | A3, B |
| `meta-med-app`, receita `med-review` (nova) | o usuário, o grupo e a unidade - separada da receita do EDFbrowser, que empacota software de terceiros sem modificar | A3 |
| `meta-med-distro`, `weston-init.bbappend` | `panel-position=none`: sai o lançador de terminal e o relógio de uma placa sem RTC | A3 |
| `eeg.conf` | `storage.review_group = medreview` | A3 |
| `MedicalStorage.h/.cpp` | publicar um arquivo fechado na árvore de revisão | A3 |
| `eeg-record-export` | publica o BDF+ depois de derivá-lo | A3 |
| `med-image-eeg.bb` | instala o revisor; a `med-image-prod` não | A3 |
| serviço, unidade, IPC, `medctl` | conforme `implementation_plan_eeg_control.md` §8 | B |
| receita do serviço, `do_derive_device_options` | gera e sela `eeg-choices.conf` | C |
| serviço, `main.cpp` | `session.configure`, `device.choices`, envelope por escolha | C |
| `eeg.conf`, envelope, firmware M33 | `afe.input` | D |
| `linux-med`, `ti-ads1299.c` | lead-off por canal | E |
| `med-check.py` | asserções novas, antes das duas que precisam ficar por último | A1, A3, C |
| `RESULTS.md`, `LAYER_MED_*.md`, `CLAUDE.md` | contagem de pacotes, delta do EEG, a superfície nova | ao fim de cada fase |

---

## 8. Fases

### Fase A1 — o ambiente sobe sozinho

**Objetivo**: cartão recém-gravado, HDMI e mouse, imagem em um boot.
**Pré-requisito**: nenhum.
**Procedimento**: as quatro primeiras linhas da §7. As três correções de vídeo já estão
especificadas e medidas em `BRINGUP_HMI_STM32MP2.md` §9; esta fase as tira da bancada e as põe nas
camadas que aquele registro já atribuiu.

**Critérios**:
1. cartão novo, sem tocar no console: painel do weston visível no HDMI;
2. `NRestarts=0` em `weston.service` e `eeg-hmi.service` **aos 5 minutos** (o OOM do QEMU chegava
   aos 150 s; um horizonte de 30 s já aprovou um sistema que morria depois);
3. o ponteiro se move com um mouse USB e o teclado chega à janela em foco;
4. `make check` continua 26/26 no QEMU.

**Injeção de falha**: três na placa, uma por correção, cada uma com o sintoma que o registro já
mediu — sem a regra udev o weston morre em ~6 s; sem `idle-time=0` a saída desliga aos 300 s; sem o
modo CEA o CRTC fica ativo e a tela preta. E duas no build: o `PACKAGECONFIG` `no-idle-timeout`
retirado, e um modo declarado sem o conector — as duas têm de falhar o `do_install` do
`weston-init`, nomeando a causa.

*Corrigido ao executar*: a primeira versão pedia que `CONFIG_INPUT_EVDEV` removido do fragmento
falhasse o build. Não falha, e não pode: os dois defconfigs fornecem o símbolo, e tirar a linha só
tira o requisito. O que a linha compra é o inverso — um kernel futuro que não o tenha falha em
`do_med_check_kernel_config` em vez de subir com um mouse que não move nada. Essa guarda já foi
injetada contra os dois `.config` reais; não há injeção nova a fazer aqui.
**O que uma reprovação significa**: se o critério 1 falha com as três correções presentes, existe
um quinto defeito que as correções manuais mascaravam — a forma que este repositório já viu duas
vezes.
**Registro**: `BRINGUP_HMI_STM32MP2.md` §9 (as pendências passam a resolvidas) e `RESULTS.md` §8.

### Fase A2 — a HMI ao vivo

**Objetivo**: ver o sinal como num osciloscópio de EEG, sem ainda comandar nada.
**Pré-requisito**: A1 para ver na placa; o desenvolvimento roda no QEMU e no host.
**Procedimento**: `WaveformItem` no scene graph; janela maximizável; 8 traços com escala por canal
e base de tempo; filtros **de exibição** (passa-alta, notch de rede) rotulados na tela como tais;
lead-off por canal a partir do bloco que o frame v2 já carrega; taxa entregue calculada dos
cabeçalhos contra a declarada.

**Critérios**:
1. com a HMI aberta, `acq-sample-rate` continua passando — a tela não rouba amostra do registro;
2. a taxa mostrada coincide com a do `metadata.json` da sessão dentro de 1%;
3. CPU da HMI na placa medida e registrada a 250 SPS (hoje: 1,099 s em 2 min 22 s, com o `Canvas`);
4. o registro de uma sessão é idêntico, byte a byte, com filtros de exibição ligados e desligados.

**Injeção de falha**: fazer o `simulated` entregar metade dos frames — a taxa entregue na tela tem
de cair e ficar em destaque. Um indicador que nunca viu a taxa errada é afirmação, não medida.
**Registro**: `RESULTS.md` §3.

### Fase A3 — o revisor

**Objetivo**: abrir no EDFbrowser, na placa, o BDF+ de uma sessão fechada.
**Pré-requisito**: A1. Independente de A2.
**Procedimento**: receita, unidade, usuário, a publicação pelo `MedicalStorage` e o exportador
chamando-a.

**A verificar no primeiro build, antes de qualquer outra coisa**: que o `PACKAGECONFIG` do `qtbase`
da imagem traz `widgets` e `printsupport`; que `CONFIG += static` do `edfbrowser.pro` convive com
um Qt compartilhado; a licença (GPL-3.0, a confirmar no `LIC_FILES_CHKSUM`) e o texto exato do
aviso de uso do autor; onde ele grava as preferências, num rootfs somente-leitura; e o consumo de
memória com as duas janelas abertas contra os 1024 MiB do QEMU.

**Critérios**:
1. fechar uma sessão; o `record.bdf` aparece na árvore de revisão e abre, com os rótulos de canal
   do registro;
2. `ps` mostra o EDFbrowser como `med-review`;
3. `bdf_oracle.py` aprova o mesmo arquivo — o revisor lê o que o oráculo lê.

**Injeção de falha**: como `med-review`, tentar abrir o bruto de uma sessão, o `metadata.json`, e
o diretório da sessão viva — os três têm de falhar com `EACCES`. E matar o processo: a janela
volta.
**O que uma reprovação significa**: se `med-review` lê o bruto, o caminho de revisão é uma segunda
porta para `/data`, e a §4 inteira é desenho sem efeito.
**Registro**: `RESULTS.md` (pacotes e delta do EEG), `LAYER_MED_APP.md`.

### Fase B — a sessão pela tela

**Objetivo**: Iniciar, Anotar e Encerrar na janela.
**Pré-requisito**: fases 2 a 5 do `implementation_plan_eeg_control.md`.
**Procedimento e critérios**: os da Fase 6 de lá, sem alteração — incluindo a HMI como `med-hmi` e
a injeção que a tira do grupo `medop` para provar que assistir e comandar são privilégios distintos.
**Acréscimo desta estação**: ao encerrar, a tela informa quando o BDF+ da sessão ficou disponível
para revisão.

### Fase C — a configuração pela tela

**Objetivo**: ganho, taxa, gerador interno e montagem escolhidos na janela.
**Pré-requisito**: Fase B.
**Procedimento**: §5 inteira.

**Critérios**:
1. cada escolha produz uma sessão cujo `metadata.json` declara o valor escolhido, e cujo sinal o
   confirma — gerador interno ligado, a amplitude medida no BDF+ muda com o ganho na razão esperada;
2. a tela de uma imagem `usb` não oferece o que a de `amp` oferece, sem uma linha de código
   diferente entre as duas;
3. `session.configure` durante `acquiring` é recusado, auditado, e a sessão continua;
4. depois de um reboot a configuração é a de fábrica.

**Injeção de falha**: adulterar um byte de `eeg-choices.conf` — o serviço não sobe. Pedir pelo
`medctl` um valor fora da tabela — recusa com motivo, serviço em `ready`. Pedir um ganho que viola
`safety.max_input_uv` — recusa pelo envelope, não pela tabela.
**Registro**: `RESULTS.md` §3; `BRINGUP_AFE.md` e `BRINGUP_STM32MP2.md` para o que for visto no
sinal em cada link.

### Fase D — `afe.input`

> **Redesenhada em 2026-10-12** - §12.8. O que segue é o plano original; a temperatura deixou
> de ser uma posição de todos os canais e a alimentação saiu.

**Objetivo**: curto, temperatura e alimentação como entradas selecionáveis.
**Pré-requisito**: Fase C.
**Procedimento**: chave nova na prescrição; tabela por link; firmware M33; o `input_mux` do driver
para `spi`/`usb`.

**Critérios**:
1. entrada em curto: ruído dentro do critério do juiz de ruído que o repositório já tem;
2. temperatura: o valor sobe ao aquecer o conversor com o dedo;
3. o BDF+ de uma sessão com entrada diferente de `normal` **não** rotula os canais como EEG.

**Injeção de falha**: derivar o BDF+ de uma sessão de temperatura com o rótulo forçado para EEG tem
de ser recusado pelo exportador.
**Decisão em aberto que bloqueia o critério 2 como número**: §10, a conversão para °C.

### Fase E — lead-off no link `usb`

**Objetivo**: o link deixa de aceitar uma prescrição que não honra.
**Pré-requisito**: nenhum deste plano; é dívida do driver.
**Procedimento**: habilitar as fontes por canal em `ti-ads1299.c`, entregar `lead_off_status` ao
frame pelo driver `iio`, e só então listar lead-off na tabela do link.
**Critério**: abrir um eletrodo muda o indicador daquele canal e só dele.
**Injeção de falha**: a mesma do `amp` — o layout da palavra de estado conferido contra
`LOFF_STAT` no silício.

---

## 9. Ordem, e o que bloqueia o quê

```
A1 ──► A2 ─────────────────────────┐
 │                                 ├──► B ──► C ──► D
 └──► A3     controle, fases 2–5 ──┘
                                        E  (independente; libera lead-off na tabela do usb)
```

**A1 é a mais barata e a que mais desbloqueia**: são três correções já especificadas e uma linha de
kernel, e sem ela nada das outras pode ser visto num cartão novo. A2 e A3 são independentes entre
si. B e C esperam o plano de controle, que é o caminho crítico do conjunto.

---

## 10. Riscos e decisões em aberto

| Item | Por que é decisão e não implementação |
|---|---|
| **Lançadores no painel** | Um lançador roda como o usuário do compositor. Ter um "menu de aplicativos" exige ou dar ao compositor leitura em registros, ou um mecanismo para pedir ao systemd que suba uma unidade. A §4.1 escolhe unidades fixas; se o ambiente tiver de parecer um desktop, a decisão se reabre |
| **O revisor na imagem de produto** | O autor do EDFbrowser limita o uso pretendido do programa. Fora da `med-image-prod` por ora; mudar isso é decisão regulatória, não de receita |
| **Temperatura em °C** | A conversão é uma constante de datasheet. Na HMI ou no framework ela quebra a regra 9; a alternativa coerente é o build declará-la no store selado, como já faz com `full_scale`. Até decidir, a tela mostra a tensão |
| **Rótulo e unidade de uma sessão que não é EEG** | Uma sessão de temperatura ou de entrada em curto é um registro de bancada num dispositivo clínico. O exportador precisa de uma regra, e alguém tem de dizer se ela gera BDF+ ou não |
| **A configuração escolhida sobrevive ao boot?** | A §5 diz que não. Guardá-la exigiria um store gravável e selado em runtime, que hoje não existe |
| **A afirmação da SRB1 pode ser falsa** | Nada no sistema detecta. O registro passa a dizer o que foi afirmado; não passa a ser verdade |
| **Duas janelas e 1024 MiB no QEMU** | O OOM do weston já apareceu uma vez mascarado por outro defeito. Medir antes de acreditar |
| **Portas USB no link `usb`** | Ponte + mouse + teclado em duas portas. Fato de bancada, mas decide se a estação é utilizável naquele link sem hub |
| **Taxas acima de 250 SPS em `amp`** | O frame é 14 amostras por canal por causa do payload de 496 bytes; taxas maiores são mais frames por segundo, e o M33 rodando de DDR já lê o SPI 18% mais devagar com o A35 saturado. Nenhuma foi medida |

---

## 11. Consequências

- **IEC 62304 §5.3**: três identidades no lugar de uma, cada uma verificável tentando o que ela não
  pode fazer. O revisor é SOUP declarado e segregado por usuário, sandbox e caminho de leitura.
- **Rastreabilidade**: toda mudança de configuração vira `ConfigurationChanged` com quem pediu, e
  toda sessão declara a configuração sob a qual foi adquirida — inclusive a posição afirmada da
  SRB1, rotulada como afirmada.
- **Métrica de reuso**: o delta específico do EEG passa de 3 pacotes para 4. A regra 6 não é
  tocada — nenhuma aplicação deste repositório linka o revisor — mas o número da `RESULTS.md`
  muda e tem de ser reescrito, não arredondado.
- **A afirmação de portabilidade**: a mesma HMI, sem uma linha diferente, mostra telas diferentes
  em `amp` e `usb` porque a tabela selada é diferente. Isso é a superfície de adaptação da
  aplicação continuando a ser dado de build, e é um resultado a registrar.

---

## 12. Validação

*(Vazio de propósito. Uma fase sem número medido e data é código escrito, não resultado.)*

| Fase | Critério | Resultado | Data |
|---|---|---|---|
| A1 | cartão novo mostra o ambiente; `NRestarts=0` aos 5 min; entrada funciona | **parcial** — §12.1 (build) e §12.2 (placa, por `rauc`): compositor e HMI de pé aos 5 min, modo CEA programado; imagem no monitor **confirmada pelo operador**, entrada **não testada**, cartão novo **não testado** | 2026-10-11 |
| A2 | `acq-sample-rate` passa com a HMI aberta; taxa na tela = taxa do registro | **parcial** — §12.3: `acq-sample-rate` passa, CPU da HMI de ≈99% para ≈19% de um núcleo; a tela **ainda não foi olhada**, e os critérios 2 e 4 não foram medidos | 2026-10-11 |
| A3 | BDF+ fechado abre; bruto, metadado e sessão viva dão `EACCES` | **parcial** — §12.4: revisor de pé como `med-review`, nove sessões publicadas, as quatro pernas do alcance medidas e injetadas na placa; **ninguém abriu um arquivo na janela ainda** | 2026-10-11 |
| B | iniciar, anotar e encerrar pela tela; HMI como `med-hmi` | — | — |
| C | escolha → sessão com metadado e sinal coerentes; fora da tabela → recusa | **cumprido no QEMU (33/33) e na placa, link `amp`** — §12.6 e §12.7; o painel não foi descrito por quem o usou | 2026-10-11 |
| D | curto dentro do critério de ruído; temperatura responde; rótulo não é EEG | — | — |
| E | um eletrodo aberto muda um indicador, no link `usb` | — | — |

### 12.1 Fase A1 — o que o build mostrou (2026-10-11)

Nada aqui é a placa. É o que se pode afirmar antes de gravar um cartão.

| O quê | Comando | Resultado |
|---|---|---|
| QEMU constrói | `make qemu` | 5672 tarefas, verde |
| QEMU, runtime | `make check` | **26/26** |
| QEMU, artefato | `weston.ini` do rootfs; `.config` final | `idle-time=0`, nenhuma seção `[output]`, nenhuma regra de GPU; `CONFIG_INPUT_EVDEV=y` |
| Placa constrói | `make stm32 KEY=development KERNEL=med LINK=amp` | 5688 tarefas, verde |
| Placa, artefato | `weston.ini` e `rules.d` do rootfs; manifesto | `idle-time=0`; `[output] name=HDMI-A-1 mode=1920x1080@50`; `70-med-gpu-access.rules` com a regra; `med-gpu-access` no manifesto ao lado de `kernel-module-galcore`; `CONFIG_INPUT_EVDEV=y` |
| Injeção: `no-idle-timeout` retirado | `bitbake weston-init` (QEMU) | `do_install` falha: *"weston.ini carries no idle-time=0"* |
| Injeção: conector sem modo | `bitbake weston-init` (placa) | `do_install` falha: *"a pinned output needs both"* |

**O que isto não significa**: que a tela acende. Os três consertos foram medidos um a um numa
placa em 2026-08-27, aplicados à mão; que os três, vindos da imagem, bastam num cartão novo é a
afirmação que a bancada ainda deve. O formato `LARGURAxALTURA@TAXA` foi lido no fonte do weston
13.0.1 (`backend-drm/modes.c`), não visto escolher o modo. E o `make check` não mede nada disto:
no QEMU não há GPU proprietária nem modo fixado, então os 26/26 dizem só que nada regrediu.

### 12.2 Fase A1 — a placa, por `rauc install` (2026-10-11)

Link `amp`, `KEY=development`, `linux-med` em `983c5cf`. Entregue duas vezes pelo caminho A/B
(A→B, depois B→A), porque a primeira entrega encontrou um defeito que nenhuma das três correções
previa.

**Antes** (imagem anterior, mesmo boot): `weston.service` em `failed`, morto em 6 s;
`/dev/galcore` `crw------- root root`; `eeg-hmi.service` `inactive`.

| O quê | Medido |
|---|---|
| Regra da GPU | `/dev/galcore` `crw-rw---- root video`; o weston passa de `_OpenDevice` e reporta `GL renderer: Vivante GCNANOULTRA31_VIP2` |
| **Defeito novo** — sem monitor no boot | com GPU e modo já certos: `connector 32 is disconnected`, saída 1 aos 4 s, e `Restart=no`. Um aparelho ligado antes do monitor fica sem compositor até o próximo reboot |
| Correção, testada antes de construir | `require-outputs=none` num `weston.ini` em `/run` por drop-in: weston e HMI ativos sem monitor (`There are no outputs - creating placeholder screen`) |
| Correção, vinda da imagem | boot sem monitor: `weston`, `seatd`, `eeg-hmi`, `eeg-acquisition` todos `active`, `NRestarts=0` **aos 5 min**; nenhuma unidade falha |
| Monitor conectado a quente, ~40 s depois | `connector 32 is connected, EDID make 'GSM', model 'M2280A'`; `Output 'HDMI-A-1' enabled` |
| Modo programado | `"1920x1080": 50 148500 … 2640 … 1125` — o CEA-861 VIC 31 fixado, e não o preferido do EDID; CRTC `active=1` |
| Planos | plano primário 1920x1080 e um segundo de 1030x633, que é a janela da HMI com a decoração |
| Caminho A/B | dois `rauc install` seguidos, cada um trocou de slot; os dois slots `good`; a aquisição confirmou o slot nas duas vezes |

**Uma medida que não é da A1 e que a A2 herda**: a HMI consome **um núcleo inteiro** dos dois —
19,8–19,9 s de CPU em 20 s, com ou sem monitor, antes e depois de reiniciá-la. Por thread, em
10 s: 8,56 s na thread principal e 1,46 s na de renderização. É o `Canvas` redesenhando em
JavaScript a partir de `QVariantList`, e não a GPU. A aquisição não sofreu (236 ms em 20 s,
`NRestarts=0`), mas o número de 2026-08-27 — 1,099 s em 2 min 22 s — não descreve mais esta HMI:
entre as duas medidas a taxa de quadros publicada subiu de 10 para 17,9 Hz, e não foi medido se
é só isso. O critério 3 da Fase A2 passa a ter linha de base: **≈ 99% de um núcleo a 250 SPS**.

**O que isto não significa**:
- que há imagem no monitor. O estado do DRM dizia "tudo certo" em agosto com a tela preta
  (`BRINGUP_HMI_STM32MP2.md` §5). O modo programado é o que funcionou naquele dia, e é só isso que
  se pode afirmar sem alguém olhar;
- que a entrada funciona: nenhum mouse ou teclado esteve conectado (`gpio-keys` é o único
  dispositivo de entrada). O critério 3 está em aberto;
- que um cartão recém-gravado se comporta assim: `/data` e `med-boot` eram os que já estavam na
  placa. O critério 1, como escrito, está em aberto;
- que `idle-time=0` segura a tela: a leitura foi aos 5 min, que é exatamente quando o apagamento
  aconteceria, e ninguém olhou depois.
- O CPU de `weston.service` lido pelo systemd (87 ms no total) **não é o do compositor**: a sessão
  PAM move os processos para outro cgroup. Não usar esse número.

### 12.3 Fase A2 — a HMI nova (2026-10-11)

O `Canvas` saiu. `TraceStore` (sem Qt) guarda a janela como varredura e a reduz a colunas de tela;
`WaveformItem` a desenha no scene graph, uma faixa de triângulos por canal; `EegClient` deixou de
entregar amostras ao QML. Sem dependência nova: a tela usa só `QtQuick`.

| O quê | Comando | Resultado |
|---|---|---|
| Aritmética da tela, no host | `make -C tests/hmi check` (agora em `make test`) | **48 verificações, 0 falhas**, com ASan/UBSan |
| Injeção: o máximo da coluna descartado | idem | 1 falha — *"a one-sample spike survives any reduction"* |
| Injeção: o medidor devolve a taxa nominal | idem | 3 falhas — só as do medidor de taxa |
| Compila limpo | `bitbake eeg-hmi-gui`, `-Wall -Wextra` | nenhum aviso |
| QML | `qmllint` do sysroot nativo | nada além do tipo registrado em C++, que o lint não conhece |
| QEMU | `make qemu`, `make check` | **26/26**, `hmi-active` e `acq-sample-rate` inclusive |
| Placa, por `rauc` (A→B) | `systemctl show`, aos 60 s | `weston`, `eeg-hmi`, `eeg-acquisition` ativos, `NRestarts=0`; diário da HMI sem erro de QML nem de fontconfig |
| **CPU da HMI na placa, 250 SPS, monitor ligado** | `CPUUsageNSec`, 20 s | **3,78 s em 20 s (≈19% de um núcleo)**; antes, 19,8–19,9 s (≈99%) |
| Por thread, 10 s | `/proc/<pid>/task/*/stat` | principal **0,19 s** (antes 8,56 s); renderização 1,60 s (antes 1,46 s) |
| Aquisição ao lado | idem | 191 ms em 20 s, sem reinício |

**O que isto não significa**:
- que a tela está certa ou bonita. Ninguém a olhou ainda: o QEMU prova que o QML carrega (uma
  falha de carga derruba o processo e `hmi-active` a pega), não que o traço está no lugar;
- que o medidor de taxa da tela está certo *na tela*. O que foi injetado é a aritmética, no host;
  a injeção do plano — o `simulated` entregando metade dos quadros — não foi feita;
- os critérios 2 (taxa na tela contra o `metadata.json`) e 4 (registro idêntico byte a byte com
  filtros ligados e desligados) não foram medidos. O 4 é verdadeiro por construção — os filtros
  vivem num processo que não abre `/data` — e por isso mesmo merece ser visto e não suposto;
- os 16% da thread de renderização são o custo de desenhar ~18 quadros por segundo em 1080p por
  este driver de GPU; não foi investigado se cabe menos.

**Acréscimo, mesmo dia — a tela olhada, e o que ela escondia.** O operador viu a HMI nova:
fluida, sem travar, clicável com o mouse USB (`2.4G Mouse`, reconhecido pelo libinput; a roda gera
`unknown libinput event 404` no diário do weston, que é ruído — o evento clássico é tratado). E
viu **uma linha reta em cada canal**.

A linha era verdadeira e a tela mentia sobre ela. O BDF+ da sessão `07:37` (28 min, link `amp`,
entradas abertas, lead-off ligado) tem os oito canais em **+187 500,0 µV — o fundo de escala — com
variação exatamente zero** nos últimos 10 s; o firmware reportava `lost=0 spi_err=0`. O passa-alta
de exibição tira o nível, e o que sobra é um traço calmo no zero com rms 0: a figura de um eletrodo
perfeito. É a regra do `BRINGUP_AFE.md` §6.9 — *um canal fora da faixa lê quieto* — reproduzida
por um filtro de tela.

Correção: cada faixa mostra o **nível da entrada medido antes de qualquer filtro** e, quando ele
não variou nada num segundo inteiro, **`NOT VARYING`** em âmbar. Nenhuma constante do conversor
entra nisso — o fluxo não carrega o fundo de escala, e a tela só afirma o que mede: uma entrada
real se move pelo menos pelo ruído do conversor. `tests/hmi` foi a **57 verificações**; a injeção
(medir o nível *depois* do filtro) derruba as duas certas. `make check` 26/26; entregue por `rauc`
(B→A); CPU da HMI 3,79 s em 20 s, igual.

**Não visto ainda**: a tela com esse aviso, e se os indicadores de contato estão vermelhos —
com lead-off ligado e entradas abertas, deveriam.

### 12.4 Fase A3 — o revisor (2026-10-11)

EDFbrowser 2.15 (commit `3cdcbf0`, GPL-3.0-only), construído sem modificação; `MedicalStorage`
ganhou `publish()`/`unpublish()`; o `eeg-record-export` publica o BDF+ de toda sessão fechada e
convertida; `med-review.service` roda o programa como `med-review`.

**O que o primeiro build respondeu** (a lista "a verificar" da Fase A3):

| Pergunta | Resposta |
|---|---|
| `widgets`/`printsupport` no `qtbase` da imagem? | sim, os três módulos já estavam no rootfs |
| `CONFIG += static` com Qt compartilhado? | compila e roda; 2,6 MB, `RDEPENDS` só `qtbase` e `qtwayland` |
| Licença e aviso do autor | GPL-3.0-only (*"version 3 of the License"*, sem *"or later"*). `about_dialog.cpp`: *"EDFbrowser may NOT be used in safety-critical applications, such as life-support medical systems. […] For research and educational purpose only."* |
| Onde grava preferências | sob `HOME`; a unidade o aponta para o diretório de runtime, volátil de propósito |
| Rede | consulta o site do autor uma vez por dia; com `PrivateNetwork=yes` a consulta falha em silêncio (`check_for_updates.cpp:75`) |
| A regra de instalação do upstream | **não serve**: nomeia o binário relativo ao diretório-fonte, e com diretório de build separado copia um arquivo que não existe. Instalado à mão, só o programa |

| O quê | Comando | Resultado |
|---|---|---|
| Publicação, no host | `make -C tests/framework check` | **1522 verificações, 0 falhas** |
| Injeção: a origem seguida por link simbólico | idem | 1 falha, a do link |
| Injeção: permissão afrouxada antes de o grupo existir | idem | 1 falha, a de *"não foi afrouxado"* |
| Exportador | `make -C tests/eeg-export check` | 82, 0 falhas (inalterado: a publicação vive no `main`, que o host não alcança) |
| QEMU | `make qemu`; `make check` | **28/28** - `review-active` e `review-reach` novas |
| Identidade no artefato | `/etc/passwd`, `/etc/group` dos dois rootfs | `med-review:1201:1201`, `medreview:1201` nos dois |
| Placa, por `rauc` (A→B), aos 70 s | `systemctl show` | `weston`, `eeg-hmi`, `med-review`, `eeg-acquisition` ativos, `NRestarts=0` |
| Identidade em execução | `/proc/<pid>/status` | `Uid 1201`, `Gid 1201`, `Groups 993 1201` (wayland, medreview) |
| Publicação retroativa | `ls -la /data/review/eeg` | **9 sessões**, de 1,0 a 82 MiB, todas `-rw-r----- root medreview`, 2 nomes; árvore `drwxr-x--- root medreview` |
| Alcance, como `med-review` | o comando da `review-reach`, na placa | `bdf=8 bruto=NEGADO metadado=NEGADO sessoes=NEGADO nomes=2` |
| Memória e CPU, três processos | `MemoryCurrent`, `CPUUsageNSec` em 20 s | HMI 82 MiB / 3,83 s; revisor 44 MiB / 0 ms (ocioso); aquisição 3 MiB / 190 ms |

**Injeções na placa, em runtime, com o comando da própria suíte** - as quatro desfeitas em
seguida e conferidas:

| Injeção | O que a asserção disse |
|---|---|
| `/data/eeg` e a sessão abertas a outros, `raw.bin` legível | `bruto=LEU sessoes=LEU` → falha |
| `metadata.json` entregue ao grupo `medreview` | `metadado=LEU` → falha |
| o nome publicado retirado | `bdf=0` → falha |
| o revisor como root, por *drop-in* em `/run` | `uid=0 esperado=1201` → falha, com `ActiveState=active NRestarts=0` - o caso que "ativo" sozinho aprovaria |

**O que isto não significa**:
- que o EDFbrowser abre e mostra um registro. A janela não foi olhada por ninguém; o que se
  mediu é que o processo está de pé e que o arquivo abre *para aquele usuário*;
- que as duas janelas convivem bem. Sem painel, alternar é pelo teclado do compositor, e não
  foi visto como elas se sobrepõem;
- que a sessão **viva** está protegida por esta asserção: ela é protegida por não ter BDF+ e
  por estar atrás dos mesmos `0700`, mas a `review-reach` testa a penúltima sessão, não a viva;
- as injeções foram feitas na placa e **não** no QEMU: a `review-reach` e a `review-active` da
  suíte passaram sem nunca ter falhado *dentro da suíte*. O comando é o mesmo, o ambiente não;
- o boot do QEMU registrou dois `timeout aguardando shell` antes do console nesta execução.
  Uma ocorrência, causa não investigada.

### 12.5 Depois das três fases — o que o uso mostrou (2026-10-11)

O operador usou a estação com o sinal de teste interno (`ROTEIRO_PLACA.md` §7) e com as duas
janelas. A onda quadrada apareceu certa nos oito canais (1,86 mV rms lidos contra 1,875 mV: a
janela do rms é de 250 amostras e o período é de 256), janelas de 5 a 30 s, roda do mouse por
canal, `Fit`, passa-alta e a reconexão depois de `systemctl stop/start` da aquisição funcionaram.
Quatro defeitos saíram disso, e uma decisão.

| Visto | Causa | Correção |
|---|---|---|
| taxa entregue oscilando entre 242 e 252 | a janela do medidor era fechada pelo temporizador da tela, e pegava um quadro a mais ou a menos em cada ponta: 14 amostras em 2 s são 7 Hz. Abaixo de 245 o número ficaria vermelho sobre um fluxo que não perde nada | a janela abre e fecha na chegada de um quadro; o temporizador só a fecha quando o fluxo parou. `tests/hmi` reproduz 243,67–251,86 ao reverter, e dá 250,00 ± 0,01 |
| botão de escala "sem efeito" | `+`/`−` moviam só as faixas ainda na escala comum; depois de um `Fit` nenhuma está | movem todas, cada uma do seu ponto; `Same for all` iguala pela mais larga |
| `Freeze` retomava de onde parou | amostras de antes e de depois da pausa lado a lado, sem costura visível | retomar recomeça a varredura limpa |
| as duas janelas minimizadas, sem volta | o compositor não tem lista de janelas, com ou sem painel | HMI sem moldura; o revisor sobe **a pedido** (botão `Review records` → arquivo num diretório de pedidos → unidade `.path` → `med-review.service`), e fechar a janela volta à HMI |

**O pedido, e um defeito dele.** A primeira versão punha o arquivo no diretório de runtime da
própria HMI. Quando a HMI caiu, o diretório foi removido e refeito e a unidade leu isso como
pedido: um revisor que ninguém pediu. O pedido passou a um diretório próprio
(`/run/med-review-request`, `DirectoryNotEmpty=`), esvaziado sem ler antes de atender.
`make check` 28/28 com a `review-active` fazendo o pedido pelo mesmo caminho. **Não instalado na
placa ainda.**

**O compositor caiu, e a aquisição não.** Às 20:33:55 (hora da placa) `systemctl stop
med-review` derrubou o weston 13.0.1 com SIGSEGV; um segundo antes o EDFbrowser registrara um
popup aberto (`Creating a popup with a parent … which does not match the current topmost grab`).
A HMI perdeu a conexão e saiu com 255; o weston voltou e a HMI reiniciou em 3 s. A aquisição
seguiu na mesma sessão, `NRestarts=0`, `lost=0` no M33, e registrou `HMI client disconnected` /
`connected`. É a partição entre aquisição e tela **observada numa falha real**, não injetada.
A causa - matar um cliente com um menu aberto - é hipótese: não há backtrace. Uma ocorrência.

**A decisão.** O operador quer um ambiente de trabalho de verdade - barra de tarefas, gerenciador
de arquivos. O weston não tem como ganhar uma lista de janelas, então isso é trocar o
compositor: `implementation_plan_desktop.md`. O que está aqui continua sendo o perfil de tela
única (a HMI sozinha, o revisor a pedido), que é o que uma imagem de produto usaria.

### 12.6 Fase C — a configuração pela tela: o que foi escrito (2026-10-11)

A Fase B entrou por outro caminho (`implementation_plan_eeg_control.md` §13.1: gravação sob
pedido, `medctl`, botão `Record`). Esta é a §5 deste plano, sobre aquela base.

| Peça | Onde | Estado |
|---|---|---|
| Uma função de tradução, chamada para a prescrição de fábrica e para cada alternativa | receita do serviço, `do_derive_device_options` | construída nos três links |
| `eeg-choices.conf`, selado | idem, e `do_seal_configuration` | **simulated 56 linhas, `amp` 42, `usb` 28** - lidas nos pacotes produzidos |
| `session.configure`, `device.prescription` | `eeg-acquisition-service`, `main.cpp` | compila sem avisos |
| A tabela lida e verificada pela HMI; painel `Settings` | `eeg-hmi-gui`, `PrescriptionTable`, `SettingsPanel.qml` | compila; `qmllint` limpo |
| Asserção `configure-choice` | `scripts/med-check.py` (32 asserções) | **não executada** |

Como a tabela ficou, por link, e é o que a tela vai mostrar sem uma linha de código diferente:

| Link | Linhas | O que não é oferecido, e por quê |
|---|---|---|
| `simulated` | 56 | nada: 7 ganhos × lead-off × montagem × gerador |
| `amp` | 42 | gerador interno com lead-off ligado - o firmware recusa, e a regra passou a ser dita no build |
| `usb` | 28 | lead-off desligado - o driver liga os comparadores no *probe* e não oferece como desligá-los |

**Onde se afasta da §5, e por quê**:
- a tabela é de **prescrições inteiras**, não de valores por chave. As chaves interagem (o
  fundo de escala depende do ganho, o gerador exclui o lead-off num link), e uma tabela por
  chave ofereceria combinações que nenhuma linha traduz;
- o verbo `device.choices` não existe: a resposta do canal de controle tem 64 octetos, e a HMI
  lê o mesmo arquivo selado que o serviço, pelo `MedicalConfiguration`;
- a **taxa** não está na tabela. A regra 1 da §5 vale: só entra depois de medida no link
  (entrou em 2026-10-13, com essa regra dentro da tradução - §12.10);
- a conferência do envelope (ganho × `safety.max_input_uv`) é feita **no build**, ao gerar cada
  linha, e não a cada pedido: uma linha que o serviço recusaria não existe;
- a afirmação da chave de referência viaja como `reference_switch` (`ground`/`electrode`), é
  exigida pela tabela nos links com conversor, e vai ao metadado como
  `affirmed_reference_switch`.

**Aplicar uma escolha** é fechar o dispositivo e abrir outro com as opções da linha. Se o
front-end recusa, as opções anteriores voltam e a recusa vai a quem pediu; se nem as anteriores
voltam, o serviço sai com falha e o reinício da unidade assume. Só vale sem sessão aberta.

**O que isto não significa**: que funciona. Nada disto rodou em imagem nenhuma. E três coisas
são sabidas de antemão:
- uma escolha **não sobrevive** ao reinício do serviço nem ao boot (§5, regra 3);
- no link `usb` a tela continuará oferecendo lead-off **ligado**, que aquele driver aceita e
  não honra (Fase E) - a tabela herda o defeito, não o cria;
- os dois selos são `0640 root`: uma HMI que deixe de ser root (Fase B, identidade) não
  consegue verificar a tabela, e o painel some. Tem de ser resolvido junto.

Corrigido de passagem: `do_derive_device_options` acrescentava ao `eeg.conf` antes de terminar
de derivar; uma falha no meio deixava o arquivo já acrescido, e a execução seguinte o recusava
como editado à mão - um build que só se consertava limpando a receita. Agora nada é escrito
antes de tudo estar derivado.

### 12.7 Fases B e C na placa, e o que a instalação encontrou primeiro (2026-10-11)

Link `amp`, `linux-med` em `983c5cf`, por `rauc install` (B→A). QEMU antes: `make check`
**33/33** com `configure-choice` e `configure-record`.

**A instalação falhou na primeira tentativa, e a causa é um resultado.**

| Visto | Medido |
|---|---|
| `/data` cheio | 450,4 de 454,6 MiB, 0 disponível: 15 sessões gravadas sem ninguém pedir, uma por boot ou reinício, duas delas com 170 e 107 MiB |
| `rauc install` falha | `Failed to save system status: Failed to write file "/data/rauc/central.raucs…": No space left on device` - o bundle estava íntegro (sha256 igual dos dois lados) |
| O slot de destino fica fora da ordem | `BOOT_ORDER=B`, `BOOT_A_LEFT=0`, slot A `bad`: o RAUC o retira antes de escrever e a falha o deixa assim. A placa seguiu no B, sem reserva |
| A aquisição perdia quadros | 9 avisos de *"sample frame could not be persisted"* no boot; `eeg-record-export` em `failed`; a última sessão sem `closed.json` e com o BDF+ incompleto; um marcador de sessão viva sem diretório |

Dois defeitos de plataforma saem daí, **e nenhum está corrigido**:

1. **Um volume de registros cheio impede a atualização.** O `data-directory` do RAUC mora em
   `/data`, junto com os registros. O dispositivo que mais precisa de uma correção - o que
   encheu o disco por um defeito de software - é o que não consegue recebê-la. É o mesmo formato
   da chave do LUKS e do journal: um estado de que a atualização depende, num lugar que outra
   coisa pode esgotar.
2. **Nada impede o volume de encher.** O serviço grava até o último octeto e depois conta
   quadros perdidos. Não há limite, não há recusa de sessão nova por falta de espaço, e a tela
   não diz quanto resta.

As 85 arquivos foram copiados para `build/med-bench/20261011-placa-data-cheio/` (sha256 de cada
um igual dos dois lados, lista em `SHA256SUMS`) e apagados da placa a pedido do operador; a
segunda instalação passou, com os dois slots `good` e `BOOT_ORDER=A B`.

**Depois do boot na imagem nova:**

| O quê | Medido |
|---|---|
| O caminho do weston com o contrato do compositor (`aae5078`), nunca inicializado antes | `med-compositor.target` ativo; `weston`, `eeg-hmi`, `eeg-acquisition`, `med-m33-firmware` ativos, `NRestarts=0` |
| Ocioso | `medctl session.status` → `idle`; `/data/eeg` com 0 sessões e sem crescer em 10 s; nenhum marcador de sessão viva |
| A tabela do link | `choice 36` em vigor: a de fábrica das 42 |
| Uma escolha chega ao conversor | pelo traço do firmware do M33: fábrica `CHnSET 60 … SENS ff ff ff ff CONFIG4 02`; gerador interno com lead-off desligado `CHnSET 65 … SENS ff ff 00 00`; ganho 12 `CHnSET 55` |
| Sessão sob gerador interno, ganho 24 | 101 quadros, `dropped 0`; BDF+ com fundo de escala declarado 187 500 µV; nos 8 canais, patamares +1844,7…+1853,3 e −1893,9…−1885,4 µV, **período 256,0 amostras** |
| Sessão sob gerador interno, ganho 12 | 216 quadros, `dropped 0`; fundo de escala declarado 375 000 µV; patamares +1824,1…+1835,1 e −1914,7…−1903,7 µV, período 256,0 |
| O registro declara a escolha | `afe.gain`, `afe.test_signal`, `afe.lead_off_detection` e `full_scale_uv` de cada sessão são os da linha escolhida; `prescription_source: operator choice` |
| Trilha | 3 `ConfigurationChanged`; serviço sem reinício do começo ao fim; os dois BDF+ publicados para revisão |

A amplitude é a mesma nos dois ganhos, como deve: o gerador está na entrada. O que muda é a
escala declarada, e o exportador - que recusa um registro fora da grade do conversor - aceitou
as duas. O meio dos patamares fica 20 a 40 µV abaixo de zero: o deslocamento do canal, não
investigado.

**O que isto não significa**:
- o operador estava usando a tela ao mesmo tempo. A primeira leitura do firmware já mostrava o
  gerador interno ligado (alguém o tinha aplicado pelo painel), e a primeira sessão foi encerrada
  aos 6 s por outra mão. Isso é evidência de que o painel e o botão funcionam, e **não** é uma
  medida: ninguém a registrou como tal, e a recusa "gerador com lead-off ligado" que se queria
  ver na placa não foi vista, porque o lead-off já estava desligado;
- montagem referencial e a afirmação da chave não foram exercitadas na placa;
- o defeito do weston com cliente morto de menu aberto continua lá: a placa segue no weston;
- a HMI reportou um laço de *binding* no painel (`SettingsPanel.qml`, propriedade `changed`) no
  primeiro boot. Corrigido no fonte; **a imagem da placa ainda é a anterior**.

**Acréscimo, mesma noite - o que o uso pediu e as pendências.** Segunda entrega por `rauc`
(A→B), depois de `make check` **34/34** no QEMU.

| O quê | Estado |
|---|---|
| O teclado na tela do weston, sem tecla de fechar e cobrindo os campos | os campos de operador e exame saíram da barra de baixo para um painel no topo, aberto pelo `Record`; `Start` e `Cancel` devolvem o foco ao traçado, que é o que recolhe o teclado. **Na placa; não descrito por quem usa** |
| Laço de *binding* no painel `Settings` | corrigido; o diário da HMI no boot novo não o traz |
| Reserva no volume de registros (`storage.reserve_mib = 32`) | sessão nova recusada abaixo da reserva; sessão em curso fechada pelo serviço, inteira, com `reason: record volume full`. Asserção `storage-reserve` passa no QEMU com o volume enchido de verdade - **nunca vista falhar** |
| `ROTEIRO_PLACA.md` §7 | aviso com o procedimento novo |
| CPU, na placa, ociosa e sem sessão | aquisição **24 ms em 10 s** (antes ~95: não há mais `fsync` por quadro fora de uma sessão). HMI **5,8 s em 20 s** (thread principal 1,49 s, renderização 4,00 s), contra 3,8 s antes de existir o controle |

O aumento da HMI tem duas causas prováveis, e só uma foi tratada: o objeto de controle anunciava
mudança a cada segundo, respondesse o serviço o que respondesse, e tudo o que está ligado a ele
era reavaliado (corrigido no fonte, **não está na placa**); e a janela agora ocupa a tela
inteira, sem moldura, o que é mais área para a GPU a cada quadro (não investigado).

Continua em aberto, e não é implementação: o estado do RAUC mora no mesmo volume dos registros
(a reserva impede que *uma sessão* o esgote, não que outra coisa o faça); a montagem referencial
com a afirmação da chave não foi exercitada na placa, porque a afirmação é de quem vê a chave.

**Terceira entrega da noite (B→A), `make check` 34/34 antes.** O teclado na tela do weston foi
julgado por quem o usou: digita, mas o apagar não faz nada sobre um campo de texto do Qt, e não
há como recolhê-lo. A HMI passou a trazer as próprias teclas dentro do painel de gravação
(`Keypad.qml`: letras, dígitos, `-`, `.`, `_`, espaço, maiúsculas, apagar), e o do compositor foi
desligado (`[input-method] path=` vazio, que o `text-backend.c` trata como "não lançar"). Na
placa: nenhum processo `weston-keyboard`, HMI e aquisição ativas sem reinício.

A correção do anúncio de estado está nesta imagem, e a regressão de CPU fechou: thread principal
da HMI **0,39 s em 20 s** (era 1,49 s; 0,38 s antes de existir o controle), renderização 2,94 s,
total ≈ 17% de um núcleo. A janela em tela inteira **não** era a causa.

**Não visto**: o teclado próprio usado por uma pessoa. O QEMU prova que o QML carrega.

### 12.8 Fase D — o que foi escrito, e o redesenho no meio (2026-10-12)

**Estado**: escrito, testado no host e no QEMU. **Nada disto rodou na placa.**

A primeira versão seguia o plano: `afe.input = normal | shorted | temperature`, uma posição da
chave de entrada para os oito canais. O operador viu o defeito de projeto antes de ir à
bancada: **medir a temperatura assim custa o exame inteiro**. Ficou assim:

| Chave | Valores | O que faz | O que é a sessão |
|---|---|---|---|
| `afe.temperature` | `false` / `true` | o **último canal** vai para o sensor de temperatura do conversor; os outros sete continuam nos eletrodos | um exame, com um canal a menos e a temperatura gravada ao lado |
| `afe.input` | `normal` / `shorted` | as duas entradas de **todos** os canais ligadas entre si dentro do conversor | medição de bancada (o ruído do próprio conversor), declarada como tal |

As duas aparecem no painel *Settings* como chaves liga/desliga, sem código novo no painel: ele
desenha o que a tabela selada lista.

Três decisões do operador, registradas como tais: o canal é o 8, fixo (escolher o canal
multiplicaria a tabela por oito); a temperatura **pode ficar ligada durante um exame**; e ela
é mostrada e gravada em **°C**, com a conversão declarada no build.

**A alimentação saiu.** A posição da chave que lê os trilhos dá 2,5 V, que só o ganho 1
representa, e o quadro do link `amp` não carrega o fundo de escala do ganho 1 (nanovolts em 32
bits). Só poderia ser oferecida na simulação, onde nada é medido.

O que mudou, por camada:

| Onde | O quê |
|---|---|
| firmware M33 | `CHnSET` por canal (era um valor para os oito). Com a temperatura, o canal 8 vai para MUX 100 e **sai das quatro máscaras** `BIAS_SENS`/`LOFF_SENS`: não entra na média do bias nem recebe corrente de lead-off; os dois bits dele no bloco de contato deixam de ser monitorados. Curto é MUX 001 em todos. Recusas, cada uma nomeando a opção: curto com sinal de teste, curto com lead-off, curto com temperatura, e qualquer das duas num produtor sintético |
| mensagem de controle | as chaves `afe.*` passaram de 6 para **8, que é quanto a mensagem carrega** (`MED_AMP_CONTROL_MAX_OPTIONS`). Não cabe mais nenhuma sem mudar a ABI |
| framework, driver `simulated` | opção `offset = "L"` ou `"L@C"`: um nível constante, em todos os canais ou em um |
| receita do serviço | tradução e tabela. Linhas: simulado 56 → 140, `amp` 42 → 98, `usb`/`spi` inalterados - nenhuma das duas chaves é oferecida num link em que não foi medida |
| registro (`metadata.json`) | `input`, e o canal que não é uma tensão de interesse: `auxiliary_channel` (0 se não há), rótulo, unidade e os três números de `valor = (uV − a) / b + c`. Declarados em toda linha, para uma escolha nunca herdar o canal de outra |
| exportador | o canal auxiliar sai com o seu rótulo e a sua unidade (`Temp 8`, `degC`, mínimo e máximo físicos convertidos); os outros sete, byte a byte como antes. Curto: `Short n`. Anotação nos dois casos |
| HMI | faixa do canal auxiliar: "not an electrode" e a leitura convertida; selo `BENCH: INPUTS SHORTED` no curto |

Medido:

| O quê | Resultado |
|---|---|
| testes de host do firmware | 231 verificações, 0 falhas |
| testes de host do exportador | 117, 0 falhas |
| testes de host do framework | 1573, 0 falhas |
| **injeção: rótulo de EEG forçado no canal de temperatura** | recusado inteiro, `InvalidArgument`, sem `record.bdf` |
| suíte no QEMU | **36/36**, com a asserção nova `configure-temperature`: `EEG 1` no primeiro sinal, `Temp 8` no oitavo, `uV` e `degC` |
| a tela, no QEMU | canal 8: "not an electrode", `Temp 27.00 °C`; os outros sete seguem (captura `12-qemu-temperatura-canal-8.png`) |
| serviço e firmware para a placa | compilam; tabela do `amp` com 98 linhas |

**O que isto não significa**:
- **nenhuma constante foi vista no silício.** Os dois códigos de MUX e a reta do sensor
  (145300 µV a 25 °C, 490 µV/°C) vêm do datasheet e estão marcados `[SBAS499?]`. Os critérios 1
  e 2 da fase - ruído com a entrada em curto dentro do critério do juiz, e a temperatura subir
  ao aquecer o conversor - são da bancada e estão em aberto;
- o "27,00 °C" do QEMU é o número que a tabela manda o simulador gerar; mede a declaração e a
  conversão, não um sensor;
- a asserção `configure-temperature` falhou uma vez por um erro de aspas meu e passou depois;
  **nunca foi vista falhar pelo defeito que procura**;
- o traço do canal 8 continua desenhado em tensão; só a leitura por canal é convertida;
- no simulador os tons continuam sob o nível do canal 8: o gerador não silencia um canal;
- o cabeçalho do BDF+ guarda os limites físicos em 8 caracteres, então a conversão para °C
  relida do arquivo erra até ~0,0005 °C nas pontas; os códigos são os do conversor, sem perda;
- **a tabela do `amp` oferece ganho ×1, que o produtor recusa** (o quadro não carrega o fundo
  de escala). Já era assim antes desta fase e não foi tocado: a tela oferece uma linha que o
  link não honra.

### 12.9 Fase D na placa: a primeira leitura do sensor (2026-10-12)

Link `amp`, operado pelo operador pelo painel *Settings*, sensor de temperatura ligado.

| Ganho | Canal 8 |
|---|---|
| ×2 | **recusado pelo front-end** ao ser escolhido |
| ×4, ×6, ×8, ×12 | **42 °C, estável** |
| ×24 (o de fábrica) | **−79 °C** - o canal lê ~94 mV em vez de ~154 mV |

O que isto mostra, e o que foi feito:

- **O código da chave e a reta do sensor estão certos**: 42 °C numa placa ligada há tempo é o
  que um conversor com aquecimento próprio lê. As duas constantes saem de `[SBAS499?]`.
- **Em ganho 24 o sensor satura o amplificador.** O datasheet só diz isso da medida de
  alimentação, ao lado (9.3.1.1.4). O comentário que eu tinha escrito - "é referida à entrada,
  logo não depende do ganho" - vale para a fórmula e não para o amplificador. O firmware passou
  a dar ao canal do sensor **um ganho próprio: o prescrito, limitado a 12**. É um ganho pelo
  qual o dos outros canais se divide por um inteiro, o que mantém cada amostra do canal num
  número inteiro de passos do registro; e o quadro carrega nanovolts referidos à entrada, então
  nada adiante vê a diferença.
- **A tela oferecia ganhos que o link não honra.** O quadro do `amp` é de nanovolts em 32 bits
  e o fundo de escala dos ganhos 1 e 2 não cabe; o produtor os recusa, e a tabela os listava.
  A tradução passou a recusá-los para esse link: a tabela do `amp` foi de 98 para **70 linhas**,
  com os ganhos 4, 6, 8, 12 e 24.

O operador também viu os canais 1 a 7 "absurdamente instáveis e diferentes" em ganhos baixos,
sem eletrodos. Não é medida de nada: entrada aberta (regra já escrita em `CLAUDE.md`). Em ganho
24 as mesmas entradas encostam no fundo de escala e parecem estáveis por estarem saturadas.

**Depois do conserto, na placa** (segunda instalação do dia): o canal 8 lê **43 °C, estável, em
todos os ganhos**, o ×24 incluído. O traço do firmware mostra o ganho próprio do canal e as
máscaras sem ele:

```
CHnSET 60..54 SENS 7f 7f 00 00    ganho 24: os sete em 24, o sensor em 12
CHnSET 30..34 SENS 7f 7f 00 00    ganho 6:  o sensor em 6
CHnSET 20..24 SENS 7f 7f 00 00    ganho 4:  o sensor em 4
CHnSET 60..60 SENS ff ff ff ff    de fábrica, sem o sensor
```

**Não medido ainda**: a temperatura subir ao aquecer o conversor (critério 2); o ruído com as
entradas em curto (critério 1); o canal 8 num BDF+ gravado na placa; se ligar o sensor muda
alguma coisa nos outros sete canais com eletrodos de verdade.

### 12.10 A taxa de amostragem na tela (2026-10-13)

Pedido do operador: escolher a taxa no painel `Settings`. A taxa era a única chave da §5 que
tinha ficado fora da tabela.

| Peça | Onde | O que mudou |
|---|---|---|
| `acquisition.sample_rate_hz` é a segunda chave de uma escolha | receita do serviço, `do_derive_device_options` | o domínio são as sete taxas do conversor; **cada link recusa, na tradução, as que não entrega**, e uma recusa é uma linha que não existe |
| A taxa da linha vai para o dispositivo reaberto | `eeg-acquisition-service`, `main.cpp` | é a única chave de uma escolha que não é `device.option.*`: todo driver a lê de `DeviceConfig::sampleRateHz` |
| Nome e rótulo (`Sample rate`, `N Hz`) | `eeg-hmi-gui`, `SettingsPanel.qml` | duas linhas; o painel continua mostrando o que a tabela lista |
| Asserção `configure-rate` | `scripts/med-check.py` (37 asserções) | executada no QEMU pedindo 1000: **reprovou**, e tinha razão (abaixo) |

O que cada link oferece, e por quê:

| Link | Taxas na tabela | Linhas | Por quê |
|---|---|---|---|
| `simulated` | 250, 500 | 280 | 1000 reprovou no QEMU, abaixo; 500 ainda não foi medida lá |
| `amp` | **250** | 70 | o firmware converte a 250 (`MED_PRODUCER_RATE_HZ`, `CONFIG1_DR_250`) e não tem chave de taxa |
| `usb` | **250** | 28 | é o teto da ponte (`BRINGUP_AFE.md` §6) |
| `spi` | **250** | 28 | o driver tem `sampling_frequency`; nada foi medido no link |

Na placa, portanto, a tela mostra `Sample rate` com um valor só. É o que os links fazem hoje, e
é a mesma regra que tirou os ganhos 1 e 2 da tabela do `amp`.

**Uma consequência que não é da tela**: como a regra está na tradução, um `eeg.conf` de fábrica
com uma taxa que o link não entrega agora **falha o build**. Antes, `amp` com
`acquisition.sample_rate_hz = 500` construía, o firmware seguia a 250, o driver `rpmsg` declarava
500 (ele copia a taxa da configuração e não confere a do cabeçalho do quadro) e o registro
mentia. Ninguém tinha feito isso; nada impedia.

**Medido** - no host, não numa imagem: o serviço e o `medctl` compilados como estão, contra
substitutos de três cabeçalhos do systemd (o host não tem `libsystemd-dev`), com o `eeg.conf` e a
tabela produzidos pela função da receita executada fora do bitbake. Sessões de 10 s, amostras
contadas do `closed.json` (`frames × 14 / duração`):

| Pedida | Entregue | |
|---|---|---|
| 250 | 250,0 | |
| 500 | 497,9 | uma medida, com o cronômetro do shell |
| 1000 | 999,8 · 999,8 · 999,9 | e **788** na primeira de todas, não reproduzida |
| 2000 | 1996 | |
| 4000 | 3992 | |
| 8000 | 7854 · 7970 | 0,4 a 1,8 % curta |
| 16000 | 8451 · 8453 · 8405 | **metade** |

Em todas, `"dropped": 0`. O laço do serviço leva perto de 600 quadros por segundo neste host, e
um quadro são 14 amostras em qualquer taxa; acima disso o gerador simulado **ressincroniza em vez
de acumular** e a sequência não pula, então o registro declara 16000, contém 8450 e não conta
perda nenhuma. É o defeito da `acq-sample-rate` outra vez, e por isso 8000 e 16000 não são linhas.
De onde vêm os ~1,6 ms por quadro não foi investigado.

| Verificação | Resultado |
|---|---|
| taxa fora do conjunto (`300`), pela linha de comando | recusada: `not offered on this link in this combination` |
| troca com sessão aberta | recusada |
| `metadata.json` da sessão a 1000 | `"sample_rate_hz": 1000.000000`, `prescription_source: operator choice` |
| taxa e ganho no mesmo pedido | uma linha, um `ConfigurationChanged` |
| fábrica fora da tabela (`amp` 500, `usb` 500, `spi` 1000, `simulated` 8000, qualquer link 300) | o build recusa, nomeando a chave |
| `configure-rate`, o comando da suíte rodado contra o serviço do host | `entregue=999`, `TAXA_OK` |
| **Injeção**: a mesma asserção pedindo 16000, com a tabela de antes do limite | `entregue=8269`, sem `TAXA_OK` |
| `make test` | 1573 + 452 + 233 + 117 + 60 verificações, 0 falhas |

**O que isto não significa**:
- que roda numa imagem. O HD dos caches não estava montado, `make service` recusa sem ele (como
  deve), e nem `make qemu` nem `make check` foram executados. A tabela foi produzida pela função
  da receita, mas por um substituto do bitbake;
- que o QEMU entrega o que o host entregou. 4000 amostras por segundo são 286 quadros por
  segundo, num convidado emulado e com a HMI ligada; a `configure-rate` mede 1000, e as outras
  três taxas do simulado estão na tabela pela medida do host;
- que a **tela** foi olhada. O `qmllint` passa e o painel não tem código novo além do nome; o
  traço, os filtros e o medidor de taxa já se reconfiguravam pela taxa do cabeçalho, e isso não
  foi visto com outra taxa;
- nada sobre o `amp`. Ver abaixo.

**O que falta para a placa ter mais de uma taxa** (é a linha "Taxas acima de 250 SPS em `amp`"
da §10, que continua aberta):
1. o firmware: uma chave de taxa na mensagem de controle, o campo DR do `CONFIG1`, a taxa no
   cabeçalho do quadro, e o gerador sintético, que é marcado pelo SysTick de 1 kHz;
2. o driver `rpmsg` conferir a taxa do cabeçalho contra a pedida, em vez de declarar a pedida;
3. medir cada taxa na placa antes de tirar a recusa: o M33 lê o conversor dentro da interrupção
   de DRDY, a 16 kSPS o período é 62,5 µs, e o teto de ~600 quadros por segundo medido acima vale
   para qualquer link.

**No QEMU, no mesmo dia** (`make qemu` verde, `make check` **36/37**): a `configure-rate` pediu
1000 e reprovou com `fora=1 ped=0 declarada=1 entregue=858 volta=0` - a escolha foi aceita, o
registro declarou 1000 e continha 858. As outras 36 passaram, as três `configure-*` anteriores
inclusive, então a chave nova não quebrou as escolhas que já existiam.

A causa é a que a medida do host deixou sem investigar: `MedicalStorage::append` faz `fdatasync`
a cada quadro, e o laço só lê o quadro seguinte depois. São ~1,6 ms no SSD do host e ~16 ms no
volume LUKS do QEMU, isto é, ~61 quadros por segundo, 858 amostras. **O teto de taxa de uma
sessão gravada é o do volume de registros, não o do link**, e a medida do host não dizia nada
sobre o alvo - que é o que a seção acima já avisava e a tabela ignorou ao listar até 4000.

O simulado ficou com **250 e 500** (500 são 36 quadros por segundo; previsão, não medida), e a
asserção passou a pedir 500, com limites de 450 e 550. Reexecutada contra o serviço do host:
passa a 500. **Falta rodar `make check` de novo.**

Duas coisas que isto deixa abertas, e nenhuma é da tela:
- **na placa o teto não é conhecido.** A 250 são 17,9 sincronizações por segundo num cartão SD
  atrás de LUKS, e funciona; quanto sobra não foi medido, e é pré-requisito de qualquer taxa
  maior no `amp`, antes do firmware;
- **o gerador simulado esconde a perda.** Ele ressincroniza sem pular a sequência, então um
  consumidor lento produz um registro curto com `dropped: 0`. Num link real a sequência do
  produtor denuncia o buraco; no simulado só a contagem da suíte vê.

Sincronizar a cada N quadros, ou fazer o quadro crescer com a taxa, levantaria o teto - e muda
o que "gravado" garante depois de uma queda de energia. É decisão, não foi tomada.

### 12.11 A gravação fora do caminho da aquisição, e a taxa no `amp` (2026-10-13)

Decisão do operador depois da §12.10: não manter um `fdatasync` por quadro no laço de aquisição,
e levar as sete taxas ao `amp`.

**A gravação.** `MedicalStorage::openStream` devolve um `RecordStream`: `append` copia o quadro
para uma fila limitada (4 MiB) e volta; uma thread gravadora pega tudo o que está na fila, escreve
e sincroniza **uma vez**, e repete. O agrupamento não é configurado - um produtor lento recebe
uma sincronização por quadro, como antes, e um rápido recebe quantos quadros chegaram enquanto a
anterior rodava. O que uma queda de energia leva é o que estava na fila e no lote em escrita;
nunca um quadro pela metade, porque um lote que o volume não aceitou inteiro é cortado de volta
ao ponto onde começou. Fila cheia é `WouldBlock`, contado: todo quadro entregue é, no
fechamento, escrito, recusado ou falho. `closed.json` só é escrito depois de a fila esvaziar.

| Verificação | Resultado |
|---|---|
| `tests/framework`, seção nova (20 000 registros, ordem, contagem, fila de 8 octetos, continuação, contenção de caminho) | 1600 verificações, 0 falhas |
| **Injeção**: um octeto a menos por lote | 4 falhas |
| **Injeção**: fila sem limite | 4 falhas |
| serviço no host, 10 s por taxa, contado do `closed.json` e do tamanho do `raw.bin` | 250,0 · 999,7 · 3999,6 · 7988 · 15 978 e 15 873 (antes: 8 450 a 16 000) |
| `make qemu`, `make check` | **37/37**, com a `configure-rate` a 1000 - a que tinha dado 858 |

Não foi exercitado: o corte de um lote recusado pelo volume (`ftruncate`), que nenhum teste de
host alcança; e uma queda de energia de verdade.

**O firmware.** Chave `acquisition.sample_rate_hz` na mensagem de controle (ausente: 250); campo
DR do `CONFIG1`; a taxa em vigor no cabeçalho de cada quadro; o gerador sintético com as mesmas
frequências em qualquer taxa, até 1000 (ele é marcado pelo SysTick). Três coisas que a leitura
do código impôs:

- **a mensagem de controle estava cheia** - 8 opções de 8. A receita deixa de enviar
  `afe.input = normal` e `afe.temperature = false`, que o produtor já define como o valor da
  ausência e que nunca são ditas juntas. Cabem 8 sempre, e **não cabe uma nona**: a próxima
  configuração do `amp` exige uma versão nova do formato;
- **uma conversão perdida no M33 encurtava o quadro em silêncio** - fila da interrupção cheia,
  erro de SPI ou palavra de status inválida faziam 14 amostras cobrirem 15 instantes, sem buraco
  na sequência. Agora o quadro em curso é abandonado e a sequência pula
  (`med_producer_on_lost`), e o exportador já conta quadros faltantes. Vale também a 250;
- **a leitura SPI medida (207-259 µs) não cabe acima de 2000 SPS.** De 250 a 2000 o prescaler
  continua 32, o de todas as medidas desta placa; 4000 usa 8, e 8000 e 16000 usam 4. A fila da
  interrupção foi de 32 para 128 conversões.

O driver `rpmsg` passou a comparar a taxa do cabeçalho com a pedida, e falha a leitura se
diferem.

| Verificação | Resultado |
|---|---|
| `tests/m33-producer`: DR das sete taxas contra a Table 13, prescrição, cabeçalho, sinal, conversão perdida | 320 verificações, 0 falhas |
| **Injeções** (cabeçalho sempre 250; perda sem buraco; tom sem escalar; DR invertido; sintético sem teto) | 6, 2, 2, 12 e 2 falhas |
| tabela do `amp`, pela função da receita | 490 linhas, 70 por taxa, no máximo 8 opções por linha |

**O que isto não significa**:
- **o firmware não foi compilado para o M33.** O build foi lançado e interrompido (o HD de
  caches caiu, e a chamada tinha sido feita fora do `make`, sem os caminhos do `local.mk`).
  `afe.c` e `main.c` nunca passaram por um compilador com estas mudanças;
- **nenhuma taxa acima de 250 foi medida no `amp`**, e as sete estão na tabela. É um desvio
  declarado da regra 1 da §5: não há como medir na bancada uma taxa que a tela não oferece. O
  que fica no lugar da medida é que a perda deixou de ser silenciosa. As velocidades de SPI de
  4000 para cima nunca rodaram nesta placa;
- a conferência de taxa do driver `rpmsg` não tem teste: nada no host faz o papel do produtor;
- o simulado lista até 4000 pela medida do host; no QEMU só 1000 foi medida;
- `usb` e `spi` continuam em 250.

**Na bancada, por taxa**: o traço do firmware (`NSPS`, `broken`, `lost`, `read=`) logo depois de
aplicar; e `frames × 14 / duração` do `closed.json` contra a taxa declarada, com o número de
quadros faltantes que o exportador relata. Uma taxa que não se sustenta sai da tabela com o
número.

### 12.12 A primeira taxa na placa: 1000 pedidos, 436 no registro (2026-10-13)

Imagem `amp` da §12.11 gravada no cartão (o firmware compilou; `make stm32` verde). Na tela, 1000
selecionado e o gerador interno ligado.

| Medida | Valor |
|---|---|
| taxa entregue, na tela | ~510 |
| sessão de 20,08 s (`closed.json`) | **625 quadros = 436 amostras/s**, `dropped 0`, `viewer_missed 0` |
| `metadata.json` da mesma sessão | declara 1000 |
| traço do firmware depois de `CONFIG1 94` | **nenhuma linha periódica**, por mais de 20 s |
| `medctl session.configure acquisition.sample_rate_hz=16000` | `the front-end did not acknowledge its settings`; no traço, nenhum `[med] message from` |
| a 250, antes | `read=220us`, `lost=0`, `broken=0` |

**Leitura, não medida**: o laço principal do M33 não dá conta de 1000 conversões por segundo, e
`drain_converter` rodava até a fila esvaziar - então nunca voltou. Daí as três coisas de uma vez:
nenhuma linha de relatório, nenhuma mensagem de controle lida (o pedido de 16000 ficou sem
resposta - **não** foi o SPI mais rápido, que nem chegou a ser tentado), e a contagem de
conversões perdidas, que eu tinha posto **fora** do laço, olhada uma vez e nunca mais. O defeito
que a §12.11 dizia ter fechado estava aberto exatamente no caso em que importa: a sessão
`2026-10-14T00:36:36.914Z` declara 1000, contém 436 e não tem buraco nenhum na sequência. **É um
registro falso sobre o próprio eixo de tempo.**

O que não se sabe: quanto custa uma conversão ao laço, e portanto qual é o teto. A leitura SPI
(220 µs) é da interrupção; o resto - oito conversões para nanovolts em ponto flutuante de dupla
precisão por software, o CRC do quadro bit a bit, o envio - nunca foi cronometrado.

Mudado no firmware, sem ter rodado na placa:
- `drain_converter` processa no máximo dois quadros por passada e volta ao laço, que só dorme se
  a fila esvaziou. Um produtor que não sustenta uma taxa continua atendendo a quem manda parar;
- a perda é conferida **a cada conversão**, dentro do laço;
- o LSB em nanovolts é calculado uma vez por prescrição (`afe_lsb_nv`), e não a cada amostra:
  uma divisão em dupla precisão por software a menos, oito vezes por conversão. O resultado é
  idêntico por construção, e `tests/m33-producer` confere (322 verificações, 0 falhas);
- o traço ganhou `queue=` (o máximo de conversões esperando) e `work=` (microssegundos por
  conversão no laço, numa passada cheia). Com `work=` e `read=` contra o período, uma taxa deixa
  de ser tentativa.

A tabela do `amp` continua com as sete até a próxima sessão de bancada dar os números; sai dela o
que não se sustentar.

**Segunda medida, com o firmware corrigido** (mesma noite). 250 e 500 se sustentam; a 1000 a tela
mostrou ~600 e quadros faltantes "explodindo" - a perda passou a aparecer, que é o que a correção
devia fazer. O traço, lido depois de voltar a 250 (os contadores são acumulados):

```
lost=5919 broken=1636 dropped=0 spi_err=0 read=211..220us work=828us
```

- `work=828us` por conversão no laço, no relógio de parede, com as interrupções dentro;
  `read=` ~220 µs. O período a 1000 são 1000 µs: o laço fica ~10 % aquém, perde uma conversão em
  dez, e cada perda derruba um quadro de 14 - daí os ~40 % a menos na tela;
- `dropped=0`: o rpmsg e o lado Linux não são o gargalo.

Onde os 828 µs vão não foi medido, mas uma parte foi **vista no objeto**: `afe_regs.o` compilado
para o M33 importa `__aeabi_dmul`, `__aeabi_ddiv`, `__aeabi_dadd`, as comparações e as conversões
- este núcleo só tem ponto flutuante de precisão simples, e cada amostra pagava a conversão para
nanovolts em dupla precisão por software.

Mudado, sem ter rodado na placa:
- **conversão em inteiros** (`afe_code_to_nv_exact`): `código × 4,5e9 / (ganho × 2²³)`,
  arredondado para longe do zero. Comparada com a de dupla precisão em **todos os 2²⁴ códigos
  nos sete ganhos: 0 diferentes** - a grade do registro, que o exportador confere, não se move;
- **CRC do quadro por tabela**, montada a partir da definição bit a bit;
- 2000 passa a usar o prescaler 8 (com 220 µs de leitura sobrariam 280 para o laço);
- `tests/m33-producer`: 322 verificações, 0 falhas; injeções (CRC por tabela errado, inteiro
  truncando) reprovam 3 e 1.

**A tabela do `amp` ficou com 250, 500, 1000 e 2000** (280 linhas). 4000, 8000 e 16000 saíram
pelos números: período de 250 µs ou menos, contra 220 de leitura mais um laço que hoje custa
várias vezes isso. 1000 e 2000 estão lá para serem medidas de novo, e saem se não se
sustentarem.

### 12.13 As sete taxas no `amp`: decisão, e a causa da lentidão (2026-10-13)

**Decisão do dono do dispositivo**: o link `amp` existe para usar o conversor inteiro, e as sete
taxas ficam na tabela. Uma taxa que não se sustenta é para ser feita sustentar, não retirada. A
poda da §12.12 (4000 para cima) foi desfeita: 490 linhas outra vez.

**A causa, lida no código e na árvore da ST.** O firmware é ligado na DDR
(`stm32mp2xx_DDR_m33_ns.ld`) e rodava **sem cache de instrução nem de dados**. O exemplo OpenAMP
da ST para esta placa usa o mesmo script e é compilado com `ICACHE_DCACHE_USE` (no `.cproject`);
este firmware copiou as definições daquele exemplo e deixou essa de fora. `enable_caches()` em
`main.c` é o `MPU_Config()`, o `MX_DCACHE_Init()` e o `MX_ICACHE_Init()` daquele exemplo, na
ordem e com os números dele. A região compartilhada com o Linux (onde ficam a tabela de recursos,
o buffer do traço e os anéis do rpmsg) é marcada **não cacheável**, como lá. Uma etapa que falha
é relatada no traço (`[med] caches: data ..., instruction ...`) e o firmware segue sem aquele
cache.

| Verificação | Resultado |
|---|---|
| `bitbake med-m33-firmware` | compila sem avisos, assinado; as mensagens novas estão no binário |
| `tests/m33-producer`, `make test` | 322 verificações, 0 falhas; as demais suítes sem mudança |

**O que isto não significa**: que funciona, nem quanto rende. Não rodou. O que se sabe medir:
- `[med] caches:` na partida diz se os dois ligaram;
- `work=` e `read=` contra o período dizem se a taxa cabe: 1000, 500, 250, 125 e **62,5 µs**;
- a 250 e 500, que já se sustentavam, nada pode piorar - são a regressão desta mudança.

**O orçamento a 16 000**, para não prometer o que não foi medido: 62,5 µs por conversão para a
leitura SPI e o laço juntos, contra 220 + ~650 de hoje. Os caches são a parte grande, e não sei
de quanto. Se não bastarem, o que vem depois, nesta ordem: (1) a leitura SPI por registrador,
sem o HAL, e no relógio mais alto que o conversor aceita; (2) tirar a conversão para nanovolts
do M33, o que muda o formato do quadro; (3) 1143 mensagens rpmsg por segundo, que ninguém mediu.

**Na placa, com os dois caches ligados: o firmware não anuncia o endpoint.** Cartão regravado. O
traço mostra `caches: data on, instruction on`, o conversor respondendo (`ID 0x3e`), `source:
converter` - e mais nada: nem `endpoint announced`, nem linha periódica, nem linha de falta. No
Linux, `rpmsg host is online` e nenhum `creating channel`; `/dev/med-amp0` não existe, e o
serviço de aquisição reinicia sem parar (`NRestarts=19`), que é o "unavailable" da tela.

**A causa não é conhecida.** O que foi conferido e está onde devia: a tabela de recursos em
`0x81200000` e o buffer do traço em `0x8120008c` (mapa do ELF), os anéis em `0x812f8000` e
`0x812f9000` e os buffers em `0x812fa000` (devicetree) - tudo dentro da região que a MPU marca
como não cacheável. O exemplo da ST foi portado sem que ninguém aqui o tenha visto rodar com os
caches: "o exemplo da ST carrega" era evidência sobre o exemplo **como o meta-st o compila**, e
eu não conferi se aquela compilação define `ICACHE_DCACHE_USE`. É a regra 1 do
`BRINGUP_STM32MP2.md` §11, cobrada outra vez.

O que mudou, para não custar um cartão por hipótese:
- `MED_M33_CACHES` (0 nenhum, 1 dados, 2 instrução, 3 os dois) é número de build. **O firmware
  que o dispositivo sobe é o 0**, o de todas as medidas anteriores;
- a receita constrói os quatro e instala os outros três ao lado, como
  `med-m33-producer-c1/-c2/-c3_sign.bin`, para serem carregados **à mão** pelo sysfs do
  remoteproc. Desfazer é reiniciar. É andaime de bancada e está rotulado assim na receita;
- a partida imprime uma linha a cada passo de subir o link (`[med] link: ...`), para o próximo
  traço dizer onde para.

**Não verificado**: a receita com os quatro builds não passou pelo bitbake (o HD não estava
montado).

### 12.14 As sete taxas no `amp`, medidas na placa (2026-10-13)

O operador mandou entrar na placa por SSH e resolver. O HD de caches estava desconectado, então
nada disto passou pelo bitbake: o firmware foi compilado à mão (o `CMakeLists.txt` da receita, a
árvore Cube tirada do espelho git em `downloads/`, o `arm-none-eabi-gcc` 13.2 do host), assinado
com o script e a chave padrão do OP-TEE que já estavam em `sysroots-components`, copiado para
`/run/fw` e carregado pelo sysfs do remoteproc com `firmware_class.path` apontado para lá. Cada
tentativa custou cerca de um minuto e nenhum cartão.

**O que cada variante fez**, na ordem em que foi carregada:

| Variante | Resultado |
|---|---|
| sem cache, com conversão em inteiros e CRC por tabela | **1000 se sustenta** (`lost=0`, fila ≤ 6). `read=224us` |
| idem, leitura da conversão com prescaler 8 a 2000 e 4000 | o primeiro registrador lê `7f` onde se escreveu `ff`: recusado. Só a leitura em prescaler 8: **toda** palavra de status inválida, e `read=` continuou 222 µs |
| só cache de instrução | sobe, e não muda nada: `read=241us` |
| só cache de dados, com as três regiões de MPU do exemplo da ST | para dentro da criação do endpoint, sem linha de falta - o mesmo da imagem da §12.13 |
| idem, região compartilhada como memória de dispositivo | para no mesmo lugar |
| cache de dados, **sem MPU** | `read=59us`, CPU sobra a 4000 - e 16 quadros enviados em 10 s, `dropped` aos milhares: os anéis do rpmsg ficaram em cache e os buffers devolvidos pelo Linux não eram vistos |
| cache de dados, MPU descrevendo **só** o megabyte compartilhado, não cacheável | **funciona.** É o que ficou |
| idem, a 16000 com a leitura em prescaler 32 | a interrupção roda emendada: 7 conversões em 8 perdidas, todo quadro abandonado. O firmware seguiu respondendo e relatando |
| idem, leitura em prescaler 16 só a 16000 | `read=33us`, status válido, **16000 se sustenta** |

Por que as outras duas regiões da ST param este firmware **não se sabe**: foram retiradas, não
entendidas. Idem o prescaler 8, e o relógio do barramento, que continua sem ter sido lido.

**A medida**, com o código final (um build só, sem variantes): uma sessão de 12 s em cada taxa,
pelo serviço, com a HMI ligada.

| Taxa | Quadros | Amostras/s no registro | `dropped` | faltantes | CRC | BDF+ |
|---|---|---|---|---|---|---|
| 250 | 216 | 249,5 | 0 | 0 | 0 | convertido |
| 500 | 430 | 498,8 | 0 | 0 | 0 | convertido |
| 1000 | 861 | 997,9 | 0 | 0 | 0 | convertido |
| 2000 | 1721 | 1994,7 | 0 | 0 | 0 | convertido |
| 4000 | 3443 | 3989 | 0 | 0 | 0 | convertido |
| 8000 | 6883 | 7978 | 0 | 0 | 0 | convertido |
| 16000 | 13762 | 15964 | 0 | 0 | 0 | convertido |

A duração é a do `closed.json`, que inclui abrir e fechar a sessão; os 0,2 % a menos são disso.
Repetida com o build final: 215, 431, 861, 1721, 3444, 6882 e 13773 quadros, mesmos zeros.
No M33 a 16000: `lost=0 broken=0 dropped=0 read=34us queue=31 work=51us`, sobre ~500 mil
conversões. `NRestarts=0` do começo ao fim.

**O que isto não significa**:
- **a 16000 a folga é pequena**: a fila chegou a 31 de 128 e a passada do laço a 51 µs de um
  período de 62,5. Doze segundos numa placa ociosa não são uma sessão de uma hora com os A35
  carregados - e a leitura já foi vista 18 % mais lenta sob carga (§9.16). Não medido;
- o gerador interno e eletrodos não foram olhados em nenhuma taxa acima de 250: mediu-se a
  contagem, não o sinal. A banda declarada (`prefiltering`) vem da tabela do datasheet;
- o que a tela faz a 1143 quadros por segundo não foi olhado; `viewer_missed` foi 0;
- **o binário que rodou não é o que a receita vai produzir**: outro compilador, mesma fonte. O
  `bitbake med-m33-firmware` com esta fonte ainda não foi feito;
- **a placa está assim só até o próximo boot.** O cartão continua com o firmware da §12.13, que
  não anuncia o endpoint. A correção definitiva é a imagem ou o pacote, com o HD;
- ficaram em `/data` catorze sessões de teste `taxaNNN`, que são registros e não foram apagadas.

O andaime de bancada da §12.13 (quatro builds, `MED_M33_CACHES`) saiu: a receita voltou a ser a
do repositório, e o firmware é um build só, com o cache de dados.

**Com o binário da receita** (mesma noite, HD reconectado). `make stm32 KEY=development KERNEL=med`
verde, 6578 tarefas; `med-m33-firmware` compilado pelo bitbake com a fonte final, sem avisos. O
`_sign.bin` do `deploy` foi carregado ao vivo na placa e a varredura repetida:

| Taxa | Quadros em 12 s | `dropped` | faltantes | CRC | BDF+ |
|---|---|---|---|---|---|
| 250 | 216 | 0 | 0 | 0 | convertido |
| 500 | 431 | 0 | 0 | 0 | convertido |
| 1000 | 862 | 0 | 0 | 0 | convertido |
| 2000 | 1724 | 0 | 0 | 0 | convertido |
| 4000 | 3447 | 0 | 0 | 0 | convertido |
| 8000 | 6886 | 0 | 0 | 0 | convertido |
| 16000 | 13770 | 0 | 0 | 0 | convertido |

`NRestarts=0`. Isto fecha a diferença de compilador da tabela anterior. **A imagem em si não foi
gravada nem vista subir sozinha**: o que rodou foi o firmware dela, carregado à mão.

Dois erros meus nesta sessão, para não repetir:
- a primeira varredura com este binário foi interrompida porque o operador desligou a placa ao
  ver a tela gravando e trocando parâmetros sozinha. Eu não tinha avisado que ia rodar sessões;
- os roteiros de bancada tinham `remoteproc1` escrito. No boot seguinte o M33 era o
  `remoteproc0` e o `remoteproc1` era o M0: a carga foi dirigida ao núcleo errado, travou, e a
  placa reiniciou nesse intervalo (a causa do reinício não foi apurada). É a regra "um nome de
  dispositivo é sempre uma corrida" do `BRINGUP_STM32MP2.md` §11, que o `med-m33-load` da
  própria imagem respeita, achando o remoteproc pelo `name`, e eu não.

