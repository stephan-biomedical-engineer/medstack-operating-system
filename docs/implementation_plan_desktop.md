# Plano de implementação — o ambiente de trabalho da estação: labwc, painel e gerenciador de arquivos

> **Status**: **nada aqui foi implementado.** Nenhuma receita escrita, nenhum compositor além do
> weston jamais rodou nesta placa.
>
> **Escopo**: um ambiente de trabalho na estação de bancada - janelas que minimizam e voltam por
> uma barra de tarefas, um gerenciador de arquivos, a HMI e o revisor como aplicativos comuns.
>
> **Não é** a tela do produto. O perfil de tela única do `implementation_plan_hmi_desktop.md`
> (a HMI sem moldura, o revisor a pedido) continua existindo e é o que uma imagem de produto usa.
>
> **Por que existe**: `implementation_plan_hmi_desktop.md` §12.5. Com as duas janelas minimizadas
> não havia volta, porque o weston não tem lista de janelas; e ele caiu com SIGSEGV quando um
> cliente foi encerrado com um menu aberto.

---

## 1. A decisão, e as duas rotas que ficaram de fora

| Rota | Estado | Por quê |
|---|---|---|
| **labwc + painel + PCManFM** | **escolhida** (2026-10-11) | Wayland puro, cabe no slot de 1 GiB, pouco software de terceiros, e tem saída por software se a GPU não cooperar |
| **GNOME** (`meta-gnome`) | **backlog**, trabalho futuro | ver §1.1 |
| **XFCE** (`meta-xfce`) | recusada | exige X11, onde qualquer janela lê o teclado e a tela das outras: acaba com a separação entre quem mostra e quem revisa |

### 1.1 GNOME, para quando for retomado

Lido nas receitas da `meta-gnome` deste repositório (Scarthgap), não suposto:

- `mutter`, `gnome-shell`, `nautilus`, `gdm` e `gnome-settings-daemon` têm `x11` em
  `REQUIRED_DISTRO_FEATURES`, e `med-os.conf` o remove. Reativá-lo é política do MedOS: vale para
  todos os perfis e as duas máquinas, e recompila quase tudo;
- o GNOME vive na sessão de um usuário logado, com o socket do compositor em `/run/user/<uid>`.
  Unidades de sistema com outros usuários não chegam a ele: o modelo de identidades teria de ser
  refeito, e passaria a existir um usuário com login automático;
- o rootfs já ocupa 748 MB de um slot de 1 GiB; `gtk4`, `gjs`/`mozjs`, `evolution-data-server`,
  `pipewire`, `networkmanager` não cabem. Os slots cresceriam, o layout mudaria e o cartão teria
  de ser regravado;
- os cinco consertos de vídeo da placa estão escritos para o weston e teriam de ser refeitos para
  o `mutter`, que não tem modo por software utilizável com o driver proprietário;
- o `gnome-shell` sem GPU não cabe nos 1024 MiB do QEMU.

---

## 2. O que existe e o que tem de ser escrito

Conferido nas camadas em `layers/`:

| Componente | Papel | Receita |
|---|---|---|
| `labwc` | compositor de janelas empilhadas, com minimizar e o protocolo que uma barra de tarefas usa | **escrever** |
| `wlroots` | a biblioteca sobre a qual o labwc é feito | **escrever** |
| `libdisplay-info` | dependência do wlroots (EDID) | **escrever** |
| painel com barra de tarefas (`sfwbar`) | lista de janelas, relógio, lançadores | **escrever** |
| `gtk-layer-shell` | dependência do painel | **escrever** |
| gerenciador de arquivos | — | **em aberto, ver §2.1** |
| `gtk+3`, `adwaita-icon-theme`, `hwdata`, `seatd`, `json-c` | bases | existem |

Cinco receitas nossas, mais o que o gerenciador de arquivos pedir. A `meta-wayland`, que as tem prontas, **não tem branch para o Scarthgap**
(`nanbield` e `master` apenas, conferido em 2026-10-11) - usá-la seria fixar uma camada de outra
série. A versão do labwc segue a do wlroots: 0.7.x com wlroots 0.17, 0.8.x com 0.18; a escolha
é do ensaio, pelo que compilar contra o `wayland-protocols` 1.33 desta série.

### 2.1 O gerenciador de arquivos não é o que se supôs

