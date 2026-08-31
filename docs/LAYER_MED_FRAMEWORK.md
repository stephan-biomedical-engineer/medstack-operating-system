# Camada `meta-med-framework` — MedFramework, a fronteira de abstração

> **Status**: documentação de referência da camada. O documento arquitetural autoritativo continua
> sendo `PROJECT_CONTEXT.md`.
>
> **Posição**: camada 3/4 do `MedStack`, prioridade **9** — acima de `meta-med-distro` (8), abaixo
> de `meta-med-app` (10).
>
> **Regra que a define**: uma aplicação médica alcança o sistema operacional **exclusivamente** por
> aqui. Nenhuma aplicação toca sysfs, devfs, driver de BSP ou syscall diretamente.

---

## 1. O que distingue esta camada

É a única das quatro cujo conteúdo é majoritariamente **código C++** em vez de metadados bitbake:
20 arquivos, ~3.400 linhas, duas receitas.

E é a camada onde a tese deixa de ser organização de repositório e vira mecanismo. A frase que
resume o desenho: **as recusas do framework são o requisito.** A persistência se recusa a abrir um
repositório de registros cujo dispositivo de suporte não seja cifrado; a configuração se recusa a
carregar uma tabela de calibração sem selo conferido. A política regulatória não é documentada —
é executada.

```
meta-med-framework/
├── conf/layer.conf                                   29 linhas
└── recipes-framework/
    ├── packagegroups/packagegroup-med-framework.bb   18
    └── med-framework-api/
        ├── med-framework-api_1.0.0.bb                45
        └── files/
            ├── CMakeLists.txt                        77
            ├── medframework.pc.in                    10
            ├── include/   (7 cabeçalhos = a API pública, 816 linhas)
            └── src/       (7 .cpp + 1 cabeçalho interno, 2.392 linhas)
```

---

## 2. `conf/layer.conf` — a dependência que é arquitetura

```
LAYERDEPENDS_meta-med-framework = "core meta-med-distro"
```

A dependência de `meta-med-distro` **não é incidental**: o framework é implementado sobre a política
que aquela camada garante — journald com FSS para o `MedicalLogger`, o serviço D-Bus do RAUC para o
`MedicalUpdate`, o volume LUKS `/data` para o `MedicalStorage`. Declará-la é o que faz
`bitbake-layers show-depends` **provar** que a pilha é acíclica e unidirecional, em vez de a
arquitetura existir só no texto.

Note o que a declaração **não** tem: nenhuma menção a `meta-med-bsp` ou a qualquer BSP de
fabricante. O framework não pode saber que placa existe.

---

## 3. A receita `med-framework-api`

```
SRC_URI = "file://CMakeLists.txt;subdir=sources  file://include;subdir=sources  …"
S = "${WORKDIR}/sources"
DEPENDS = "systemd openssl"
inherit cmake pkgconfig features_check
REQUIRED_DISTRO_FEATURES = "systemd"
```

**`subdir=sources` não é organização.** Com `S = ${WORKDIR}`, o `PKGD` (`${WORKDIR}/package`) fica
*dentro* de `S`, e o passo de debug-source do `do_package` cria hardlink de cada fonte para
`package/usr/src/debug/…`. O pseudo passa a ver um inode sob dois caminhos e aborta a build ao
reexecutar qualquer task num workdir existente (`path mismatch [3 links]: ino N`). Efeito colateral
bom: a árvore de build do cmake passou a ser genuinamente out-of-tree.

**`REQUIRED_DISTRO_FEATURES = "systemd"`** com `features_check`: sd-journal e sd-bus não são detalhe
de implementação, são o mecanismo em que duas das seis APIs estão *especificadas*. Numa distro sem
systemd isso falha em tempo de parse, não em tempo de link.

**O nome do pacote instalado não é o nome da receita.** O manifest da imagem traz
`libmedframework1`: o `debian.bbclass` renomeia `${PN}` quando ele contém uma única biblioteca
compartilhada com SONAME. O `RDEPENDS = "med-framework-api"` do packagegroup continua resolvendo
porque a classe acrescenta o nome original ao `RPROVIDES` do pacote renomeado
(`debian.bbclass:51`). Vale saber: procurar `med-framework-api` no manifest não encontra nada, e a
conclusão errada é óbvia.

**`packagegroup-med-framework`** é o contrato de runtime — a biblioteca mais os serviços do SO com
que as implementações conversam (`dbus`, `rauc`). Todo perfil de dispositivo instala este
packagegroup **inalterado**: é a metade compartilhada da métrica de reuso.

---

## 4. `CMakeLists.txt` — duas dependências, e só duas

```
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
find_package(OpenSSL REQUIRED)
```

`libsystemd` (sd-journal + sd-bus) e `libcrypto` (SHA-256). **Todo o resto é POSIX** — é isso que
faz a mesma biblioteca compilar para `qemux86-64` e para o Cortex-A35 **sem um único `#ifdef`**, que
é a afirmação de portabilidade no ponto onde ela é mais verificável.

