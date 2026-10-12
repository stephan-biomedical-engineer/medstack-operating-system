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
| D1 | alcance por identidade, medido tentando | **cumprido no QEMU** — §6.2: o usuário do ambiente lê o BDF+ e não o bruto, não escreve; a HMI não é dele | 2026-10-12 |
| D2 | imagem no HDMI; HMI ≤ 25% de um núcleo | **parcial** — reprovada duas vezes (§6.3, §6.4) e depois destravada por `implementation_plan_desktop_gpu_clients.md`: imagem no HDMI em `1920x1080@50`, HMI a 18% de um núcleo dentro do ambiente, barra e `Files` vistos pelo operador. Boot sem monitor e `NRestarts` aos 5 min não medidos | 2026-10-12 |
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
- o critério 2 (gerenciador de arquivos) não tinha o que medir nesta data: nenhum fora escolhido
  (§2.1). Foi medido depois, na §6.2;
- o critério 4 e a injeção de falha (matar o compositor) não foram executados;
- nada disto é a placa. O caminho do weston, que ela usa, foi alterado pelo contrato do
  compositor e **não foi inicializado desde então** em lugar nenhum.

### 6.2 O gerenciador de arquivos e a Fase D1 (2026-10-12)

Caminho **B** da §2.1, escolhido pelo operador: PCManFM-Qt 2.1.0 com o X11 retirado por *patch*.
QEMU, mesma imagem da §6.1 mais seis receitas.

| O quê | Como | Resultado |
|---|---|---|
| A cadeia compila sem X11 | `make qemu` | `lxqt-build-tools`, `lxqt-menu-data`, `extra-cmake-modules`, `layer-shell-qt`, `libfm-qt`, `pcmanfm-qt`. Os dois *patches* só removem: 60 linhas na `libfm-qt`, 16 no `pcmanfm-qt`. Cada receita relê os fontes depois do *patch* e recusa o build se sobrar um `#include` do X |
| O que a imagem ganhou | manifesto | 228 → 237 pacotes |
| O lançador da barra | clique em "Files" (QMP `input-send-event`), `screendump` | janela com a moldura do compositor, ícones do tema, entrada na barra |
| Um programa só | três cliques, `ps` | um processo `pcmanfm-qt`, três janelas |
| Sem registro ainda | clique numa imagem recém-gravada | abre a pasta do usuário, vazia |
| Com registro | `medctl session.start` / `session.stop`, clique | abre `/data/review/eeg`; o BDF+ aparece com o ícone e o tipo "European Data Format 24-bits" |
| **Duplo clique abre o registro** | duplo clique, `ps`, `screendump` | `edfbrowser /data/review/eeg/<sessão>.bdf`, 8 sinais `EEG n+-n-` a 250 Hz, 12 s, as três anotações da sessão |
| Identidade | `/proc/<pid>/status` | gerenciador e revisor: uid 1202, grupos `input video wayland medreview`. HMI: outro usuário |
| **Alcance, medido tentando** | asserção `desktop-reach` | `bdf=8 bruto=NEGADO escrita=NEGADO` |
| **A asserção vê a falha** | `medreview` retirado da unidade, `make qemu && make check` | **34/35**, só `desktop-reach`: `grupos=[video input wayland] bdf=0 bruto=NEGADO escrita=NEGADO`. Restaurado: 35/35 |
| A suíte inteira | `make check` | **35/35** |
| Memória | `ps -o rss` | gerenciador 53 MiB, revisor 61 MiB, numa máquina de 1 GiB |

Quatro defeitos, e nenhum falhou um build:

- **Sem barramento de sessão o gerenciador não abre e não diz nada.** Ele decide se é o primeiro
  de si mesmo reivindicando um nome no D-Bus; sem barramento a reivindicação falha, ele conclui
  que já existe outro, pede a esse outro - que não existe - uma janela, e sai com código zero.
  Lido no fonte antes de ser visto (`application.cpp`, o construtor); a sessão passou a iniciar
  um `dbus-daemon --session` num caminho fixo. É também o que faz três cliques serem um programa.
- **O tema de ícones padrão do programa não está na imagem** (`elementary`, depois `oxygen`). A
  estação entrega o perfil `default` com o tema que a imagem tem.
- **O tipo era reconhecido e o duplo clique perguntava "com qual programa?"** A definição do
  tipo estava instalada e compilada; o que faltava era o índice de tipo para programa
  (`mimeinfo.cache`), que a GLib exige para associações declaradas no `.desktop`. O QA do
  bitbake tinha avisado (`mime-xdg`), e foi lido como aviso. Visto na tela, corrigido, visto de
  novo.