O plano nasceu dizendo "PCManFM, que já existe no poky". Existe, e **não serve como está**: a
receita `pcmanfm_1.3.2.bb` tem `REQUIRED_DISTRO_FEATURES = "x11"`, por causa do
`startup-notification`, e o próprio programa usa Xlib para desenhar a área de trabalho. Lido na
receita em 2026-10-11, depois de a rota já ter sido escolhida - o mesmo erro de método que o
plano do front-end registra: concluir sobre uma receita sem abri-la.

| Candidato | Situação, lida nos fontes e nas receitas em 2026-10-11 |
|---|---|
| **PCManFM-Qt** 2.1.0 (LXQt) | **Não constrói sem X11.** `libfm-qt` tem `find_package(XCB REQUIRED)` (até a 2.4.0) e inclui `<xcb/xcb.h>`; `pcmanfm-qt` inclui `<X11/Xlib.h>` e usa `QNativeInterface::QX11Application`, que só existe num Qt construído com o plugin xcb. O `libxcb` desta série tem `REQUIRED_DISTRO_FEATURES = "x11"`. Roda em Wayland, mas é **compilado** contra o X. E a cadeia são seis receitas, não três: `lxqt-build-tools`, `lxqt-menu-data`, `libfm-qt`, `extra-cmake-modules`, `layer-shell-qt`, `pcmanfm-qt`, mais `qttools-native` |
| **Thunar** 4.19 (`meta-xfce`) | A receita aceita Wayland, mas depende de `xfce4-panel`, cujo `DEPENDS` tem `virtual/libx11`. E habilitar a `meta-xfce` traz, por `LAYERDEPENDS`, a `meta-gnome` e a `meta-multimedia` |
| PCManFM (GTK), do `poky` | `REQUIRED_DISTRO_FEATURES = "x11"` e Xlib no próprio programa |
| Nautilus (`meta-gnome`) | `REQUIRED_DISTRO_FEATURES = "x11 opengl …"` e a cadeia do GNOME |

**Nenhum gerenciador de arquivos de uso geral desta série constrói, como empacotado, sem as
bibliotecas do X11.** É a segunda vez neste plano que um candidato foi nomeado antes de os seus
arquivos de build serem abertos - o PCManFM primeiro, o PCManFM-Qt depois, este dito "Wayland
nativo" e de "três receitas". As duas afirmações eram falsas e foram feitas ao operador.

O que resta são quatro caminhos, e a escolha é de projeto:

| Caminho | O que é | Custo |
|---|---|---|
| **A. Um navegador de registros próprio** | programa pequeno em Qt, deste repositório: lista as sessões da árvore de revisão, abre uma no EDFbrowser, copia para um pendrive | código nosso, sem SOUP e sem biblioteca do X. **Não é um gerenciador de arquivos de uso geral** |
| B. PCManFM-Qt com o X11 retirado por *patch* | `#ifdef` em `libfm-qt` e `pcmanfm-qt`, mantidos por nós a cada versão | seis receitas e dois *forks* de fato |
| C. Bibliotecas clientes do X na imagem, sem servidor | `libxcb`/`libX11` e um Qt com xcb, sem Xorg e sem Xwayland | exige `x11` nas `DISTRO_FEATURES` (política do MedOS, recompila tudo), ou derrubar a exigência receita por receita. "Sem X11" vira "sem servidor X" |
| D. Thunar sem o painel | receitas próprias ou `bbappend`, se o Thunar e a `libxfce4ui` 4.19 fecharem sem X | não verificado; três camadas a mais no parse |

### 2.2 Onde isso mora

Numa **camada adjunta**, `meta-med-desktop`, pelo mesmo argumento da `meta-med-afe-ads1299`: é
conteúdo de um perfil, fica fora das quatro camadas e fora da métrica de reuso, provê e não
consome. "Um ambiente de trabalho custa uma camada adjunta e zero linhas nas quatro camadas da
plataforma" é o resultado a medir - e só é verdade se a §2.3 for resolvida por variável.

### 2.3 O que tem de mudar nas quatro camadas, uma vez

`packagegroup-med-gui` nomeia `weston` e `weston-init`. Dois compositores na mesma imagem são
duas unidades disputando a mesma tela. O grupo passa a instalar o que uma variável disser
(`MED_GUI_COMPOSITOR`, padrão o de hoje), no formato de `MED_GPU_PACKAGES`; a camada adjunta
responde por ela no perfil de estação. É a única edição prevista fora da camada adjunta.