Compila com `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, limpo nas 7 unidades de tradução. O
`SOVERSION` tem justificativa regulatória explícita: uma aplicação validada contra a ABI 1 não pode
carregar silenciosamente a ABI 2 depois de uma atualização. E gera `medframework.pc`, a única
superfície de descoberta para as camadas de cima.

---

## 5. As seis APIs

### 5.1 `MedicalTypes` — o vocabulário

Não há exceções em lugar nenhum: toda falha volta como `Status` ou `Result<T>`. O motivo está no
cabeçalho — uma exceção escapando de um laço de aquisição é um *hazard*, não um diagnóstico.

Dois detalhes de projeto defensivo:

- `Result::fail(Status::Ok)` é convertido em `Internal`, senão `if (r)` levaria o chamador ao ramo
  de sucesso com valor não preenchido.
- `value()` num `Result` falho devolve o valor default-construído — quem ignora o status ainda lê
  dado definido, nunca lixo.

`formatTimestamp` é **UTC only**, deliberadamente: um dispositivo que muda de fuso não pode produzir
trilha de auditoria que pareça andar para trás.

### 5.2 `MedicalDevice` — a abstração que sustenta a portabilidade

Interface abstrata de seis métodos, incluindo `selfTest()` (IEC 60601-1 §14), mais uma **fábrica por
nome**:

```cpp
MedicalDeviceFactory::registerDriver("simulated", …);
MedicalDeviceFactory::registerDriver("rpmsg",     …);
```

O nome vem da configuração (`MED_EEG_DRIVER` → `/etc/medplatform/eeg.conf`), então trocar o backend
de aquisição é mudança de configuração e não de código. **É aqui, concretamente, que a métrica de
portabilidade acontece.** `registerDriver` **recusa** um nome já ocupado em vez de substituir:
substituir silenciosamente significaria que o nome no arquivo de configuração não identifica mais o
código que vai rodar.

`SimulatedDevice` tem semente fixa (`0x4D454547`), o que torna uma sessão gravada reproduzível e
utilizável como fixture de regressão. `RpmsgDevice` valida magic, versão, comprimento e **CRC32**
antes de converter de LSB para µV — quatro modos de falha distintos, nenhum deles deixando um quadro
corrompido parecer dado.

E o `namespace amp` carrega o `FrameHeader`: 40 bytes, `#pragma pack(1)`, com

```cpp
static_assert(sizeof(FrameHeader) == 40, "AMP frame header layout is ABI");
```

É o contrato de fio entre dois toolchains e duas arquiteturas (o M33 e o A35), e o `static_assert` é
o único mecanismo que impede que uma mudança inocente quebre o outro lado em silêncio.

### 5.3 `MedicalIPC` — dois transportes, uma interface

`RpmsgChar` (`/dev/rpmsg*`) e `UnixSeqpacket` (`AF_UNIX SOCK_SEQPACKET`). **A simetria é o
argumento**: os dois são orientados a mensagem — uma escrita é uma mensagem, uma leitura devolve
exatamente uma. Por isso o serviço escrito contra um funciona contra o outro sem alteração: no QEMU
fala com uma fonte simulada por socket, no hardware falaria com firmware por rpmsg.

`send()` reporta envio parcial como `IoError` em vez de truncar — "um quadro truncado de um
front-end é dado corrompido, e dado corrompido nunca pode parecer dado". `listen()` para `RpmsgChar`
devolve `NotSupported`, porque rpmsg não tem conceito de listener (o endpoint é criado pelo firmware
via remoteproc). O `descriptor()` existe para quem roda o próprio event loop — é como a HMI Qt
integra por `QSocketNotifier` sem que o framework precise conhecer Qt.

### 5.4 `MedicalLogger` — a trilha à prova de adulteração

Singleton por processo. Emite para journald com campos tipados (`MED_AUDIT_EVENT`, `MED_DEVICE_ID`,
`MED_SW_VERSION`, `MED_AUDIT_SEQ`, `MED_AUDIT_PREV`, `MED_AUDIT_HASH`) — estruturado para ser
*consultado*, não grepado.

A cadeia de hash é o núcleo: `digest = SHA256(digest_anterior + "|" + registro_serializado)`. Três
decisões que mostram cuidado real:

1. **Gênese explícita** (64 zeros) permite ao verificador distinguir cadeia nova de cadeia truncada.
2. **A cadeia só avança depois que o journald aceitou o registro** — avançar antes faria o próximo
   registro apontar para um predecessor que nenhum verificador encontra.
3. Digest vazio (falha do OpenSSL) → `Status::Internal` e nada é escrito: registro de auditoria sem
   evidência de adulteração é pior que falha barulhenta.

`AuditEvent` é um **enum fechado** de 17 valores, de propósito: uma trilha cujo vocabulário cada
aplicação inventa não é revisável através de uma família de produtos. O cabeçalho também é honesto
sobre o alcance — a cadeia *prova* alteração, não a *impede*; quem cobre o que este processo nunca
viu é o FSS do journald, que é política da camada de baixo.