- **A pasta inicial era o diretório de runtime**, e o gerenciador mostrava o socket do
  compositor e o do barramento como dois arquivos para clicar. O usuário ganhou uma pasta
  própria ao lado, igualmente volátil.

Duas decisões que isto fixa:

- **O revisor se descreve; o ambiente não o nomeia.** A entrada de menu, a definição dos dois
  tipos e o ícone saem da receita do `edfbrowser`, na camada de aplicação. A camada adjunta do
  ambiente não tem o nome dele em lugar nenhum, e declara o grupo `medreview` pelo número da
  distro - o mesmo par de metades do grupo `wayland`.
- **No perfil de estação o usuário do ambiente é o revisor** (§3), e o registro fica
  inalcançável para escrita por construção: `ProtectSystem=strict` na unidade do compositor
  vale para tudo o que a barra inicia. O gerenciador não apaga, não renomeia e não copia para
  fora. Copiar um registro para um pendrive **não existe** nesta entrega.

**O que isto não significa**:
- a injeção de falha cobre uma perna das três de `desktop-reach` (a leitura do BDF+). As duas
  recusas nunca foram vistas falhar;
- uma execução da suíte, logo depois de restaurar a unidade, **não chegou ao prompt de login**
  ("timeout aguardando prompt de login"). A execução seguinte, sobre a mesma imagem, deu 35/35.
  A causa não foi procurada; a hipótese é a máquina virtual anterior ainda encerrando, e é só
  hipótese;
- os cliques foram injetados pelo monitor do QEMU, não por uma pessoa com um mouse; o operador
  ainda não viu esta tela;
- nada disto é a placa, que continua no weston e na imagem anterior (Fase D2);
- a barra de ferramentas do gerenciador tem botões sem ícone ("Go Back", "Go Up" em texto) -
  o tema da imagem não tem os nomes que ele pede. É acabamento, Fase D3;
- o gerenciador mostra o sistema de arquivos inteiro, somente leitura, com as permissões do
  usuário do ambiente. O que esse usuário **não** pode ler foi medido para o registro bruto e
  para nada mais;
- a 1280x800 a faixa inferior da HMI sobrepõe "Review records" e "Settings" aos controles de
  exibição. Não se vê a 1920x1080, que é o modo da placa; visto aqui pela primeira vez e não
  corrigido.

Evidência: `build/med-bench/20261012-desktop-files/`, quatro capturas.

### 6.3 Fase D2 — a placa (2026-10-12): reprovada

`MED_GUI_COMPOSITOR = "med-desktop-session"` no perfil da placa, `make bundle BOARD=stm32
KEY=development KERNEL=med LINK=amp`, entregue por `rauc install` no slot B. Operado pelo
operador; os números abaixo são do que ele colou.

| O quê | Como | Resultado |
|---|---|---|
| As doze receitas compilam para aarch64 | o bundle e o manifesto da imagem | compilam; a imagem tem `labwc`, `sfwbar`, `pcmanfm-qt`, `med-desktop-session` e não tem weston |
| O compositor sobe | `systemctl`, diário | `med-desktop` ativo, `NRestarts=0`, socket `med-desktop:wayland 0775` |
| **Renderizador** | diário do `labwc` | `Failed to query EGL devices`, `eglQueryDeviceStringEXT(EGL_EXTENSIONS) failed`, `Could not initialize EGL`: o GLES2 do wlroots **não inicializa sobre o EGL desta GPU** e o compositor fica em software |
| Acesso ao nó da GPU | `ls -la` | `/dev/galcore` `root:video 0660`, `/dev/dri/card0` `root:video 0660` - não é permissão |
| **A HMI** | diário de `eeg-hmi` | **SIGSEGV a cada ~4 s**, `NRestarts=15` em 67 s, sem uma linha de erro antes do sinal |
| **O monitor** | operador | **tela preta, sem sinal no HDMI** |
| A aquisição | `systemctl` | ativa - e por isso o slot é confirmado com a tela morta |

**A rota fica como a §4 disse que ficaria**: a estação permanece no weston, o ambiente fica no
QEMU como resultado parcial declarado, e a linha foi retirada do perfil da placa.

O que se sabe e o que é hipótese:

- **Medido**: o renderizador acelerado do wlroots 0.17 não sobe sobre o EGL do driver
  proprietário desta placa. O risco estava escrito na §5 e na §3; agora é resultado.