---

## 3. As decisões de projeto que o ensaio tem de responder

| Pergunta | Por que não é detalhe |
|---|---|
| **Como três usuários chegam ao socket do compositor?** | O weston recebe o socket do systemd (`weston.socket`, grupo `wayland`). O wlroots não aceita socket herdado: cria o seu em `XDG_RUNTIME_DIR`. O diretório de runtime do compositor teria de ser legível pelo grupo, e isso precisa ser visto funcionar |
| **Quem é o usuário do ambiente?** | Um gerenciador de arquivos lançado pelo painel roda como o usuário do compositor. Para ele abrir um BDF+, esse usuário entra em `medreview` - a objeção da §4.1 do plano da HMI. A saída coerente: no perfil de estação, **o usuário do ambiente é o revisor**; continuam separados quem adquire e quem mostra ao vivo. São três identidades de novo, com outro recorte, e isso tem de ser escrito na tese como decisão |
| **A HMI continua unidade do sistema?** | Sim: `eeg-hmi.service`, com o seu usuário, aparece como uma janela a mais. O EDFbrowser, no ambiente, passa a ser lançado pelo painel; `med-review.service` e o pedido ficam para o perfil de tela única |
| **GPU ou software?** | O renderizador GLES2 do wlroots nunca rodou sobre o driver Vivante desta placa. Com o renderizador por software (`pixman`) o compositor sobe de qualquer jeito, mas clientes acelerados podem não conseguir entregar os seus quadros - e a HMI voltaria a custar CPU |
| **Onde o modo de vídeo é fixado?** | Hoje no `weston.ini`. No labwc seria um comando na partida da sessão. As duas variáveis do `meta-med-bsp` continuam valendo; muda quem as lê |

---

## 4. Fases

### Fase D0 — o ensaio, no QEMU

**Objetivo**: saber se o ambiente existe, antes de tocar na placa.
**Pré-requisito**: nenhum.
**Procedimento**: as cinco receitas; o labwc como unidade do sistema sob o `seatd`, usuário sem
privilégio; painel; o gerenciador de arquivos que a §2.1 escolher; a HMI e o EDFbrowser como
janelas.

**Critérios**:
1. minimizar a HMI e o EDFbrowser e trazê-los de volta **pela barra de tarefas, com o mouse**;
2. o gerenciador de arquivos abre a árvore de revisão e não abre `/data/eeg`;
3. encerrar à força um cliente com um menu aberto **não derruba o compositor** - a ocorrência do
   weston, refeita de propósito;
4. `make check` passa, com as asserções da tela apontando para o compositor novo.

**Injeção de falha**: matar o compositor. A aquisição tem de continuar na mesma sessão, e a HMI
tem de voltar sozinha - o que já foi visto por acidente e passa a ser asserção.
**O que uma reprovação significa**: se o critério 1 falhar, a rota inteira falhou, e a decisão
volta para a §1 antes de se gastar uma sessão de bancada.

### Fase D1 — as identidades

**Objetivo**: a §3, linhas 1 a 3, medida tentando.
**Critério**: como o usuário do ambiente, o BDF+ abre e o bruto não; como `med-hmi`, nenhum dos
dois; o processo da HMI não é do usuário do ambiente.

### Fase D2 — a placa

**Objetivo**: o mesmo ambiente no HDMI.
**Critérios**: imagem no monitor em um boot, sem monitor no boot inclusive; modo CEA programado;
`NRestarts=0` aos 5 min; **CPU da HMI ≤ 25% de um núcleo** (hoje ≈ 19%).
**O que uma reprovação significa**: a estação fica no weston, e o ambiente fica no QEMU como
resultado parcial declarado. O weston não é removido de nada até esta fase passar.

### Fase D3 — o acabamento

Tema, ícones, o que o painel mostra, o relógio de uma placa sem RTC (hoje removido por estar
errado), e os lançadores.

---

## 5. Riscos

| Risco | O que se sabe |
|---|---|
| wlroots sobre o driver proprietário | nada medido; cada um dos cinco defeitos de vídeo desta placa só apareceu na bancada |
| cinco receitas de terceiros mantidas por este repositório | cada uma é SOUP a justificar, e dívida no sentido da regra 1 do `BRINGUP_STM32MP2.md` §11 |
| o ambiente dá ao operador um gerenciador de arquivos | o rootfs é somente-leitura e `/data/eeg` é `0700`, mas `/etc` é legível: nada secreto pode morar lá. A chave de `/data` mora em `med-boot`, `0400` - a conferir de novo com um usuário interativo |
| memória no QEMU | 1024 MiB já foram o limite uma vez (`BRINGUP_HMI_QEMU.md`) |

