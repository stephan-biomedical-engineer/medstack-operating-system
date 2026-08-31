# Camada `meta-med-app` — MedApp, aplicações e perfis de dispositivo

> **Status**: documentação de referência da camada. O documento arquitetural autoritativo continua
> sendo `PROJECT_CONTEXT.md`.
>
> **Posição**: camada 4/4 do `MedStack`, prioridade **10** — o topo da pilha.
>
> **Regra que a define**: **nada depende desta camada.** É isso que torna uma classe nova de
> dispositivo puramente aditiva.

---

## 1. Estrutura

A menor das quatro em linhas (1.412) e a que carrega mais coisas distintas: duas aplicações, três
packagegroups, dois perfis de dispositivo e um bundle de atualização.

```
meta-med-app/
├── conf/layer.conf                                     27 linhas
├── recipes-apps/
│   ├── eeg-acquisition-service/                        (receita 84 + 493 de conteúdo)
│   │   ├── eeg-acquisition-service_1.0.0.bb
│   │   └── files/{CMakeLists.txt, eeg.conf, eeg-acquisition.service, src/main.cpp}
│   └── eeg-hmi-gui/                                    (receita 48 + 447 de conteúdo)
│       ├── eeg-hmi-gui_1.0.0.bb
│       └── files/{CMakeLists.txt, eeg-hmi.service, src/{main.cpp,EegClient.h,EegClient.cpp}, qml/Main.qml}
└── recipes-core/
    ├── packagegroups/  packagegroup-med-{core,gui,amp}.bb
    ├── images/         med-image-{eeg,tomograph}.bb
    └── bundles/        med-bundle-eeg.bb
```

```
LAYERDEPENDS_meta-med-app = "core meta-med-framework qt6-layer"
```

`meta-med-framework` é o único caminho pelo qual esta camada alcança o sistema; `qt6-layer` fornece
o toolkit da HMI. **O que está ausente é o conteúdo da afirmação**: nenhuma camada de BSP, nenhuma
dependência de máquina de espécie alguma. É a afirmação de portabilidade do TCC, e
`bitbake-layers show-depends` é onde ela é conferida — não o texto.

---

## 2. Os três packagegroups — capacidade de plataforma, nunca aplicação

Essa é a regra que dá sentido à métrica de reuso: um packagegroup descreve *o que a plataforma sabe
fazer*, e a imagem é que instala *o que o dispositivo é*.

| Packagegroup | Conteúdo | Gancho de BSP |
|---|---|---|
| `packagegroup-med-core` | `packagegroup-med-framework`, e mais nada | — |
| `packagegroup-med-gui` | weston, weston-init, qtbase, qtdeclarative, qtwayland, fontes | `MED_GPU_PACKAGES ?= ""` |
| `packagegroup-med-amp` | `med-framework-api` + firmware do coprocessador | `MED_AMP_FIRMWARE ?= ""` |

**`packagegroup-med-core`** tem uma única entrada no `RDEPENDS`. É o runtime que todo perfil
instala, seja EEG, bomba de infusão ou tomógrafo: "diferem no que acrescentam sobre isto, nunca
nisto".

**`packagegroup-med-gui`** contém **nenhuma aplicação**. A pilha gráfica é a parte reutilizável; a
interface pertence ao perfil que a possui — é isso que permite ao tomógrafo reusar este packagegroup
byte a byte e mostrar uma tela completamente diferente. Tem `features_check` com
`REQUIRED_DISTRO_FEATURES = "wayland opengl"`.

**`packagegroup-med-amp`** faz uma distinção que vale reter: o lado do kernel (REMOTEPROC,
RPMSG_CHAR, virtio) é *política de distro* e vem do `linux-%.bbappend`, presente em todo alvo; o que
é genuinamente específico de máquina é o **firmware do coprocessador**, e isso é do BSP.

Os dois ganchos `?= ""` são o mecanismo que impede esta camada de aprender um nome de máquina. Valor
vazio em `MED_GPU_PACKAGES` significa renderização por software — **estado suportado, não
degradado**.

Os quatro packagegroups do projeto adotam `PACKAGE_ARCH = "${MACHINE_ARCH}"` por convenção (o
default de `packagegroup.bbclass` é `allarch`); o único cujo conteúdo realmente varia por máquina é
o de AMP.