- **Hipótese, não medida**: a HMI morre porque um cliente Qt Quick sobre esse EGL entrega seus
  quadros por um protocolo que só um compositor acelerado com o mesmo driver oferece. Não há
  *backtrace*.
- **Não separado**: "sem sinal" pode ser o compositor em software, pode ser o modo de vídeo -
  nada fixa `1920x1080@50` para o labwc, e esta placa já mostrou tela preta com modos que não
  são CEA (`BRINGUP_HMI_STM32MP2.md`). As duas causas estavam presentes no mesmo boot.
- **Defeito que isto expõe, fora do ambiente**: uma imagem cuja tela não funciona foi
  **confirmada como boa**. Só a aquisição pertence a `boot-complete.target`; a HMI em laço de
  queda não impede a confirmação nem chega a `failed`.

Caminhos para retomar, nenhum avaliado: o driver livre da mesma GPU no lugar do proprietário
(com o histórico de `BRINGUP_STM32MP2.md` sobre os dois juntos); um compositor que já fala com
este EGL (o weston fala) com um shell de área de trabalho por cima; ou a HMI em software dentro
do compositor em software, que troca a queda por CPU.

### 6.4 Fase D2, segunda tentativa (2026-10-12): o compositor na GPU, a HMI não

Duas mudanças sobre a §6.3: um *patch* no wlroots (`render/egl.c`: o driver anuncia
`EGL_EXT_device_query` e devolve nulo em `eglQueryDeviceStringEXT`; passa a ser lido como "sem
extensões de dispositivo") e a receita `wlr-randr`, com que a sessão fixa o modo das duas
variáveis do `meta-med-bsp`. Operado pelo operador.

| O quê | Como | Resultado |
|---|---|---|
| Antes de construir: o driver tem o que o wlroots exige? | `strings` nas bibliotecas da placa | `EGL_KHR_no_config_context`, `EGL_KHR_surfaceless_context`, `EGL_EXT_image_dma_buf_import`(`_modifiers`), `GL_EXT_texture_format_BGRA8888`, `GL_EXT_unpack_subimage` |
| **Renderizador** | diário do `labwc` | sobra uma linha, `Failed to query EGL devices` (a enumeração, contornada pelo GBM). **Nenhum erro de inicialização: o compositor está na GPU** |
| **O monitor** | operador | **há sinal**: tela preta com o ponteiro |
| **Modo** | `wlr-randr` na sessão | `HDMI-A-1`, `1920x1080 px, 50.000000 Hz (current)` |
| A barra | `pidof` | `sfwbar` vivo; **se está desenhada, ninguém confirmou** |
| **A HMI** | diário, `exception-trace` | **continua em SIGSEGV**, na `QSGRenderThread`, dentro de `libwayland-client`, logo depois de `wl_compositor.create_surface` |
| O que o driver espera | `strings` | `wl_viv`, `wl_drm`, `EGL_WL_bind_wayland_display` |
| O que o compositor anuncia | `WAYLAND_DEBUG=1`, 42 globais | `wl_drm` e `zwp_linux_dmabuf_v1`; **`wl_viv` não** |

A hipótese da §6.3 para a HMI ("compositor em software") estava **errada**: o compositor
acelerou e ela cai igual. A causa, agora medida nas duas pontas: o EGL do cliente deste driver
entrega quadros por um protocolo próprio (`wl_viv`), que só existe quando o compositor chama
`eglBindWaylandDisplayWL`. O weston chama; o wlroots deixou de chamar na série 0.15 e removeu o
caminho que importava esses buffers. O cliente não encontra o global e usa um objeto nulo.

O que isto decide: **qualquer cliente que desenhe pela GPU cai neste compositor, nesta placa** -
a HMI hoje, e não só ela. Clientes por memória compartilhada (a barra, o gerenciador de
arquivos, o revisor) não passam por esse caminho; nenhum deles foi visto na tela da placa.

Para a HMI rodar aqui o wlroots teria de reganhar o que removeu: chamar o *bind* e importar
buffers `EGL_WAYLAND_BUFFER_WL` como textura. É um porte, não um ajuste, e não foi escrito. O
renderizador por software do Qt Quick não serve de atalho: a HMI desenha os traços com geometria
própria no *scene graph*, que esse renderizador não desenha.

**Estado, ao fim do dia**: o porte foi escrito e funcionou - o registro está em
`implementation_plan_desktop_gpu_clients.md` §7. O perfil da placa em `kas/` tem a linha do
ensaio, **não commitada**. Os dois slots da placa têm imagens do ambiente; a de tela única não
está mais em nenhum.