---

## 6. Validação

*(Vazio de propósito.)*

| Fase | Critério | Resultado | Data |
|---|---|---|---|
| D0 | minimizar e restaurar pela barra, no QEMU; compositor sobrevive a um cliente morto com menu aberto | **parcial** — §6.1: minimizar e restaurar pela barra **funcionou, com o mouse**; o resto em aberto | 2026-10-11 |
| D1 | alcance por identidade, medido tentando | — | — |
| D2 | imagem no HDMI; HMI ≤ 25% de um núcleo | — | — |
| D3 | — | — | — |

### 6.1 Fase D0 — o que o ensaio mostrou (2026-10-11)

QEMU, `MED_GUI_COMPOSITOR = "med-desktop-session"`, sem weston na imagem.

| O quê | Como | Resultado |
|---|---|---|
| As receitas compilam | `make qemu` | `libdisplay-info`, `wlroots`, `labwc`, `gtk-layer-shell`, `sfwbar`, `grim`: todas, 6261 tarefas. O `labwc` precisou de um ajuste de empacotamento (dois ícones) |
| O compositor sobe sem privilégio | `systemctl show med-desktop` | `active`, `NRestarts=0`, como `med-desktop` (uid 1202), pelo `seatd` |
| Renderizador | diário do `labwc` | o GLES2 recusa o EGL por software do QEMU e o wlroots cai sozinho para o `pixman`. **Fato do QEMU, que não tem GPU**; não diz nada sobre a placa |
| O contrato do compositor | `ls -la /run/med-desktop` | socket `med-desktop:wayland 0775`; `med-compositor.target` alcançado antes da HMI |
| A HMI dentro do ambiente | `make check` | `hmi-active` e `acq-sample-rate` passam |
| A suíte inteira, neste perfil | `make qemu && make check`, com as duas correções abaixo | **28/28** (6261 tarefas). Antes delas, 27/28 em duas execuções: a 28ª era a asserção com a corrida |
| O revisor, com outro usuário | leitura de `/proc/<pid>/status` 1 s e 7 s após o pedido | uid 1201 nas duas, unidade ativa |
| A tela | `grim`, trazido pelo console em hexadecimal | janela do EDFbrowser com a moldura do compositor e três botões; barra com "MedPlatform EEG" e "EDFbrowser" |
| **Critério 1: minimizar e restaurar pela barra, com o mouse** | operador, por VNC (`BRINGUP_HMI_QEMU.md` §4) | **funcionou** |
| Critério 3: cliente encerrado, compositor de pé | operador: `systemctl stop med-review`, depois `NRestarts` | `NRestarts=0` — **sem confirmação de que havia um menu aberto**, que é a condição do critério |

Três defeitos que o ensaio encontrou, e nenhum era do ambiente:

- **A unidade da HMI nomeava o weston** (`Requires=weston.service weston.socket`), e um *drop-in*
  não retira um `Requires=`. Com outro compositor ela não partia: *"Unit weston.socket not
  found"*. Virou o contrato da §2.3 - alvo, arquivo de ambiente, grupo `wayland`.
- **A asserção `review-active` lia a identidade cedo demais.** `Type=simple` fica "ativo" no
  fork, e o filho ainda é root até trocar de usuário: um revisor correto apareceu com `uid=0`.
  Passava por sorte. Agora relê até coincidir, por no máximo 10 s.
- **A barra ignorou a configuração entregue** e usou a de fábrica, com relógio, menu e medidores,
  e um erro `can't execute 'python3'` do menu. Passa a ser iniciada com o arquivo nomeado.

**O que isto não significa**:
- a barra **vista** pelo operador era, até onde se sabe, a de fábrica: a imagem com a
  configuração nomeada foi construída depois e ninguém olhou a tela dela;
- o critério 2 (gerenciador de arquivos) não tem o que medir: nenhum foi escolhido (§2.1);
- o critério 4 e a injeção de falha (matar o compositor) não foram executados;
- nada disto é a placa. O caminho do weston, que ela usa, foi alterado pelo contrato do
  compositor e **não foi inicializado desde então** em lugar nenhum.