---

## 3. Os dois perfis de dispositivo

Ambos começam igual:

```
require recipes-core/images/med-image-base.bb   # em meta-med-distro
require recipes-core/images/med-image-dev.inc   # idem
```

O `require` atravessa a fronteira de camada porque o `BBPATH` cobre todas — o perfil se constrói
*sobre* a imagem base do MedOS em vez de reafirmá-la. O comentário do `med-image-eeg.bb` diz o que
isso vale: "o delta abaixo é a diferença inteira entre uma plataforma Linux médica e um EEG".

O segundo `require` tem história: o `med-image-dev.inc` existe porque as duas imagens carregavam
cópias próprias das configurações de desenvolvimento **e elas divergiram**. Isso importa mais aqui
do que importaria em outro lugar — o tomógrafo se descreve como *caso de controle da avaliação
experimental*, e divergência entre os dois é contada pela métrica como se fosse diferença de classe
de dispositivo.

| | EEG | Tomógrafo |
|---|---|---|
| `packagegroup-med-core` | ✔ | ✔ |
| `packagegroup-med-gui` | ✔ | ✔ |
| `packagegroup-med-amp` | ✔ | — (sem coprocessador) |
| Aplicações | `eeg-acquisition-service`, `eeg-hmi-gui` | nenhuma |
| `IMAGE_ROOTFS_EXTRA_SPACE` | 256 MiB (sessão de aquisição) | 512 MiB (fatias reconstruídas) |

O tomógrafo não instalar aplicação nenhuma é por construção — é o que torna honesto reportar a
métrica como "a plataforma que as duas classes herdam tem 206 pacotes, o delta do EEG são 3" em vez
de como porcentagem de similaridade.

`MED_EEG_INSTALL ?=` é o único knob: sobrescrevê-lo sem as entradas de GUI constrói e boota o
caminho de aquisição no QEMU sem compilar Qt.

---

## 4. `med-bundle-eeg.bb` — o teste mais barato do caminho de atualização

`inherit bundle`, `RAUC_BUNDLE_SLOTS = "rootfs"`, `RAUC_SLOT_rootfs = "med-image-eeg"`. Três valores
são armadilhas fechadas explicitamente:

| Valor | Se ficasse no default |
|---|---|
| `RAUC_BUNDLE_COMPATIBLE = "${DISTRO}-${MACHINE}"` | O default da classe é `${MACHINE}-${TARGET_VENDOR}`; o bundle construiria limpo, assinaria limpo e seria **recusado no dispositivo** |
| `RAUC_BUNDLE_FORMAT = "verity"` | O default é vazio, emite um `bbwarn` discreto e produz um bundle plain que o `system.conf` recusa |
| `do_bundle[prefuncs]` verificando material de assinatura | A falha numa clonagem nova seria um erro opaco do openssl; com o guarda, ela nomeia o `make pki` |

A receita carrega ainda uma nota **para a dissertação**, e é honesta de um jeito que vale preservar:
`bundle.bbclass` assina durante `do_bundle` chamando `rauc bundle --key=`, ou seja, **a chave privada
de assinatura é legível pela build**. Isso é aceitável para a CA de desenvolvimento e é incompatível
com o modelo de produção (chave em HSM, assinatura como passo separado sobre o artefato pronto). O
texto diz para apresentar assim, não como se fosse embarcável.

---

## 5. `eeg-acquisition-service` — a aplicação de referência

### 5.1 A receita

`DEPENDS = "med-framework-api"` — só. É a **regra 6** do `CLAUDE.md` visível numa linha: se um
recurso novo parece precisar de uma biblioteca de sistema no `DEPENDS` de uma aplicação, o recurso
pertence a uma API do framework.

As duas variáveis que fazem o mesmo binário servir os dois alvos são substituídas por `sed` no
`do_install:append`:

```
MED_EEG_DRIVER ?= "simulated"
MED_EEG_REQUIRE_ENCRYPTION ?= "true"
```

E então `do_seal_configuration`: como o `MedicalConfiguration` se recusa a carregar um store que não
pode verificar, o sidecar `.sha256` tem de ser produzido por quem produziu o store — aqui, a build.
Feito em Python (`bb.utils.sha256_file`) para não depender do que estiver em `HOSTTOOLS`. Mas o
detalhe caro são as duas linhas seguintes:

```
do_seal_configuration[fakeroot] = "1"
do_seal_configuration[depends] += "virtual/fakeroot-native:do_populate_sysroot"
```

`base.bbclass` concede pseudo ao `do_install` apenas; uma task adicionada com `addtask` não herda
nada. Sem esses dois flags o sidecar sai com o uid do usuário da build — o QA pega como
`[host-user-contaminated]`, e o que embarcaria seria um dispositivo cujo **selo de configuração
pertence a uma conta sem privilégio**, justamente o único arquivo que não pode.

### 5.2 `eeg.conf`

Front-end, aquisição (8 canais, 250 Hz, 25 amostras/quadro), armazenamento, socket de publicação e
envelope de segurança (`safety.max_input_uv = 500`). Um comentário ali é uma restrição de hardware
descoberta antes da hora e registrada no lugar certo: um buffer rpmsg padrão de 512 bytes comporta
40 bytes de cabeçalho + 114 int32, então um perfil AMP de 8 canais tem de cair para **12 amostras
por quadro**.

### 5.3 A unit

Duas metades. **Tempo real**: `CPUSchedulingPolicy=rr`, prioridade 50, `IOSchedulingClass=realtime`
— sem isso a aquisição compete com a HMI e os quadros chegam atrasados sob carga; o `make check`
verifica que o kernel *concedeu* a política, não que a unit a declarou.

**Sandbox**, que é a partição IEC 62304 sendo executada e não afirmada: `PrivateNetwork=yes` +
`RestrictAddressFamilies=AF_UNIX` (qualquer outra coisa seria caminho de exfiltração de dado de
paciente), `ReadWritePaths=/data` como único destino de escrita, `MemoryDenyWriteExecute`,
`SystemCallFilter=@system-service` com `SystemCallErrorNumber=EPERM`, e `DeviceAllow=/dev/rpmsg0`,
inofensivo onde o nó não existe.

`RuntimeDirectory`/`StateDirectory` fazem o systemd criar `/run/medplatform` e
`/var/lib/medplatform` com o modo certo — é assim que isso funciona num rootfs somente-leitura. E
`Wants=data.mount`, **não** `Requires`: num alvo sem a partição, o serviço deve falhar alto nos
próprios termos, com registro de auditoria, em vez de ser silenciosamente segurado pelo systemd.

### 5.4 `main.cpp` — 361 linhas sem uma syscall

Sete `#include <medplatform/...>` e mais nada do sistema. A ordem de partida é o desenho principal,
e cada passo é uma condição de parada, não um aviso:

1. **Logger primeiro** (com `deviceId` de `/etc/machine-id`), para que a falha seguinte já tenha
   onde ser registrada.
2. **Configuração verificada**; falhar aqui é `SelfTestFailed` e `return 1`, porque toda amostra que
   o serviço produziria é escalada por esses números.
3. **Limites de segurança** declarados **na aplicação, não no framework** — o que conta como taxa de
   amostragem segura é propriedade do dispositivo médico, não da plataforma. Os quatro limites vêm
   com descrição clínica ("abaixo de 125 Hz um EEG não resolve a banda beta").
4. **Device + `selfTest()`**, auditado nos dois desfechos.
5. **Storage**; e se a exigência de criptografia estiver desligada, isso vai para a auditoria como
   `SecurityEvent` — é configuração legítima de desenvolvimento e ilegítima clinicamente, e de
   qualquer forma é uma decisão.
6. **Servidor IPC** publicando o socket.
7. **Só então `markBootedGood()`.** O comentário é a definição operacional de "atualização
   bem-sucedida": chegar aqui significa que o software instalado *neste slot* subiu, passou no
   autoteste e abriu todos os recursos — isso, e não "o kernel bootou", é a condição para confirmar
   um update.