### 5.5 `MedicalStorage` — a recusa que virou asserção de runtime

O método que importa é `hasEncryptedBacking()`: `stat()` no `/data`, monta
`/sys/dev/block/<major>:<minor>/dm/uuid` e exige que comece com `CRYPT-`. Se a exigência estiver
ligada e o backing não for dm-crypt, `open()` devolve `PermissionDenied` **antes de qualquer
escrita**.

É por isso que `MED_EEG_REQUIRE_ENCRYPTION = "true"` funciona como prova: o serviço de aquisição
*ter subido* já é a afirmação de que `/data` é um volume cifrado de verdade — medido na placa
física.

As outras duas propriedades: **atomicidade** (temp + `fsync` + `rename` + `fsync` do diretório — uma
queda de energia deixa o registro velho ou o novo, nunca metade) e **contenção** (`..`, caminho
absoluto e escape por symlink rejeitados *antes* de qualquer syscall tocar o filesystem).

### 5.6 `MedicalConfiguration` — integridade e o padrão DERS

`load()` exige um sidecar `<path>.sha256` que confira; sidecar ausente também é `IntegrityError`,
com a justificativa de que "uma tabela de calibração não verificável não é uma tabela utilizável". É
essa recusa que obriga a regra 8 do `CLAUDE.md`: toda receita que embarca um store tem de gerar o
sidecar depois de substituir variáveis.

`validate()` é o padrão DERS generalizado: uma tabela de `SafetyLimit` com mínimo, máximo e
descrição, e violar um limite é **evento reportável, nunca um clamp**. A verificação não para na
primeira violação — devolve todas, porque o operador precisa do quadro inteiro antes de decidir.

Detalhe de fronteira: o framework **não** registra na auditoria em nome do chamador. É deliberado,
para que a trilha sempre nomeie o componente responsável.

### 5.7 `MedicalUpdate` — cliente tipado do RAUC

Cliente sd-bus de `de.pengutronix.rauc.Installer`. A justificativa de existir, em vez de a aplicação
chamar o binário `rauc`: atualizar dispositivo médico é atividade regulada — a aplicação precisa
dizer ao operador qual versão roda, **recusar iniciar update durante aquisição**, e confirmar o slot
novo como bom só depois do próprio autoteste pós-update passar. Os três precisam de estado
estruturado, não de saída de CLI parseada. E a fronteira está declarada: esta classe nunca toca
bloco de disco nem ambiente de bootloader.

### 5.8 `src/MedDigest.h` — o único cabeçalho **não instalado**

SHA-256 sobre a interface EVP do OpenSSL. Fica em `src/` por decisão explícita: aplicações obtêm
integridade *através* de `MedicalConfiguration` e `MedicalLogger`, não hasheando coisas por conta
própria. É a regra 6 do `CLAUDE.md` aplicada dentro da própria camada.

---

## 6. Como as camadas de cima consomem

```
DEPENDS = "med-framework-api"                                            # eeg-acquisition-service
DEPENDS = "qtbase qtdeclarative qtdeclarative-native med-framework-api"  # eeg-hmi-gui
pkg_check_modules(MEDFRAMEWORK REQUIRED IMPORTED_TARGET medframework)    # ambos
```

Em runtime, `packagegroup-med-core` (em `meta-med-app`) puxa `packagegroup-med-framework` — e é essa
indireção que faz a métrica de reuso ser contável: EEG e tomógrafo instalam esse packagegroup byte a
byte igual.

---

## 7. As invariantes desta camada

1. **Nenhum `#ifdef` de máquina, nenhum caminho de dispositivo fora de configuração.**
2. **Duas dependências externas**, e crescer essa lista é um evento arquitetural, não um detalhe de
   build.
3. **Toda falha é tipada**; nenhuma exceção atravessa a fronteira da API.
4. **Todo dado persistido é verificável** — selo na configuração, cadeia de hash na auditoria, CRC
   no quadro de amostras.
5. **O framework não registra em nome do chamador**, para que a trilha nomeie o responsável.

---

## 8. O que esta camada ainda não tem evidência de

- **O driver `rpmsg` nunca rodou em alvo nenhum.** O caminho medido é o `simulated`; o
  `FrameHeader` continua sendo um contrato com um lado só implementado.
- **O `MedicalUpdate` foi exercitado apenas até onde o RAUC do QEMU chega** — que o serviço de
  atualização esteja *ativo* na suíte prova que o wrapper alcançou o daemon por D-Bus, mas
  `markBootedGood()` só terá efeito real quando a seleção de slot existir no alvo físico.
- **Os carimbos de tempo são corretos e a fonte de tempo não.** Na placa, sem RTC inicializado, o
  `MedicalLogger` e o `MedicalStorage` produzem registros com data errada — a API está certa e o
  registro sai errado, que é o tipo de defeito que nenhuma leitura de código encontra.