O laço tem três decisões que merecem nota: `accept()` com timeout **zero** (um visualizador
conectando nunca trava a aquisição); **o registro é gravado antes de ser publicado** ("o que um
clínico revisa depois é autoritativo, o que a tela mostra é conveniência"); e `SIGPIPE` ignorado,
com cliente que falha no `send` removido da lista — uma HMI que morre não pode derrubar a aquisição.

---

## 6. `eeg-hmi-gui` — o visualizador

### 6.1 A receita

`DEPENDS = "qtbase qtdeclarative qtdeclarative-native med-framework-api"`. O `qtdeclarative-native`
está ali com a explicação da primeira build verde: `qt6-cmake.bbclass` só puxa `qtbase-native` e
aponta o `QT_HOST_PATH` para ele, então o `Qt6QmlConfig.cmake` do alvo não acha
`Qt6QmlTools`/`Qt6QuickTools` (`qmlcachegen`, `qmltyperegistrar`, `qmlimportscanner`) e o
`find_package` falha no `do_configure`. É dependência de ferramenta de *host*, então não conta
contra a regra 6.

### 6.2 O código

`main.cpp` tem 36 linhas: cria o `EegClient`, expõe como context property `eeg`, carrega o QML.

`EegClient` é o modelo QML do fluxo, e o cabeçalho diz o que ele **não** contém: nenhum código de
socket, nenhum framing, nenhum tratamento de endianness próprio — consome `MedicalIPC` e a definição
de quadro AMP do framework, e é por isso que é indiferente a se as amostras vieram de um Cortex-M33
ou do simulador. Quatro comportamentos:

- **Reconexão como estado normal**: o serviço pode ainda não ter criado o socket, ou pode reiniciar
  embaixo. Nada disso é erro para uma HMI — ela continua tentando e diz na tela.
- **Integração por `QSocketNotifier`** sobre o `descriptor()` do canal: o framework não conhece Qt, e
  o Qt não conhece o transporte.
- **Validação antes de desenhar** (magic, versão, tamanho, CRC). Quadro reprovado incrementa
  `framesRejected` e **não é desenhado**, porque "um display que desenha um quadro corrompido é pior
  que um que não desenha nada: o operador não sabe distinguir".
- **Buffer limitado** (750 amostras por canal) e detecção de lacuna por número de sequência — perda
  de quadro é visível, não silenciosa.

`Main.qml` são 172 linhas: cabeçalho com indicador, `Canvas` com as raias por canal, rodapé com
contadores. "Deliberadamente simples: uma tela de forma de onda clínica é julgada por legibilidade e
por nunca mostrar algo de que não tem certeza".

### 6.3 A unit, e os dois defeitos que ela já custou

`XDG_RUNTIME_DIR=/run` — e **não** `/run/user/0`: o `weston-init` roda o compositor como
`User=weston` e publica um socket *de sistema* em `/run/wayland-0`. O valor errado fazia o Qt
resolver um caminho inexistente e abortar, reiniciando a cada ~7 s.

`Requires`/`After` nomeiam `weston.socket`, e não apenas `weston.service`: aquela unidade é
`Type=notify` e o socket vem de uma unidade separada, então ordenar só pelo serviço deixava uma
janela de corrida.

O sandbox aqui é o outro lado da partição IEC 62304 — e a descrição da receita coloca a tese em uma
frase: a separação entre o caminho de aquisição e o display é **imposta pelo sandbox no arquivo de
unidade, não apenas afirmada num documento**.

---

## 7. As invariantes desta camada

1. **Nenhum nome de máquina**, em nenhum arquivo.
2. **Nenhuma aplicação dentro de um packagegroup**; nenhuma política de SO dentro de uma imagem.
3. **Aplicações nunca ganham uma segunda dependência** além do framework (e do toolkit, para a HMI).
4. **Os dois perfis de dispositivo diferem em exatamente uma lista de instalação.**
5. **Ganchos de BSP são variáveis com default vazio**, nunca overrides de máquina.

---

## 8. O que esta camada ainda não tem evidência de

- **A HMI nunca rodou estável por mais de ~90 s em alvo nenhum.** No QEMU, o `QSGRenderThread` é
  morto por SIGSYS ao chamar `mincore`, syscall que não pertence a nenhum grupo funcional do systemd
  — ver `BRINGUP_HMI_QEMU.md` §7. Na placa, o weston só passou a subir depois do `seatd`, e a
  correção não foi exercitada em hardware.
- **O perfil do tomógrafo nunca foi construído para o STM32MP257.** A métrica de reuso é inteira
  sobre `qemux86-64`.
- **`MED_AMP_FIRMWARE` está vazio nos dois alvos**, o que significa que o caminho que justifica a
  existência do `packagegroup-med-amp` continua sendo o único que nada exercitou.
